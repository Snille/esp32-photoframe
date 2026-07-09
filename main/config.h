#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

typedef enum { ROTATION_MODE_STORAGE = 0, ROTATION_MODE_URL = 1 } rotation_mode_t;

typedef enum { SD_ROTATION_RANDOM = 0, SD_ROTATION_SEQUENTIAL = 1 } sd_rotation_mode_t;

typedef enum {
    DISPLAY_ORIENTATION_LANDSCAPE = 0,
    DISPLAY_ORIENTATION_PORTRAIT = 1
} display_orientation_t;

#define DEVICE_NAME_MAX_LEN 64
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define IMAGE_URL_MAX_LEN 256
#define HA_URL_MAX_LEN 256
#define ROTATION_MODE_MAX_LEN 16
#define TIMEZONE_MAX_LEN 64
#define NTP_SERVER_MAX_LEN 128
#define ACCESS_TOKEN_MAX_LEN 512
#define HTTP_HEADER_KEY_MAX_LEN 64
#define HTTP_HEADER_VALUE_MAX_LEN 512
#define CA_CERT_MAX_LEN 4096
#define HTTP_ETAG_MAX_LEN 128

#define DEFAULT_DEVICE_NAME "PhotoFrame"
#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"
#define DEFAULT_IMAGE_URL "https://loremflickr.com/800/480"
#define DEFAULT_HA_URL ""
#define DEFAULT_TIMEZONE "UTC0"
#define DEFAULT_NTP_SERVER "pool.ntp.org"

#define DEFAULT_ALBUM_NAME "Default"

#include "board_hal.h"

#define FS_MOUNT_POINT "/storage"

#define IMAGE_DIRECTORY FS_MOUNT_POINT "/images"
#define DOWNLOAD_DIRECTORY IMAGE_DIRECTORY "/Downloads"

#define CURRENT_UPLOAD_PATH FS_MOUNT_POINT "/.current.tmp"
#define CURRENT_JPG_PATH FS_MOUNT_POINT "/.current.jpg"
#define CURRENT_BMP_PATH FS_MOUNT_POINT "/.current.bmp"
#define CURRENT_PNG_PATH FS_MOUNT_POINT "/.current.png"
#define CURRENT_EPD_PATH FS_MOUNT_POINT "/.current.epdgz"
#define CURRENT_IMAGE_LINK FS_MOUNT_POINT "/.current.lnk"
#define CURRENT_CALIBRATION_PATH FS_MOUNT_POINT "/.calibration.png"

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 60
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

// Longer auto-sleep window during out-of-box setup (captive-portal
// provisioning), so the user has time to scan the QR code and configure WiFi
// via the app before the device sleeps.
#define OOBE_AUTO_SLEEP_TIMEOUT_SEC 600

#define IMAGE_ROTATE_INTERVAL_SEC 3600

// WiFi
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"

// General
#define NVS_NAMESPACE "photoframe"
#define NVS_SETUP_COMPLETE_KEY "setup_complete"
#define NVS_DEVICE_NAME_KEY "device_name"
#define NVS_TIMEZONE_KEY "timezone"
#define NVS_NTP_SERVER_KEY "ntp_server"
#define NVS_DISPLAY_ORIENTATION_KEY "disp_orient"
#define NVS_DISPLAY_ROTATION_DEG_KEY "disp_rot_deg"

// Auto Rotate
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_ALIGNED_KEY "ar_align"
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SLEEP_SCHEDULE_ENABLED_KEY "sleep_sched_en"
#define NVS_SLEEP_SCHEDULE_START_KEY "sleep_start"
#define NVS_SLEEP_SCHEDULE_END_KEY "sleep_end"

// Auto Rotate - SDCard
#define NVS_SD_ROTATION_MODE_KEY "sd_rot_mode"
#define NVS_LAST_INDEX_KEY "last_idx"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"

// Auto Rotate - URL
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_CA_CERT_KEY "ca_cert"
#define NVS_ACCESS_TOKEN_KEY "access_token"
#define NVS_HTTP_HEADER_KEY_KEY "http_hdr_key"
#define NVS_HTTP_HEADER_VALUE_KEY "http_hdr_val"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"
#define NVS_IMAGE_ETAG_KEY "image_etag"

// Power
#define NVS_DEEP_SLEEP_KEY "deep_sleep"
// GPIO for an optional user-wired external battery voltage divider, on boards
// with no built-in battery ADC (see board_hal_get_battery_adc_pin_options()).
// -1 = not configured.
#define NVS_BATTERY_ADC_GPIO_KEY "batt_adc_gpio"
// Per-unit battery voltage calibration scale (i32, in ten-thousandths, e.g.
// 10270 = 1.0270), from a one-point multimeter calibration on the frame's own
// WebGUI. Absent = use the board driver's factory default. See
// board_hal_set_battery_cal_scale().
#define NVS_BATTERY_CAL_SCALE_KEY "batt_cal"

// OTA auto-update: server-owned, pushed via config-sync (X-Config-Payload).
// When enabled, the periodic OTA check auto-installs a found update, gated by
// battery level. Default off — each frame is a manual canary until turned on.
#define NVS_AUTO_UPDATE_KEY "auto_update"
#define NVS_AUTO_UPDATE_BATT_MIN_KEY "au_batt_min"
#define DEFAULT_AUTO_UPDATE_BATTERY_MIN 30

// Button gestures → actions (configurable from the WebUI). Action ids:
// "none", "next_image", "sleep", "toggle_deep_sleep", "info_screen".
// Wake-from-deep-sleep is implicit (any press wakes the frame) and not mapped.
#define BUTTON_ACTION_MAX_LEN 24
#define NVS_BTN_SHORT_KEY "btn_short"  // short press (<2s)
#define NVS_BTN_LONG_KEY "btn_long"    // long press (2-5s)
#define NVS_BTN_HOLD_KEY "btn_hold"    // hold (>=5s)
#define DEFAULT_BTN_SHORT_ACTION "next_image"
#define DEFAULT_BTN_LONG_ACTION "sleep"
#define DEFAULT_BTN_HOLD_ACTION "info_screen"

// Home Assistant
#define NVS_HA_URL_KEY "ha_url"

// AI API Keys (for webapp client use)
#define AI_API_KEY_MAX_LEN 256
#define NVS_OPENAI_API_KEY_KEY "openai_key"
#define NVS_GOOGLE_API_KEY_KEY "google_key"

// AI generation prompt (sent to the server as X-AI-Prompt on ai_generation pulls)
#define AI_PROMPT_MAX_LEN 512
#define NVS_AI_PROMPT_KEY "ai_prompt"

// OTA Configuration
// Point OTA at this fork's releases. We query the releases *list* (not
// /releases/latest) because the repo carries two parallel release lines: the
// classic-ESP32 FireBeetle (tags firebeetle-v*, no OTA, asset is a merged bin)
// and the ESP32-S3 boards (tags v*, OTA-capable, asset esp32-photoframe-<board>.bin).
// /releases/latest returns whichever was published most recently regardless of
// board, so a FireBeetle-only release would hide S3 updates. Instead the OTA
// code walks this list (newest first) and picks the newest release that actually
// contains this board's binary. per_page=10 keeps the response small (each
// release now carries 12 assets, so the list is tens of KB over TLS) — the
// newest release always carries every board's asset, so the board-specific
// binary is near the top and the check finishes well within its timeout. See
// ota_check_for_update()'s timeout handling.
#define GITHUB_API_URL "https://api.github.com/repos/Snille/esp32-photoframe/releases?per_page=10"
#define OTA_CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)  // 24 hours

#endif