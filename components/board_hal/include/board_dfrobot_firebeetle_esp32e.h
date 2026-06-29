#ifndef BOARD_DFROBOT_FIREBEETLE_ESP32E_H
#define BOARD_DFROBOT_FIREBEETLE_ESP32E_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "dfrobot_firebeetle_esp32e"
#define BOARD_HAL_TYPE BOARD_TYPE_DFROBOT_FIREBEETLE_ESP32E

// Wake button: momentary switch between D7 (GPIO13, RTC-capable) and GND.
// EXT1 wakes on the pin going LOW; firmware enables an internal pull-up and
// holds it across deep sleep. Add an external 10k pull-up to 3V3 only if
// spurious wake-ups occur. No rotate/clear buttons wired.
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_13
#define BOARD_HAL_WAKEUP_KEY_NAME "wake button (D7)"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_NC
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_NC

// SPI Pins — VSPI (SPI3_HOST) native pins on classic ESP32
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_18
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_23
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_NC

// E-Paper Display Pins (Waveshare 4inch e-Paper HAT+ E)
// NOTE: FireBeetle 2 ESP32-E silkscreen "Dx" labels do NOT map 1:1 to GPIOx.
// Verified mapping: D9=GPIO2, D6=GPIO14, D3=GPIO26, D2=GPIO25 (D12=GPIO4).
// CS   = Orange wire → D9  (GPIO 2)
// BUSY = Purple wire → D6  (GPIO 14)  <-- D6 is GPIO14, not GPIO4
// RST  = White wire  → D3  (GPIO 26)
// DC   = Green wire  → D2  (GPIO 25)
// PWR  = Brown wire  → hardwired to 3V3 (no GPIO needed)
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_2
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_14
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_26
#define BOARD_HAL_EPD_DC_PIN GPIO_NUM_25

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

#endif  // BOARD_DFROBOT_FIREBEETLE_ESP32E_H
