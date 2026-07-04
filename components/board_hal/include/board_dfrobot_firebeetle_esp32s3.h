#ifndef BOARD_DFROBOT_FIREBEETLE_ESP32S3_H
#define BOARD_DFROBOT_FIREBEETLE_ESP32S3_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "dfrobot_firebeetle_esp32s3"
#define BOARD_HAL_TYPE BOARD_TYPE_DFROBOT_FIREBEETLE_ESP32S3

// Wake button: momentary switch between D7 (GPIO9) and GND. No rotate/clear
// buttons wired.
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_9
#define BOARD_HAL_WAKEUP_KEY_NAME "wake button (D7)"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_NC
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_NC

// SPI Pins — arbitrary GPIOs via the ESP32-S3 GPIO matrix (not native SPI pins)
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_17
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_15
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_NC

// E-Paper Display Pins (Waveshare 4inch e-Paper HAT+ E).
// CS and DC were originally wired to D9/IO0 and D2/IO3, but GPIO0 and GPIO3
// are ESP32-S3 boot-strapping pins — confirmed on hardware (2026-07-03) that
// the display's CS line holds GPIO0 low at reset, forcing the chip into
// permanent USB/UART download mode (app never runs, no crash, no console).
// CS and DC were rewired to non-strapping GPIOs (ESP32-S3 strapping pins are
// GPIO0/3/45/46 — all avoided here):
// CS   = Orange wire → A4/IO10 (GPIO 10) — moved from D9/IO0
// BUSY = Lila wire   → D6/IO18 (GPIO 18)
// RST  = White wire  → D3/IO38 (GPIO 38)
// DC   = Green wire  → A5/IO11 (GPIO 11) — moved from D2/IO3
// PWR  = Brun wire   → 3V3 (hardwired), VCC = Red wire → 3V3 (hardwired)
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_10
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_18
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_38
#define BOARD_HAL_EPD_DC_PIN GPIO_NUM_11

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

#endif  // BOARD_DFROBOT_FIREBEETLE_ESP32S3_H
