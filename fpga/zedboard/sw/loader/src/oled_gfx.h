/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Drawing into a frame buffer for the ZedBoard's 128x32 OLED (SSD1306).
 * Platform independent.
 *
 * Layout is the SSD1306 horizontal addressing order: byte page * 128 + x holds
 * pixels (x, page * 8 + n) in bit n. Text uses a 5x7 font in 6x8 cells, so the
 * screen has 4 rows of 21 characters.
 */

#pragma once

#include <stdint.h>

#define GFX_W        128
#define GFX_H        32
#define GFX_FB_SIZE  (GFX_W * GFX_H / 8)
#define GFX_ROWS     4
#define GFX_COLS     21
#define GFX_CHAR_W   6

void gfx_clear(uint8_t * fb);

// Sets the bytes of text row `row` from x0 to x1 - 1 to `bits`
void gfx_fill(uint8_t * fb, int row, int x0, int x1, uint8_t bits);

// Draws s at text row `row`, pixel column x, clipped at the right edge.
// inv draws dark text on a lit background. Returns the x after the text.
int gfx_text(uint8_t * fb, int row, int x, const char *s, int inv);

// Like gfx_text, but at most n characters
int gfx_textn(uint8_t * fb, int row, int x, const char *s, int n, int inv);

// Vertical line from pixel y0 to y1 - 1
void gfx_vline(uint8_t * fb, int x, int y0, int y1);

// Progress bar across text row `row`, filled num/den
void gfx_progress(uint8_t * fb, int row, uint32_t num, uint32_t den);
