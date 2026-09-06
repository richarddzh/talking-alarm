#pragma once

#include <stdint.h>

#define FONT_ZH16_BYTES_PER_GLYPH 32

const uint8_t *font_zh16_bitmap(uint32_t codepoint);
