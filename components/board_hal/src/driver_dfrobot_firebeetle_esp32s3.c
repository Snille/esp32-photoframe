#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "board_hal_firebeetle_s3";

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing DFRobot FireBeetle 2 ESP32-S3 Board HAL");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 600 * 400 / 2 + 100,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = -1,
        .pin_enable = -1,  // PWR is hardwired to 3V3
    };
    epaper_init(&ep_cfg);

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing FireBeetle S3 for sleep");
    epaper_enter_deepsleep();
    return ESP_OK;
}

// --- Battery monitoring ---
// No VBAT sense pin or charge-status line is documented for this board (charge
// IC is DFRobot's ETA6003, confirmed against the schematic 2026-07-05 -- STAT
// only drives a status LED, and the AXP313A PMIC on this board powers the
// camera FPC from VCC/USB, not the battery). Unlike the other boards here,
// there is no factory-wired divider to read at all.
//
// What IS supported: an OPTIONAL user-wired external divider (VBAT -- 1M --
// GPIO -- 1M -- GND) on one of a handful of free, ADC1-capable, non-strapping
// header pins, selected at runtime via the WebGUI (see
// board_hal_get_battery_adc_pin_options() / board_hal_set_battery_adc_pin()
// below) and persisted by config_manager. Until configured, everything below
// reports unknown, same as before.
//
// Candidate pins were cross-checked against this board's own board_hal usage
// (SPI SCLK/MOSI = GPIO17/15, EPD CS/DC = GPIO10/11 = A4/A5, EPD BUSY/RST =
// GPIO18/38, wake button = GPIO9) and against the ESP32-S3 strapping pins
// (GPIO0/3/45/46 -- avoid, see the CS/DC rework comment above) plus the
// console UART (GPIO43/44). A0-A3 remain free and are all on ADC1, which is
// safe to sample alongside WiFi (unlike ADC2).
static const board_hal_battery_adc_pin_t s_battery_adc_pin_options[] = {
    {GPIO_NUM_4, "A0 (GPIO4)"},
    {GPIO_NUM_5, "A1 (GPIO5)"},
    {GPIO_NUM_6, "A2 (GPIO6)"},
    {GPIO_NUM_8, "A3 (GPIO8)"},
};
#define BATTERY_ADC_PIN_OPTIONS_COUNT \
    (sizeof(s_battery_adc_pin_options) / sizeof(s_battery_adc_pin_options[0]))

#define VBAT_VOLTAGE_DIVIDER 2.0f  // two equal resistors (e.g. 1M + 1M)

// Lowest believable resting voltage for a running pack; below this the read
// is likely a transient collapse rather than a genuinely low pack.
#define VBAT_MIN_PLAUSIBLE_MV 3300

static battery_adc_t *s_vbat_adc = NULL;
static int s_vbat_adc_gpio = -1;

// Set once per wake by board_hal_battery_prime_reading() (before WiFi starts)
// and reused for the rest of the wake; see the EE02 driver for the full
// rationale. Plain (non-RTC) static: doesn't need to survive deep sleep.
static int s_vbat_cached_mv = -1;

// GPIO -> ADC1 channel, for the pins in s_battery_adc_pin_options. ESP32-S3
// ADC1 channels map 1:1 to GPIO1-10 (channel = gpio - 1).
static adc_channel_t gpio_to_adc1_channel(int gpio_num)
{
    return (adc_channel_t) (gpio_num - 1);
}

int board_hal_get_battery_adc_pin_options(const board_hal_battery_adc_pin_t **out_pins)
{
    *out_pins = s_battery_adc_pin_options;
    return BATTERY_ADC_PIN_OPTIONS_COUNT;
}

esp_err_t board_hal_set_battery_adc_pin(int gpio_num)
{
    if (s_vbat_adc) {
        battery_adc_destroy(s_vbat_adc);
        s_vbat_adc = NULL;
    }
    s_vbat_adc_gpio = -1;
    s_vbat_cached_mv = -1;  // stale for the previous pin/divider, if any

    if (gpio_num < 0) {
        ESP_LOGI(TAG, "Battery ADC pin disabled");
        return ESP_OK;
    }

    bool valid = false;
    for (size_t i = 0; i < BATTERY_ADC_PIN_OPTIONS_COUNT; i++) {
        if (s_battery_adc_pin_options[i].gpio_num == gpio_num) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        ESP_LOGE(TAG, "GPIO%d is not a valid battery ADC pin on this board", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    battery_adc_config_t vbat_cfg = {
        .unit = ADC_UNIT_1,
        .channel = gpio_to_adc1_channel(gpio_num),
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = -1,  // plain always-on divider, no load switch
        .settle_ms = 0,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = 1.0f,
    };
    esp_err_t ret = battery_adc_create(&vbat_cfg, &s_vbat_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC init on GPIO%d failed: %s", gpio_num, esp_err_to_name(ret));
        return ret;
    }

    s_vbat_adc_gpio = gpio_num;
    ESP_LOGI(TAG, "Battery ADC configured on GPIO%d", gpio_num);
    return ESP_OK;
}

int board_hal_get_battery_adc_pin(void)
{
    return s_vbat_adc_gpio;
}

// Cross-wake confirmation to filter WiFi-TX-induced ADC sag: the same fix
// applied to the EE02 and FireBeetle-E this session (see
// driver_seeedstudio_xiao_ee02.c's confirm_vbat_reading() for the full
// rationale/data behind the thresholds). A single suspiciously-low reading is
// held back until it repeats VBAT_REQUIRED_AGREEMENTS times in a row; until
// then the last confirmed value is returned instead.
#define VBAT_SUSPECT_DROP_MV 300
#define VBAT_CONFIRM_TOLERANCE_MV 150
#define VBAT_REQUIRED_AGREEMENTS 3

RTC_DATA_ATTR static int s_vbat_last_confirmed_mv = -1;
RTC_DATA_ATTR static int s_vbat_pending_mv = -1;
RTC_DATA_ATTR static int s_vbat_pending_streak = 0;

static int confirm_vbat_reading(int raw_mv)
{
    if (raw_mv <= 0)
        return s_vbat_last_confirmed_mv;

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

esp_err_t board_hal_battery_prime_reading(void)
{
    if (!s_vbat_adc) {
        // No divider wired / no pin selected on the frame's own WebGUI.
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Three quick reads, taken before WiFi starts (see main.c), averaged by
    // the shared helper — the quietest point in the wake cycle.
    int mv = confirm_vbat_reading(battery_adc_read_mv_multi(s_vbat_adc, 3, VBAT_MIN_PLAUSIBLE_MV));
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
    if (!s_vbat_adc)
        return -1;
    // Priming wasn't called or failed; fall back to a live read (may be
    // exposed to WiFi-TX sag if this happens mid-pull).
    int mv = confirm_vbat_reading(battery_adc_read_mv(s_vbat_adc));
    if (mv > 0) {
        s_vbat_cached_mv = mv;
    }
    return mv;
}

bool board_hal_is_battery_connected(void)
{
    // No dedicated detect line; infer from a plausible LiPo voltage.
    int mv = board_hal_get_battery_voltage();
    return mv > 2500 && mv < 4400;
}

int board_hal_get_battery_percent(void)
{
    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0)
        return -1;

    // LiPo discharge curve (open-circuit, approximate) -- same table used by
    // the FireBeetle-E driver.
    static const struct {
        int mv;
        int pct;
    } cal[] = {
        {4150, 100}, {3960, 90}, {3910, 80}, {3850, 70}, {3800, 60}, {3750, 50},
        {3680, 40},  {3580, 30}, {3490, 20}, {3410, 10}, {3300, 5},  {3270, 0},
    };

    if (voltage >= cal[0].mv)
        return 100;
    if (voltage <= cal[sizeof(cal) / sizeof(cal[0]) - 1].mv)
        return 0;

    for (int i = 0; i < (int) (sizeof(cal) / sizeof(cal[0])) - 1; i++) {
        if (voltage >= cal[i + 1].mv) {
            int dv = cal[i].mv - cal[i + 1].mv;
            int dp = cal[i].pct - cal[i + 1].pct;
            return cal[i + 1].pct + (voltage - cal[i + 1].mv) * dp / dv;
        }
    }
    return 0;
}

bool board_hal_is_charging(void)
{
    return false;
}

bool board_hal_supports_charge_status(void)
{
    return false;
}

bool board_hal_is_usb_connected(void)
{
    // ESP32-S3 native USB is broken out on this board (unlike the classic
    // ESP32-E FireBeetle), so real USB-presence detection is available.
    return usb_serial_jtag_is_connected();
}

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown not supported, entering deep sleep instead");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_get_temperature(float *t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_get_humidity(float *h)
{
    return ESP_ERR_NOT_SUPPORTED;
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

void board_hal_led_set(board_hal_led_t led, bool on)
{
    (void) led;
    (void) on;
}
