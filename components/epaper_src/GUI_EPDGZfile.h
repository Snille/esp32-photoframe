// filename: GUI_EPDGZfile.h
#ifndef __GUI_EPDGZFILE_H
#define __GUI_EPDGZFILE_H

#include "GUI_Paint.h"

/**
 * @brief Read EPDGZ file and display it on the e-paper display
 *
 * Reads a gzip-compressed 4-bit-per-pixel raw e-paper image file.
 * Each byte contains two pixels (high nibble = first pixel, low nibble = second).
 * Color indices: 0=Black, 1=White, 2=Yellow, 3=Red, 5=Blue, 6=Green.
 *
 * @param path Path to the EPDGZ file
 * @return 0 on success, non-zero on error
 */
int GUI_ReadEPDGZ(const char *path);

/**
 * @brief Decompress EPDGZ from a memory buffer directly into the Paint buffer.
 *
 * Same as GUI_ReadEPDGZ but takes already-loaded compressed data — avoids
 * file I/O and the extra 120 KB uncompressed alloc on SRAM-only boards.
 *
 * @param data    Pointer to gzip-compressed EPDGZ bytes
 * @param len     Length of compressed data in bytes
 * @return 0 on success, non-zero on error
 */
int GUI_ReadEPDGZBuffer(const uint8_t *data, size_t len);

#endif
