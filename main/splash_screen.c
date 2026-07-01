#include "splash_screen.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "GUI_Paint.h"
#include "board_hal.h"
#include "display_manager.h"
#include "epaper.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "qrcode.h"
#include "splash_data/splash_meta.h"
#include "utils.h"
#include "wifi_manager.h"

static const char *TAG = "splash_screen";

// Embedded EPDGZ screens (generated at build time for the target board)
extern const uint8_t splash_epdgz_start[] asm("_binary_splash_epdgz_start");
extern const uint8_t splash_epdgz_end[] asm("_binary_splash_epdgz_end");
extern const uint8_t setup_complete_epdgz_start[] asm("_binary_setup_complete_epdgz_start");
extern const uint8_t setup_complete_epdgz_end[] asm("_binary_setup_complete_epdgz_end");

// E-paper palette indices
#define EPD_BLACK 0
#define EPD_WHITE 1

/**
 * Set a pixel in the 4-bit-per-pixel e-paper buffer.
 * Two pixels per byte: high nibble = even pixel, low nibble = odd pixel.
 */
static void set_pixel(uint8_t *buffer, int width, int x, int y, uint8_t color)
{
    if (x < 0 || x >= width || y < 0 || y >= BOARD_HAL_DISPLAY_HEIGHT)
        return;

    int byte_idx = (y * width + x) / 2;
    if (x % 2 == 0) {
        buffer[byte_idx] = (buffer[byte_idx] & 0x0F) | (color << 4);
    } else {
        buffer[byte_idx] = (buffer[byte_idx] & 0xF0) | (color & 0x0F);
    }
}

/**
 * Draw a scaled ASCII string straight into the native e-paper buffer, mapping
 * every glyph pixel from the viewing (mounting) orientation into native panel
 * coords with the SAME rotation the QR + pre-rotated background use — so the
 * text lands aligned and upright without any GUI_Paint rotation/bounds
 * ambiguity. `deg` is SPLASH_ROTATE_DEG (the rotation generate_splash baked in).
 * (start_x, start_y) is the string's top-left in viewing coords.
 */
static void splash_draw_text_native(uint8_t *buffer, const char *text, const sFONT *font,
                                    int start_x, int start_y, int scale, int deg, uint8_t color)
{
    const int native_w = BOARD_HAL_DISPLAY_WIDTH;
    const int native_h = BOARD_HAL_DISPLAY_HEIGHT;
    const int bytes_per_row = font->Width / 8 + (font->Width % 8 ? 1 : 0);

    for (int i = 0; text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char) text[i];
        if (ch < ' ')
            continue;
        const uint8_t *glyph = &font->table[(ch - ' ') * font->Height * bytes_per_row];
        int char_x = start_x + i * font->Width * scale;

        for (int page = 0; page < font->Height; page++) {
            const uint8_t *row = glyph + page * bytes_per_row;
            for (int col = 0; col < font->Width; col++) {
                if (!(row[col / 8] & (0x80 >> (col % 8))))
                    continue;
                // Filled font pixel -> a scale x scale block in viewing space.
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int vx = char_x + col * scale + sx;
                        int vy = start_y + page * scale + sy;
                        int nx, ny;
                        if (deg == 90) {  // matches to_native(90) = PIL ROTATE_270
                            nx = native_w - 1 - vy;
                            ny = vx;
                        } else if (deg == 270) {  // matches to_native(270) = PIL ROTATE_90
                            nx = vy;
                            ny = native_h - 1 - vx;
                        } else {  // deg == 0: viewing == native
                            nx = vx;
                            ny = vy;
                        }
                        set_pixel(buffer, native_w, nx, ny, color);
                    }
                }
            }
        }
    }
}

// Context passed to the QR code display callback
typedef struct {
    uint8_t *buffer;
    int buf_width;
    int pos_x;
    int pos_y;
    int target_size;
} qr_draw_ctx_t;

static qr_draw_ctx_t s_qr_draw_ctx;

/**
 * Check if a pixel is inside a rounded rectangle.
 * (x, y) relative to rect origin, (w, h) rect dimensions, r corner radius.
 */
static bool in_rounded_rect(int x, int y, int w, int h, int r)
{
    if (x < r && y < r) {
        int dx = r - x, dy = r - y;
        return dx * dx + dy * dy <= r * r;
    }
    if (x >= w - r && y < r) {
        int dx = x - (w - r - 1), dy = r - y;
        return dx * dx + dy * dy <= r * r;
    }
    if (x < r && y >= h - r) {
        int dx = r - x, dy = y - (h - r - 1);
        return dx * dx + dy * dy <= r * r;
    }
    if (x >= w - r && y >= h - r) {
        int dx = x - (w - r - 1), dy = y - (h - r - 1);
        return dx * dx + dy * dy <= r * r;
    }
    return true;
}

/**
 * QR code display callback — draws the QR code onto the e-paper buffer.
 */
static void qr_draw_callback(esp_qrcode_handle_t qrcode)
{
    qr_draw_ctx_t *ctx = &s_qr_draw_ctx;
    int qr_size = esp_qrcode_get_size(qrcode);

    // Fill the exact SVG placeholder area white (target_size + 4px border, rounded)
    // This matches the SVG's <rect rx="4" width="target_size+8" height="target_size+8"/>
    int pad = 4;
    int rect_w = ctx->target_size + pad * 2;
    int rect_h = ctx->target_size + pad * 2;
    for (int y = 0; y < rect_h; y++) {
        for (int x = 0; x < rect_w; x++) {
            if (in_rounded_rect(x, y, rect_w, rect_h, pad)) {
                set_pixel(ctx->buffer, ctx->buf_width, ctx->pos_x - pad + x, ctx->pos_y - pad + y,
                          EPD_WHITE);
            }
        }
    }

    // Use fractional module size so the QR content fills target_size exactly,
    // matching how the SVG app QR is rendered with fractional scaling.
    float module_f = (float) ctx->target_size / qr_size;

    for (int qy = 0; qy < qr_size; qy++) {
        int py0 = ctx->pos_y + (int) (qy * module_f);
        int py1 = ctx->pos_y + (int) ((qy + 1) * module_f);
        for (int qx = 0; qx < qr_size; qx++) {
            if (!esp_qrcode_get_module(qrcode, qx, qy))
                continue;

            int px0 = ctx->pos_x + (int) (qx * module_f);
            int px1 = ctx->pos_x + (int) ((qx + 1) * module_f);
            for (int dy = py0; dy < py1; dy++) {
                for (int dx = px0; dx < px1; dx++) {
                    set_pixel(ctx->buffer, ctx->buf_width, dx, dy, EPD_BLACK);
                }
            }
        }
    }
}

/**
 * Decompress gzipped data from memory buffer.
 * Returns ESP_OK on success.
 */
static esp_err_t decompress_gzip(const uint8_t *compressed, size_t compressed_size, uint8_t *output,
                                 size_t output_size)
{
    z_stream strm = {0};
    strm.avail_in = compressed_size;
    strm.next_in = (Bytef *) compressed;
    strm.avail_out = output_size;
    strm.next_out = output;

    // 16 + MAX_WBITS enables gzip decoding
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        return ESP_FAIL;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret != Z_STREAM_END && ret != Z_OK) {
        ESP_LOGE(TAG, "Decompression failed: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Decompressed %d -> %d bytes", (int) compressed_size,
             (int) (output_size - strm.avail_out));
    return ESP_OK;
}

esp_err_t splash_screen_display(void)
{
    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    int buf_size = ((width + 1) / 2) * height;

    ESP_LOGI(TAG, "Loading splash screen (%dx%d)", width, height);

    // Reuse display_manager's pre-allocated buffer to avoid a second large allocation
    // (important on boards without PSRAM where heap is tight)
    uint32_t dm_buf_size = 0;
    uint8_t *epd_buffer = display_manager_get_epd_buffer(&dm_buf_size);
    bool owns_buffer = false;
    if (!epd_buffer || dm_buf_size < (uint32_t) buf_size) {
        // Fallback: allocate own buffer (try PSRAM then SRAM)
        epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!epd_buffer) {
            epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_8BIT);
        }
        if (!epd_buffer) {
            ESP_LOGE(TAG, "Failed to allocate display buffer");
            return ESP_ERR_NO_MEM;
        }
        owns_buffer = true;
    }

    // Fill with white initially
    memset(epd_buffer, 0x11, buf_size);  // 0x11 = white|white (index 1)

    // Decompress the embedded EPDGZ into the buffer
    size_t epdgz_size = splash_epdgz_end - splash_epdgz_start;
    ESP_LOGI(TAG, "Decompressing EPDGZ (%d bytes)", (int) epdgz_size);

    esp_err_t ret = decompress_gzip(splash_epdgz_start, epdgz_size, epd_buffer, buf_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decompress splash EPDGZ");
        if (owns_buffer)
            heap_caps_free(epd_buffer);
        return ret;
    }

    // Generate WiFi QR code for the device's unique hotspot SSID
    const char *ap_ssid = get_setup_ap_ssid();
    char wifi_qr_data[128];
    snprintf(wifi_qr_data, sizeof(wifi_qr_data), "WIFI:T:nopass;S:%s;;", ap_ssid);

    ESP_LOGI(TAG, "Generating WiFi QR code for: %s", ap_ssid);
    ESP_LOGI(TAG, "Drawing QR at (%d,%d) target %dpx", SPLASH_WIFI_QR_X, SPLASH_WIFI_QR_Y,
             SPLASH_WIFI_QR_SIZE);

    // Set up draw context for the callback
    s_qr_draw_ctx = (qr_draw_ctx_t){
        .buffer = epd_buffer,
        .buf_width = width,
        .pos_x = SPLASH_WIFI_QR_X,
        .pos_y = SPLASH_WIFI_QR_Y,
        .target_size = SPLASH_WIFI_QR_SIZE,
    };

    esp_qrcode_config_t qr_cfg = {
        .display_func = qr_draw_callback,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    ret = esp_qrcode_generate(&qr_cfg, wifi_qr_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate WiFi QR code");
        if (owns_buffer)
            heap_caps_free(epd_buffer);
        return ret;
    }

    // Display on e-paper
    ESP_LOGI(TAG, "Displaying splash screen");
    epaper_display(epd_buffer);

    if (owns_buffer)
        heap_caps_free(epd_buffer);
    ESP_LOGI(TAG, "Splash screen displayed successfully");
    return ESP_OK;
}

esp_err_t splash_screen_display_setup_complete(const char *hostname)
{
    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    int buf_size = ((width + 1) / 2) * height;

    ESP_LOGI(TAG, "Showing setup complete screen (%dx%d)", width, height);

    // Reuse display_manager's pre-allocated buffer (avoids second large allocation on SRAM-only
    // boards)
    uint32_t dm_buf_size = 0;
    uint8_t *epd_buffer = display_manager_get_epd_buffer(&dm_buf_size);
    bool owns_buffer = false;
    if (!epd_buffer || dm_buf_size < (uint32_t) buf_size) {
        epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!epd_buffer) {
            epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_8BIT);
        }
        if (!epd_buffer) {
            ESP_LOGE(TAG, "Failed to allocate display buffer");
            return ESP_ERR_NO_MEM;
        }
        owns_buffer = true;
    }

    memset(epd_buffer, 0x11, buf_size);

    // Decompress the setup-complete EPDGZ
    size_t epdgz_size = setup_complete_epdgz_end - setup_complete_epdgz_start;
    ESP_LOGI(TAG, "Decompressing setup_complete EPDGZ (%d bytes)", (int) epdgz_size);

    esp_err_t ret = decompress_gzip(setup_complete_epdgz_start, epdgz_size, epd_buffer, buf_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decompress setup_complete EPDGZ");
        if (owns_buffer)
            heap_caps_free(epd_buffer);
        return ret;
    }

    // Generate QR code for the device's web interface URL
    char url[128];
    snprintf(url, sizeof(url), "http://%s.local", hostname);

    ESP_LOGI(TAG, "Generating web UI QR code for: %s", url);

    s_qr_draw_ctx = (qr_draw_ctx_t){
        .buffer = epd_buffer,
        .buf_width = width,
        .pos_x = SETUP_COMPLETE_QR_X,
        .pos_y = SETUP_COMPLETE_QR_Y,
        .target_size = SETUP_COMPLETE_QR_SIZE,
    };

    esp_qrcode_config_t qr_cfg = {
        .display_func = qr_draw_callback,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    ret = esp_qrcode_generate(&qr_cfg, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate web UI QR code");
        if (owns_buffer)
            heap_caps_free(epd_buffer);
        return ret;
    }

    // Draw the device's IP under the QR code. The IP is only known at runtime so
    // it is NOT baked into the splash art — we render it live. SETUP_COMPLETE_IP_*
    // are viewing/logical coords; splash_draw_text_native maps every pixel into
    // the native buffer with SPLASH_ROTATE_DEG (the same rotation the QR +
    // background use), and white (EPD_WHITE) reads on the black background.
    char ip[20] = "";
    wifi_manager_get_ip(ip, sizeof(ip));
    if (ip[0] != '\0' && strchr(ip, '.') != NULL && SETUP_COMPLETE_IP_HEIGHT > 0) {
        char ipline[28];
        snprintf(ipline, sizeof(ipline), "IP: %s", ip);
        // Floor to the largest whole Font24 scale that fits the target height, so
        // the line reads at roughly the subtitle size (never rounding up larger).
        int scale = SETUP_COMPLETE_IP_HEIGHT / Font24.Height;
        if (scale < 1)
            scale = 1;
        int char_w = (int) strlen(ipline) * Font24.Width;
        // Shrink to fit the reserved column width so a long IP never overflows.
        while (scale > 1 && char_w * scale > SETUP_COMPLETE_IP_MAXW)
            scale--;
        int start_x = SETUP_COMPLETE_IP_CX - (char_w * scale) / 2;
        int start_y = SETUP_COMPLETE_IP_BOTTOM - Font24.Height * scale;
        splash_draw_text_native(epd_buffer, ipline, &Font24, start_x, start_y, scale,
                                SPLASH_ROTATE_DEG % 360, EPD_WHITE);
        ESP_LOGI(TAG, "Drew '%s' at viewing (%d,%d) scale %d rot %d", ipline, start_x, start_y,
                 scale, SPLASH_ROTATE_DEG);
    }

    ESP_LOGI(TAG, "Displaying setup complete screen");
    epaper_display(epd_buffer);

    if (owns_buffer)
        heap_caps_free(epd_buffer);
    ESP_LOGI(TAG, "Setup complete screen displayed successfully");
    return ESP_OK;
}
