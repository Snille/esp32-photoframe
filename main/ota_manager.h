#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_CHECKING,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_INSTALLING,
    OTA_STATE_SUCCESS,
    OTA_STATE_ERROR,
    OTA_STATE_UP_TO_DATE  // checked, no newer release — distinct from IDLE (never checked)
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current_version[32];
    char latest_version[32];
    char error_message[128];
    int progress_percent;
} ota_status_t;

esp_err_t ota_manager_init(void);
esp_err_t ota_check_for_update(bool *update_available, int timeout);
esp_err_t ota_start_update(void);
void ota_get_status(ota_status_t *status);
const char *ota_get_current_version(void);
bool ota_should_check_daily(void);
void ota_update_last_check_time(void);

// True while a check, download or install is running.
bool ota_is_busy(void);

// Block until any in-flight OTA finishes (it reboots the frame on success), or
// until max_wait_sec elapses. Call before a scheduled deep sleep so the sleep
// does not cut off an install that is already writing flash.
void ota_wait_while_busy(int max_wait_sec);

#endif  // OTA_MANAGER_H
