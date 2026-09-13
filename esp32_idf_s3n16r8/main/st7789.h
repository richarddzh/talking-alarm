#pragma once

#include <stdint.h>
#include <esp_err.h>

esp_err_t st7789_init(void);

// Displays a packed 4-bit indexed-color framebuffer. The high nibble is
// the left/even pixel and the low nibble is the right/odd pixel.
esp_err_t st7789_show_indexed4(const uint8_t *framebuffer,
                               int width, int height,
                               const uint16_t palette[16]);

esp_err_t st7789_show_indexed4_region(const uint8_t *framebuffer,
                                      int framebuffer_width,
                                      int x, int y, int width, int height,
                                      const uint16_t palette[16]);

// Draws a logical RGB565 bitmap, blending its 8-bit alpha channel over a
// solid logical RGB565 background before applying the panel color polarity.
esp_err_t st7789_draw_rgb565_bitmap(int x, int y, int width, int height,
                                    const uint16_t *pixels,
                                    const uint8_t *alpha,
                                    uint16_t background);
