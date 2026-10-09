/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for the menu, terminal UI, OLED drawing and loader screens:
 *   cc -I../src -o test_ui test_ui.c ../src/menu.c ../src/tui.c ../src/oled_gfx.c ../src/ui.c ../src/rom_util.c ../src/bus_diag.c && ./test_ui
 */

#include <stdio.h>
#include <string.h>

#include "bus_diag.h"
#include "menu.h"
#include "oled_gfx.h"
#include "tui.h"
#include "ui.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// ---------------------------------------------------------------------------
// Minimal VT100 screen: cursor moves, erase line/screen, SGR ignored
// ---------------------------------------------------------------------------

static char term[TUI_MAX_ROWS][TUI_MAX_COLS + 1];
static int term_r, term_c, term_bytes;

static void term_clear(void)
{
	int r;

	for (r = 0; r < TUI_MAX_ROWS; r++) {
		memset(term[r], ' ', TUI_MAX_COLS);
		term[r][TUI_MAX_COLS] = '\0';
	}
}

static void term_write(const char *buf, int len)
{
	static int state, p[2], np;
	int i;

	term_bytes += len;
	for (i = 0; i < len; i++) {
		char c = buf[i];

		if (state == 0) {
			if (c == 0x1B) {
				state = 1;
			} else if (term_r < TUI_MAX_ROWS && term_c < TUI_MAX_COLS) {
				term[term_r][term_c++] = c;
			}
		} else if (state == 1) {
			state = (c == '[') ? 2 : 0;
			p[0] = p[1] = 0;
			np = 0;
		} else {
			if (c >= '0' && c <= '9') {
				p[np] = p[np] * 10 + (c - '0');
			} else if (c == ';') {
				np = 1;
			} else if (c == '?') {
			} else {
				state = 0;
				if (c == 'H') {
					term_r = p[0] - 1;
					term_c = p[1] - 1;
				} else if (c == 'K') {
					memset(term[term_r] + term_c, ' ', TUI_MAX_COLS - term_c);
				} else if (c == 'J') {
					term_clear();
				}
			}
		}
	}
}

// Text of a terminal line without trailing spaces
static const char *line(int r, int cols)
{
	static char buf[TUI_MAX_COLS + 1];
	int n = cols;

	memcpy(buf, term[r], (size_t)cols);
	while (n > 0 && buf[n - 1] == ' ')
		n--;
	buf[n] = '\0';
	return buf;
}

// ---------------------------------------------------------------------------

static void test_menu(void)
{
	menu_t m;

	menu_init(&m, 5, 3);
	CHECK(m.sel == 3);
	menu_init(&m, 5, 7);
	CHECK(m.sel == 0);
	menu_init(&m, 5, -1);
	CHECK(m.sel == 0);
	menu_move(&m, -1);
	CHECK(m.sel == 4);
	menu_move(&m, 1);
	CHECK(m.sel == 0);
	menu_move(&m, 3);
	CHECK(m.sel == 3);
	menu_move(&m, 3);
	CHECK(m.sel == 4);
	menu_move(&m, -10);
	CHECK(m.sel == 0);
	menu_init(&m, 0, 0);
	menu_move(&m, 1);
	CHECK(m.sel == 0);
	menu_init(&m, 1, 0);
	menu_move(&m, -1);
	CHECK(m.sel == 0);

	menu_init(&m, 12, 0);
	CHECK(menu_digit(&m, 1, 1000) && m.sel == 0);
	CHECK(menu_digit(&m, 2, 1500) && m.sel == 11 && m.num == 12);
	CHECK(menu_digit(&m, 5, 4000) && m.sel == 4);	// timed out, starts over
	CHECK(menu_digit(&m, 7, 4100) && m.sel == 6);	// 57 > 12
	CHECK(!menu_digit(&m, 0, 9000) && m.sel == 6 && m.num == 0);
	CHECK(menu_digit(&m, 1, 9100) && menu_digit(&m, 0, 9200) && m.sel == 9);
	menu_tick(&m, 9200 + MENU_DIGIT_MS - 1);
	CHECK(m.num == 10);
	menu_tick(&m, 9200 + MENU_DIGIT_MS);
	CHECK(m.num == 0);
	menu_move(&m, 1);
	CHECK(m.num == 0);
	menu_init(&m, 3, 0);
	CHECK(!menu_digit(&m, 9, 0) && m.sel == 0 && m.num == 0);

	CHECK(menu_scroll(0, 0, 10, 4) == 0);
	CHECK(menu_scroll(0, 5, 10, 4) == 2);
	CHECK(menu_scroll(2, 3, 10, 4) == 2);
	CHECK(menu_scroll(5, 1, 10, 4) == 1);
	CHECK(menu_scroll(8, 9, 10, 4) == 6);
	CHECK(menu_scroll(3, 1, 2, 4) == 0);

	CHECK(menu_marquee(10, 21, 5000) == 0);
	CHECK(menu_marquee(25, 21, 0) == 0);
	CHECK(menu_marquee(25, 21, MARQUEE_HOLD_MS) == 1);
	CHECK(menu_marquee(25, 21, MARQUEE_HOLD_MS + 3 * MARQUEE_STEP_MS) == 4);
	CHECK(menu_marquee(25, 21, MARQUEE_HOLD_MS + 4 * MARQUEE_STEP_MS) == 4);
	CHECK(menu_marquee(25, 21, 2 * MARQUEE_HOLD_MS + 4 * MARQUEE_STEP_MS) == 0);
}

static int feed(tui_keydec_t *d, const char *s, int *keys, int max)
{
	int n = 0;

	for (; *s; s++) {
		int k = tui_key_feed(d, (uint8_t)*s);

		if (k != TUI_KEY_NONE && n < max)
			keys[n++] = k;
	}
	return n;
}

static int one_key(const char *s)
{
	tui_keydec_t d;
	int k[4];

	tui_key_init(&d);
	return feed(&d, s, k, 4) == 1 ? k[0] : -1;
}

static void test_keys(void)
{
	tui_keydec_t d;
	int k[8];

	CHECK(one_key("\033[A") == TUI_KEY_UP);
	CHECK(one_key("\033[B") == TUI_KEY_DOWN);
	CHECK(one_key("\033[C") == TUI_KEY_RIGHT);
	CHECK(one_key("\033[D") == TUI_KEY_LEFT);
	CHECK(one_key("\033OA") == TUI_KEY_UP);
	CHECK(one_key("\033OB") == TUI_KEY_DOWN);
	CHECK(one_key("\033[1;5A") == TUI_KEY_UP);
	CHECK(one_key("\033[5~") == TUI_KEY_PGUP);
	CHECK(one_key("\033[6~") == TUI_KEY_PGDN);
	CHECK(one_key("\033[H") == TUI_KEY_HOME);
	CHECK(one_key("\033[1~") == TUI_KEY_HOME);
	CHECK(one_key("\033[7~") == TUI_KEY_HOME);
	CHECK(one_key("\033[F") == TUI_KEY_END);
	CHECK(one_key("\033[4~") == TUI_KEY_END);
	CHECK(one_key("\033OF") == TUI_KEY_END);
	CHECK(one_key("\r") == TUI_KEY_ENTER);
	CHECK(one_key("\n") == TUI_KEY_ENTER);
	CHECK(one_key("\r\n") == TUI_KEY_ENTER);
	CHECK(one_key("\x7F") == TUI_KEY_BACKSPACE);
	CHECK(one_key("\b") == TUI_KEY_BACKSPACE);
	CHECK(one_key("\x0C") == TUI_KEY_REDRAW);
	CHECK(one_key("m") == 'm');
	CHECK(one_key("\033x") == 'x');
	CHECK(one_key("\033[2~") == -1);	// Insert: ignored
	CHECK(one_key("\x01") == -1);

	tui_key_init(&d);
	CHECK(feed(&d, "\r\r", k, 8) == 2 && k[0] == TUI_KEY_ENTER && k[1] == TUI_KEY_ENTER);

	tui_key_init(&d);
	CHECK(feed(&d, "\033[24;80R", k, 8) == 1 && k[0] == TUI_KEY_SIZE);
	CHECK(d.param[0] == 24 && d.param[1] == 80);

	tui_key_init(&d);
	CHECK(feed(&d, "a\033[Bb", k, 8) == 3 && k[0] == 'a' && k[1] == TUI_KEY_DOWN && k[2] == 'b');

	// A lone ESC is reported once the line goes quiet
	tui_key_init(&d);
	CHECK(feed(&d, "\033", k, 8) == 0);
	CHECK(tui_key_idle(&d) == TUI_KEY_ESC);
	CHECK(tui_key_idle(&d) == TUI_KEY_NONE);
	CHECK(feed(&d, "\033[A", k, 8) == 1 && k[0] == TUI_KEY_UP);
}

static void test_tui(void)
{
	char expect[81];

	term_clear();
	tui_init(term_write);
	CHECK(tui_rows() == 24 && tui_cols() == 80);

	term_bytes = 0;
	tui_begin();
	tui_line(0, TUI_BAR, "Title");
	tui_line(1, TUI_NORMAL, "Hello %d", 42);
	tui_line(2, TUI_NORMAL, "a\tb\x82");
	tui_end();
	CHECK(strcmp(line(0, 80), "Title") == 0);
	CHECK(strcmp(line(1, 80), "Hello 42") == 0);
	CHECK(strcmp(line(2, 80), "a?b?") == 0);
	CHECK(term_bytes > 0);

	// Same frame: nothing to send
	term_bytes = 0;
	tui_begin();
	tui_line(0, TUI_BAR, "Title");
	tui_line(1, TUI_NORMAL, "Hello %d", 42);
	tui_line(2, TUI_NORMAL, "a\tb\x82");
	tui_end();
	CHECK(term_bytes == 0);

	// One line changed: only that line is sent, and it erases the old text
	term_bytes = 0;
	tui_begin();
	tui_line(0, TUI_BAR, "Title");
	tui_line(1, TUI_NORMAL, "Hi");
	tui_line(2, TUI_NORMAL, "a\tb\x82");
	tui_end();
	CHECK(strcmp(line(1, 80), "Hi") == 0);
	CHECK(term_bytes > 0 && term_bytes < 30);

	// Attribute change alone is sent
	term_bytes = 0;
	tui_begin();
	tui_line(0, TUI_BAR, "Title");
	tui_line(1, TUI_SELECTED, "Hi");
	tui_line(2, TUI_NORMAL, "a\tb\x82");
	tui_end();
	CHECK(term_bytes > 80);

	// Removed lines are erased
	tui_begin();
	tui_line(0, TUI_BAR, "Title");
	tui_end();
	CHECK(strcmp(line(1, 80), "") == 0 && strcmp(line(2, 80), "") == 0);

	// Right-aligned text
	tui_begin();
	tui_line_lr(3, TUI_NORMAL, "left", "right");
	tui_line_lr(4, TUI_NORMAL, "left", "");
	tui_end();
	memset(expect, ' ', 80);
	memcpy(expect, "left", 4);
	memcpy(expect + 75, "right", 5);
	expect[80] = '\0';
	CHECK(strcmp(line(3, 80), expect) == 0);
	CHECK(strcmp(line(4, 80), "left") == 0);

	// Lines are cut at the terminal width
	tui_resize(30, 40);
	CHECK(tui_rows() == 30 && tui_cols() == 40);
	tui_begin();
	tui_line(0, TUI_NORMAL, "%s", "0123456789012345678901234567890123456789XYZ");
	tui_end();
	CHECK(strcmp(line(0, 80), "0123456789012345678901234567890123456789") == 0);
	CHECK(strcmp(line(1, 80), "") == 0);	// full redraw cleared the rest

	tui_resize(5, 5);
	CHECK(tui_rows() == 30 && tui_cols() == 40);
	tui_resize(100, 500);
	CHECK(tui_rows() == TUI_MAX_ROWS && tui_cols() == TUI_MAX_COLS);
	tui_resize(24, 80);
}

static void test_gfx(void)
{
	static const uint8_t glyph_a[5] = { 0x7E, 0x11, 0x11, 0x11, 0x7E };
	uint8_t fb[GFX_FB_SIZE];
	int i;

	gfx_clear(fb);
	CHECK(gfx_text(fb, 0, 0, "A", 0) == 6);
	CHECK(memcmp(fb, glyph_a, 5) == 0 && fb[5] == 0);

	CHECK(gfx_text(fb, 1, 6, "A", 1) == 12);
	for (i = 0; i < 5; i++)
		CHECK(fb[GFX_W + 6 + i] == (uint8_t)~glyph_a[i]);
	CHECK(fb[GFX_W + 11] == 0xFF);

	// Clipped at the right edge, nothing past the row
	gfx_clear(fb);
	CHECK(gfx_text(fb, 3, 125, "AB", 0) == GFX_W);
	CHECK(fb[3 * GFX_W + 125] == 0x7E && fb[3 * GFX_W + 127] == 0x11);
	CHECK(gfx_textn(fb, 2, 0, "ABCDEF", 2, 0) == 12);
	CHECK(fb[2 * GFX_W + 12] == 0);

	// Unknown characters draw as '?'
	gfx_clear(fb);
	gfx_text(fb, 0, 0, "\x82", 0);
	CHECK(fb[0] == 0x02 && fb[2] == 0x51);

	gfx_clear(fb);
	gfx_vline(fb, 127, 8, 32);
	CHECK(fb[GFX_W + 127] == 0xFF && fb[3 * GFX_W + 127] == 0xFF && fb[127] == 0);

	gfx_progress(fb, 2, 50, 100);
	CHECK(fb[2 * GFX_W + 2] == 0x7E && fb[2 * GFX_W + 30] == 0x7E);
	CHECK(fb[2 * GFX_W + 100] == 0x42 && fb[2 * GFX_W + 125] == 0x7E && fb[2 * GFX_W + 126] == 0);
	gfx_progress(fb, 2, 7, 0);
	CHECK(fb[2 * GFX_W + 30] == 0x42);
}

static void test_text(void)
{
	char out[32];
	char lines[3][GFX_COLS + 1];

	ui_base_name("Resident Evil 2 (USA).z64", out, sizeof(out));
	CHECK(strcmp(out, "Resident Evil 2 (USA)") == 0);
	ui_base_name("noext", out, sizeof(out));
	CHECK(strcmp(out, "noext") == 0);
	ui_base_name(".z64", out, sizeof(out));
	CHECK(strcmp(out, ".z64") == 0);
	ui_base_name("abcdefgh.z64", out, 5);
	CHECK(strcmp(out, "abcd") == 0);

	CHECK(ui_wrap("Turn the N64 off before loading a ROM.", lines, 3) == 2);
	CHECK(strcmp(lines[0], "Turn the N64 off") == 0);
	CHECK(strcmp(lines[1], "before loading a ROM.") == 0);
	CHECK(ui_wrap("ABCDEFGHIJKLMNOPQRSTUVWXYZ", lines, 3) == 2);
	CHECK(strcmp(lines[0], "ABCDEFGHIJKLMNOPQRSTU") == 0 && strcmp(lines[1], "VWXYZ") == 0);
	CHECK(ui_wrap("one two three four five six seven eight nine ten eleven twelve", lines, 3) == 3);
	CHECK(ui_wrap("   ", lines, 3) == 0);
}

static int find_row(const char *s, int rows, int cols)
{
	int r;

	for (r = 0; r < rows; r++)
		if (strstr(line(r, cols), s))
			return r;
	return -1;
}

static void test_screens(void)
{
	static ui_rom_t roms[30];
	uint8_t fb[GFX_FB_SIZE];
	int i, r;

	for (i = 0; i < 30; i++) {
		snprintf(roms[i].name, sizeof(roms[i].name), "Game %02d with a fairly long name (USA).z64", i + 1);
		roms[i].size = (uint32_t)(i + 1) << 20;
	}
	memset(&ui, 0, sizeof(ui));
	ui.roms = roms;
	ui.rom_count = 30;
	ui.last_idx = 2;
	ui.sd_ok = 1;
	menu_init(&ui.menu, 30, 2);
	ui.screen = UI_MENU;

	term_clear();
	tui_init(term_write);
	ui_draw(1000, fb);
	CHECK(strstr(line(0, 80), "PicoCart64") && strstr(line(0, 80), "30 ROMs"));
	r = find_row("Game 03", 24, 80);
	CHECK(r == 5 && line(r, 80)[1] == '>' && strstr(line(r, 80), "3*") && strstr(line(r, 80), "3 MB"));
	CHECK(find_row("1-18 of 30", 24, 80) == 21);
	CHECK(strstr(line(22, 80), "last played: Game 03"));
	// OLED: inverted header, the selection on the third list row, others not inverted
	CHECK(fb[0] == 0xFF);
	CHECK(fb[3 * GFX_W + 5] != 0 && fb[GFX_W + 5] == 0 && fb[2 * GFX_W + 5] == 0);
	CHECK(fb[GFX_W + 127] != 0);	// scroll bar

	// Selection moves down past the window on both
	for (i = 0; i < 25; i++)
		menu_move(&ui.menu, 1);
	ui_draw(2000, fb);
	r = find_row("Game 28", 24, 80);
	CHECK(r == 20 && line(r, 80)[1] == '>');
	CHECK(find_row("Game 03", 21, 80) == -1);
	CHECK(fb[3 * GFX_W + 5] != 0 && fb[GFX_W + 5] == 0);	// last OLED row

	// Number being typed
	menu_digit(&ui.menu, 7, 3000);
	ui_draw(3000, fb);
	CHECK(strstr(line(22, 80), "Go to #7") && find_row("Game 07", 24, 80) >= 0);
	menu_tick(&ui.menu, 3000 + MENU_DIGIT_MS);

	// An error shows on the status line and over the OLED list, until dismissed
	ui_log(UI_ERROR, "Turn the N64 off before loading a ROM.");
	ui_draw(4000, fb);
	CHECK(strstr(line(22, 80), "Turn the N64 off"));
	CHECK(ui_toast_active(4000) && !ui_toast_active(4000 + UI_TOAST_MS));
	ui_toast_dismiss();
	CHECK(!ui_toast_active(4001));

	// No ROMs
	ui.rom_count = 0;
	menu_init(&ui.menu, 0, 0);
	ui_draw(5000, fb);
	CHECK(find_row("No .z64", 24, 80) >= 0);

	// Loading
	snprintf(ui.rom_name, sizeof(ui.rom_name), "Resident Evil 2 (USA).z64");
	ui.rom_size = 64u << 20;
	ui.load_done = 16u << 20;
	ui.screen = UI_LOADING;
	ui_draw(6000, fb);
	CHECK(strstr(line(2, 80), "Loading Resident Evil 2 (USA).z64"));
	CHECK(strstr(line(4, 80), " 25%") && strstr(line(4, 80), "16 MB / 64 MB"));
	CHECK(fb[2 * GFX_W + 20] == 0x7E && fb[2 * GFX_W + 60] == 0x42);

	// Game
	ui_log_clear();
	snprintf(ui.title, sizeof(ui.title), "RESIDENT EVIL II");
	ui.rom_format = "z64";
	snprintf(ui.save_name, sizeof(ui.save_name), "Resident Evil 2 (USA).sra");
	ui.cic = "6102/7101";
	ui.save_type = "SRAM 256 Kbit";
	ui.pal = 1;
	ui.screen = UI_GAME;
	ui_log(UI_GOOD, "Cart enabled. Power on the N64.");
	ui_draw(7000, fb);
	CHECK(strstr(line(2, 80), "RESIDENT EVIL II"));
	CHECK(strstr(line(3, 80), "64 MB, z64, PAL"));
	CHECK(strstr(line(4, 80), "CIC        6102/7101"));
	CHECK(strstr(line(5, 80), "SRAM 256 Kbit, to Resident Evil 2 (USA).sra"));
	CHECK(find_row("Cart enabled", 24, 80) == 13);
	CHECK(strstr(line(9, 80), "the N64 hasn't been turned on"));
	CHECK(strstr(line(23, 80), "s save now"));
	ui.n64_on = 1;
	ui.saves = 2;
	ui.bus_valid = 1;
	ui.bus.ale = 0xFFFF0BF1;
	ui.bus.rw = 0x0000FFFF;
	ui.bus.first_addr = 0x10000000;
	ui.bus.first_data = 0x80371240;
	ui.bus.rom = 0x00000302;
	ui.last_addr = 0x10000FFC;
	ui_log(UI_GOOD, "Saved Resident Evil 2 (USA).sra");
	ui_draw(8000, fb);
	CHECK(strstr(line(0, 80), "N64 ON") && strstr(line(7, 80), "2 times"));
	CHECK(strstr(line(9, 80), "ALE_H 3057 ALE_L 65535+ ~READ 65535+ ~WRITE 0 ROM 770"));
	CHECK(strstr(line(10, 80), "first 10000000  last 10000FFC  word 80371240  ALE noise 0") && !strstr(line(10, 80), "mismatch"));
	CHECK(find_row("Cart enabled", 24, 80) == 13 && find_row("Saved", 24, 80) == 14);
	ui.bus.rom = 0x00010302;
	ui.bus.glitch = 0x0002FFFF;
	ui_draw(8100, fb);
	CHECK(strstr(line(10, 80), "word 80371240  ALE noise 65535+"));
}

// ---------------------------------------------------------------------------
// Bus diagnosis
// ---------------------------------------------------------------------------

static bus_verdict_t diag(uint32_t ale, uint32_t rw, uint32_t first, uint32_t data, uint32_t rom, uint32_t ad_or, char *out)
{
	bus_mon_t m = { ale, rw, first, data, rom, ad_or, 0, 0, 0, 0, 1 };

	return bus_diagnose(&m, 0x80371240, out, 120);
}

static void test_bus_diag(void)
{
	char s[120];

	CHECK(strcmp(bus_ad_pin(0), "JA1") == 0 && strcmp(bus_ad_pin(4), "JA7") == 0);
	CHECK(strcmp(bus_ad_pin(11), "JB4") == 0 && strcmp(bus_ad_pin(15), "JB10") == 0);

	// Nothing at all: the PIF stopped the CPU before IPL2
	CHECK(diag(0, 0, 0, 0, 0, 0, s) == BUS_FAULT && strstr(s, "No bus activity") && strstr(s, "Pico"));
	CHECK(strlen(s) < 120);

	// One ALE line missing
	CHECK(diag(0x00000005, 0x10, 0, 0, 0, 0, s) == BUS_FAULT && strstr(s, "ALE_L wire (JC1)"));
	CHECK(diag(0x00050000, 0x10, 0, 0, 0, 0, s) == BUS_FAULT && strstr(s, "ALE_H wire (JC2)"));

	// ~READ missing, or swapped with ~WRITE
	CHECK(diag(0x00010001, 0, 0x10000000, 0, 0, 0, s) == BUS_FAULT && strstr(s, "~READ wire (JC4)"));
	CHECK(diag(0x00010001, 0x00020000, 0x10000000, 0, 0, 0, s) == BUS_FAULT && strstr(s, "swapped (JC4, JC3)"));

	// ALE_L and ALE_H swapped: the high half is latched as the low half
	CHECK(diag(0x00030003, 6, 0x00001000, 0, 0, 0, s) == BUS_FAULT && strstr(s, "ALE_L and ALE_H look swapped"));

	// AD12 stuck low, AD3 stuck high
	CHECK(diag(0x00030003, 6, 0x00000008, 0, 0, 0, s) == BUS_FAULT && strstr(s, "not 10000000"));
	CHECK(strstr(s, "AD3 (JA4), AD12 (JB7)"));
	CHECK(strlen(s) < 120);
	diag(0x00030003, 6, 0x0000FFFF, 0, 0, 0, s);
	CHECK(strstr(s, "AD0 (JA1), AD1 (JD7), AD2 (JA3), AD3 (JA4), ...") && strlen(s) < 120);

	// Cart not answering, another driver on AD, bad data from DDR
	CHECK(diag(0x00010001, 2, 0x10000000, 0, 0, 0x10000000, s) == BUS_FAULT && strstr(s, "didn't answer"));
	CHECK(diag(0x00010001, 2, 0x10000000, 0x80371240, 0x00010002, 0x10000000, s) == BUS_FAULT && strstr(s, "1 ROM reads"));
	CHECK(strlen(s) < 120);
	CHECK(diag(0x00010001, 2, 0x10000000, 0xFFFFFFFF, 2, 0x10000000, s) == BUS_FAULT && strstr(s, "FFFFFFFF instead of 80371240"));

	// Stopped in IPL2, after IPL2 (CIC), after the first 1 MB, and running
	CHECK(diag(0x00010001, 2, 0x10000000, 0x80371240, 2, 0x10000000, s) == BUS_STOPPED && strstr(s, "1 of 1009"));
	CHECK(diag(0x03F103F1, 2018, 0x10000000, 0x80371240, 2018, 0x10000FFC, s) == BUS_STOPPED && strstr(s, "CIC check failed"));
	CHECK(strlen(s) < 120);
	CHECK(diag(0x0BF10BF1, 0xFFFF, 0x10000000, 0x80371240, 0xFFFF, 0x101FFFFC, s) == BUS_STOPPED && strstr(s, "checksum"));
	CHECK(strlen(s) < 120);
	CHECK(diag(0x2000FFFF, 0xFFFF, 0x10000000, 0x80371240, 0xFFFF, 0x101FFFFC, s) == BUS_OK && strstr(s, "runs the game"));
	CHECK(strstr(s, "8192 addresses, 65535+ ROM reads"));

	// AD5 never high during the boot code reads
	CHECK(diag(0x03F103F1, 2018, 0x10000000, 0x80371240, 2018, 0x10000FDC, s) == BUS_FAULT && strstr(s, "AD5 (JD8)"));
	// AD14 never high in the first 1 MB
	CHECK(diag(0x0C000C00, 0xFFFF, 0x10000000, 0x80371240, 0xFFFF, 0x101FBFFC, s) == BUS_FAULT && strstr(s, "AD14 (JB9)"));

	// Noise on the ALE lines
	{
		bus_mon_t m;

		memset(&m, 0, sizeof(m));

		CHECK(!bus_noise(&m, s, 120));
		m.glitch = 5;
		CHECK(bus_noise(&m, s, 120) && strstr(s, "Ignored 5 short pulses") && strstr(s, "ground"));
		m.glitch = 0x00020007;
		CHECK(bus_noise(&m, s, 120) && strstr(s, "2 ROM words were late") && strstr(s, "7 ALE pulses"));
		CHECK(strlen(s) < 120);
		m.odd = 0xFFFFFFFF;
		CHECK(bus_noise(&m, s, 120) && strstr(s, "~READ noise: 65535 transfers") && strlen(s) < 120);

		bus_noise_event(0x2060000E, s, 120);
		CHECK(strcmp(s, "ALE_L pulse 60 ns at 14 us") == 0);
		bus_noise_event(0x30112345, s, 120);
		CHECK(strcmp(s, "odd read count 1 at 74565 us") == 0);
		bus_noise_event(0x40A00010, s, 120);
		CHECK(strcmp(s, "short ~READ 100 ns at 16 us") == 0);
	}

	// S_DAT, the EEPROM line
	{
		bus_mon_t m;

		memset(&m, 0, sizeof(m));
		m.sdat_high = 1;
		CHECK(!bus_sdat(&m, s, 120) && strcmp(s, "high, no EEPROM traffic yet") == 0);
		m.sdat = 0x0003012C;
		m.sdat_low = 3;
		CHECK(!bus_sdat(&m, s, 120)
		      && strcmp(s, "high, 3 EEPROM commands answered, 300 falls, longest low 3 us") == 0);
		m.sdat = 0xFFFFFFFF;
		CHECK(!bus_sdat(&m, s, 120) && strstr(s, "65535+ EEPROM commands") && strstr(s, "65535+ falls"));

		// Held low now, or earlier and back high
		m.sdat_high = 0;
		m.sdat = 0x00000009;
		m.sdat_low = 0xFFFF0003;
		CHECK(bus_sdat(&m, s, 120) && strstr(s, "LOW, held low for 65535+ us after 9 falls, 0 EEPROM commands")
		      && strstr(s, "the PIF hangs on it"));
		CHECK(strlen(s) + strlen("S_DAT (EEPROM line) ") < 120);
		m.sdat_high = 1;
		m.sdat_low = 0x000004B0;
		CHECK(bus_sdat(&m, s, 120) && strstr(s, "high, held low for 1200 us"));
		m.sdat_low = BUS_SDAT_HELD_US - 1;
		CHECK(!bus_sdat(&m, s, 120));

		// Event log entries: 40 ns units
		bus_sdat_event(12500000, 12500000, s, 120);
		CHECK(strcmp(s, "0.500000 s  +0.00 us  line fell") == 0);
		bus_sdat_event(0x40000000 | 12500076, 12500000, s, 120);
		CHECK(strcmp(s, "0.500003 s  +3.04 us  line rose") == 0);
		bus_sdat_event(0xC0000000 | 0x3FFFFFFF, 0x80000000 | 0x3FFFFFFE, s, 120);
		CHECK(strcmp(s, "42.949672 s  +0.04 us  cart lets go") == 0);
	}
}

// ---------------------------------------------------------------------------
// Address trace
// ---------------------------------------------------------------------------

static uint32_t ta[BUS_TRACE_LEN], tb[BUS_TRACE_LEN];

// The boot addresses, then bits moved by f
static void make_trace(uint32_t (*f)(uint32_t))
{
	int k;

	for (k = 0; k < BUS_TRACE_LEN; k++) {
		uint32_t e = k == 0 ? 0x10000000 : 0x10000040 + 4u * (uint32_t)(k - 1);
		uint32_t lo = e & 0xFFFF;

		ta[k] = f ? f(e) : e;
		tb[k] = (lo << 16) | lo;
	}
}

static uint32_t ad1_follows_ad2(uint32_t e)
{
	return e | ((e >> 1) & 2);
}

static uint32_t ad5_open(uint32_t e)
{
	return e & ~0x20u;
}

static uint32_t ad3_ad4_swapped(uint32_t e)
{
	return (e & ~0x18u) | ((e & 8) << 1) | ((e & 0x10) >> 1);
}

static uint32_t high_bit13(uint32_t e)
{
	return e | 0x20000000u;
}

static void test_trace(void)
{
	char out[3][BUS_MSG_LEN];
	int n, clean;

	make_trace(NULL);
	n = bus_trace_check(ta, tb, 1009, out, 3, &clean);
	CHECK(n == 1 && clean && strstr(out[0], "1009 boot addresses: every AD input follows its own line"));
	CHECK(bus_trace_check(ta, tb, 1, out, 3, &clean) == 0 && !clean);

	// JD7 wired to the AD2 line: only the inputs are named, not the timing
	make_trace(ad1_follows_ad2);
	n = bus_trace_check(ta, tb, 1009, out, 3, &clean);
	CHECK(n == 2 && !clean && strstr(out[0], "wrong in 504 sampled before ALE_L fell, 0 just after, 0 35 ns later"));
	CHECK(strcmp(out[1], "Wiring: JD7 (AD1) follows AD2 (Pico pin 4)") == 0);

	make_trace(ad5_open);
	n = bus_trace_check(ta, tb, 600, out, 3, &clean);
	CHECK(n == 2 && strcmp(out[1], "Wiring: JD8 (AD5) stuck at 0") == 0);

	make_trace(ad3_ad4_swapped);
	n = bus_trace_check(ta, tb, 1009, out, 3, &clean);
	CHECK(n == 2 && strcmp(out[1], "Wiring: JA4 (AD3) follows AD4 (Pico pin 6), JA7 (AD4) follows AD3 (Pico pin 5)") == 0);

	make_trace(high_bit13);
	n = bus_trace_check(ta, tb, 1009, out, 3, &clean);
	CHECK(n == 2 && strstr(out[0], "differs from 1000 in bits 2000") && strstr(out[1], "every AD input"));

	// Wrong only before ALE_L fell: the inputs are fine, the sampling point isn't
	make_trace(NULL);
	ta[7] |= 2;
	ta[300] ^= 0x40;
	n = bus_trace_check(ta, tb, 1009, out, 3, &clean);
	CHECK(n == 2 && strstr(out[0], "wrong in 2 sampled before ALE_L fell, 0 just after"));
	CHECK(strstr(out[1], "JD7 (AD1) wrong in 1 of 1009") && strstr(out[1], "JA9 (AD6) wrong in 1 of 1009"));
	CHECK(strlen(out[1]) < BUS_MSG_LEN);
}

static void test_xfer(void)
{
	static uint8_t rom[0x2000];
	bus_xfer_t x = { 0x10001000, 3, 0, 0 };
	uint32_t exp = 0;
	int i;

	for (i = 0; i < (int)sizeof(rom); i++)
		rom[i] = (uint8_t)(i * 7 + 3);
	// halfwords at 0x1000, 0x1002, 0x1004
	x.sum = ((rom[0x1000] << 8) | rom[0x1001]) + ((rom[0x1002] << 8) | rom[0x1003]) + ((rom[0x1004] << 8) | rom[0x1005]);
	CHECK(bus_xfer_check(&x, rom, sizeof(rom), &exp) == 1 && exp == x.sum);
	x.sum++;
	CHECK(bus_xfer_check(&x, rom, sizeof(rom), &exp) == 0 && exp == x.sum - 1);
	x.addr = 0x08000000;			// SRAM
	CHECK(bus_xfer_check(&x, rom, sizeof(rom), &exp) == -1);
	x.addr = 0x10001FFE;			// runs past the end of the ROM
	CHECK(bus_xfer_check(&x, rom, sizeof(rom), &exp) == -1);
	x.addr = 0x10000000;
	x.rw = 0x00010002;			// writes
	CHECK(bus_xfer_check(&x, rom, sizeof(rom), &exp) == -1);
}

int main(void)
{
	test_menu();
	test_keys();
	test_tui();
	test_gfx();
	test_text();
	test_screens();
	test_bus_diag();
	test_trace();
	test_xfer();

	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
