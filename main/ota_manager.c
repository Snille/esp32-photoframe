#include "ota_manager.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_hal.h"
#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha_integration.h"
#include "nvs.h"
#include "periodic_tasks.h"
#include "power_manager.h"

static const char *TAG = "ota_manager";
#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_LATEST_VERSION_KEY "latest_ver"
#define OTA_NVS_STATE_KEY "state"
#define OTA_CHECK_INTERVAL_SECONDS (24 * 60 * 60)  // 24 hours

static ota_status_t ota_status = {.state = OTA_STATE_IDLE,
                                  .current_version = "",
                                  .latest_version = "",
                                  .error_message = "",
                                  .progress_percent = 0};

static SemaphoreHandle_t ota_status_mutex = NULL;
static bool update_available = false;
static char firmware_url[256] = "";

// Forward declarations
static void ota_save_status_to_nvs(void);
static void ota_load_status_from_nvs(void);
static esp_err_t ota_check_periodic_callback(void);

static void set_ota_state(ota_state_t state, const char *error_msg)
{
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.state = state;
        if (error_msg) {
            snprintf(ota_status.error_message, sizeof(ota_status.error_message), "%s", error_msg);
        } else {
            ota_status.error_message[0] = '\0';
        }
        xSemaphoreGive(ota_status_mutex);
    }
}

static int version_compare(const char *v1, const char *v2)
{
    // Simple version comparison
    // Handles formats like "v1.2.3" or "1.2.3" or "dev-abc123"

    // Skip 'v' prefix if present
    if (v1[0] == 'v')
        v1++;
    if (v2[0] == 'v')
        v2++;

    // Dev versions are always considered older than release versions
    bool v1_is_dev = (strncmp(v1, "dev-", 4) == 0);
    bool v2_is_dev = (strncmp(v2, "dev-", 4) == 0);

    if (v1_is_dev && !v2_is_dev) {
        return -1;  // v1 (dev) is older than v2 (release)
    }
    if (!v1_is_dev && v2_is_dev) {
        return 1;  // v1 (release) is newer than v2 (dev)
    }
    if (v1_is_dev && v2_is_dev) {
        return strcmp(v1, v2);  // Both dev, compare strings
    }

    // Parse version numbers for release versions
    int v1_major = 0, v1_minor = 0, v1_patch = 0;
    int v2_major = 0, v2_minor = 0, v2_patch = 0;

    sscanf(v1, "%d.%d.%d", &v1_major, &v1_minor, &v1_patch);
    sscanf(v2, "%d.%d.%d", &v2_major, &v2_minor, &v2_patch);

    if (v1_major != v2_major)
        return v1_major - v2_major;
    if (v1_minor != v2_minor)
        return v1_minor - v2_minor;
    return v1_patch - v2_patch;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t fetch_github_release_info(char *latest_version, size_t version_len,
                                           char *download_url, size_t url_len, char *err_msg,
                                           size_t err_msg_len)
{
    esp_err_t err = ESP_FAIL;
    char *response_buffer = NULL;
    int response_len = 0;

    // Default reason; overwritten at the specific failure points below so the
    // WebUI/HA can show WHY a check failed (rate limit vs network vs no release)
    // instead of a single opaque "Failed to check for updates".
    if (err_msg && err_msg_len) {
        snprintf(err_msg, err_msg_len, "Failed to check for updates");
    }

    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Set User-Agent header (GitHub API requires it)
    esp_http_client_set_header(client, "User-Agent", "ESP32-PhotoFrame");

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        if (err_msg && err_msg_len) {
            snprintf(err_msg, err_msg_len, "Couldn't reach GitHub (check WiFi)");
        }
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed, status = %d", status_code);
        if (err_msg && err_msg_len) {
            if (status_code == 403 || status_code == 429) {
                // Unauthenticated GitHub API allows only 60 requests/hour per IP;
                // heavy checking/OTA testing exhausts it and returns 403/429.
                snprintf(err_msg, err_msg_len, "GitHub rate limit – try again in a while");
            } else {
                snprintf(err_msg, err_msg_len, "GitHub returned HTTP %d", status_code);
            }
        }
        err = ESP_FAIL;
        goto cleanup;
    }

    // GitHub serves the releases *list* chunked (Transfer-Encoding: chunked, no
    // Content-Length) once the repo has accumulated enough releases — so
    // content_length is -1 here and the old single-shot read failed with
    // "Invalid content length" (the generic check-failed message users saw).
    // Read the body incrementally into a growable PSRAM buffer until the stream
    // ends, working for both sized and chunked responses.
    size_t cap =
        (content_length > 0 && content_length < INT_MAX) ? (size_t) content_length + 1 : 16384;
    response_buffer = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t total = 0;
    while (true) {
        // Keep some headroom; grow when nearly full (chunked path).
        if (cap - total <= 512) {
            if (cap >= 256 * 1024) {
                ESP_LOGE(TAG, "Release list exceeds 256 KB; aborting");
                err = ESP_FAIL;
                goto cleanup;
            }
            size_t new_cap = cap * 2;
            char *grown = heap_caps_realloc(response_buffer, new_cap, MALLOC_CAP_SPIRAM);
            if (grown == NULL) {
                ESP_LOGE(TAG, "Failed to grow response buffer to %u bytes", (unsigned) new_cap);
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            response_buffer = grown;
            cap = new_cap;
        }
        int r = esp_http_client_read(client, response_buffer + total, cap - total - 1);
        if (r < 0) {
            ESP_LOGE(TAG, "Failed to read response");
            err = ESP_FAIL;
            goto cleanup;
        }
        if (r == 0) {
            break;  // end of body (sized or chunked)
        }
        total += r;
    }

    if (total == 0) {
        ESP_LOGE(TAG, "Empty response");
        err = ESP_FAIL;
        goto cleanup;
    }

    response_len = (int) total;
    response_buffer[response_len] = '\0';

    // Parse JSON response. GITHUB_API_URL is the releases *list* endpoint, so the
    // root is an array of release objects sorted newest-first (by created_at).
    cJSON *json = cJSON_Parse(response_buffer);
    if (json == NULL || !cJSON_IsArray(json)) {
        ESP_LOGE(TAG, "Failed to parse releases JSON array");
        if (json) {
            cJSON_Delete(json);
        }
        err = ESP_FAIL;
        goto cleanup;
    }

    // Build the board-specific asset name we need (e.g.
    // esp32-photoframe-seeedstudio_xiao_ee02.bin). This repo mixes releases for
    // several boards, so we walk releases newest-first and stop at the first
    // published (non-draft, non-prerelease) release that carries OUR asset. That
    // is the newest update relevant to this board, ignoring releases cut for
    // other boards (e.g. FireBeetle merged bins).
    const char *board_name = BOARD_HAL_NAME;
    char target_binary[64];
    snprintf(target_binary, sizeof(target_binary), "esp32-photoframe-%s.bin", board_name);
    ESP_LOGI(TAG, "Searching releases for board-specific OTA binary: %s", target_binary);

    bool found_binary = false;
    cJSON *release = NULL;

    cJSON_ArrayForEach(release, json)
    {
        cJSON *draft = cJSON_GetObjectItem(release, "draft");
        cJSON *prerelease = cJSON_GetObjectItem(release, "prerelease");
        if (cJSON_IsTrue(draft) || cJSON_IsTrue(prerelease)) {
            continue;
        }

        cJSON *tag_name = cJSON_GetObjectItem(release, "tag_name");
        cJSON *assets = cJSON_GetObjectItem(release, "assets");
        if (tag_name == NULL || !cJSON_IsString(tag_name) || assets == NULL ||
            !cJSON_IsArray(assets)) {
            continue;
        }

        cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets)
        {
            cJSON *name = cJSON_GetObjectItem(asset, "name");
            if (name == NULL || !cJSON_IsString(name) ||
                strcmp(name->valuestring, target_binary) != 0) {
                continue;
            }

            cJSON *browser_download_url = cJSON_GetObjectItem(asset, "browser_download_url");
            if (browser_download_url && cJSON_IsString(browser_download_url)) {
                snprintf(latest_version, version_len, "%s", tag_name->valuestring);
                snprintf(download_url, url_len, "%s", browser_download_url->valuestring);
                found_binary = true;
                ESP_LOGI(TAG, "Found firmware binary %s in release %s", name->valuestring,
                         tag_name->valuestring);
            }
            break;
        }

        if (found_binary) {
            break;
        }
    }

    cJSON_Delete(json);

    if (!found_binary) {
        ESP_LOGE(TAG, "No release with asset %s found", target_binary);
        if (err_msg && err_msg_len) {
            snprintf(err_msg, err_msg_len, "No firmware release found for this board");
        }
        err = ESP_FAIL;
        goto cleanup;
    }

    err = ESP_OK;
    ESP_LOGI(TAG, "Latest version: %s", latest_version);
    ESP_LOGI(TAG, "Download URL: %s", download_url);

cleanup:
    if (response_buffer) {
        free(response_buffer);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return err;
}

static void ota_check_task(void *pvParameter)
{
    // pvParameter is a boolean: true = notify HA, false/NULL = don't notify
    bool notify_ha = (pvParameter != NULL);

    ESP_LOGI(TAG, "Checking for firmware updates...");

    set_ota_state(OTA_STATE_CHECKING, NULL);

    char latest_version[32] = {0};
    char download_url[256] = {0};
    char err_msg[64] = {0};

    esp_err_t err = fetch_github_release_info(latest_version, sizeof(latest_version), download_url,
                                              sizeof(download_url), err_msg, sizeof(err_msg));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch release info: %s", err_msg);
        set_ota_state(OTA_STATE_ERROR, err_msg[0] ? err_msg : "Failed to check for updates");
        vTaskDelete(NULL);
        return;
    }

    // Store latest version and URL
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        snprintf(ota_status.latest_version, sizeof(ota_status.latest_version), "%s",
                 latest_version);
        xSemaphoreGive(ota_status_mutex);
    }
    snprintf(firmware_url, sizeof(firmware_url), "%s", download_url);

    // Compare versions
    int cmp = version_compare(ota_status.current_version, latest_version);

    if (cmp < 0) {
        ESP_LOGI(TAG, "Update available: %s -> %s", ota_status.current_version, latest_version);
        update_available = true;
        set_ota_state(OTA_STATE_UPDATE_AVAILABLE, NULL);
    } else {
        ESP_LOGI(TAG, "Already on latest version: %s", ota_status.current_version);
        update_available = false;
        // Distinct from IDLE (= never checked) so the WebUI can confirm "you're
        // on the latest version" instead of showing a blank result.
        set_ota_state(OTA_STATE_UP_TO_DATE, NULL);
    }

    // Update last check time after successful check
    ota_update_last_check_time();

    // Save OTA status to NVS for persistence across reboots
    ota_save_status_to_nvs();

    // Notify HA if requested
    if (notify_ha) {
        ESP_LOGI(TAG, "Notifying HA of OTA status update");
        ha_notify_update();
    }

    vTaskDelete(NULL);
}

static void ota_update_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA update...");

    // Reset sleep timer to prevent auto-sleep during OTA
    power_manager_reset_sleep_timer();

    set_ota_state(OTA_STATE_DOWNLOADING, NULL);
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.progress_percent = 0;
        xSemaphoreGive(ota_status_mutex);
    }

    esp_http_client_config_t config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        set_ota_state(OTA_STATE_ERROR, "Failed to start OTA update");
        vTaskDelete(NULL);
        return;
    }

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "OTA image size: %d bytes", image_size);

    set_ota_state(OTA_STATE_INSTALLING, NULL);

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int downloaded = esp_https_ota_get_image_len_read(https_ota_handle);
        if (image_size > 0) {
            int progress = (downloaded * 100) / image_size;
            if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
                ota_status.progress_percent = progress;
                xSemaphoreGive(ota_status_mutex);
            }
            ESP_LOGI(TAG, "OTA progress: %d%%", progress);
        }

        // Reset sleep timer periodically during OTA to prevent auto-sleep
        power_manager_reset_sleep_timer();

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        set_ota_state(OTA_STATE_ERROR, "OTA update failed");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            set_ota_state(OTA_STATE_ERROR, "Firmware validation failed");
        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
            set_ota_state(OTA_STATE_ERROR, "Failed to finalize OTA update");
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 3 seconds...");
    set_ota_state(OTA_STATE_SUCCESS, NULL);
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.progress_percent = 100;
        xSemaphoreGive(ota_status_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    vTaskDelete(NULL);
}

// True only when a second app partition exists to receive an update. On
// single-factory-partition boards (e.g. FireBeetle 2 ESP32-E — 4MB flash, a
// 2.6MB app leaves no room for two OTA slots) there is nowhere to write an
// update, so OTA is impossible. Skipping it avoids a wasted WiFi/TLS round-trip
// (and the battery it costs) on every wake.
static bool ota_is_supported(void)
{
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

esp_err_t ota_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager");

    // Zero out the entire ota_status struct to prevent garbage data
    memset(&ota_status, 0, sizeof(ota_status_t));
    ota_status.state = OTA_STATE_IDLE;

    // Create mutex for ota_status protection
    ota_status_mutex = xSemaphoreCreateMutex();
    if (ota_status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA status mutex");
        return ESP_ERR_NO_MEM;
    }

    // Get current firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    snprintf(ota_status.current_version, sizeof(ota_status.current_version), "%s",
             app_desc->version);

    ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);

    // Load last known OTA status from NVS (latest_version, state)
    ota_load_status_from_nvs();

    // Mark current partition as valid (for rollback support)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA update, marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Auto-disable OTA on boards without an update partition. No periodic
    // check is registered, so the device never wakes the radio to poll.
    if (!ota_is_supported()) {
        ESP_LOGI(TAG, "No OTA update partition — OTA disabled on this board");
        return ESP_OK;
    }

    // Register OTA check as a periodic task (24 hours)
    esp_err_t err = periodic_tasks_register(OTA_CHECK_TASK_NAME, ota_check_periodic_callback,
                                            OTA_CHECK_INTERVAL_SECONDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA periodic task: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t ota_check_for_update(bool *update_available_out, int timeout)
{
    // No update partition → nothing to check; avoids the boot-time TLS attempt.
    if (!ota_is_supported()) {
        if (update_available_out) {
            *update_available_out = false;
        }
        return ESP_OK;
    }

    if (ota_status.state == OTA_STATE_CHECKING || ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_INSTALLING) {
        return ESP_ERR_INVALID_STATE;
    }

    update_available = false;
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    // Wait for check to complete (with timeout)
    while (timeout > 0 && ota_status.state == OTA_STATE_CHECKING) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout--;
    }

    if (update_available_out) {
        *update_available_out = update_available;
    }

    return ESP_OK;
}

esp_err_t ota_start_update(void)
{
    if (!update_available) {
        ESP_LOGW(TAG, "No update available");
        return ESP_ERR_INVALID_STATE;
    }

    if (ota_status.state == OTA_STATE_DOWNLOADING || ota_status.state == OTA_STATE_INSTALLING) {
        ESP_LOGW(TAG, "Update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    xTaskCreate(&ota_update_task, "ota_update_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

void ota_get_status(ota_status_t *status)
{
    if (status && ota_status_mutex) {
        if (xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
            memcpy(status, &ota_status, sizeof(ota_status_t));
            xSemaphoreGive(ota_status_mutex);
        }
    }
}

const char *ota_get_current_version(void)
{
    return ota_status.current_version;
}

bool ota_should_check_daily(void)
{
    return periodic_tasks_should_run(OTA_CHECK_TASK_NAME);
}

void ota_update_last_check_time(void)
{
    periodic_tasks_update_last_run(OTA_CHECK_TASK_NAME);
}

static esp_err_t ota_check_periodic_callback(void)
{
    ESP_LOGI(TAG, "Periodic OTA check triggered");

    // Check for updates without notifying HA (HA will poll for status)
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

static void ota_save_status_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for saving OTA status: %s", esp_err_to_name(err));
        return;
    }

    // Save latest_version and state
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        err = nvs_set_str(nvs_handle, OTA_NVS_LATEST_VERSION_KEY, ota_status.latest_version);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save latest_version to NVS: %s", esp_err_to_name(err));
        }

        err = nvs_set_u8(nvs_handle, OTA_NVS_STATE_KEY, (uint8_t) ota_status.state);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save state to NVS: %s", esp_err_to_name(err));
        }

        xSemaphoreGive(ota_status_mutex);
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void ota_load_status_from_nvs(void)
{
    // Initialize to safe defaults first
    ota_status.latest_version[0] = '\0';
    ota_status.state = OTA_STATE_IDLE;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved OTA status in NVS (first boot or cleared), using defaults");
        return;
    }

    // Load latest_version
    size_t required_size = sizeof(ota_status.latest_version);
    err = nvs_get_str(nvs_handle, OTA_NVS_LATEST_VERSION_KEY, ota_status.latest_version,
                      &required_size);
    if (err != ESP_OK) {
        ota_status.latest_version[0] = '\0';
    }

    // Load state
    uint8_t saved_state = 0;
    err = nvs_get_u8(nvs_handle, OTA_NVS_STATE_KEY, &saved_state);
    if (err == ESP_OK) {
        ota_status.state = (ota_state_t) saved_state;
    } else {
        ota_status.state = OTA_STATE_IDLE;
    }

    nvs_close(nvs_handle);
}
