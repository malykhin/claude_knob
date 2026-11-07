// Adafruit GFX font format (BSD licence, from Adafruit-GFX-Library). The font files next
// to this one are GNU FreeFont converted by Adafruit (GPLv3 with the font exception).
#pragma once
#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

typedef struct {
    uint16_t bitmapOffset; // into GFXfont->bitmap
    uint8_t width, height; // bitmap size in pixels
    uint8_t xAdvance;      // cursor advance
    int8_t xOffset, yOffset; // from the cursor (on the baseline) to the bitmap's top-left
} GFXglyph;

typedef struct {
    uint8_t *bitmap;  // glyph bitmaps, concatenated, 1 bit per pixel, row-major
    GFXglyph *glyph;
    uint16_t first, last;
    uint8_t yAdvance; // line height
} GFXfont;
