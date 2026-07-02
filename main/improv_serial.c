#include "improv_serial.h"

#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"

// ---------------------------------------------------------------------------
// Console transport selection
//
// ESP Web Tools talks to whichever serial port it used to flash the board:
//   - classic ESP32 (FireBeetle): a CH340 USB-UART bridge on UART0
//   - ESP32-S3 boards: the chip's native USB-Serial-JTAG controller
// Selected by CHIP TARGET, deliberately NOT by the console config: the S3
// console stays on UART0 (routing it to USB-SJ froze the frame — see the board
// sdkconfig notes), so we install the USB-SJ driver ourselves just for Improv
// RX + packet TX and leave ESP_LOG on its own console path.
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define IMPROV_TRANSPORT_USJ 1
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif defined(CONFIG_ESP_CONSOLE_UART)
#define IMPROV_TRANSPORT_UART 1
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#else
#define IMPROV_TRANSPORT_NONE 1
#endif

static const char *TAG = "improv";

// ---- Improv-Serial protocol constants -------------------------------------
#define IMPROV_HEADER "IMPROV"
#define IMPROV_HEADER_LEN 6
#define IMPROV_VERSION 1

// Packet types
#define IMPROV_TYPE_CURRENT_STATE 0x01
#define IMPROV_TYPE_ERROR_STATE 0x02
#define IMPROV_TYPE_RPC 0x03
#define IMPROV_TYPE_RPC_RESPONSE 0x04

// Device states
#define IMPROV_STATE_AUTHORIZED 0x02
#define IMPROV_STATE_PROVISIONING 0x03
#define IMPROV_STATE_PROVISIONED 0x04

// Error states
#define IMPROV_ERROR_NONE 0x00
#define IMPROV_ERROR_INVALID_RPC 0x01
#define IMPROV_ERROR_UNKNOWN_RPC 0x02
#define IMPROV_ERROR_UNABLE_TO_CONNECT 0x03

// RPC commands (host -> device)
#define IMPROV_CMD_SEND_WIFI 0x01
#define IMPROV_CMD_REQUEST_STATE 0x02
#define IMPROV_CMD_REQUEST_DEVICE_INFO 0x03
#define IMPROV_CMD_REQUEST_SCAN 0x04

// Largest payload a single packet can carry (length field is one byte)
#define IMPROV_MAX_PAYLOAD 255
// Full frame: header(6) + version + type + length + payload + checksum + '\n'
#define IMPROV_MAX_FRAME (IMPROV_HEADER_LEN + 3 + IMPROV_MAX_PAYLOAD + 2)

static bool s_started = false;

// ---------------------------------------------------------------------------
// Transport I/O
// ---------------------------------------------------------------------------
static esp_err_t improv_io_init(void)
{
    // NOTE: we deliberately do NOT route the console VFS through the driver
    // (no uart_vfs_dev_use_driver / usb_serial_jtag_vfs_use_driver). Doing so
    // swallowed all ESP_LOG output once Improv started — silencing the device
    // during provisioning and hiding Wi-Fi connect failures. Instead we install
    // the driver only to read RX and to write our binary packets; ESP_LOG keeps
    // its normal console TX path. Improv writes and log writes can interleave on
    // TX, but the Improv client resynchronizes on the "IMPROV" header, and log
    // traffic during provisioning is sparse, so this is harmless in practice.
#if IMPROV_TRANSPORT_UART
    esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
#elif IMPROV_TRANSPORT_USJ
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 512,
        .tx_buffer_size = 512,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    // ESP_ERR_INVALID_STATE means the driver is already installed — app_main's
    // console_make_nonblocking() installs it at boot on the S3 boards. That's
    // fine; we share the same driver for RX/TX.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

// Read a single byte. Returns 1 on success, 0 on timeout/none.
static int improv_io_read_byte(uint8_t *b, uint32_t timeout_ms)
{
#if IMPROV_TRANSPORT_UART
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, b, 1, pdMS_TO_TICKS(timeout_ms));
#elif IMPROV_TRANSPORT_USJ
    return usb_serial_jtag_read_bytes(b, 1, pdMS_TO_TICKS(timeout_ms));
#else
    (void) b;
    (void) timeout_ms;
    return 0;
#endif
}

static void improv_io_write(const uint8_t *data, size_t len)
{
#if IMPROV_TRANSPORT_UART
    uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, data, len);
#elif IMPROV_TRANSPORT_USJ
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(200));
#else
    (void) data;
    (void) len;
#endif
}

// ---------------------------------------------------------------------------
// Packet building
// ---------------------------------------------------------------------------
static void improv_send_packet(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t pkt[IMPROV_MAX_FRAME];
    size_t i = 0;

    memcpy(pkt, IMPROV_HEADER, IMPROV_HEADER_LEN);
    i = IMPROV_HEADER_LEN;
    pkt[i++] = IMPROV_VERSION;
    pkt[i++] = type;
    pkt[i++] = len;
    if (len > 0 && payload != NULL) {
        memcpy(pkt + i, payload, len);
        i += len;
    }
    uint8_t checksum = 0;
    for (size_t k = 0; k < i; k++) {
        checksum += pkt[k];
    }
    pkt[i++] = checksum;
    pkt[i++] = '\n';  // aids readability in serial logs; clients ignore it

    improv_io_write(pkt, i);
}

static void improv_send_current_state(uint8_t state)
{
    improv_send_packet(IMPROV_TYPE_CURRENT_STATE, &state, 1);
}

static void improv_send_error(uint8_t error)
{
    improv_send_packet(IMPROV_TYPE_ERROR_STATE, &error, 1);
}

// Append a length-prefixed string into buf at *pos (clamped to fit the frame).
static void append_lp_string(uint8_t *buf, size_t cap, size_t *pos, const char *s)
{
    size_t n = strlen(s);
    if (n > 255) {
        n = 255;
    }
    if (*pos + 1 + n > cap) {
        return;  // would overflow the payload; drop (shouldn't happen for our data)
    }
    buf[(*pos)++] = (uint8_t) n;
    memcpy(buf + *pos, s, n);
    *pos += n;
}

// Send an RPC_RESPONSE: [command, payload_len, <length-prefixed strings...>]
static void improv_send_rpc_response(uint8_t command, const char *const *strings, int count)
{
    uint8_t data[IMPROV_MAX_PAYLOAD];
    size_t pos = 0;

    data[pos++] = command;
    data[pos++] = 0;  // payload length placeholder (filled below)
    size_t payload_start = pos;
    for (int i = 0; i < count; i++) {
        append_lp_string(data, sizeof(data), &pos, strings[i]);
    }
    data[1] = (uint8_t) (pos - payload_start);

    improv_send_packet(IMPROV_TYPE_RPC_RESPONSE, data, (uint8_t) pos);
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------
static const char *improv_chip_family(void)
{
#if CONFIG_IDF_TARGET_ESP32
    return "ESP32";
#elif CONFIG_IDF_TARGET_ESP32S3
    return "ESP32-S3";
#elif CONFIG_IDF_TARGET_ESP32S2
    return "ESP32-S2";
#elif CONFIG_IDF_TARGET_ESP32C3
    return "ESP32-C3";
#else
    return CONFIG_IDF_TARGET;
#endif
}

static void improv_send_state(void)
{
    improv_send_current_state(wifi_provisioning_is_provisioned() ? IMPROV_STATE_PROVISIONED
                                                                 : IMPROV_STATE_AUTHORIZED);
}

static void improv_handle_device_info(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *info[4] = {
        "PhotoFrame",                     // firmware name
        desc ? desc->version : "?",       // firmware version (git-tag stamp)
        improv_chip_family(),             // chip family
        config_manager_get_device_name()  // device name
    };
    improv_send_rpc_response(IMPROV_CMD_REQUEST_DEVICE_INFO, info, 4);
}

static void improv_handle_scan(void)
{
    wifi_ap_record_t aps[20];
    int n = wifi_manager_scan(aps, sizeof(aps) / sizeof(aps[0]));

    for (int i = 0; i < n; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue;  // skip hidden/empty SSIDs
        }
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
        const char *row[3] = {
            (const char *) aps[i].ssid, rssi,
            (aps[i].authmode == WIFI_AUTH_OPEN) ? "NO" : "YES",  // auth required
        };
        improv_send_rpc_response(IMPROV_CMD_REQUEST_SCAN, row, 3);
    }
    // Empty result terminates the list.
    improv_send_rpc_response(IMPROV_CMD_REQUEST_SCAN, NULL, 0);
}

static void improv_handle_send_wifi(const uint8_t *cdata, uint8_t clen)
{
    // Layout: [ssid_len][ssid...][pass_len][pass...]
    if (clen < 1) {
        improv_send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    uint8_t ssid_len = cdata[0];
    if ((size_t) 1 + ssid_len + 1 > clen) {
        improv_send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    uint8_t pass_len = cdata[1 + ssid_len];
    if ((size_t) 1 + ssid_len + 1 + pass_len > clen) {
        improv_send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }

    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1] = {0};
    size_t sn = ssid_len < WIFI_SSID_MAX_LEN ? ssid_len : WIFI_SSID_MAX_LEN;
    size_t pn = pass_len < WIFI_PASS_MAX_LEN ? pass_len : WIFI_PASS_MAX_LEN;
    memcpy(ssid, cdata + 1, sn);
    memcpy(pass, cdata + 1 + ssid_len + 1, pn);

    ESP_LOGI(TAG, "Improv: received Wi-Fi credentials for SSID '%s'", ssid);
    improv_send_current_state(IMPROV_STATE_PROVISIONING);

    // During provisioning the radio is in AP mode. wifi_manager_connect() sets
    // the STA config + starts WiFi but does NOT switch the mode, so we must move
    // to STA ourselves first — otherwise the "connect" runs while still in AP
    // mode and never associates (every network would fail).
    esp_wifi_set_mode(WIFI_MODE_STA);

    // Connect test (bounded: wifi_manager retries a few times then fails). This
    // tears down the setup AP; we restore it on failure so the captive portal
    // and a retry stay available.
    if (wifi_manager_connect(ssid, pass) == ESP_OK) {
        wifi_manager_save_credentials(ssid, pass);

        char url[48];
        char ip[16];
        if (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK) {
            snprintf(url, sizeof(url), "http://%s", ip);
        } else {
            snprintf(url, sizeof(url), "http://photoframe.local");
        }
        const char *res[1] = {url};
        improv_send_rpc_response(IMPROV_CMD_SEND_WIFI, res, 1);
        improv_send_current_state(IMPROV_STATE_PROVISIONED);
        ESP_LOGI(TAG, "Improv: provisioned, device URL %s — main loop will reboot", url);
        // wifi_provisioning_is_provisioned() now returns true; the provisioning
        // loop in main.c finishes setup and restarts.
    } else {
        ESP_LOGW(TAG, "Improv: could not connect to '%s'", ssid);
        improv_send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
        improv_send_current_state(IMPROV_STATE_AUTHORIZED);
        wifi_provisioning_start_ap();  // bring the setup AP back for a retry
    }
}

// Dispatch a fully-parsed, checksum-verified packet.
static void improv_dispatch(uint8_t type, const uint8_t *data, uint8_t len)
{
    if (type != IMPROV_TYPE_RPC) {
        return;  // we only act on RPC commands from the host
    }
    if (len < 2) {
        improv_send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    uint8_t command = data[0];
    uint8_t cmd_len = data[1];
    const uint8_t *cmd_data = data + 2;
    if ((size_t) 2 + cmd_len > len) {
        improv_send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }

    switch (command) {
    case IMPROV_CMD_REQUEST_STATE:
        improv_send_state();
        break;
    case IMPROV_CMD_REQUEST_DEVICE_INFO:
        improv_handle_device_info();
        break;
    case IMPROV_CMD_REQUEST_SCAN:
        improv_handle_scan();
        break;
    case IMPROV_CMD_SEND_WIFI:
        improv_handle_send_wifi(cmd_data, cmd_len);
        break;
    default:
        improv_send_error(IMPROV_ERROR_UNKNOWN_RPC);
        break;
    }
}

// ---------------------------------------------------------------------------
// Receive parser + task
// ---------------------------------------------------------------------------
static void improv_task(void *arg)
{
    (void) arg;

    // Announce our state so a listening client detects an Improv device even
    // without first probing.
    improv_send_state();

    uint8_t buf[IMPROV_HEADER_LEN + 3 + IMPROV_MAX_PAYLOAD + 1];
    size_t pos = 0;        // bytes accumulated in buf
    uint8_t data_len = 0;  // payload length once the length byte is seen

    while (1) {
        uint8_t b;
        if (improv_io_read_byte(&b, 50) <= 0) {
            continue;
        }

        // Match the "IMPROV" header (resynchronize on a stray 'I').
        if (pos < IMPROV_HEADER_LEN) {
            if (b == (uint8_t) IMPROV_HEADER[pos]) {
                buf[pos++] = b;
            } else {
                pos = (b == (uint8_t) IMPROV_HEADER[0]) ? 1 : 0;
                if (pos == 1) {
                    buf[0] = b;
                }
            }
            continue;
        }

        buf[pos++] = b;

        if (pos == IMPROV_HEADER_LEN + 1) {  // version byte
            if (buf[IMPROV_HEADER_LEN] != IMPROV_VERSION) {
                pos = 0;  // unsupported version; resync
            }
            continue;
        }
        if (pos == IMPROV_HEADER_LEN + 2) {  // type byte
            continue;
        }
        if (pos == IMPROV_HEADER_LEN + 3) {  // length byte
            data_len = buf[IMPROV_HEADER_LEN + 2];
            continue;
        }

        // Accumulate payload + the trailing checksum byte.
        size_t frame_len = IMPROV_HEADER_LEN + 3 + (size_t) data_len + 1;
        if (pos < frame_len) {
            continue;
        }

        uint8_t checksum = 0;
        for (size_t k = 0; k < frame_len - 1; k++) {
            checksum += buf[k];
        }
        if (checksum == buf[frame_len - 1]) {
            improv_dispatch(buf[IMPROV_HEADER_LEN + 1], &buf[IMPROV_HEADER_LEN + 3], data_len);
        } else {
            ESP_LOGW(TAG, "Improv: bad checksum, dropping frame");
        }
        pos = 0;
    }
}

esp_err_t improv_serial_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
#if IMPROV_TRANSPORT_NONE
    ESP_LOGW(TAG, "Improv-Serial unsupported: no UART/USB-Serial-JTAG console");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err = improv_io_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(improv_task, "improv", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create improv task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "Improv-Serial provisioning listener started");
    return ESP_OK;
#endif
}
