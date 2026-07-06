#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hal_ee04";

// Pin Definitions for XIAO EE04
// XIAO ESP32-S3 has precise battery voltage on IO1 (A0) via voltage divider (R11=100k, R10=100k =>
// factor 2)
#define VBAT_ADC_CHANNEL ADC_CHANNEL_0  // GPIO 1 is ADC1_CHANNEL_0
#define VBAT_VOLTAGE_DIVIDER 2.0f
#define VBAT_ADC_ENABLE_PIN GPIO_NUM_6  // TPS22916 enable - must be HIGH to read battery voltage

static adc_oneshot_unit_handle_t adc_handle = NULL;

// Lowest believable resting voltage for a running pack; below this the read
// is likely a transient collapse (e.g. WiFi-TX rail sag) rather than a
// genuinely low pack. Same value used by the EE02/FireBeetle drivers.
#define VBAT_MIN_PLAUSIBLE_MV 3300

// A single-wake drop bigger than this (mV) vs. the last CONFIRMED reading is
// treated as a suspect WiFi-TX rail sag rather than a real level change — see
// driver_seeedstudio_xiao_ee02.c's confirm_vbat_reading() for the full
// rationale (identical logic, duplicated here since each board driver in this
// codebase owns its own static state rather than sharing it).
#define VBAT_SUSPECT_DROP_MV 300
#define VBAT_CONFIRM_TOLERANCE_MV 150
#define VBAT_REQUIRED_AGREEMENTS 3

RTC_DATA_ATTR static int s_vbat_last_confirmed_mv = -1;
RTC_DATA_ATTR static int s_vbat_pending_mv = -1;
RTC_DATA_ATTR static int s_vbat_pending_streak = 0;

// Set once per wake by board_hal_battery_prime_reading() (before WiFi
// starts) and reused for the rest of the wake. Plain (non-RTC) static:
// doesn't need to survive deep sleep.
static int s_vbat_cached_mv = -1;

static int confirm_vbat_reading(int raw_mv)
{
    if (raw_mv <= 0) {
        return s_vbat_last_confirmed_mv;
    }
    if (s_vbat_last_confirmed_mv <= 0) {
        s_vbat_last_confirmed_mv = raw_mv;
        s_vbat_pending_mv = -1;
        s_vbat_pending_streak = 0;
        return raw_mv;
    }

    int drop = s_vbat_last_confirmed_mv - raw_mv;
    if (drop < VBAT_SUSPECT_DROP_MV) {
        s_vbat_last_confirmed_mv = raw_mv;
        s_vbat_pending_mv = -1;
        s_vbat_pending_streak = 0;
        return raw_mv;
    }

    int pending_diff = s_vbat_pending_mv > 0 ? raw_mv - s_vbat_pending_mv : 0;
    if (pending_diff < 0)
        pending_diff = -pending_diff;
    if (s_vbat_pending_mv > 0 && pending_diff < VBAT_CONFIRM_TOLERANCE_MV) {
        s_vbat_pending_streak++;
    } else {
        s_vbat_pending_mv = raw_mv;
        s_vbat_pending_streak = 1;
    }

    if (s_vbat_pending_streak >= VBAT_REQUIRED_AGREEMENTS) {
        s_vbat_last_confirmed_mv = raw_mv;
        s_vbat_pending_mv = -1;
        s_vbat_pending_streak = 0;
        return raw_mv;
    }

    return s_vbat_last_confirmed_mv;
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE04 Board HAL");

    // Release any pad hold latched by the previous deep-sleep cycle so we
    // can reconfigure the TPS22916 enable pin during init.
    gpio_hold_dis(VBAT_ADC_ENABLE_PIN);

    // Initialize SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 800 * 480 / 2 + 100,  // Sufficient for 7.3" EPD
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Initialize E-Paper Display Port
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = -1,
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,
    };
    epaper_init(&ep_cfg);

    // Initialize ADC for battery voltage
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten =
            ADC_ATTEN_DB_12,  // Full range up to ~3.3V (renamed from ADC_ATTEN_DB_11 in IDF 5.x)
    };
    ret = adc_oneshot_config_channel(adc_handle, VBAT_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure ADC enable pin (TPS22916 load switch)
    // GPIO6 must be HIGH before reading battery voltage on GPIO1 (A0).
    // The TPS22916 gates the voltage divider; without enabling it, ADC reads 0V.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VBAT_ADC_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);  // Keep LOW by default to save power

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing EE04 for sleep");

    epaper_enter_deepsleep();

    // Drive TPS22916 enable LOW and latch the pad so the load switch stays
    // disabled once the digital IO domain powers down. Without the hold the
    // pin can float, re-enable the switch, and continuously drain the
    // 100k+100k battery divider.
    gpio_set_direction(VBAT_ADC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);
    gpio_hold_en(VBAT_ADC_ENABLE_PIN);
    gpio_deep_sleep_hold_en();

    // Disable ADC to save power
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    int voltage = board_hal_get_battery_voltage();
    return voltage > 2500;  // If we read > 2.5V, a battery is connected
}

static int read_vbat_mv_raw(void)
{
    if (!adc_handle)
        return -1;

    // Enable TPS22916 load switch (GPIO6 HIGH) to connect voltage divider to ADC
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));  // Allow voltage to stabilize

    int adc_raw;
    int result = -1;
    if (adc_oneshot_read(adc_handle, VBAT_ADC_CHANNEL, &adc_raw) == ESP_OK) {
        // TODO: Use esp_adc_cal / adc_cali for calibrated readings; raw conversion
        // can be off by 5-15% due to non-linear ADC response on ESP32-S3.
        float voltage_mv = (float) adc_raw * (3300.0f / 4095.0f) * VBAT_VOLTAGE_DIVIDER;
        result = (int) voltage_mv;
    }

    // Disable TPS22916 to save power; only enable briefly during reads
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);

    return result;
}

esp_err_t board_hal_battery_prime_reading(void)
{
    // Three quick reads, taken before WiFi starts (see main.c), averaged —
    // the quietest point in the wake cycle.
    int best = -1;
    long sum = 0;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        int mv = read_vbat_mv_raw();
        if (mv <= 0)
            continue;
        if (mv > best)
            best = mv;
        if (mv >= VBAT_MIN_PLAUSIBLE_MV) {
            sum += mv;
            n++;
        }
    }
    int avg_mv = n > 0 ? (int) (sum / n) : best;
    int mv = confirm_vbat_reading(avg_mv);
    if (mv <= 0) {
        return ESP_FAIL;
    }
    s_vbat_cached_mv = mv;
    ESP_LOGI(TAG, "Battery primed: %d mV", mv);
    return ESP_OK;
}

int board_hal_get_battery_voltage(void)
{
    if (s_vbat_cached_mv > 0) {
        return s_vbat_cached_mv;
    }
    // Priming wasn't called or failed; fall back to a live read (may be
    // exposed to WiFi-TX sag if this happens mid-pull).
    int mv = confirm_vbat_reading(read_vbat_mv_raw());
    if (mv > 0) {
        s_vbat_cached_mv = mv;
    }
    return mv;
}

int board_hal_get_battery_percent(void)
{
    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0)
        return -1;

    // Simple linear approximation for LiPo
    // 4.2V = 100%, 3.3V = 0%
    if (voltage >= 4200)
        return 100;
    if (voltage <= 3300)
        return 0;

    return (voltage - 3300) * 100 / (4200 - 3300);
}

bool board_hal_is_charging(void)
{
    // Not detectable: like the EE02, the BQ24070 charge-status pins drive
    // on-board LEDs only and are not routed to a GPIO (verify against the EE04
    // schematic; revisit if a future board rev breaks STAT out to a GPIO).
    return false;
}

bool board_hal_supports_charge_status(void)
{
    // No real charge status and no USB+voltage heuristic implemented yet.
    // Could mirror the EE02 heuristic (USB works via usb_serial_jtag) once
    // verified on EE04 hardware.
    return false;
}

// Battery ADC is fixed at compile time on this board (see above) rather than
// user-selectable, so there's nothing to enumerate/reconfigure at runtime.
int board_hal_get_battery_adc_pin_options(const board_hal_battery_adc_pin_t **out_pins)
{
    (void) out_pins;
    return 0;
}

esp_err_t board_hal_set_battery_adc_pin(int gpio_num)
{
    (void) gpio_num;
    return ESP_ERR_NOT_SUPPORTED;
}

int board_hal_get_battery_adc_pin(void)
{
    return -1;
}

bool board_hal_is_usb_connected(void)
{
    return usb_serial_jtag_is_connected();
}

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown not supported on BQ24070, entering deep sleep instead");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool board_hal_rtc_is_available(void)
{
    return false;
}

esp_err_t board_hal_get_temperature(float *t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_get_humidity(float *h)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void board_hal_led_set(board_hal_led_t led, bool on)
{
    // No onboard LED on XIAO EE04
    (void) led;
    (void) on;
}
