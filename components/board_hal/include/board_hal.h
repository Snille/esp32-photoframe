#pragma once

#include <hal/gpio_types.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/gpio.h"
#include "epaper.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_HAL_DISPLAY_WIDTH epaper_get_width()
#define BOARD_HAL_DISPLAY_HEIGHT epaper_get_height()

typedef enum {
    BOARD_TYPE_WAVESHARE_PHOTOPAINTER,
    BOARD_TYPE_SEEEDSTUDIO_XIAO_EE02,
    BOARD_TYPE_SEEEDSTUDIO_XIAO_EE04,
    BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1002,
    BOARD_TYPE_DFROBOT_FIREBEETLE_ESP32E,
    BOARD_TYPE_DFROBOT_FIREBEETLE_ESP32S3,
    BOARD_TYPE_UNKNOWN
} board_type_t;

#ifdef CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73
#include "board_waveshare_photopainter_73.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE02)
#include "board_seeedstudio_xiao_ee02.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE04)
#include "board_seeedstudio_xiao_ee04.h"
#elif defined(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1002)
#include "board_seeedstudio_reterminal_e1002.h"
#elif defined(CONFIG_BOARD_DRIVER_DFROBOT_FIREBEETLE_ESP32E)
#include "board_dfrobot_firebeetle_esp32e.h"
#elif defined(CONFIG_BOARD_DRIVER_DFROBOT_FIREBEETLE_ESP32S3)
#include "board_dfrobot_firebeetle_esp32s3.h"
#else
// Default definitions if no board selected (fallback)
#error "No board selected! Please define CONFIG_BOARD_DRIVER_..."
#endif

/**
 * @brief Initialize the Board HAL
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_init(void);

/**
 * @brief Prepare the system for deep sleep
 *
 * This function should be called just before esp_deep_sleep_start().
 * It handles PMIC-specific shutdown sequences (e.g. disabling rails).
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_prepare_for_sleep(void);

/**
 * @brief Is battery connected
 *
 * @return true if connected, false otherwise
 */
bool board_hal_is_battery_connected(void);

/**
 * @brief Get battery percentage
 *
 * @return int Battery percentage (0-100), or -1 if unknown
 */
int board_hal_get_battery_percent(void);

/**
 * @brief Get battery voltage in millivolts
 *
 * @return int Battery voltage in mV, or -1 if unknown
 */
int board_hal_get_battery_voltage(void);

/**
 * @brief Check if battery is currently charging
 *
 * @return true if charging, false otherwise
 */
bool board_hal_is_charging(void);

/**
 * @brief Check if USB power is connected
 *
 * @return true if USB connected, false otherwise
 */
bool board_hal_is_usb_connected(void);

/**
 * @brief Whether this board can report a meaningful charge status
 *
 * True only when board_hal_is_charging() carries real information — either a
 * hardware charge-status line or a USB+voltage heuristic. Boards that cannot
 * sense USB / charge state at all (e.g. FireBeetle) return false so callers can
 * omit charge reporting entirely instead of publishing a misleading "not
 * charging". Used to gate the X-Battery-Status header sent to the server.
 *
 * @return true if charge status is meaningful on this board
 */
bool board_hal_supports_charge_status(void);

/**
 * @brief Perform a hard shutdown (power off)
 *
 * Note: Behavior depends on hardware. Some PMICs can cut power completely.
 */
void board_hal_shutdown(void);

/**
 * @brief Get ambient temperature (if sensor available)
 *
 * @param[out] t Pointer to float to store temperature in Celsius
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no sensor
 */
esp_err_t board_hal_get_temperature(float *t);

/**
 * @brief Get ambient humidity (if sensor available)
 *
 * @param[out] h Pointer to float to store humidity in %RH
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no sensor
 */
esp_err_t board_hal_get_humidity(float *h);

/**
 * @brief Initialize the external RTC (if available)
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no RTC, or other error
 */
esp_err_t board_hal_rtc_init(void);

/**
 * @brief Get time from external RTC
 *
 * @param[out] t Time value to populate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_rtc_get_time(time_t *t);

/**
 * @brief Set time to external RTC
 *
 * @param t Time value to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_rtc_set_time(time_t t);

/**
 * @brief Check if external RTC is available/initialized
 *
 * @return true if available
 */
bool board_hal_rtc_is_available(void);

typedef enum {
    BOARD_HAL_LED_POWER,     // Power/status indicator (red on waveshare, no-op if not present)
    BOARD_HAL_LED_ACTIVITY,  // Activity indicator (green on waveshare, single LED on reterminal)
} board_hal_led_t;

/**
 * @brief Set an onboard LED state
 *
 * @param led Which LED to control
 * @param on true to turn LED on, false to turn off
 */
void board_hal_led_set(board_hal_led_t led, bool on);

// One selectable GPIO for a user-wired external battery voltage divider, on
// boards with no built-in battery sensing circuit.
typedef struct {
    int gpio_num;
    const char *label;  // e.g. "A3 (GPIO8)" -- matches the board's silkscreen
} board_hal_battery_adc_pin_t;

/**
 * @brief List the GPIOs this board allows for a user-wired battery voltage
 * divider (VBAT -- 1M -- pin -- 1M -- GND), for boards with no built-in
 * battery ADC circuit.
 *
 * @param[out] out_pins Receives a pointer to a static array of options.
 *                      Untouched if this board has no configurable pin.
 * @return Number of options; 0 if this board doesn't support a
 *         user-configurable battery ADC pin at all.
 */
int board_hal_get_battery_adc_pin_options(const board_hal_battery_adc_pin_t **out_pins);

/**
 * @brief Select (and immediately activate) the GPIO wired to an external
 * battery voltage divider. Assumes a 1:2 divider (two equal resistors).
 *
 * Does not persist the choice -- callers should also save it (e.g. via
 * config_manager) so it survives reboot, and re-apply it at boot.
 *
 * @param gpio_num One of the gpio_num values from
 *        board_hal_get_battery_adc_pin_options(), or -1 to disable.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if gpio_num isn't a valid
 *         option, ESP_ERR_NOT_SUPPORTED if this board has no configurable
 *         battery ADC pin.
 */
esp_err_t board_hal_set_battery_adc_pin(int gpio_num);

/**
 * @brief Currently configured battery ADC GPIO.
 *
 * @return GPIO number, or -1 if none is configured / not supported.
 */
int board_hal_get_battery_adc_pin(void);

#ifdef __cplusplus
}
#endif
