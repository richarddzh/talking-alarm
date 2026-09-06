#pragma once
#include <stdint.h>
// 5x7 ASCII font, codepoints 0x20..0x7E.
// Each glyph is 5 vertical bytes; bit 0 = top, bit 6 = bottom row.
extern const uint8_t font5x7[(0x7E - 0x20 + 1) * 5];
