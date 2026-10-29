#pragma once
#include <stdint.h>
#define FONT_W 8
#define FONT_H 16
#define FONT_FIRST 0x20
#define FONT_NGLYPH 224                          /* U+0020 .. U+00FF */
extern const uint8_t font8x16_data[FONT_NGLYPH][FONT_H];
