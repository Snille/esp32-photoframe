#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hal_firebeetle";

// Classic ESP32: GPIO18/GPIO23 are the VSPI (SPI3_HOST) native pins
#define FIREBEETLE_SPI_HOST SPI3_HOST

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing DFRobot FireBeetle 2 ESP32-E Board HAL");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 600 * 400 / 2 + 100,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(FIREBEETLE_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    epaper_config_t ep_cfg = {
        .spi_host = FIREBEETLE_SPI_HOST,
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
    ESP_LOGI(TAG, "Preparing FireBeetle for sleep");
    epaper_enter_deepsleep();
    return ESP_OK;
}

// --- Battery monitoring ---
// The FireBeetle 2 ESP32-E ties the LiPo (VBAT) to GPIO34 through two 1M
// resistors (a 1:2 divider), so the pad sees half the battery voltage.
// GPIO34 = ADC1 channel 6. The 1M divider is high-impedance and noisy, so we
// average many samples and use the eFuse ADC calibration for accuracy.
#define VBAT_ADC_UNIT    ADC_UNIT_1
#define VBAT_ADC_CHANNEL ADC_CHANNEL_6  // GPIO34
#define VBAT_ADC_ATTEN   ADC_ATTEN_DB_12
#define VBAT_DIVIDER     2.0f
#define VBAT_SAMPLES     64

static adc_oneshot_unit_handle_t bat_adc_handle = NULL;
static adc_cali_handle_t bat_cali_handle = NULL;
static bool bat_calibrated = false;

static void board_hal_battery_adc_init(void)
{
    if (bat_adc_handle)
        return;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = VBAT_ADC_UNIT,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    if (adc_oneshot_new_unit(&init_config, &bat_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC init failed");
        bat_adc_handle = NULL;
        return;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = VBAT_ADC_ATTEN,
    };
    if (adc_oneshot_config_channel(bat_adc_handle, VBAT_ADC_CHANNEL, &chan_config) != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC channel config failed");
        adc_oneshot_del_unit(bat_adc_handle);
        bat_adc_handle = NULL;
        return;
    }

    // Classic ESP32 supports line-fitting calibration from eFuse.
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = VBAT_ADC_UNIT,
        .atten = VBAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    bat_calibrated =
        (adc_cali_create_scheme_line_fitting(&cali_config, &bat_cali_handle) == ESP_OK);
    if (!bat_calibrated) {
        ESP_LOGW(TAG, "Battery ADC calibration unavailable, using raw scaling");
    }
}

int board_hal_get_battery_voltage(void)
{
    board_hal_battery_adc_init();
    if (!bat_adc_handle)
        return -1;

    int64_t acc = 0;
    int n = 0;
    for (int i = 0; i < VBAT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(bat_adc_handle, VBAT_ADC_CHANNEL, &raw) == ESP_OK) {
            acc += raw;
            n++;
        }
    }
    if (n == 0)
        return -1;
    int avg_raw = (int) (acc / n);

    int mv;
    if (bat_calibrated && adc_cali_raw_to_voltage(bat_cali_handle, avg_raw, &mv) == ESP_OK) {
        return (int) (mv * VBAT_DIVIDER);
    }
    // Fallback: linear scaling (12-bit, ~3.3V full-scale at this attenuation)
    return (int) ((float) avg_raw * (3300.0f / 4095.0f) * VBAT_DIVIDER);
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

    // LiPo discharge curve (open-circuit, approximate)
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
    // The charge IC's status line is not broken out on this board, so charging
    // state cannot be read. "Full" is inferred from voltage/percent (~4.2V).
    return false;
}

bool board_hal_is_usb_connected(void)
{
    // Classic ESP32 has no USB-JTAG; USB detection is not available.
    // Deep sleep will trigger on its normal timer/button schedule.
    return false;
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
