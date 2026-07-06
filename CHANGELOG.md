# Changelog

This changelog covers the **Snille fork's** additions to the firmware — board
support for the **DFRobot FireBeetle 2 ESP32-E** driving a **Waveshare 4" e-Paper
HAT+ E (Spectra 6, 400×600)** — layered on top of upstream
[aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe).

CI now builds **all six boards** (the five ESP32-S3 frames plus the classic-ESP32
DFRobot FireBeetle) on **ESP-IDF v6.0** from a single `v<version>` tag; each
release carries every board's flashable factory bin and drives the web flasher.
(The old manual `firebeetle-v<version>` line is retired.)

## 2.12.0

### Fixed
- **Battery percent/voltage no longer jump around between wakes on boards with a
  raw voltage-divider ADC** (EE02, EE04, reTerminal E1002, FireBeetle 2
  ESP32-E/ESP32-S3). The reading used to happen live at HTTP-request time —
  after WiFi was already connected and transmitting — so a WiFi-TX current
  sag could land a bogus low reading right in the "plausible" band the
  existing cross-wake filter couldn't catch. The battery is now read (a few
  quick samples, averaged) once per wake **before WiFi starts**, cached, and
  reused for the HTTP headers — the quietest point in the whole wake cycle.
  New `board_hal_battery_prime_reading()` API, called from `main.c` right
  after board init; every board with a battery ADC implements it, and
  `waveshare_photopainter_73` (AXP2101 PMIC, unaffected by this) gets a no-op.
- Boards running with deep sleep disabled (or on a board that stays awake
  while USB-powered) now also get a fresh, WiFi-quiet-as-possible battery
  reading every rotation — previously the one-per-wake cache never refreshed
  because these boards never actually reboot between rotations, so the
  reading froze at whatever it read the one time the frame did boot.
- `X-Reset-Reason` (and the boot log) now names USB / JTAG / eFuse-error /
  power-glitch / CPU-lockup / SDIO / external-pin resets instead of lumping
  them all into an opaque "unknown" — useful for telling apart "still on the
  bench, tethered to a debugger" from a genuine unexplained reset.

## 2.11.0

### Added
- **Battery voltage sensing on the FireBeetle 2 ESP32-S3** via an optional
  user-wired external voltage divider (VBAT — 1MΩ — GPIO — 1MΩ — GND). This
  board has no built-in battery ADC (confirmed against the schematic: the
  charger IC's STAT line only drives a status LED, and the onboard AXP313A
  PMIC powers only the camera FPC from VCC/USB, not the battery). The frame's
  local WebGUI (Power tab) now offers a pin picker — A0/A1/A2/A3 (GPIO4/5/6/8),
  the only free ADC1-capable, non-strapping pins on this board — persisted in
  NVS and applied live. The chosen pin (and the resulting voltage) is reported
  to the server as before, plus a new `X-Battery-ADC-Pin` header so the server
  can mirror which pin is configured.
- New generic `board_hal` capability API
  (`board_hal_get_battery_adc_pin_options` / `set_battery_adc_pin` /
  `get_battery_adc_pin`) for boards that support a user-configurable battery
  ADC pin; every other board reports zero options (no-op).

### Fixed
- **Battery percentage no longer briefly flickers to an implausible value on
  the FireBeetle 2 ESP32-E and Seeed XIAO EE02.** WiFi-TX current draw can sag
  the battery rail momentarily into a lower-but-still-"plausible" voltage band,
  which the LiPo discharge curve reads as a real, much lower charge level. A
  reading that drops more than 300mV from the last confirmed value is now held
  back until it repeats 3 times in a row (quantified against overnight data
  from three frames: ~22% suspect-drop rate per read, reduced to <1% residual
  false positives with 3-reading confirmation) — otherwise the last confirmed
  reading is reported instead of the spike.

## 2.10.13

### Added
- **The frame reports its last reset cause** (`X-Reset-Reason` header on every
  pull): poweron / deepsleep / sw / task_wdt / int_wdt / wdt / panic / brownout.
  A hung frame can't report while it's hung, but the reset cause persists across
  the reboot, so the next successful check-in tells the server why it last
  reset. The server flags a crash cause (watchdog/panic/brownout) in the Devices
  list, so a crash loop becomes visible after the frame recovers.

## 2.10.12

### Fixed
- **S3 frames froze in a rotate-then-reboot loop — properly fixed now.** The
  console had been routed to USB-Serial-JTAG (for boot logs + Improv over one
  cable). But USB-SJ console TX *blocks* when a USB host holds the port open
  without draining it — exactly a frame USB-powered from a PC. A rotation's log
  burst filled the buffer, the render task stalled, and the task watchdog reset
  the board: the frame rotated once, then its USB port vanished as it rebooted,
  over and over. v2.10.10's non-blocking-console attempt only delayed it (the
  IDF USB-SJ VFS doesn't honor `O_NONBLOCK` on write). **The console now stays
  on the default UART0** on all S3 boards (EE02 / EE04 / reTerminal /
  PhotoPainter-S3) — nothing in the log path can stall the app. Improv-Serial
  still provisions over USB-SJ (now selected by chip target, not the console
  config). Trade-off: no boot logs over USB on the S3 boards. FireBeetle was
  never affected (UART0 console).
- **Wi-Fi setup in the browser flasher (Improv) now appears.** During out-of-box
  setup the frame rendered the QR splash *before* starting Improv, and the
  6-color EPD refresh (~15-30 s) pushed Improv past ESP Web Tools' detection
  window, so the "enter Wi-Fi" step never showed. The AP + Improv now start first
  and the splash paints afterward (Improv is a background task), so the flasher
  sees the frame immediately.

## 2.10.10

### Fixed
- **First attempt at the S3 freeze (superseded by 2.10.12).** Made the
  USB-Serial-JTAG console non-blocking (install driver + `O_NONBLOCK`). It
  reduced but did not eliminate the freeze, because the IDF USB-SJ VFS ignores
  `O_NONBLOCK` on write — see 2.10.12, which moves the console off USB-SJ.

## 2.10.7

### Fixed
- **The update check now confirms "You're running the latest version".** When a
  check found no newer release the firmware reported the plain `idle` state, which
  the WebUI renders as a blank result — so an up-to-date frame just showed nothing
  after CHECK. It now reports a distinct `up_to_date` state (separate from "never
  checked"), and the WebUI shows a green "You're running the latest version."
  confirmation.

## 2.10.6

### Fixed
- **Update check works again now that the releases list is served chunked.** As
  the fork accumulated releases, the GitHub releases-list response grew past the
  point where GitHub sends a `Content-Length` and switched to
  `Transfer-Encoding: chunked` (~65 KB). The update check required a positive
  content length for its single-shot read, so it failed with the generic "Failed
  to check for updates" on every check — looking like a permanent error (it was
  *not* the API rate limit). The check now reads the response incrementally into a
  growable PSRAM buffer until the stream ends, handling both sized and chunked
  responses (capped at 256 KB). NOTE: a frame already stuck on an older build
  can't OTA to this fix (its broken check can't discover the release) — it needs
  one USB flash to break the deadlock; OTA works again afterward.

## 2.10.5

### Fixed
- **XIAO EE02: stable, consistent battery reading under WiFi load.** The pack
  voltage and percentage were read as two separate ADC acquisitions during the
  image pull, and a single read could land entirely inside a WiFi-TX current
  burst that sags the rail — so the frame occasionally reported garbage like
  "0 % @ 4090 mV" or a sub-3 V "collapse" while actually sitting at ~84 %. The
  EE02 driver now (a) takes a few reads and keeps the plausible ones (rejecting
  transient sub-3.3 V collapses), and (b) caches the result so percent is derived
  from the *same* voltage acquisition — the two can no longer disagree. This also
  stops the server from drawing a false "charging" bolt on battery (it had treated
  the collapsed readings as "plugged in").
- **WebUI auto-reloads after an OTA update.** After "Update successful! Device
  will reboot…" the page used to sit on that message until you manually refreshed.
  The OTA panel now watches for the device dropping offline (reboot starting) and
  coming back reachable, then reloads the page automatically so the new version and
  state show without a manual refresh (with a ~90 s safety-timeout fallback and an
  indeterminate progress bar while it waits). Note: this fix ships *in* the webapp,
  so it takes effect from the next OTA onward — the update that installs it still
  reboots the old page once.

## 2.10.4

### Fixed
- **No more false "charging" badge on the XIAO EE02.** The EE02 guessed charge
  status from `usb_serial_jtag_is_connected()` (USB present) plus battery voltage.
  That signal is the wrong primitive: it detects a USB *data host* (a PC), never a
  wall charger; it defaults to "connected" until its SOF monitor settles; and it's
  flaky under tickless idle — so the frame reported "charging" while running on
  battery, and flip-flopped between "charging" and a battery % while plugged in.
  Rev V1.0 hardware genuinely can't sense USB power or charge state (no VBUS GPIO;
  the BQ24070 STAT pins drive LEDs only — verified against the Seeed schematic), so
  the frame now reports **no** charge status (`board_hal_supports_charge_status()`
  → false; `is_charging`/`is_usb_connected` → false). The server still infers
  charge *direction* honestly from the battery-voltage trend over time (the HA
  "Battery Trend" sensor), and the on-photo badge shows a steady battery %.
- **Update check explains *why* it failed instead of a blanket "Failed to check
  for updates".** The check queries the GitHub API unauthenticated (60 requests/
  hour per IP), so repeated checks + OTA testing return **403** and the WebUI just
  said "Failed". `fetch_github_release_info` now surfaces the real reason:
  "GitHub rate limit – try again in a while" (HTTP 403/429), "GitHub returned
  HTTP N", "Couldn't reach GitHub (check WiFi)", or "No firmware release found for
  this board". (When the check *succeeds* and you're current it already says
  you're on the latest — the old "Failed" was almost always the rate limit.)

## 2.10.3

### Fixed
- **Stable, calibrated battery reading on the XIAO EE02.** The EE02's
  battery-voltage read took a single uncalibrated ADC sample (`raw * 3300/4095`),
  so the reported percentage jumped wildly between reads (e.g. 20 % → 85 % → 53 %)
  — the ESP32-S3 ADC is noisy and nonlinear, and the steep LiPo curve (~9 mV per
  percent) amplified every bit of noise. The board now uses the shared
  `battery_adc` helper (ported from upstream) which applies **eFuse curve-fitting
  calibration** and **averages 8 samples** per read, giving a steady, accurate
  voltage. Only affects the EE02 (the FireBeetle already calibrated + averaged its
  own read).

## 2.10.2

Pulls two upstream improvements (cherry-picked from
[aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe)) that
apply to every board.

### Added
- **WebUI keeps the frame awake while its tab is focused, with an "asleep" hint.**
  The browser pings `/api/keep_alive` while the WebUI tab is in focus so the frame
  doesn't deep-sleep out from under you mid-configuration; when it stops
  responding the UI shows a "Device is asleep" hint naming the board's wakeup
  key — the **wake button (D7)** on the FireBeetle and **Button 3** on the EE02.

### Changed
- **Longer auto-sleep timeout during first-time WiFi setup (OOBE).** The frame
  now waits up to 10 minutes before deep-sleeping while you're still in the
  out-of-box setup flow, giving more time to join the AP and enter WiFi
  credentials before it sleeps.

## 2.10.1

### Changed
- **OTA now checks this fork instead of upstream.** The firmware update check
  pointed at `aitjcize/esp32-photoframe`, so a frame only ever saw the upstream
  releases (and could pull an unrelated upstream build). It now queries
  **`Snille/esp32-photoframe`**. Because the repo carries two parallel release
  lines — the classic-ESP32 FireBeetle (`firebeetle-v*`, no OTA) and the
  ESP32-S3 boards (`v*`, OTA-capable) — the check walks the releases *list*
  (newest first) and picks the newest published release that actually contains
  this board's binary (`esp32-photoframe-<board>.bin`), rather than GitHub's
  single "latest" release which could be a FireBeetle-only one with no S3 asset.

### Added
- **Charge status on the XIAO EE02 (best-effort) → Home Assistant.** The EE02's
  BQ24070 charger exposes no MCU-readable charge line — its status pins drive the
  on-board LEDs only (confirmed against the Seeed schematic) — so charging can't
  be read directly. The board now derives a coarse status from USB-present +
  battery voltage (`charging` / `full` / `on_battery`) and reports it to the
  server (`X-Battery-Status`), which publishes a **Battery Status** sensor over
  MQTT. A new `board_hal_supports_charge_status()` capability gates this so
  boards that can't sense it (e.g. the FireBeetle) report nothing instead of a
  misleading value. It is an estimate — it can read `full` during the final
  charge taper, and a non-data USB power source may not register as connected.

### Fixed
- **Corrected charge-status documentation for the EE02/EE04.** The board Kconfig
  claimed "Charging status via GPIO"; the schematic shows the BQ24070 status
  pins are wired to LEDs only, not a GPIO. Updated the help text and the driver
  comments to match.

## 2.10.0

First release covering the **Seeed Studio XIAO EE02** (13.3" Spectra 6, ESP32-S3,
PSRAM + 16 MB flash + OTA) alongside the FireBeetle. The EE02 and the other
ESP32-S3 boards are built by CI on the `v*` tag and published as flashable
factory bins in the GitHub Release.

### Fixed
- **On-device display orientation at rotated mounts (deg 90/270).** Images
  produced on the device itself (`/api/display-image` raw push and the WebUI
  upload → display path) were rendered at native panel dimensions, but
  `GUI_Paint`'s drawing surface is width/height-swapped to the logical/viewing
  dimensions at `ROTATE_90/270`. The mismatch clipped the image and made it
  appear 180° rotated on a landscape-mounted panel. On-device processing now
  fits to the logical (viewing) dimensions derived from `display_rotation_deg`,
  matching the server's EPDGZ path. No-op at deg 0/180; server pull/push were
  always correct and are unchanged.

### Changed
- **Splash generation has a layout-faithful Pillow fallback** for build hosts
  without an SVG renderer (librsvg/Cairo): the baked splash now reserves the
  same QR slots the firmware draws its live WiFi / web-UI QR into (and renders
  the static app-download QR), instead of a placeholder whose centered text
  collided with the live QR.

## 2.9.5

### Added
- **Reports the wake reason** (`X-Wake-Reason` header) on each image fetch: `timer` (auto-rotate timer wake), `button` (wake button), or `boot` (cold boot / reset). The server surfaces this as the Home Assistant **"Last Trigger"** sensor so automations can tell *why* the frame changed image. No new hardware; derived from the existing deep-sleep wakeup cause.

## 2.9.4

### Fixed
- **Processing settings can be pushed to the frame again.** The `/api/settings/processing` POST handler allocated the request buffer from PSRAM (`MALLOC_CAP_SPIRAM`), but this board has no usable PSRAM — every POST returned 500, so the server's direct-push of processing settings (and the frame's own WebUI "save processing") silently failed. It now uses the internal heap like the colour-palette handler beside it; the body is a tiny JSON blob.
- **Processing-preset detection in the device WebUI tolerates float drift.** Values loaded from the frame's 32-bit-float NVS come back slightly off (e.g. `1.4` → `1.4000000953…`), which an exact comparison mislabelled as "Custom". Detection now compares numbers with a small epsilon, so a stored preset is recognised correctly.

## 2.9.3

### Changed
- **Theme-following logo in the device WebUI**: the app-bar logo now follows the active theme (its shell, sun and horizon take the theme's primary colour) with a light-blue sky, instead of a fixed terracotta mark. Cosmetic; no functional firmware change.

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
