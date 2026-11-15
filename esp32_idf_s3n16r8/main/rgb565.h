#pragma once

#include <stdint.h>

static inline uint16_t rgb565_blend(uint16_t foreground, uint16_t background,
                                    uint8_t alpha) {
    if (alpha == 0) return background;
    if (alpha == UINT8_MAX) return foreground;
    uint32_t inverse = UINT8_MAX - alpha;
    uint32_t red = (((foreground >> 11) & 0x1F) * alpha +
                    ((background >> 11) & 0x1F) * inverse + 127) / 255;
    uint32_t green = (((foreground >> 5) & 0x3F) * alpha +
                      ((background >> 5) & 0x3F) * inverse + 127) / 255;
    uint32_t blue = ((foreground & 0x1F) * alpha +
                     (background & 0x1F) * inverse + 127) / 255;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}
