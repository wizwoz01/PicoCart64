/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Loader screens, drawn on the serial terminal (tui.c) and the OLED (oled_gfx.c)
 * from the same state, so a ROM can be picked from either. Platform independent.
 */

#pragma once

#include <stdint.h>

#include "bus_diag.h"
#include "menu.h"
#include "oled_gfx.h"
#include "rom_util.h"
#include "tui.h"

#define UI_NAME_LEN        256
#define UI_LOG_LINES       32
#define UI_LOG_LEN         120
#define UI_MSG_MS          8000		// how long the last message stays on the status line
#define UI_TOAST_MS        4000		// and over the OLED list, if it is a warning or error
#define UI_OLED_LIST_ROWS  (GFX_ROWS - 1)

// Push buttons, numbered like the BUTTONS register bits (C, D, L, R, U)
enum {
	UI_KEY_BTN_C = 0x200,
	UI_KEY_BTN_D,
	UI_KEY_BTN_L,
	UI_KEY_BTN_R,
	UI_KEY_BTN_U,
};

typedef enum {
	UI_MENU,
	UI_LOADING,
	UI_GAME,
} ui_screen_t;

typedef enum {
	UI_INFO,
	UI_GOOD,
	UI_WARN,
	UI_ERROR,
} ui_level_t;

typedef struct {
	char name[UI_NAME_LEN];
	uint32_t size;
} ui_rom_t;

typedef struct {
	ui_screen_t screen;
	int n64_on;

	// Menu
	const ui_rom_t *roms;
	int rom_count;
	int last_idx;				// last game played, -1 if none
	int sd_ok;
	menu_t menu;

	// Loading and game
	char rom_name[UI_NAME_LEN];
	char title[ROM_TITLE_LEN + 1];
	const char *rom_format;
	uint32_t rom_size;
	uint32_t load_done;
	int pal;
	const char *cic;			// "6105/7105"
	const char *save_type;		// "EEPROM 16 Kbit"
	char save_name[2 * UI_NAME_LEN];	// files the game saves to, "" for none
	int save_pending;
	uint32_t saves;
	uint32_t underruns;
	int bus_valid;				// the N64 was turned on since the game was loaded
	bus_mon_t bus;				// what it did since then
	uint32_t last_addr;

	uint32_t log_seq;			// incremented by ui_log()
} ui_t;

extern ui_t ui;

void ui_log(ui_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_log_clear(void);

// True while a warning or error covers the OLED list
int ui_toast_active(uint32_t now_ms);
void ui_toast_dismiss(void);

// ROM list lines on the terminal, for PgUp/PgDn
int ui_list_height(void);

// Draws the current screen on the terminal and into the OLED frame buffer
void ui_draw(uint32_t now_ms, uint8_t * fb);

// "Game (USA).z64" -> "Game (USA)"
void ui_base_name(const char *name, char *out, int out_len);

// Splits s at spaces into at most max_lines lines of up to GFX_COLS characters.
// Returns the number of lines.
int ui_wrap(const char *s, char lines[][GFX_COLS + 1], int max_lines);
