#include "power_manager.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <time.h>

// IDF < 6.0 compat: esp_sleep_get_wakeup_causes() didn't exist; emulate as bitmask
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
static inline uint32_t esp_sleep_get_wakeup_causes(void)
{
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    return (c == ESP_SLEEP_WAKEUP_UNDEFINED) ? 0u : (1u << (uint32_t)c);
}
#endif

// Classic ESP32 calls it ALL_LOW; newer IDF / S3 renamed it to ANY_LOW
#ifndef ESP_EXT1_WAKEUP_ANY_LOW
#define ESP_EXT1_WAKEUP_ANY_LOW ESP_EXT1_WAKEUP_ALL_LOW
#endif

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#include <hal/usb_serial_jtag_ll.h>
#endif

#include <string.h>

#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "ha_integration.h"
#include "periodic_tasks.h"
#include "storage.h"
#include "utils.h"

// RTC memory to store expected wakeup time (persists across deep sleep)
RTC_DATA_ATTR static time_t expected_wakeup_time = 0;

static const char *TAG = "power_manager";

static TaskHandle_t sleep_timer_task_handle = NULL;
static TaskHandle_t rotation_timer_task_handle = NULL;
static int64_t next_sleep_time = 0;  // Use absolute time for sleep timer
static uint32_t auto_sleep_timeout_sec = AUTO_SLEEP_TIMEOUT_SEC;
static wakeup_source_t wakeup_source = WAKEUP_SOURCE_NONE;
static int64_t next_rotation_time = 0;  // Use absolute time for rotation
static uint64_t ext1_wakeup_pin_mask = 0;

// Heavy button actions (next_image / info_screen) do an HTTP fetch + image
// decode + display refresh that needs far more stack than button_monitor_task
// has (2560 B). Running them there overflows the stack and reboots the device.
// btn_monitor instead hands them to rotation_timer_task (16 KB stack) via this
// one-slot mailbox; light actions still run inline on the button task.
static volatile bool pending_button_action_ready = false;
static char pending_button_action_buf[BUTTON_ACTION_MAX_LEN];

static void execute_button_action(const char *action);

static void rotation_timer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Run a heavy button action deferred from button_monitor_task. This
        // runs on the rotation task's 16 KB stack, regardless of auto-rotate.
        if (pending_button_action_ready) {
            pending_button_action_ready = false;
            execute_button_action(pending_button_action_buf);
        }

        // Run active rotation when:
        // 1. USB is connected (device stays awake), OR
        // 2. Deep sleep is disabled (device stays awake on battery)
        bool should_use_active_rotation =
            board_hal_is_usb_connected() || !config_manager_get_deep_sleep_enabled();

        if (!should_use_active_rotation) {
            // Device will auto-sleep after 120 seconds, no need to reset timer
            continue;
        }

        // Handle active rotation when device stays awake and auto-rotate enabled
        if (config_manager_get_auto_rotate()) {
            // Check if we're in sleep schedule
            if (config_manager_is_in_sleep_schedule()) {
                // During sleep schedule, don't rotate
                next_rotation_time = 0;  // Reset timer
                continue;
            }

            int64_t now = esp_timer_get_time();  // Get absolute time in microseconds

            if (next_rotation_time == 0) {
                // Initialize next rotation time
                int seconds_until_next = get_seconds_until_next_wakeup();

                next_rotation_time = now + (seconds_until_next * 1000000LL);
                const char *reason =
                    board_hal_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation scheduled in %d seconds (%s, %s)",
                         seconds_until_next,
                         config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval",
                         reason);
            } else if (now >= next_rotation_time) {
                // Time to rotate
                const char *reason =
                    board_hal_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation triggered (%s)", reason);

                trigger_image_rotation();
                ha_notify_update();

                // Schedule next rotation
                int seconds_until_next = get_seconds_until_next_wakeup();

                next_rotation_time = now + (seconds_until_next * 1000000LL);
                ESP_LOGI(TAG, "Next rotation scheduled in %d seconds (%s)", seconds_until_next,
                         config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval");
            }
        } else {
            next_rotation_time = 0;  // Reset if auto-rotate disabled
        }
    }
}

static void sleep_timer_task(void *arg)
{
    int64_t last_blink_time = 0;
    int64_t last_log_time = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

#ifndef DEBUG_DEEP_SLEEP_WAKE
        // Skip auto-sleep when USB is connected
        if (board_hal_is_usb_connected()) {
            // Reset timer so it doesn't trigger immediately when USB is unplugged
            next_sleep_time = 0;
            continue;
        }
#endif

        // Handle auto-sleep timer when on battery (only if deep sleep is enabled)
        if (config_manager_get_deep_sleep_enabled()) {
            int64_t now = esp_timer_get_time();

            if (next_sleep_time == 0) {
                // Initialize sleep timer
                next_sleep_time = now + ((int64_t) auto_sleep_timeout_sec * 1000000LL);
                last_blink_time = now;
                last_log_time = now;
                ESP_LOGI(TAG, "Auto-sleep timer started, will sleep in %lu seconds",
                         (unsigned long) auto_sleep_timeout_sec);
            }

            int64_t remaining_us = next_sleep_time - now;
            int32_t remaining_sec = (int32_t) (remaining_us / 1000000LL);

            if (remaining_sec > 0) {
                // Visual indicator: blink GREEN LED every 10 seconds
                if ((now - last_blink_time) >= 10000000LL) {
                    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, true);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);
                    last_blink_time = now;
                }

                // Log countdown every 30 seconds
                if ((now - last_log_time) >= 30000000LL) {
                    ESP_LOGI(TAG, "Auto-sleep countdown: %ld seconds remaining", remaining_sec);
                    last_log_time = now;
                }
            } else {
                // Time to sleep
                ESP_LOGI(TAG, "Sleep timeout reached, entering deep sleep");
                power_manager_enter_sleep();
            }
        } else {
            // Deep sleep disabled - reset timer to prevent it from triggering
            next_sleep_time = 0;
        }
    }
}

// Carry out a button action. Action ids mirror the firmware config / WebUI:
// "none", "next_image", "sleep", "toggle_deep_sleep", "info_screen". The heavy
// branches (next_image, info_screen) must run on a big-stack task — see
// dispatch_button_action, which is what callers use.
static void execute_button_action(const char *action)
{
    if (action == NULL || strcmp(action, "none") == 0) {
        return;
    }
    if (strcmp(action, "next_image") == 0) {
        ESP_LOGI(TAG, "Button action: next image");
        power_manager_reset_sleep_timer();
        trigger_image_rotation();
        ha_notify_update();
    } else if (strcmp(action, "sleep") == 0) {
        ESP_LOGI(TAG, "Button action: enter deep sleep");
        power_manager_enter_sleep();  // does not return
    } else if (strcmp(action, "toggle_deep_sleep") == 0) {
        bool next = !config_manager_get_deep_sleep_enabled();
        ESP_LOGI(TAG, "Button action: toggle deep sleep -> %s", next ? "enabled" : "disabled");
        power_manager_set_deep_sleep_enabled(next);
        power_manager_reset_sleep_timer();
    } else if (strcmp(action, "info_screen") == 0) {
        ESP_LOGI(TAG, "Button action: show info screen");
        power_manager_reset_sleep_timer();
        display_manager_show_info_screen();
    } else {
        ESP_LOGW(TAG, "Unknown button action: %s", action);
    }
}

// Called from button_monitor_task (2560 B stack). next_image / info_screen are
// too stack-heavy to run here (they reboot the device), so hand them to
// rotation_timer_task; everything else runs inline.
static void dispatch_button_action(const char *action)
{
    if (action == NULL || strcmp(action, "none") == 0) {
        return;
    }
    if (strcmp(action, "next_image") == 0 || strcmp(action, "info_screen") == 0) {
        strncpy(pending_button_action_buf, action, sizeof(pending_button_action_buf) - 1);
        pending_button_action_buf[sizeof(pending_button_action_buf) - 1] = '\0';
        pending_button_action_ready = true;
    } else {
        execute_button_action(action);
    }
}

// Detects button gestures on the wake key while the device is awake and runs
// the configured action: short press (<2s), long press (2-5s), hold (>=5s).
// Uses the same key that wakes the device (BOARD_HAL_WAKEUP_KEY, active-low
// with internal pull-up). Wake-from-deep-sleep is implicit and not a gesture.
static void button_monitor_task(void *arg)
{
    if (BOARD_HAL_WAKEUP_KEY == GPIO_NUM_NC) {
        vTaskDelete(NULL);
        return;
    }

    // power_manager_init() latched the button pad with gpio_hold for false-wake
    // protection during deep sleep. Release that hold now so we can read the
    // live button state while awake; power_manager_enter_sleep() re-applies it.
    gpio_hold_dis(BOARD_HAL_WAKEUP_KEY);

    const int64_t LONG_US = 2000000;  // >=2s = long press
    const int64_t HOLD_US = 5000000;  // >=5s = hold
    bool released_since_boot = false;
    int64_t press_start = 0;
    bool hold_fired = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));

        // Active-low (pull-up enabled): level 0 means the button is pressed.
        bool pressed = (gpio_get_level(BOARD_HAL_WAKEUP_KEY) == 0);

        // Ignore the press that woke the device until it has been released once,
        // so a still-held wake press doesn't immediately trigger a gesture.
        if (!released_since_boot) {
            if (!pressed) {
                released_since_boot = true;
            }
            continue;
        }

        if (pressed) {
            if (press_start == 0) {
                press_start = esp_timer_get_time();
                hold_fired = false;
            } else if (!hold_fired && esp_timer_get_time() - press_start >= HOLD_US) {
                // Hold threshold crossed while still pressed. Wait for release
                // first (a still-low pin would re-wake EXT1 if the action sleeps)
                // then run the hold action. Mark fired so release isn't also
                // counted as a long press.
                hold_fired = true;
                while (gpio_get_level(BOARD_HAL_WAKEUP_KEY) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                vTaskDelay(pdMS_TO_TICKS(100));  // debounce after release
                dispatch_button_action(config_manager_get_button_action_hold());
                press_start = 0;
            }
        } else {
            // Released — classify by how long it was held (unless hold already fired).
            if (press_start != 0 && !hold_fired) {
                int64_t dur = esp_timer_get_time() - press_start;
                const char *action = (dur >= LONG_US) ? config_manager_get_button_action_long()
                                                       : config_manager_get_button_action_short();
                dispatch_button_action(action);
            }
            press_start = 0;
            hold_fired = false;
        }
    }
}

static void power_manager_enable_auto_light_sleep(void)
{
    // Configure automatic light sleep with CPU frequency scaling
    // This allows the ESP32 to automatically enter light sleep when idle
    // and scale CPU frequency down to save power while maintaining WiFi connectivity
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,         // Maximum CPU frequency (160MHz for ESP32-S3)
        .min_freq_mhz = 40,          // Minimum CPU frequency (40MHz when idle)
        .light_sleep_enable = true,  // Enable automatic light sleep
    };

    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        ESP_LOGI(TAG, "Automatic light sleep enabled (CPU: 160MHz -> 40MHz)");
    } else {
        ESP_LOGW(TAG, "Failed to configure power management: %s", esp_err_to_name(pm_ret));
    }
}

static void power_manager_disable_auto_light_sleep(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,  // Maximum CPU frequency (160MHz for ESP32-S3)
        .min_freq_mhz = 40,   // Minimum CPU frequency (40MHz when idle)
        .light_sleep_enable = false,
    };

    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        ESP_LOGI(TAG, "Automatic light sleep disabled");
    } else {
        ESP_LOGW(TAG, "Failed to configure power management: %s", esp_err_to_name(pm_ret));
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

esp_err_t power_manager_init(void)
{
    bool deep_sleep_enabled = config_manager_get_deep_sleep_enabled();
    ESP_LOGI(TAG, "Deep sleep %s", deep_sleep_enabled ? "enabled" : "disabled");

    // Get wakeup causes bitmap (new API in ESP-IDF v6.0)
    uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    ext1_wakeup_pin_mask = 0;

    // Determine wakeup source
    if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_TIMER)) {
        wakeup_source = WAKEUP_SOURCE_TIMER;
        ESP_LOGI(TAG, "Wakeup caused by timer (auto-rotate)");

        // Check time drift and force NTP sync if needed
        if (expected_wakeup_time > 0) {
            time_t now;
            time(&now);
            int drift = (int) (now - expected_wakeup_time);
            ESP_LOGI(TAG, "Wakeup time drift: %d seconds (expected: %ld, actual: %ld)", drift,
                     (long) expected_wakeup_time, (long) now);

            // If drift exceeds 30 seconds, force NTP sync
            if (drift > 30 || drift < -30) {
                ESP_LOGW(TAG, "Time drift exceeds 30s, will force NTP sync");
                periodic_tasks_force_run(SNTP_TASK_NAME);
            }
            expected_wakeup_time = 0;  // Reset after checking
        }
    } else if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_EXT1)) {
        // ESP32-S3 only supports EXT1, check which GPIO triggered it
        ext1_wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

        if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC &&
            (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_WAKEUP_KEY))) {
            wakeup_source = WAKEUP_SOURCE_BOOT_BUTTON;
            ESP_LOGI(TAG, "Wakeup caused by BOOT button (GPIO %d)", BOARD_HAL_WAKEUP_KEY);
        } else if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC &&
                   (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_ROTATE_KEY))) {
            wakeup_source = WAKEUP_SOURCE_ROTATE_BUTTON;
            ESP_LOGI(TAG, "Wakeup caused by ROTATE button (GPIO %d)", BOARD_HAL_ROTATE_KEY);
        } else if (BOARD_HAL_CLEAR_KEY != GPIO_NUM_NC &&
                   (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_CLEAR_KEY))) {
            wakeup_source = WAKEUP_SOURCE_CLEAR_BUTTON;
            ESP_LOGI(TAG, "Wakeup caused by CLEAR button (GPIO %d)", BOARD_HAL_CLEAR_KEY);
        } else {
            wakeup_source = WAKEUP_SOURCE_EXT1_UNKNOWN;
            ESP_LOGI(TAG, "Wakeup caused by EXT1 (unknown GPIO: 0x%llx)", ext1_wakeup_pin_mask);
        }
    } else {
        wakeup_source = WAKEUP_SOURCE_NONE;
        ESP_LOGI(TAG, "Not a deep sleep wakeup");
    }

    // Configure button GPIOs as input with pull-ups
    uint64_t pin_mask = 0;
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        pin_mask |= (1ULL << BOARD_HAL_WAKEUP_KEY);
    }
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        pin_mask |= (1ULL << BOARD_HAL_ROTATE_KEY);
    }
    if (BOARD_HAL_CLEAR_KEY != GPIO_NUM_NC) {
        pin_mask |= (1ULL << (BOARD_HAL_CLEAR_KEY < 0 ? 0 : BOARD_HAL_CLEAR_KEY));
    }

    if (pin_mask != 0) {
        gpio_config_t io_conf = {.intr_type = GPIO_INTR_DISABLE,
                                 .mode = GPIO_MODE_INPUT,
                                 .pin_bit_mask = pin_mask,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .pull_up_en = GPIO_PULLUP_ENABLE};
        gpio_config(&io_conf);

        // Hold GPIO state during deep sleep to prevent floating
        // This prevents false EXT1 wake-ups when timer fires
        if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
            gpio_hold_en(BOARD_HAL_WAKEUP_KEY);
        }
        if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
            gpio_hold_en(BOARD_HAL_ROTATE_KEY);
        }
        if (BOARD_HAL_CLEAR_KEY != GPIO_NUM_NC) {
            gpio_hold_en(BOARD_HAL_CLEAR_KEY);
        }
        gpio_deep_sleep_hold_en();
    }

    // LEDs are initialized by board_hal_init(), just set initial state
    // Power LED on when deep sleep is enabled (to indicate battery mode)
    board_hal_led_set(BOARD_HAL_LED_POWER, deep_sleep_enabled);
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // Skip auto-sleep timer if woken by ROTATE button or timer (image generation can take >120s)
    if (wakeup_source == WAKEUP_SOURCE_ROTATE_BUTTON ||
        wakeup_source == WAKEUP_SOURCE_CLEAR_BUTTON || wakeup_source == WAKEUP_SOURCE_TIMER) {
        ESP_LOGI(TAG, "Woken by ROTATE button, KEY button or timer, disabling auto-sleep timer");
    } else {
        xTaskCreate(sleep_timer_task, "sleep_timer", 4096, NULL, 5, &sleep_timer_task_handle);
    }
    xTaskCreate(rotation_timer_task, "rotation_timer", 16384, NULL, 5, &rotation_timer_task_handle);

    // Long-press the wake button (>=2s) while awake to re-enter deep sleep.
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        xTaskCreate(button_monitor_task, "btn_monitor", 2560, NULL, 5, NULL);
    }

    power_manager_enable_auto_light_sleep();

    ESP_LOGI(TAG, "Power manager initialized");
    return ESP_OK;
}

void power_manager_enter_sleep(void)
{
    power_manager_disable_auto_light_sleep();

    ESP_LOGI(TAG, "Preparing to enter deep sleep mode");

    ha_notify_offline();

    // Turn off LEDs before sleep
    board_hal_led_set(BOARD_HAL_LED_POWER, false);
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // Check if auto-rotate is enabled
    if (config_manager_get_auto_rotate()) {
        // Use timer-based sleep for auto-rotate
        int wake_seconds = get_seconds_until_next_wakeup();

        ESP_LOGI(TAG, "Auto-rotate enabled, setting timer wake-up for %d seconds (%s)",
                 wake_seconds,
                 config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval");
        esp_sleep_enable_timer_wakeup(wake_seconds * 1000000ULL);

        // Store expected wakeup time in RTC memory for drift detection
        time_t now;
        time(&now);
        expected_wakeup_time = now + wake_seconds;
    }

    // Enable boot button and key button wake-up (ESP32-S3 only supports EXT1)
    uint64_t wakeup_mask = 0;
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        wakeup_mask |= (1ULL << BOARD_HAL_WAKEUP_KEY);
    }
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        wakeup_mask |= (1ULL << BOARD_HAL_ROTATE_KEY);
    }
    if (BOARD_HAL_CLEAR_KEY != GPIO_NUM_NC) {
        wakeup_mask |= (1ULL << (BOARD_HAL_CLEAR_KEY < 0 ? 0 : BOARD_HAL_CLEAR_KEY));
    }

    if (wakeup_mask != 0) {
        esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }

    // Re-latch the button pads (the awake button-monitor task released the hold
    // to read live state). With the buttons released/high, this retains the
    // internal pull-ups during deep sleep and prevents false EXT1 wake-ups.
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        gpio_hold_en(BOARD_HAL_WAKEUP_KEY);
    }
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        gpio_hold_en(BOARD_HAL_ROTATE_KEY);
    }
    if (BOARD_HAL_CLEAR_KEY != GPIO_NUM_NC) {
        gpio_hold_en(BOARD_HAL_CLEAR_KEY);
    }
    gpio_deep_sleep_hold_en();

    // Stop WiFi cleanly before deep sleep so the MAC/PHY drains pending
    // state and the modem domain transitions through a normal teardown
    // rather than being yanked by the deep-sleep entry. Ignored if WiFi
    // was never started.
    esp_wifi_stop();

    ESP_LOGI(TAG, "Configuring Board HAL for deep sleep");
    board_hal_prepare_for_sleep();

    // Unmount LittleFS and force flash power domain off to prevent
    // VDD_SPI from staying active during deep sleep (~1-2mA drain).
    // See: https://github.com/aitjcize/esp32-photoframe/issues/74
#ifdef CONFIG_USE_INTERNAL_FLASH_STORAGE
    storage_unmount();
#endif

    ESP_LOGI(TAG, "Entering deep sleep now");
    vTaskDelay(pdMS_TO_TICKS(100));

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    // Power down the USB-Serial-JTAG PHY pads. The console pipes through
    // this peripheral on S3 (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED),
    // and leaving the PHY enabled draws ~250 µA in deep sleep. After this
    // call the console is dead until the next cold boot, which is fine —
    // we never return from esp_deep_sleep_start().
    usb_serial_jtag_ll_phy_enable_pad(false);
#endif

    esp_deep_sleep_start();
}

void power_manager_reset_sleep_timer(void)
{
    next_sleep_time = esp_timer_get_time() + ((int64_t) auto_sleep_timeout_sec * 1000000LL);
}

void power_manager_set_auto_sleep_timeout(uint32_t seconds)
{
    auto_sleep_timeout_sec = seconds;
    next_sleep_time = 0;  // re-init on next tick so the new timeout takes effect
    ESP_LOGI(TAG, "Auto-sleep timeout set to %lu seconds", (unsigned long) seconds);
}

void power_manager_reset_rotate_timer(void)
{
    int seconds_until_next = get_seconds_until_next_wakeup();

    next_rotation_time = esp_timer_get_time() + (seconds_until_next * 1000000LL);
    ESP_LOGI(TAG, "Rotation timer reset, next rotation in %d seconds (%s)", seconds_until_next,
             config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval");
}

wakeup_source_t power_manager_get_wakeup_source(void)
{
    return wakeup_source;
}

void power_manager_set_deep_sleep_enabled(bool enabled)
{
    // Save to NVS via config_manager
    config_manager_set_deep_sleep_enabled(enabled);

    // Update power LED: on when deep sleep enabled, off when disabled
    board_hal_led_set(BOARD_HAL_LED_POWER, enabled);
}
