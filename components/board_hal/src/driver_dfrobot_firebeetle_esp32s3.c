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
// IC is DFRobot's ETA6003). Report as unknown rather than guessing a pin;
// revisit if a schematic/wiki update ever exposes one.
int board_hal_get_battery_voltage(void)
{
    return -1;
}

bool board_hal_is_battery_connected(void)
{
    return false;
}

int board_hal_get_battery_percent(void)
{
    return -1;
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
