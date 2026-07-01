#ifndef IMPROV_SERIAL_H
#define IMPROV_SERIAL_H

#include <stdbool.h>

#include "esp_err.h"

// Improv Wi-Fi provisioning over the USB/serial console.
//
// Implements the Improv-Serial protocol (https://www.improv-wifi.com/serial/)
// so a browser running ESP Web Tools (the same flasher served from the project's
// GitHub Pages) can hand Wi-Fi credentials to a freshly flashed frame straight
// over USB — no need to join the device's setup AP and open the captive portal.
//
// The device scans for nearby networks on request, so the browser can present a
// dropdown of SSIDs the *frame* can actually see (plus free-text entry).
//
// Intended to run only during the out-of-box provisioning window (when no Wi-Fi
// credentials are stored), alongside the captive-portal AP. On success the
// credentials are written to NVS via wifi_manager_save_credentials(), which the
// main provisioning loop detects to finish setup and reboot.

// Start the Improv-Serial listener task. Safe to call once. Returns ESP_OK on
// success, or ESP_ERR_NOT_SUPPORTED if the active console transport can't carry
// the protocol (no UART / USB-Serial-JTAG console configured for this board).
esp_err_t improv_serial_start(void);

#endif  // IMPROV_SERIAL_H
