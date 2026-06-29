// filename: GUI_EPDGZfile.c
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdlib.h>
#include <zlib.h>

#include "GUI_Paint.h"

static const char *TAG = "GUI_EPDGZfile";

/**
 * @brief Read EPDGZ file and display it on the e-paper display
 *
 * Reads a gzip-compressed 4-bit-per-pixel raw e-paper image file,
 * decompresses it, and paints directly to the display buffer using
 * Paint_SetPixel.
 *
 * @param path Path to the EPDGZ file
 * @return 0 on success, non-zero on error
 */
int GUI_ReadEPDGZ(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open EPDGZ file: %s", path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long compressed_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Try SPIRAM first, fall back to internal SRAM
    uint8_t *compressed_data = heap_caps_malloc(compressed_size, MALLOC_CAP_SPIRAM);
    if (!compressed_data) {
        compressed_data = heap_caps_malloc(compressed_size, MALLOC_CAP_8BIT);
    }
    if (!compressed_data) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for compressed data", compressed_size);
        fclose(fp);
        return 1;
    }
    fread(compressed_data, 1, compressed_size, fp);
    fclose(fp);

    int width = Paint.Width;
    int height = Paint.Height;
    uLongf uncompressed_size = (width * height + 1) / 2;

    // Decompress directly into the Paint buffer (already allocated, avoids extra 120KB alloc)
    uint8_t *out_buf = Paint.Image;
    if (!out_buf) {
        ESP_LOGE(TAG, "Paint buffer not initialized");
        heap_caps_free(compressed_data);
        return 1;
    }

    z_stream strm = {0};
    strm.avail_in = compressed_size;
    strm.next_in = compressed_data;
    strm.avail_out = uncompressed_size;
    strm.next_out = out_buf;

    // 16 + MAX_WBITS enables gzip decoding
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        heap_caps_free(compressed_data);
        return 1;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    heap_caps_free(compressed_data);

    if (ret != Z_STREAM_END && ret != Z_OK) {
        ESP_LOGE(TAG, "Decompression failed: %d", ret);
        return 1;
    }

    ESP_LOGI(TAG, "EPDGZ: %dx%d, compressed %ld -> %lu bytes", width, height, compressed_size,
             uncompressed_size);
    return 0;
}

int GUI_ReadEPDGZBuffer(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        ESP_LOGE(TAG, "Invalid buffer");
        return 1;
    }

    int width = Paint.Width;
    int height = Paint.Height;
    uLongf uncompressed_size = (width * height + 1) / 2;

    uint8_t *out_buf = Paint.Image;
    if (!out_buf) {
        ESP_LOGE(TAG, "Paint buffer not initialized");
        return 1;
    }

    z_stream strm = {0};
    strm.avail_in = len;
    strm.next_in = (Bytef *) data;
    strm.avail_out = uncompressed_size;
    strm.next_out = out_buf;

    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        return 1;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret != Z_STREAM_END && ret != Z_OK) {
        ESP_LOGE(TAG, "Decompression failed: %d", ret);
        return 1;
    }

    ESP_LOGI(TAG, "EPDGZ buffer: %dx%d, %zu -> %lu bytes", width, height, len, uncompressed_size);
    return 0;
}
