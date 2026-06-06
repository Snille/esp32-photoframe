#include <assert.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "epaper_ws4in_e6";

static epaper_config_t g_cfg;
static spi_device_handle_t spi;

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t pm_lock = NULL;
#endif

// Native panel geometry is PORTRAIT 400x600 (the "600x400" on the silkscreen is
// the landscape viewing orientation). The controller addresses it as 400 wide x
// 600 tall, exactly like Waveshare's official EPD_4in0e driver. The server
// rotates landscape photos into this native layout before sending.
#define EPD_WIDTH 400
#define EPD_HEIGHT 600
// 2 pixels per byte (4-bit color depth, same as all Spectra 6 displays)
#define EPD_BUF_SIZE (EPD_WIDTH / 2 * EPD_HEIGHT)

#define SPI_MAX_CHUNK 4092
#define DATA_CHUNK_SIZE 128

// --- Low-level SPI helpers ---

static void spi_begin(void)
{
    esp_err_t ret = spi_device_acquire_bus(spi, portMAX_DELAY);
    assert(ret == ESP_OK);
}

static void spi_end(void)
{
    spi_device_release_bus(spi);
}

static void spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {};
    t.rxlength = 0;
    while (len > 0) {
        size_t chunk = (len > SPI_MAX_CHUNK) ? SPI_MAX_CHUNK : len;
        t.length = chunk * 8;
        t.tx_buffer = data;
        esp_err_t ret = spi_device_polling_start(spi, &t, portMAX_DELAY);
        if (ret == ESP_OK) {
            ret = spi_device_polling_end(spi, portMAX_DELAY);
        }
        assert(ret == ESP_OK);
        data += chunk;
        len -= chunk;
    }
}

// Send a command with optional data bytes. CS stays LOW for entire sequence.
static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(g_cfg.pin_dc, 0);  // DC low = command
    spi_begin();
    gpio_set_level(g_cfg.pin_cs, 0);

    spi_transaction_ext_t cmd_t = {
        .command_bits = 8,
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD,
            .cmd = cmd,
        },
    };
    esp_err_t ret = spi_device_polling_start(spi, &cmd_t.base, portMAX_DELAY);
    if (ret == ESP_OK) {
        spi_device_polling_end(spi, portMAX_DELAY);
    }
    assert(ret == ESP_OK);

    if (len > 0) {
        gpio_set_level(g_cfg.pin_dc, 1);  // DC high = data
        uint8_t buf[16];
        assert(len <= sizeof(buf));
        memcpy(buf, data, len);
        spi_write(buf, len);
    }

    gpio_set_level(g_cfg.pin_cs, 1);
    spi_end();
}

static void send_command(uint8_t cmd)
{
    cmd_data(cmd, NULL, 0);
}

static void send_buffer(uint8_t *data, int len)
{
    uint8_t buf[DATA_CHUNK_SIZE];
    uint8_t *ptr = data;
    int remaining = len;

    ESP_LOGI(TAG, "Sending %d bytes in %d-byte chunks", len, DATA_CHUNK_SIZE);

    while (remaining > 0) {
        int chunk = (remaining > DATA_CHUNK_SIZE) ? DATA_CHUNK_SIZE : remaining;
        memcpy(buf, ptr, chunk);

        gpio_set_level(g_cfg.pin_dc, 1);
        spi_begin();
        gpio_set_level(g_cfg.pin_cs, 0);
        spi_write(buf, chunk);
        gpio_set_level(g_cfg.pin_cs, 1);
        spi_end();

        ptr += chunk;
        remaining -= chunk;
    }

    ESP_LOGI(TAG, "Buffer send complete");
}

static bool is_busy(void)
{
    return gpio_get_level(g_cfg.pin_busy) == 0;
}

// Wait for the controller to assert BUSY (active low) and then release it.
//
// If BUSY never asserts within ~1s the line is most likely not wired (it idles
// high through the internal pull-up). In that case we cannot observe when the
// panel finishes, so we fall back to a fixed worst-case delay. This guarantees
// the panel gets enough time to complete the operation before the caller issues
// POWER_OFF / DEEP_SLEEP — otherwise a refresh is aborted mid-update and the
// panel stays blank/white.
static void wait_busy(const char *label, uint32_t fallback_ms)
{
    // Wait up to ~1s for the panel to pull BUSY low (operation started).
    int assert_wait = 0;
    while (!is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (++assert_wait > 200) {  // 1s
            ESP_LOGW(TAG, "[%s] BUSY never asserted (line not wired?); waiting %lu ms",
                     label, (unsigned long) fallback_ms);
            vTaskDelay(pdMS_TO_TICKS(fallback_ms));
            return;
        }
    }

    // BUSY asserted — wait for it to release (operation finished).
    // Poll coarsely (50 ms): a Spectra-6 refresh takes ~19 s, so 50 ms of extra
    // detection latency is irrelevant, while the longer vTaskDelay lets tickless
    // idle (CONFIG_PM_ENABLE + FREERTOS_USE_TICKLESS_IDLE) keep the CPU in light
    // sleep between polls instead of waking every 10 ms — meaningful battery
    // savings across the whole refresh window.
    int wait_count = 0;
    while (is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (++wait_count > 800) {  // 40s timeout
            ESP_LOGW(TAG, "[%s] BUSY timeout after 40s", label);
            return;
        }
    }
    ESP_LOGI(TAG, "[%s] complete after %d ms", label, wait_count * 50);
}

// --- Hardware setup ---

static void gpio_init(void)
{
    gpio_hold_dis(g_cfg.pin_cs);
    gpio_hold_dis(g_cfg.pin_dc);
    gpio_hold_dis(g_cfg.pin_rst);

    gpio_set_level(g_cfg.pin_cs, 1);
    gpio_set_level(g_cfg.pin_dc, 0);
    gpio_set_level(g_cfg.pin_rst, 1);

    gpio_config_t out_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << g_cfg.pin_rst) | (1ULL << g_cfg.pin_dc) | (1ULL << g_cfg.pin_cs),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&out_conf));

    gpio_config_t in_conf = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << g_cfg.pin_busy),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&in_conf));
}

static void spi_add_device(void)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,  // 4 MHz — safer for jumper wires
        .mode = 0,
        .spics_io_num = -1,  // CS is manually controlled
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(g_cfg.spi_host, &devcfg, &spi));
}

static void hw_reset(void)
{
    gpio_set_level(g_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(g_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(g_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

// --- Spectra 6 initialization sequence for the 4inch (E) panel ---
// Matches Waveshare's official EPD_4in0e reference (native 400x600 portrait).
static void send_init_sequence(void)
{
    cmd_data(0xAA, (uint8_t[]){0x49, 0x55, 0x20, 0x08, 0x09, 0x18}, 6);  // CMDH
    cmd_data(0x01, (uint8_t[]){0x3F}, 1);                                 // PWRR
    cmd_data(0x00, (uint8_t[]){0x5F, 0x69}, 2);                           // PSR
    cmd_data(0x05, (uint8_t[]){0x40, 0x1F, 0x1F, 0x2C}, 4);               // BTST1
    cmd_data(0x08, (uint8_t[]){0x6F, 0x1F, 0x1F, 0x22}, 4);               // BTST3
    cmd_data(0x06, (uint8_t[]){0x6F, 0x1F, 0x17, 0x17}, 4);               // BTST2
    cmd_data(0x03, (uint8_t[]){0x00, 0x54, 0x00, 0x44}, 4);               // POFS
    cmd_data(0x60, (uint8_t[]){0x02, 0x00}, 2);                            // TCON
    cmd_data(0x30, (uint8_t[]){0x08}, 1);                                  // PLL
    cmd_data(0x50, (uint8_t[]){0x3F}, 1);                                  // CDI
    // TRES: native resolution 400x600 (0x0190=400 width, 0x0258=600 height)
    cmd_data(0x61, (uint8_t[]){0x01, 0x90, 0x02, 0x58}, 4);               // TRES
    cmd_data(0xE3, (uint8_t[]){0x2F}, 1);                                  // PWS
    cmd_data(0x84, (uint8_t[]){0x01}, 1);                                  // T_VDCS
}

static void display_update_cycle(uint8_t *image)
{
#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    hw_reset();

    ESP_LOGI(TAG, "init sequence...");
    send_init_sequence();

    ESP_LOGI(TAG, "sending %d bytes...", EPD_BUF_SIZE);
    send_command(0x10);  // DATA_START_TRANSMISSION
    send_buffer(image, EPD_BUF_SIZE);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "power on...");
    send_command(0x04);  // POWER_ON
    wait_busy("power_on", 1000);  // power-up is quick; 1s fallback if BUSY unwired
    vTaskDelay(pdMS_TO_TICKS(200));

    // Re-send BTST2 booster settings before refresh (required by controller).
    // Matches Waveshare EPD_4in0e TurnOnDisplay (last byte 0x27).
    cmd_data(0x06, (uint8_t[]){0x6F, 0x1F, 0x17, 0x27}, 4);  // BTST2
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "display refresh...");
    cmd_data(0x12, (uint8_t[]){0x00}, 1);  // DISPLAY_REFRESH
    // Full Spectra-6 refresh takes ~19-35s; use a generous fallback if BUSY is
    // not observable so POWER_OFF below never interrupts the update.
    wait_busy("display_refresh", 35000);

    ESP_LOGI(TAG, "power off...");
    cmd_data(0x02, (uint8_t[]){0x00}, 1);  // POWER_OFF
    vTaskDelay(pdMS_TO_TICKS(200));

    cmd_data(0x07, (uint8_t[]){0xA5}, 1);  // DEEP_SLEEP
    ESP_LOGI(TAG, "done");

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
    }
#endif
}

// --- Public API ---

uint16_t epaper_get_width(void)
{
    return EPD_WIDTH;
}

uint16_t epaper_get_height(void)
{
    return EPD_HEIGHT;
}

void epaper_init(const epaper_config_t *cfg)
{
    g_cfg = *cfg;
    ESP_LOGI(TAG, "Initializing Waveshare 4inch E-Paper HAT+ E (Spectra 6, 600x400)");

    spi_add_device();
    gpio_init();

#ifdef CONFIG_PM_ENABLE
    esp_err_t ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd_update", &pm_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PM lock: %s", esp_err_to_name(ret));
    }
#endif
}

void epaper_clear(uint8_t *image, uint8_t color)
{
    uint8_t packed = (color << 4) | color;
    memset(image, packed, EPD_BUF_SIZE);
    ESP_LOGI(TAG, "Clearing display with color 0x%02x", color);
    display_update_cycle(image);
    ESP_LOGI(TAG, "Clear complete");
}

void epaper_display(uint8_t *image)
{
    ESP_LOGI(TAG, "Starting display update: %d bytes", EPD_BUF_SIZE);
    display_update_cycle(image);
    ESP_LOGI(TAG, "Display update complete");
}

void epaper_enter_deepsleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep");

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    cmd_data(0x02, (uint8_t[]){0x00}, 1);  // POWER_OFF
    wait_busy("deepsleep_power_off", 1000);
    cmd_data(0x07, (uint8_t[]){0xA5}, 1);  // DEEP_SLEEP

    // Drive panel-facing GPIOs LOW before system sleep to avoid back-feeding
    gpio_set_level(g_cfg.pin_cs, 0);
    gpio_set_level(g_cfg.pin_dc, 0);
    gpio_set_level(g_cfg.pin_rst, 0);

    gpio_hold_en(g_cfg.pin_cs);
    gpio_hold_en(g_cfg.pin_dc);
    gpio_hold_en(g_cfg.pin_rst);

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
    }
#endif
}
