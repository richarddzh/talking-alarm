#pragma once

#include <stdint.h>
#include <esp_err.h>

esp_err_t st7789_init(void);

// Displays a packed 4-bit indexed-color framebuffer. The high nibble is
// the left/even pixel and the low nibble is the right/odd pixel.
esp_err_t st7789_show_indexed4(const uint8_t *framebuffer,
                               int width, int height,
                               const uint16_t palette[16]);
