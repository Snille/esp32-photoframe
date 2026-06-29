#include "board_hal.h"
#include "battery_adc.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hal_ee02";

// Pin Definitions for XIAO EE02
// Based on typical XIAO ESP32-S3 usage:
// VBAT read often uses GPIO 1 with divider, or internal measurement.
// XIAO ESP32-S3 has precise battery voltage on IO1 (A0) via voltage divider (R11=100k, R10=100k =>
// factor 2) D0/GPIO1 is A0
#define VBAT_ADC_CHANNEL ADC_CHANNEL_0  // GPIO 1 is ADC1_CHANNEL_0
#define VBAT_ADC_ENABLE_PIN GPIO_NUM_6  // TPS22916 load switch enable
#define VBAT_VOLTAGE_DIVIDER 2.0f

// Optional per-unit correction for resistor-divider tolerance, measured with a
// multimeter (1.0 = none). Override at build time if a board reads consistently
// high/low.
#ifndef VBAT_CAL_SCALE
#define VBAT_CAL_SCALE 1.0f
#endif

// USB detection (VBUS): handled via usb_serial_jtag_is_connected() in
// board_hal_is_usb_connected() below — VBUS is not broken out to a readable GPIO
// on this board (rev V1.0), so the USB-Serial/JTAG link state is the signal.
// Charge status: per the official Seeed EE02 schematic the BQ24070 STAT pins go
// to LEDs only, not a GPIO — see board_hal_is_charging() below.
// Verified pin map (rev V1.0): BAT_ADC -> GPIO1 (D0/A0); ADC load-switch enable
// (TPS22916) -> GPIO6; buttons -> GPIO2/3/5.

static battery_adc_t *vbat_adc = NULL;

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE02 Board HAL");

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
        .max_transfer_sz = 1200 * 1600 / 2 + 100,  // Sufficient for 13.3" EPD
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Initialize E-Paper Display Port
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = BOARD_HAL_EPD_CS1_PIN,
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,
    };
    epaper_init(&ep_cfg);

    // Configure TPS22916 enable pin (GPIO6) as output, default LOW (disabled)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VBAT_ADC_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);

    // Initialize the shared, eFuse-calibrated + averaged battery ADC reader.
    // The TPS22916 enable pin is already configured as an output above; the
    // helper only toggles it during a read.
    battery_adc_config_t vbat_cfg = {
        .unit = ADC_UNIT_1,
        .channel = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = VBAT_ADC_ENABLE_PIN,
        .settle_ms = 10,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = VBAT_CAL_SCALE,
    };
    esp_err_t ret = battery_adc_create(&vbat_cfg, &vbat_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing EE02 for sleep");

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
    if (vbat_adc) {
        battery_adc_destroy(vbat_adc);
        vbat_adc = NULL;
    }

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return board_hal_get_battery_voltage() > 2500;
}

int board_hal_get_battery_voltage(void)
{
    // The helper toggles the TPS22916 load switch, averages several samples and
    // applies eFuse calibration + the divider; returns mV or -1 on failure.
    return battery_adc_read_mv(vbat_adc);
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

// Battery is treated as essentially full at/above this terminal voltage.
// During the charger's CV phase the pack sits near 4.2 V while current still
// tapers, so reaching this threshold means "near full", not strictly complete.
#define CHARGE_FULL_THRESHOLD_MV 4150

bool board_hal_is_charging(void)
{
    // The real charge state is NOT detectable on this board (rev V1.0):
    // confirmed against the official Seeed EE02 schematic, the BQ24070 status
    // pins STAT1 (pin 2) / STAT2 (pin 3) drive only the on-board green LEDs
    // (D5/D16) and are not routed to any ESP32-S3 GPIO. Revisit if a future
    // board rev breaks STAT out to a GPIO.
    //
    // Heuristic instead: if USB is present and the pack is below ~full, assume
    // it's charging. Consumers distinguish "full" as usb_connected && !charging.
    // This is an estimate — it flips to "full" during the CV taper (last few %
    // still going in), and a non-data USB power source may not register as
    // connected (usb_serial_jtag link state is the only USB signal available).
    if (!board_hal_is_usb_connected()) {
        return false;  // on battery → definitely not charging
    }

    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0) {
        return false;  // no reading → don't claim charging
    }

    return voltage < CHARGE_FULL_THRESHOLD_MV;
}

bool board_hal_supports_charge_status(void)
{
    // USB detection works (usb_serial_jtag) and is_charging() applies the
    // USB+voltage heuristic, so we can report charging/full/on_battery.
    return true;
}

bool board_hal_is_usb_connected(void)
{
    return usb_serial_jtag_is_connected();
}

void board_hal_shutdown(void)
{
    // BQ24070 doesn't have software shutdown command via I2C like AXP.
    // We can only enter deep sleep.
    ESP_LOGI(TAG, "Shutdown not supported on BQ24070, entering deep sleep instead");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    // No external RTC on XIAO EE02, rely on internal
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    // TODO: Could return internal time if desired, but main.c handles fallback.
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
    // No onboard LED on XIAO EE02
    (void) led;
    (void) on;
}
