#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hal_ee02";

// Pin Definitions for XIAO EE02
// Based on typical XIAO ESP32-S3 usage:
// VBAT read often uses GPIO 1 with divider, or internal measurement.
// XIAO ESP32-S3 has precise battery voltage on IO1 (A0) via voltage divider (R11=100k, R10=100k =>
// factor 2) D0/GPIO1 is A0
#define VBAT_ADC_CHANNEL ADC_CHANNEL_0  // GPIO 1 is ADC1_CHANNEL_0
#define VBAT_ADC_ENABLE_PIN GPIO_NUM_6  // TPS22916 load switch enable
#define VBAT_VOLTAGE_DIVIDER 2.0f

// Optional per-unit correction for resistor-divider tolerance, measured with a
// multimeter (1.0 = none). Override at build time if a board reads consistently
// high/low.
#ifndef VBAT_CAL_SCALE
#define VBAT_CAL_SCALE 1.0f
#endif

// USB / charge sensing is NOT possible on this board (rev V1.0): VBUS is not
// broken out to a readable GPIO, and per the official Seeed EE02 schematic the
// BQ24070 STAT pins drive on-board LEDs only, not a GPIO. The USB-Serial/JTAG
// link state was tried as a proxy but it only detects a USB *data host* (a PC,
// not a wall charger), defaults to "connected" until the SOF monitor settles,
// and is flaky under tickless idle — so it produced false "charging" on battery.
// We therefore report no charge status from the frame; the server infers charge
// direction honestly from the battery-voltage trend over time. See
// board_hal_is_charging() / board_hal_supports_charge_status() below.
// Verified pin map (rev V1.0): BAT_ADC -> GPIO1 (D0/A0); ADC load-switch enable
// (TPS22916) -> GPIO6; buttons -> GPIO2/3/5.

static battery_adc_t *vbat_adc = NULL;

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE02 Board HAL");

    // Release any pad hold latched by the previous deep-sleep cycle so we
    // can reconfigure the TPS22916 enable pin during init.
    gpio_hold_dis(VBAT_ADC_ENABLE_PIN);

    // Initialize SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1200 * 1600 / 2 + 100,  // Sufficient for 13.3" EPD
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Initialize E-Paper Display Port
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = BOARD_HAL_EPD_CS1_PIN,
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,
    };
    epaper_init(&ep_cfg);

    // Configure TPS22916 enable pin (GPIO6) as output, default LOW (disabled)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VBAT_ADC_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);

    // Initialize the shared, eFuse-calibrated + averaged battery ADC reader.
    // The TPS22916 enable pin is already configured as an output above; the
    // helper only toggles it during a read.
    battery_adc_config_t vbat_cfg = {
        .unit = ADC_UNIT_1,
        .channel = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = VBAT_ADC_ENABLE_PIN,
        .settle_ms = 10,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = VBAT_CAL_SCALE,
    };
    esp_err_t ret = battery_adc_create(&vbat_cfg, &vbat_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing EE02 for sleep");

    epaper_enter_deepsleep();

    // Drive TPS22916 enable LOW and latch the pad so the load switch stays
    // disabled once the digital IO domain powers down. Without the hold the
    // pin can float, re-enable the switch, and continuously drain the
    // 100k+100k battery divider.
    gpio_set_direction(VBAT_ADC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);
    gpio_hold_en(VBAT_ADC_ENABLE_PIN);
    gpio_deep_sleep_hold_en();

    // Disable ADC to save power
    if (vbat_adc) {
        battery_adc_destroy(vbat_adc);
        vbat_adc = NULL;
    }

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return board_hal_get_battery_voltage() > 2500;
}

// Lowest believable resting voltage for a running pack. The battery rail sags
// hard under WiFi TX bursts (the read happens mid-pull), and a single
// battery_adc_read_mv() can land entirely inside such a burst → a bogus
// sub-3.3 V "collapse". Reads at or above this are taken at face value.
#define VBAT_MIN_PLAUSIBLE_MV 3300

// Cache the last good read so battery percent and voltage (read by two separate
// top-level calls during the same pull) are derived from ONE acquisition and
// can't disagree (the old split caused "0 % @ 4090 mV" reports to the server).
// Plain (non-RTC) static: only needs to survive within one awake period, not
// across deep sleep.
static int s_vbat_cached_mv = -1;
static int64_t s_vbat_cached_us = 0;
#define VBAT_CACHE_TTL_US (3 * 1000 * 1000)  // 3 s

// Read the pack voltage, rejecting transient collapses. The rail only ever sags
// DOWN under load, so we sample a few times and keep the plausible reads: up to
// 3 reads >= VBAT_MIN_PLAUSIBLE_MV are averaged; if none qualify (genuinely low
// pack) we return the highest read seen, which is closest to the true resting
// voltage. Returns mV, or -1 if every acquisition failed.
static int read_vbat_mv_robust(void)
{
    int best = -1;
    long sum = 0;
    int n = 0;
    for (int i = 0; i < 5 && n < 3; i++) {
        // The helper toggles the TPS22916 load switch, averages several samples
        // and applies eFuse calibration + the divider; mV or -1 on failure.
        int mv = battery_adc_read_mv(vbat_adc);
        if (mv <= 0) {
            continue;
        }
        if (mv > best) {
            best = mv;
        }
        if (mv >= VBAT_MIN_PLAUSIBLE_MV) {
            sum += mv;
            n++;
        }
    }
    if (n > 0) {
        return (int) (sum / n);
    }
    return best;
}

// A single-wake drop bigger than this (mV) vs. the last CONFIRMED reading is
// treated as a suspect WiFi-TX rail sag rather than a real level change: in
// the field, real sags land at 3.3-3.4 V (inside VBAT_MIN_PLAUSIBLE_MV's own
// "plausible" band above, so read_vbat_mv_robust's own filter doesn't catch
// them), then bounce back to normal the very next wake. A healthy pack can't
// move this much between 5-60 min wakes under normal drain; a real fast
// drain confirms itself within a few more wakes instead (see below), so this
// only ever delays reporting a genuine drop, never hides it.
#define VBAT_SUSPECT_DROP_MV 300
// Two candidates within this much of each other count as "the same" reading
// for confirmation purposes (ADC/sampling noise, not a second sag).
#define VBAT_CONFIRM_TOLERANCE_MV 150
// How many consecutive wakes must agree (within tolerance) before a suspect
// drop is trusted — i.e. this many total sightings of roughly the same value,
// the first of which is what flags it as suspect. Replayed against a night of
// real 5-min-interval field data (all 3 EE02 frames, ~130 wakes each, ~22% of
// wakes showing a suspect drop): requiring 2 sightings left 4-9 false reports
// per frame still getting through; requiring 3 cut that to 1-2. The cost is
// only reporting latency, not battery life (no extra reads/wakes) — 2 extra
// wake cycles' worth (e.g. ~30 min at a 15-min rotate interval) is a good
// trade for cutting the false-positive rate under 1%.
#define VBAT_REQUIRED_AGREEMENTS 3

// Persisted across deep sleep (RTC memory) so a run of agreeing suspect
// readings can be confirmed over several NEXT wakes rather than re-triggering
// the same false alarm every time with no memory of it (deep sleep wipes
// ordinary RAM, so plain statics would reset every wake and never accumulate
// a streak).
RTC_DATA_ATTR static int s_vbat_last_confirmed_mv = -1;
RTC_DATA_ATTR static int s_vbat_pending_mv = -1;
RTC_DATA_ATTR static int s_vbat_pending_streak = 0;

// Requires a suspiciously large single-wake drop to repeat (within tolerance)
// for VBAT_REQUIRED_AGREEMENTS consecutive wakes before trusting it — a
// transient rail sag won't repeat that consistently, a real depleting battery
// will. A rise is always trusted immediately: the rail only ever sags DOWN
// under load, so a jump up can only be a real recharge, never sag noise, and
// delaying it would hide genuine charge-detection signal for no reason.
// Returns the value that should actually be reported this wake (may be the
// previous confirmed value, if this wake's reading is still unconfirmed).
static int confirm_vbat_reading(int raw_mv)
{
    if (raw_mv <= 0) {
        // Acquisition failed entirely; report the last known-good value
        // rather than nothing.
        return s_vbat_last_confirmed_mv;
    }
    if (s_vbat_last_confirmed_mv <= 0) {
        // First reading ever (fresh boot, nothing to compare against yet).
        s_vbat_last_confirmed_mv = raw_mv;
        s_vbat_pending_mv = -1;
        s_vbat_pending_streak = 0;
        return raw_mv;
    }

    int drop = s_vbat_last_confirmed_mv - raw_mv;
    if (drop < VBAT_SUSPECT_DROP_MV) {
        // Steady, rising, or a normal gradual decline — trust immediately.
        s_vbat_last_confirmed_mv = raw_mv;
        s_vbat_pending_mv = -1;
        s_vbat_pending_streak = 0;
        return raw_mv;
    }

    int pending_diff = s_vbat_pending_mv > 0 ? raw_mv - s_vbat_pending_mv : 0;
    if (pending_diff < 0)
        pending_diff = -pending_diff;
    if (s_vbat_pending_mv > 0 && pending_diff < VBAT_CONFIRM_TOLERANCE_MV) {
        // This wake agrees with the run so far.
        s_vbat_pending_streak++;
    } else {
        // A new (or first) suspect value — restart the streak.
        s_vbat_pending_mv = raw_mv;
        s_vbat_pending_streak = 1;
    }

    if (s_vbat_pending_streak >= VBAT_REQUIRED_AGREEMENTS) {
        // Seen consistently enough times in a row — confirmed.
        s_vbat_last_confirmed_mv = raw_mv;
        s_vbat_pending_mv = -1;
        s_vbat_pending_streak = 0;
        return raw_mv;
    }

    // Still unconfirmed: keep reporting the last confirmed value.
    return s_vbat_last_confirmed_mv;
}

int board_hal_get_battery_voltage(void)
{
    int64_t now = esp_timer_get_time();
    if (s_vbat_cached_mv > 0 && (now - s_vbat_cached_us) < VBAT_CACHE_TTL_US) {
        return s_vbat_cached_mv;
    }
    int mv = confirm_vbat_reading(read_vbat_mv_robust());
    if (mv > 0) {
        s_vbat_cached_mv = mv;
        s_vbat_cached_us = now;
    }
    return mv;
}

int board_hal_get_battery_percent(void)
{
    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0)
        return -1;

    // Simple linear approximation for LiPo
    // 4.2V = 100%, 3.3V = 0%
    if (voltage >= 4200)
        return 100;
    if (voltage <= 3300)
        return 0;

    return (voltage - 3300) * 100 / (4200 - 3300);
}

bool board_hal_is_charging(void)
{
    // The real charge state is NOT detectable on this board (rev V1.0):
    // confirmed against the official Seeed EE02 schematic, the BQ24070 status
    // pins STAT1 (pin 2) / STAT2 (pin 3) drive only the on-board green LEDs
    // (D5/D16) and are not routed to any ESP32-S3 GPIO. The earlier
    // USB-Serial/JTAG + voltage heuristic produced false positives (see the
    // header comment), so we no longer guess. Revisit if a future board rev
    // breaks STAT or VBUS out to a GPIO.
    return false;
}

bool board_hal_supports_charge_status(void)
{
    // No reliable on-frame charge/USB-power signal exists on rev V1.0, so don't
    // report a (misleading) status. The server derives charge direction from the
    // battery-voltage trend instead.
    return false;
}

// Battery ADC is fixed at compile time on this board (see above) rather than
// user-selectable, so there's nothing to enumerate/reconfigure at runtime.
int board_hal_get_battery_adc_pin_options(const board_hal_battery_adc_pin_t **out_pins)
{
    (void) out_pins;
    return 0;
}

esp_err_t board_hal_set_battery_adc_pin(int gpio_num)
{
    (void) gpio_num;
    return ESP_ERR_NOT_SUPPORTED;
}

int board_hal_get_battery_adc_pin(void)
{
    return -1;
}

bool board_hal_is_usb_connected(void)
{
    // VBUS is not readable on rev V1.0; the only USB signal (usb_serial_jtag
    // link state) detects a data host, not charging power, and is unreliable —
    // so report nothing rather than a value that misleads the charge logic.
    return false;
}

void board_hal_shutdown(void)
{
    // BQ24070 doesn't have software shutdown command via I2C like AXP.
    // We can only enter deep sleep.
    ESP_LOGI(TAG, "Shutdown not supported on BQ24070, entering deep sleep instead");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    // No external RTC on XIAO EE02, rely on internal
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    // TODO: Could return internal time if desired, but main.c handles fallback.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool board_hal_rtc_is_available(void)
{
    return false;
}

esp_err_t board_hal_get_temperature(float *t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_get_humidity(float *h)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void board_hal_led_set(board_hal_led_t led, bool on)
{
    // No onboard LED on XIAO EE02
    (void) led;
    (void) on;
}
