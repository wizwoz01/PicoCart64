/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ROM list selection shared by the terminal UI and the OLED/buttons.
 * Platform independent.
 */

#pragma once

#include <stdint.h>

#define MENU_DIGIT_MS    1500		// digits typed within this time form one number
#define MARQUEE_HOLD_MS  1200
#define MARQUEE_STEP_MS  300

typedef struct {
	int count;
	int sel;
	int num;					// number typed so far, 0 = none
	uint32_t num_ms;			// when its last digit was typed
} menu_t;

void menu_init(menu_t * m, int count, int sel);

// Steps of 1 wrap around the ends, longer jumps stop at them
void menu_move(menu_t * m, int delta);

// Jumps to the ROM number being typed. Returns 1 if the selection moved.
int menu_digit(menu_t * m, int digit, uint32_t now_ms);

// Forgets the typed number once MENU_DIGIT_MS have passed
void menu_tick(menu_t * m, uint32_t now_ms);

// First visible entry of a window of `height` lines that shows sel
int menu_scroll(int top, int sel, int count, int height);

// First character to show of a len-character text in a width-character
// field, ms after it appeared: holds, scrolls one character per step, holds.
int menu_marquee(int len, int width, uint32_t ms);
