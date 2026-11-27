#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static int s_max_retries = 5;
static bool s_is_connected = false;
static bool s_credentials_saved = false;
static int s_last_connect_ms = -1;
static int s_last_connect_attempts = 0;
static bool s_last_connect_scanned = false;
// False in the setup AP: the station must not connect on STA_START by itself.
static bool s_autoconnect = true;

// Upper bound for one connect attempt. The retry counter normally ends it far
// sooner; this only guards against a lost event.
#define WIFI_CONNECT_ATTEMPT_TIMEOUT_MS 30000
// Scan records kept when looking for saved networks (strongest first).
#define WIFI_SAVED_SCAN_MAX 24

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_autoconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // Bring up an IPv6 link-local address so mDNS can answer AAAA queries.
        // Without one the responder stays silent on AAAA, and clients resolving
        // <name>.local wait out their full resolver timeout (~5s per request)
        // before falling back to the A record.
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            esp_netif_create_ip6_linklocal(sta_netif);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < s_max_retries) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_is_connected = false;
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *) event_data;
        ESP_LOGI(TAG, "got ip6:" IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

esp_err_t wifi_manager_set_performance_mode(bool enable)
{
    // Modem power save adds ~100ms+ of latency to every round trip, which
    // throttles the web UI hard: bulk transfer speed is roughly one TCP send
    // buffer (~5.7KB) per round trip, i.e. ~45KB/s at 130ms RTT. Full RX
    // (WIFI_PS_NONE) costs ~60-70mA extra while the radio is up, so it is only
    // enabled when someone may actually be using the UI — the policy lives in
    // power_manager's sleep_timer_task.
    static bool applied = false;
    static bool current = false;
    if (applied && current == enable) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_ps(enable ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err == ESP_OK) {
        applied = true;
        current = enable;
        ESP_LOGI(TAG, "WiFi power save %s (%s mode)", enable ? "disabled" : "enabled",
                 enable ? "performance" : "power-save");
    }
    return err;
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create both STA and AP network interfaces
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // Set DHCP hostname
    if (sta_netif) {
        esp_netif_set_hostname(sta_netif, "photoframe");
        ESP_LOGI(TAG, "DHCP hostname set to: photoframe");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, &instance_got_ip));
    esp_event_handler_instance_t instance_got_ip6;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler,
                                                        NULL, &instance_got_ip6));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Don't start WiFi here - let wifi_manager_connect() or wifi_provisioning_start_ap() start it
    // ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_manager_init finished.");

    return ESP_OK;
}

static esp_err_t connect_one(const char *ssid, const char *password, int max_retries)
{
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *) wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Stop WiFi if it's running, then set config
    esp_wifi_stop();
    s_autoconnect = true;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));  // Enable power save at boot/connect

    s_retry_num = 0;
    s_max_retries = max_retries;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_ATTEMPT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
        return ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "Timed out connecting to SSID:%s", ssid);
        // Stop the driver's own reconnect loop so the next attempt starts clean.
        s_max_retries = 0;
        esp_wifi_disconnect();
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    return connect_one(ssid, password, 5);
}

esp_err_t wifi_manager_disconnect(void)
{
    s_is_connected = false;
    return esp_wifi_disconnect();
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t len)
{
    if (!ip_str || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Saved network list
// ---------------------------------------------------------------------------

// Read the legacy single-network keys (= last-good network).
static esp_err_t read_legacy(char *ssid, size_t ssid_size, char *password, size_t pass_size)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t ssid_len = ssid_size;
    err = nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, ssid, &ssid_len);
    if (err == ESP_OK) {
        size_t pass_len = pass_size;
        if (nvs_get_str(nvs_handle, NVS_WIFI_PASS_KEY, password, &pass_len) != ESP_OK) {
            password[0] = '\0';
        }
    }
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t store_list(const wifi_saved_network_t *list, int count)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    if (count > 0) {
        err = nvs_set_blob(nvs_handle, NVS_WIFI_NETS_KEY, list,
                           sizeof(wifi_saved_network_t) * (size_t) count);
    } else {
        err = nvs_erase_key(nvs_handle, NVS_WIFI_NETS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

// Load the saved list into list[WIFI_MAX_SAVED_NETWORKS]. Frames from before
// the list existed only have the legacy keys: those become entry 0 and are
// written back as the list (one-time migration).
static int load_list(wifi_saved_network_t *list)
{
    memset(list, 0, sizeof(wifi_saved_network_t) * WIFI_MAX_SAVED_NETWORKS);

    // Only a list that does not exist yet is migrated (and written). A list
    // that exists but cannot be read (e.g. written by a build with a larger
    // WIFI_MAX_SAVED_NETWORKS) is never overwritten; the last-good network is
    // then used from RAM only.
    bool list_missing = false;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len = sizeof(wifi_saved_network_t) * WIFI_MAX_SAVED_NETWORKS;
        esp_err_t err = nvs_get_blob(nvs_handle, NVS_WIFI_NETS_KEY, list, &len);
        nvs_close(nvs_handle);
        list_missing = (err == ESP_ERR_NVS_NOT_FOUND);
        if (err != ESP_OK && !list_missing) {
            ESP_LOGW(TAG, "Saved network list unreadable (%s), using last-good only",
                     esp_err_to_name(err));
            memset(list, 0, sizeof(wifi_saved_network_t) * WIFI_MAX_SAVED_NETWORKS);
        }
        if (err == ESP_OK) {
            int count = (int) (len / sizeof(wifi_saved_network_t));
            // Drop empty entries and force termination, defensively.
            int w = 0;
            for (int r = 0; r < count; r++) {
                list[r].ssid[WIFI_SSID_MAX_LEN] = '\0';
                list[r].password[WIFI_PASS_MAX_LEN] = '\0';
                if (list[r].ssid[0] != '\0') {
                    if (w != r) {
                        list[w] = list[r];
                    }
                    w++;
                }
            }
            for (int k = w; k < WIFI_MAX_SAVED_NETWORKS; k++) {
                memset(&list[k], 0, sizeof(list[k]));
            }
            return w;
        }
    }

    // Migration from the single-network layout.
    if (read_legacy(list[0].ssid, sizeof(list[0].ssid), list[0].password,
                    sizeof(list[0].password)) == ESP_OK &&
        list[0].ssid[0] != '\0') {
        if (list_missing && store_list(list, 1) == ESP_OK) {
            ESP_LOGI(TAG, "Migrated saved Wi-Fi network '%s' into the network list", list[0].ssid);
        }
        return 1;
    }
    memset(&list[0], 0, sizeof(list[0]));
    return 0;
}

static int find_in_list(const wifi_saved_network_t *list, int count, const char *ssid)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

// Make a network the last-good one (legacy keys + config_manager's RAM copy).
// Only writes flash when something actually changed.
static void set_last_good(const char *ssid, const char *password)
{
    if (strcmp(config_manager_get_wifi_ssid(), ssid) != 0) {
        config_manager_set_wifi_ssid(ssid);
    }
    if (strcmp(config_manager_get_wifi_password(), password ? password : "") != 0) {
        config_manager_set_wifi_password(password ? password : "");
    }
}

// Insert or update. Returns the index of the entry, or -1 on bad input.
static int upsert(wifi_saved_network_t *list, int *count, const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX_LEN ||
        (password && strlen(password) > WIFI_PASS_MAX_LEN)) {
        return -1;
    }
    int idx = find_in_list(list, *count, ssid);
    if (idx >= 0) {
        if (password && password[0] != '\0') {
            strncpy(list[idx].password, password, WIFI_PASS_MAX_LEN);
            list[idx].password[WIFI_PASS_MAX_LEN] = '\0';
        }
        return idx;
    }

    if (*count >= WIFI_MAX_SAVED_NETWORKS) {
        // Full: drop the oldest entry that is not the last-good network.
        const char *last_good = config_manager_get_wifi_ssid();
        int victim = 0;
        for (int i = 0; i < *count; i++) {
            if (strcmp(list[i].ssid, last_good) != 0) {
                victim = i;
                break;
            }
        }
        ESP_LOGW(TAG, "Network list full, dropping oldest saved network '%s'", list[victim].ssid);
        for (int i = victim; i < *count - 1; i++) {
            list[i] = list[i + 1];
        }
        (*count)--;
    }

    idx = *count;
    memset(&list[idx], 0, sizeof(list[idx]));
    strncpy(list[idx].ssid, ssid, WIFI_SSID_MAX_LEN);
    if (password) {
        strncpy(list[idx].password, password, WIFI_PASS_MAX_LEN);
    }
    (*count)++;
    return idx;
}

esp_err_t wifi_manager_add_network(const char *ssid, const char *password)
{
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    int idx = upsert(list, &count, ssid, password);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = store_list(list, count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved Wi-Fi network '%s' (%d saved)", ssid, count);
        // Keep the last-good copy's password in step when it was updated.
        if (strcmp(config_manager_get_wifi_ssid(), ssid) == 0) {
            set_last_good(list[idx].ssid, list[idx].password);
        }
    }
    return err;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    int idx = upsert(list, &count, ssid, password);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // A proven network: store exactly the password that just worked, even if
    // it is empty (open network).
    strncpy(list[idx].password, password ? password : "", WIFI_PASS_MAX_LEN);
    list[idx].password[WIFI_PASS_MAX_LEN] = '\0';
    esp_err_t err = store_list(list, count);
    if (err != ESP_OK) {
        return err;
    }
    set_last_good(list[idx].ssid, list[idx].password);
    s_credentials_saved = true;
    ESP_LOGI(TAG, "Wi-Fi network '%s' saved as last-good (%d saved)", ssid, count);
    return ESP_OK;
}

esp_err_t wifi_manager_remove_network(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    int idx = find_in_list(list, count, ssid);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (count <= 1) {
        return ESP_ERR_INVALID_STATE;
    }
    bool was_last_good = strcmp(config_manager_get_wifi_ssid(), ssid) == 0;
    for (int i = idx; i < count - 1; i++) {
        list[i] = list[i + 1];
    }
    count--;
    memset(&list[count], 0, sizeof(list[count]));
    esp_err_t err = store_list(list, count);
    if (err != ESP_OK) {
        return err;
    }
    if (was_last_good) {
        set_last_good(list[0].ssid, list[0].password);
    }
    ESP_LOGI(TAG, "Removed saved Wi-Fi network '%s' (%d left)", ssid, count);
    return ESP_OK;
}

int wifi_manager_get_saved_networks(wifi_saved_network_t *out, int max)
{
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    int n = count < max ? count : max;
    if (out && n > 0) {
        memcpy(out, list, sizeof(wifi_saved_network_t) * (size_t) n);
    }
    return n;
}

bool wifi_manager_get_saved_password(const char *ssid, char *password, size_t len)
{
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    int idx = ssid ? find_in_list(list, count, ssid) : -1;
    if (idx < 0 || !password || len == 0) {
        return false;
    }
    strncpy(password, list[idx].password, len - 1);
    password[len - 1] = '\0';
    return true;
}

bool wifi_manager_credentials_saved(void)
{
    return s_credentials_saved;
}

esp_err_t wifi_manager_load_credentials(char *ssid, char *password)
{
    return read_legacy(ssid, WIFI_SSID_MAX_LEN, password, WIFI_PASS_MAX_LEN);
}

esp_err_t wifi_manager_prefer_network(const char *ssid)
{
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    int idx = ssid ? find_in_list(list, count, ssid) : -1;
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    set_last_good(list[idx].ssid, list[idx].password);
    ESP_LOGI(TAG, "Wi-Fi '%s' will be tried first on the next connect", ssid);
    return ESP_OK;
}

void wifi_manager_get_last_connect_stats(int *ms, int *attempts, bool *scanned)
{
    if (ms) {
        *ms = s_last_connect_ms;
    }
    if (attempts) {
        *attempts = s_last_connect_attempts;
    }
    if (scanned) {
        *scanned = s_last_connect_scanned;
    }
}

static esp_err_t connect_saved_inner(void);

esp_err_t wifi_manager_connect_saved(void)
{
    int64_t start_us = esp_timer_get_time();
    s_last_connect_attempts = 0;
    s_last_connect_scanned = false;
    esp_err_t err = connect_saved_inner();
    s_last_connect_ms = (int) ((esp_timer_get_time() - start_us) / 1000);
    ESP_LOGI(TAG, "connect_saved: %s in %d ms (%d attempt(s)%s)",
             err == ESP_OK ? "connected" : "failed", s_last_connect_ms, s_last_connect_attempts,
             s_last_connect_scanned ? ", scanned" : "");
    return err;
}

static esp_err_t connect_saved_inner(void)
{
    wifi_saved_network_t list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_list(list);
    if (count == 0) {
        ESP_LOGE(TAG, "No saved Wi-Fi networks");
        return ESP_ERR_NOT_FOUND;
    }

    bool tried[WIFI_MAX_SAVED_NETWORKS] = {false};

    // 1. Last-good network, straight away (no scan): the normal timer wake at
    //    home costs exactly what it did with a single saved network. With other
    //    networks to fall back to, give up on it sooner.
    int last = find_in_list(list, count, config_manager_get_wifi_ssid());
    if (last < 0) {
        last = 0;
    }
    ESP_LOGI(TAG, "Connecting to last-good Wi-Fi '%s' (%d saved)", list[last].ssid, count);
    s_last_connect_attempts++;
    if (connect_one(list[last].ssid, list[last].password, count > 1 ? 2 : 5) == ESP_OK) {
        set_last_good(list[last].ssid, list[last].password);
        return ESP_OK;
    }
    tried[last] = true;
    if (count == 1) {
        return ESP_FAIL;
    }

    // 2. One scan, then the saved networks that are visible, strongest first.
    //    Saved networks that are not visible are skipped (this also skips
    //    hidden SSIDs, which do not show up in a scan).
    s_max_retries = 0;
    esp_wifi_disconnect();
    wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * WIFI_SAVED_SCAN_MAX);
    int n = aps ? wifi_manager_scan(aps, WIFI_SAVED_SCAN_MAX) : 0;
    s_last_connect_scanned = true;
    bool any_visible = false;
    for (int a = 0; a < n; a++) {
        int idx = find_in_list(list, count, (const char *) aps[a].ssid);
        if (idx < 0 || tried[idx]) {
            continue;
        }
        any_visible = true;
        tried[idx] = true;
        ESP_LOGI(TAG, "Trying saved Wi-Fi '%s' (RSSI %d)", list[idx].ssid, aps[a].rssi);
        s_last_connect_attempts++;
        if (connect_one(list[idx].ssid, list[idx].password, 3) == ESP_OK) {
            free(aps);
            set_last_good(list[idx].ssid, list[idx].password);
            ESP_LOGI(TAG, "Last-good Wi-Fi is now '%s'", list[idx].ssid);
            return ESP_OK;
        }
    }
    free(aps);

    // 3. The scan itself failed: try the rest blind rather than give up.
    if (n == 0) {
        for (int i = 0; i < count; i++) {
            if (tried[i]) {
                continue;
            }
            ESP_LOGI(TAG, "Scan empty, trying saved Wi-Fi '%s'", list[i].ssid);
            s_last_connect_attempts++;
            if (connect_one(list[i].ssid, list[i].password, 2) == ESP_OK) {
                set_last_good(list[i].ssid, list[i].password);
                return ESP_OK;
            }
        }
    } else if (!any_visible) {
        ESP_LOGW(TAG, "None of the other saved networks is in range");
    }

    return ESP_FAIL;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

int wifi_manager_scan(wifi_ap_record_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }

    // Save current WiFi mode
    wifi_mode_t original_mode;
    esp_err_t err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(err));
        return 0;
    }

    // Switch to APSTA mode if currently in AP-only mode
    if (original_mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
            return 0;
        }
    }

    // Start blocking scan on all channels
    wifi_scan_config_t scan_config = {0};
    err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_ERR_WIFI_STATE) {
        // The station is busy connecting; the driver will not scan meanwhile.
        // Stop that attempt and its retries, then scan once more.
        ESP_LOGW(TAG, "Scan refused while connecting - stopping the attempt and retrying");
        s_max_retries = 0;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&scan_config, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(original_mode);
        }
        return 0;
    }

    // Get number of APs found
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        ESP_LOGI(TAG, "No APs found");
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(original_mode);
        }
        return 0;
    }

    // Limit to max_results
    uint16_t fetch_count = (ap_count > (uint16_t) max_results) ? (uint16_t) max_results : ap_count;
    err = esp_wifi_scan_get_ap_records(&fetch_count, results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(err));
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(original_mode);
        }
        return 0;
    }

    // Restore original WiFi mode
    if (original_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(original_mode);
    }

    ESP_LOGI(TAG, "WiFi scan found %d APs (returning %d)", ap_count, fetch_count);
    return (int) fetch_count;
}

void wifi_manager_enter_setup_mode(void)
{
    s_autoconnect = false;
    s_max_retries = 0;
    s_retry_num = 0;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
}

esp_err_t wifi_manager_test_connect(const char *ssid, const char *password, int timeout_ms)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *) sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *) sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    }
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    // Keep the setup AP up while testing. STA_START must not connect on its
    // own here: the config below is not set yet at that moment.
    s_autoconnect = false;
    s_max_retries = 0;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Test connect: set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start from a clean result: a fail left over from the boot-time attempt
    // would otherwise answer at once, before this network was even tried.
    s_retry_num = 0;
    s_max_retries = 3;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "Test connect to '%s'", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Test connect: esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_max_retries = 0;
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Test connect to '%s' OK", ssid);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Test connect to '%s' %s", ssid, (bits & WIFI_FAIL_BIT) ? "failed" : "timed out");
    s_max_retries = 0;
    esp_wifi_disconnect();
    return ESP_FAIL;
}
