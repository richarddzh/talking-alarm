/*
 * GNU Unifont 16.0.04 BMP glyph access.
 * The embedded font is dual-licensed under SIL OFL 1.1 or
 * GPL-2.0-or-later with the GNU Font Embedding Exception.
 */
#include "font_zh16.h"

#include <stddef.h>

extern const uint8_t s_unifont_start[]
    asm("_binary_unifont_bmp16_bin_start");
extern const uint8_t s_unifont_end[]
    asm("_binary_unifont_bmp16_bin_end");

const uint8_t *font_zh16_bitmap(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return NULL;
    size_t offset = (size_t)codepoint * FONT_ZH16_BYTES_PER_GLYPH;
    if (s_unifont_start + offset + FONT_ZH16_BYTES_PER_GLYPH >
        s_unifont_end) {
        return NULL;
    }
    const uint8_t *bitmap = s_unifont_start + offset;
    for (size_t i = 0; i < FONT_ZH16_BYTES_PER_GLYPH; ++i) {
        if (bitmap[i] != 0) return bitmap;
    }
    return NULL;
}
