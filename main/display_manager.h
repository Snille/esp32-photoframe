#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_manager_init(void);
esp_err_t display_manager_show_image(const char *filename);

esp_err_t display_manager_show_calibration(void);
esp_err_t display_manager_clear(void);
esp_err_t display_manager_show_setup_screen(void);
bool display_manager_is_busy(void);
void display_manager_rotate_from_storage(void);
const char *display_manager_get_current_image(void);
void display_manager_initialize_paint(void);

/**
 * @brief Display an RGB buffer directly on the e-paper display
 *
 * This function takes an already-processed RGB buffer (with colors matching
 * the 6-color palette) and displays it directly. This is more efficient for
 * SD-card-less systems where no file I/O is needed.
 *
 * @param rgb_buffer RGB888 buffer (3 bytes per pixel, already dithered to palette)
 * @param width Image width
 * @param height Image height
 * @return esp_err_t ESP_OK on success
 */
esp_err_t display_manager_show_rgb_buffer(const uint8_t *rgb_buffer, int width, int height);

/**
 * @brief Get the pre-allocated EPD image buffer and its size.
 *
 * Allows callers (e.g. splash_screen) to reuse the display manager's buffer
 * instead of allocating a second one — important on boards without PSRAM.
 */
uint8_t *display_manager_get_epd_buffer(uint32_t *out_size);

/**
 * @brief Display whatever is currently in the EPD paint buffer.
 * Used after streaming EPDGZ inflate to avoid re-reading the file.
 */
esp_err_t display_manager_refresh_from_paint_buffer(void);

/**
 * @brief Decompress and display an EPDGZ image from a memory buffer.
 * Decompresses directly into the Paint buffer — no extra 120KB allocation needed.
 */
esp_err_t display_manager_show_epdgz_buffer(const uint8_t *data, size_t len);

/**
 * @brief Temporarily free the EPD buffer to reclaim ~120KB SRAM during HTTPS downloads.
 * Must call display_manager_reclaim_epd_buffer() before any display operations.
 */
esp_err_t display_manager_release_epd_buffer(void);

/**
 * @brief Re-allocate the EPD buffer after a download. Reinitializes the paint context.
 */
esp_err_t display_manager_reclaim_epd_buffer(void);

#endif
