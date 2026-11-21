#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "config.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Saved Wi-Fi networks. The frame keeps up to WIFI_MAX_SAVED_NETWORKS of them
// in NVS (blob NVS_WIFI_NETS_KEY). The legacy single-network keys
// (NVS_WIFI_SSID_KEY / NVS_WIFI_PASS_KEY) always hold the network that last
// connected ("last good"); it is tried first on every wake.
#define WIFI_MAX_SAVED_NETWORKS 5

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
} wifi_saved_network_t;

esp_err_t wifi_manager_init(void);
// Toggle between full-RX performance (WIFI_PS_NONE, low latency / fast web UI)
// and modem power save (WIFI_PS_MIN_MODEM). Idempotent; safe to call every
// second. The policy for when to use which lives in power_manager.
esp_err_t wifi_manager_set_performance_mode(bool enable);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);
// Add (or update) a network in the saved list AND make it the last-good one.
// Used after a network was proven to work (captive portal, Improv, config
// change, wifi.txt). Never removes other saved networks (except to make room
// when the list is full: the oldest one that is not last-good is dropped).
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
// Load the last-good network.
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);
// Add or update a network in the saved list without connecting to it. A NULL
// or empty password on an existing entry keeps its stored password.
esp_err_t wifi_manager_add_network(const char *ssid, const char *password);
// Remove a network from the saved list. ESP_ERR_NOT_FOUND if it is not saved,
// ESP_ERR_INVALID_STATE if it is the only one left (the frame would be stranded).
// Removing the last-good network promotes the first remaining one.
esp_err_t wifi_manager_remove_network(const char *ssid);
// Copy the saved list (with passwords - never send those off the device).
// Returns the number of entries written.
int wifi_manager_get_saved_networks(wifi_saved_network_t *out, int max);
// Look up the stored password of a saved network. Returns false if not saved.
bool wifi_manager_get_saved_password(const char *ssid, char *password, size_t len);
// Make a saved network the one tried first on the next connect, without
// testing it now (e.g. right before the frame is moved to that place).
// ESP_ERR_NOT_FOUND if it is not saved.
esp_err_t wifi_manager_prefer_network(const char *ssid);
// Result of the last wifi_manager_connect_saved() in this boot/wake: time it
// took in ms, connect attempts (1 = last-good worked directly), and whether a
// scan was needed. -1 ms when it has not run.
void wifi_manager_get_last_connect_stats(int *ms, int *attempts, bool *scanned);
// Connect using the saved list: last-good first (no scan), then ONE scan and
// the saved networks that are visible, strongest first. On success the network
// that worked becomes last-good. Returns ESP_OK when connected.
esp_err_t wifi_manager_connect_saved(void);
// True once wifi_manager_save_credentials() ran in this boot. The provisioning
// loop in main.c waits on this.
bool wifi_manager_credentials_saved(void);
esp_err_t wifi_manager_load_credentials_from_sdcard(char *ssid, char *password);
EventGroupHandle_t wifi_manager_get_event_group(void);
int wifi_manager_scan(wifi_ap_record_t *results, int max_results);

#endif
