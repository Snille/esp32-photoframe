# Changelog

This changelog covers the **Snille fork's** additions to the firmware — board
support for the **DFRobot FireBeetle 2 ESP32-E** driving a **Waveshare 4" e-Paper
HAT+ E (Spectra 6, 400×600)** — layered on top of upstream
[aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe).

Versions below are the FireBeetle `FIRMWARE_VERSION`. Flashable factory bins are
published as GitHub Releases tagged `firebeetle-v<version>` (the board-prefixed
tag keeps them out of the upstream `v*` CI that only builds the ESP32-S3 boards;
the FireBeetle bin is built manually — classic ESP32, ESP-IDF v5.3.3, app at
`0x10000`, 4 MB / dio / 40 MHz).

## 2.9.2

### Added
- **Reports battery voltage** (`X-Battery-Voltage` header, mV) alongside the existing percentage on each image fetch. The frame already measures it (GPIO34) — no new hardware. The server uses the raw voltage for a finer battery-drain estimate than the coarse integer percentage (LiPo voltage moves continuously). Sent only when a plausible reading is available.

## 2.9.1

### Fixed
- **Wake button no longer reboots the frame**: a short press while awake ran the "next image" action (HTTP fetch + image decode + display refresh) directly on the 2560-byte `button_monitor_task` stack, overflowing it and rebooting the device. The dispatcher is now split — the button task hands the heavy actions (`next_image`, `info_screen`) to the 16 KB `rotation_timer_task` via a one-slot mailbox, while light actions still run inline.

## 2.9.0

### Added
- **HTTPS-capability flag for no-PSRAM frames**: the FireBeetle (no PSRAM) can't complete a TLS handshake alongside the 120 KB framebuffer, so `https://` image rotation failed with mbedTLS allocation errors and never displayed. `system-info` now exposes an `https_supported` flag (true only when PSRAM is present) so the frame/server web UIs can warn against and block `https://` image URLs on boards that can't do it.
- **Heap-headroom instrumentation**: one-line free + largest-contiguous-block log before each image fetch, which pinned down the RAM wall behind the TLS failure.

### Docs
- Worked **battery-life estimate** for the FireBeetle 4" Spectra 6 build (per-cycle energy, per-day math, interval + sleep-schedule effect — ~3 months on a 4000 mAh LiPo at 15-min rotation with a 22:00–04:30 schedule), plus how to measure exactly via `/api/battery`.

### Build
- Gitignore local `release-firebeetle/` release artifacts.

## 2.8.0

### Added
- **Configurable wake-button gestures**: map short (<2 s) / long (2–5 s) / hold (≥5 s) presses on the wake button to actions (next image, sleep, toggle deep sleep, info screen). Configurable from the frame and server web UIs and synced through device config (`button_action_short` / `long` / `hold`).
- **Local info screen** (`display_manager_show_info_screen`): name, IP, Wi-Fi SSID, server URL and firmware version rendered on the panel via GUI_Paint.

### Changed
- Replaced the old overlapping short-press/hold pollers with a single unified gesture detector in `power_manager.c`.

### Fixed
- **GPIO2 LED current draw in deep sleep**: GPIO2 (dual-purpose e-paper CS + on-board active-high LED) is now driven and held low before deep sleep so the LED stops drawing current; the intent lives in the FireBeetle board HAL.

## 2.7.1

### Build
- **Pin the full CA bundle for reliable HTTPS**: set `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` + `DEFAULT_FULL` in the FireBeetle board defaults so clean builds verify public certs (Let's Encrypt etc.) from a reverse proxy instead of falling back to the smaller CMN list. Verified on hardware — valid cert fetches succeed, an expired cert is rejected.

## 2.7.0

### Added
- **FireBeetle 2 ESP32-E + Waveshare 4" Spectra 6 board support**: new `epaper_driver_ws4in_e6` driver, board HAL + header, single-factory partition layout, battery monitoring on GPIO34, wake button + deep sleep. Classic ESP32, 4 MB flash, no PSRAM.
- **SRAM-only adaptations** for the no-PSRAM board: raw-EPD image transfer (uncompressed, so the device doesn't OOM inflating compressed images) and a shared framebuffer.

### Changed
- **OTA auto-disabled on single-partition boards**, skipping the failing boot-time TLS check on every wake.
- **Coarser e-paper BUSY polling** (10 ms → 50 ms) so tickless idle keeps the CPU in light sleep during the ~19 s panel refresh.

### Build
- Windows build fixes in `build.py` / `generate_splash.py`.
