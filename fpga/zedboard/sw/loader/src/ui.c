/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Loader screens on the serial terminal and the OLED.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define TITLE " PicoCart64 ZedBoard loader"

ui_t ui;

static struct {
	char text[UI_LOG_LEN];
	uint8_t level;
} log_ring[UI_LOG_LINES];
static int log_head, log_count;

static uint32_t seen_seq, msg_ms;
static int msg_dismissed;
static int prev_screen = -1, prev_sel = -1;
static uint32_t sel_ms;
static int tui_top, oled_top;

static const tui_attr_t level_attr[] = {
	[UI_INFO] = TUI_NORMAL,
	[UI_GOOD] = TUI_GOOD,
	[UI_WARN] = TUI_WARN,
	[UI_ERROR] = TUI_ERROR,
};

void ui_log(ui_level_t level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(log_ring[log_head].text, UI_LOG_LEN, fmt, ap);
	va_end(ap);
	log_ring[log_head].level = (uint8_t)level;
	log_head = (log_head + 1) % UI_LOG_LINES;
	if (log_count < UI_LOG_LINES)
		log_count++;
	ui.log_seq++;
}

void ui_log_clear(void)
{
	log_count = 0;
	msg_dismissed = 1;
}

// i = 0 is the oldest line
static const char *log_line(int i, ui_level_t *level)
{
	int k = (log_head - log_count + i + 2 * UI_LOG_LINES) % UI_LOG_LINES;

	*level = (ui_level_t)log_ring[k].level;
	return log_ring[k].text;
}

// Last message if it is recent, else NULL
static const char *recent_msg(uint32_t now, uint32_t max_ms, ui_level_t *level)
{
	if (log_count == 0 || msg_dismissed || now - msg_ms >= max_ms)
		return NULL;
	return log_line(log_count - 1, level);
}

int ui_toast_active(uint32_t now_ms)
{
	ui_level_t level;

	return ui.screen == UI_MENU && recent_msg(now_ms, UI_TOAST_MS, &level) && level >= UI_WARN;
}

void ui_toast_dismiss(void)
{
	msg_dismissed = 1;
}

int ui_list_height(void)
{
	int h = tui_rows() - 6;

	return h < 1 ? 1 : h;
}

void ui_base_name(const char *name, char *out, int out_len)
{
	const char *dot = strrchr(name, '.');
	int n = dot && dot != name ? (int)(dot - name) : (int)strlen(name);

	if (n > out_len - 1)
		n = out_len - 1;
	memcpy(out, name, (size_t)n);
	out[n] = '\0';
}

int ui_wrap(const char *s, char lines[][GFX_COLS + 1], int max_lines)
{
	int n = 0;

	while (*s && n < max_lines) {
		int len, cut;

		while (*s == ' ')
			s++;
		if (!*s)
			break;
		len = (int)strlen(s);
		cut = len <= GFX_COLS ? len : GFX_COLS;
		if (cut < len && s[cut] != ' ') {
			int sp = cut;

			while (sp > 0 && s[sp] != ' ')
				sp--;
			if (sp > 0)
				cut = sp;
		}
		memcpy(lines[n], s, (size_t)cut);
		lines[n][cut] = '\0';
		while (cut > 0 && lines[n][cut - 1] == ' ')
			lines[n][--cut] = '\0';
		n++;
		s += cut;
	}
	return n;
}

static void fmt_size(char *buf, size_t len, uint32_t bytes)
{
	uint32_t kb = bytes >> 10, mb = bytes >> 20;

	if (bytes < (1u << 20))
		snprintf(buf, len, "%lu KB", (unsigned long)kb);
	else if ((bytes & ((1u << 20) - 1)) == 0)
		snprintf(buf, len, "%lu MB", (unsigned long)mb);
	else
		snprintf(buf, len, "%lu.%lu MB", (unsigned long)mb, (unsigned long)(((bytes >> 10) & 1023) * 10 / 1024));
}

// A bus monitor count, which stops at 0xFFFF
static const char *count(char *buf, uint32_t v)
{
	snprintf(buf, 8, v >= 0xFFFF ? "%lu+" : "%lu", (unsigned long)v);
	return buf;
}

// Copies s into out, cut to width characters with ".." at the end
static void fit(char *out, const char *s, int width)
{
	int n = (int)strlen(s);

	if (width < 0)
		width = 0;
	if (n <= width) {
		memcpy(out, s, (size_t)n + 1);
	} else if (width < 3) {
		memcpy(out, s, (size_t)width);
		out[width] = '\0';
	} else {
		memcpy(out, s, (size_t)width - 2);
		memcpy(out + width - 2, "..", 3);
	}
}

static uint32_t percent(uint32_t num, uint32_t den)
{
	return den ? (uint32_t)((uint64_t)num * 100 / den) : 0;
}

// Text at the right end of an OLED row
static void oled_right(uint8_t *fb, int row, const char *s, int inv)
{
	gfx_text(fb, row, GFX_W - 1 - (int)strlen(s) * GFX_CHAR_W, s, inv);
}

static void oled_header(uint8_t *fb, const char *left, const char *right)
{
	gfx_fill(fb, 0, 0, GFX_W, 0xFF);
	gfx_textn(fb, 0, 1, left, GFX_COLS - (int)strlen(right) - 1, 1);
	oled_right(fb, 0, right, 1);
}

static void oled_lines(uint8_t *fb, const char *a, const char *b, const char *c)
{
	gfx_text(fb, 1, 0, a, 0);
	gfx_text(fb, 2, 0, b, 0);
	gfx_text(fb, 3, 0, c, 0);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

static void draw_menu(uint32_t now, uint8_t *fb)
{
	const int rows = tui_rows(), cols = tui_cols(), h = ui_list_height();
	const int count = ui.rom_count, sel = ui.menu.sel;
	char buf[TUI_MAX_COLS + 1], name[TUI_MAX_COLS - 18], size[16], right[48];
	const char *msg;
	ui_level_t level = UI_INFO;
	int i;

	snprintf(right, sizeof(right), "%s%d ROM%s ", ui.n64_on ? "N64 ON   " : "", count, count == 1 ? "" : "s");
	tui_line_lr(0, TUI_BAR, TITLE, right);

	if (!ui.sd_ok) {
		tui_line(3, TUI_ERROR, "  Cannot read the SD card (FAT32 expected).");
		tui_line(5, TUI_NORMAL, "  Insert it, then press r or BTNC to try again.");
	} else if (count == 0) {
		tui_line(3, TUI_WARN, "  No .z64, .v64 or .n64 files in the root folder of the SD card.");
		tui_line(5, TUI_NORMAL, "  Copy some there, then press r or BTNC to look again.");
	} else {
		tui_top = menu_scroll(tui_top, sel, count, h);
		tui_line_lr(2, TUI_HEADER, "     #  ROM", "Size ");
		for (i = 0; i < h && tui_top + i < count; i++) {
			int k = tui_top + i;

			fit(name, ui.roms[k].name, cols - 19);
			fmt_size(size, sizeof(size), ui.roms[k].size);
			snprintf(buf, sizeof(buf), " %c%4d%c %s", k == sel ? '>' : ' ', k + 1, k == ui.last_idx ? '*' : ' ', name);
			snprintf(right, sizeof(right), "%s ", size);
			tui_line_lr(3 + i, k == sel ? TUI_SELECTED : TUI_NORMAL, buf, right);
		}
		if (count > h) {
			snprintf(right, sizeof(right), "%d-%d of %d ", tui_top + 1, tui_top + i, count);
			tui_line_lr(rows - 3, TUI_DIM, "", right);
		}
	}

	if (ui.menu.num) {
		tui_line(rows - 2, TUI_HEADER, " Go to #%d", ui.menu.num);
	} else if ((msg = recent_msg(now, UI_MSG_MS, &level)) != NULL) {
		tui_line(rows - 2, level_attr[level], " %s", msg);
	} else if (ui.n64_on) {
		tui_line(rows - 2, TUI_WARN, " The N64 is on. Turn it off to load a ROM.");
	} else if (ui.last_idx >= 0 && count > 0) {
		tui_line(rows - 2, TUI_DIM, " * last played: %s", ui.roms[ui.last_idx].name);
	} else {
		tui_line(rows - 2, TUI_DIM, " Pick a ROM here or with the buttons next to the OLED.");
	}
	tui_line(rows - 1, TUI_BAR, " Up/Dn move  PgUp/PgDn page  Enter load  0-9 jump to #  r rescan  f flip OLED");

	// OLED
	if (ui_toast_active(now)) {
		char lines[UI_OLED_LIST_ROWS][GFX_COLS + 1];
		int n;

		msg = recent_msg(now, UI_TOAST_MS, &level);
		oled_header(fb, level == UI_ERROR ? "ERROR" : "NOTE", "");
		n = ui_wrap(msg, lines, UI_OLED_LIST_ROWS);
		for (i = 0; i < n; i++)
			gfx_text(fb, 1 + i, 0, lines[i], 0);
		return;
	}

	right[0] = '\0';
	if (count)
		snprintf(right, sizeof(right), "%d/%d", sel + 1, count);
	oled_header(fb, ui.n64_on ? "N64 IS ON" : "SELECT ROM", right);
	if (!ui.sd_ok) {
		oled_lines(fb, "SD card not found.", "Insert it, then", "press C to retry.");
	} else if (count == 0) {
		oled_lines(fb, "No ROMs on the SD", "card. Copy some,", "then press C.");
	} else {
		oled_top = menu_scroll(oled_top, sel, count, UI_OLED_LIST_ROWS);
		for (i = 0; i < UI_OLED_LIST_ROWS && oled_top + i < count; i++) {
			int k = oled_top + i;

			ui_base_name(ui.roms[k].name, name, sizeof(name));
			if (k == sel) {
				int off = menu_marquee((int)strlen(name), GFX_COLS, now - sel_ms);

				gfx_fill(fb, 1 + i, 0, GFX_COLS * GFX_CHAR_W, 0xFF);
				gfx_textn(fb, 1 + i, 0, name + off, GFX_COLS, 1);
			} else {
				gfx_textn(fb, 1 + i, 0, name, GFX_COLS, 0);
			}
		}
		if (count > UI_OLED_LIST_ROWS) {
			const int track = GFX_H - 8;
			int th = track * UI_OLED_LIST_ROWS / count;
			int ty;

			if (th < 3)
				th = 3;
			ty = 8 + (track - th) * oled_top / (count - UI_OLED_LIST_ROWS);
			gfx_vline(fb, GFX_W - 1, ty, ty + th);
		}
	}
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void draw_log(int first, int last)
{
	ui_level_t level;
	int n = last - first + 1, i, start;

	start = log_count > n ? log_count - n : 0;
	for (i = start; i < log_count; i++) {
		const char *s = log_line(i, &level);

		tui_line(first + i - start, level_attr[level], "  %s", s);
	}
}

static void draw_loading(uint8_t *fb)
{
	const int rows = tui_rows(), cols = tui_cols();
	char buf[TUI_MAX_COLS + 1], done[16], total[16], name[UI_NAME_LEN];
	uint32_t pct = percent(ui.load_done, ui.rom_size);
	int w = cols - 32, fill, i;

	if (w < 10)
		w = 10;
	fill = (int)((uint64_t)w * pct / 100);

	tui_line_lr(0, TUI_BAR, TITLE, "Loading ");
	tui_line(2, TUI_HEADER, "  Loading %s", ui.rom_name);
	buf[0] = '[';
	for (i = 0; i < w; i++)
		buf[1 + i] = i < fill ? '#' : '.';
	buf[1 + w] = ']';
	buf[2 + w] = '\0';
	fmt_size(done, sizeof(done), ui.load_done);
	fmt_size(total, sizeof(total), ui.rom_size);
	tui_line(4, TUI_NORMAL, "  %s %3lu%%  %s / %s", buf, (unsigned long)pct, done, total);
	draw_log(6, rows - 3);
	tui_line(rows - 1, TUI_BAR, " Loading the ROM into DDR...");

	snprintf(buf, sizeof(buf), "%lu%%", (unsigned long)pct);
	oled_header(fb, "LOADING", buf);
	ui_base_name(ui.rom_name, name, sizeof(name));
	gfx_textn(fb, 1, 0, name, GFX_COLS, 0);
	gfx_progress(fb, 2, ui.load_done, ui.rom_size);
	snprintf(buf, sizeof(buf), "%lu / %lu MB", (unsigned long)(ui.load_done >> 20), (unsigned long)((ui.rom_size + (1u << 20) - 1) >> 20));
	gfx_text(fb, 3, 0, buf, 0);
}

// ---------------------------------------------------------------------------
// Game running
// ---------------------------------------------------------------------------

static void draw_game(uint32_t now, uint8_t *fb)
{
	const int rows = tui_rows();
	char size[16], name[UI_NAME_LEN];
	const char *msg;
	ui_level_t level;

	tui_line_lr(0, TUI_BAR, TITLE, ui.n64_on ? "N64 ON " : "N64 off ");
	if (ui.dd_status[0])
		tui_line(1, TUI_NORMAL, "  64DD       %s", ui.dd_status);
	tui_line(2, TUI_HEADER, "  Game       %s", ui.title[0] ? ui.title : "(no title in the header)");
	fmt_size(size, sizeof(size), ui.rom_size);
	tui_line(3, TUI_NORMAL, "  ROM        %s  (%s, %s%s)", ui.rom_name, size, ui.rom_format ? ui.rom_format : "?", ui.pal ? ", PAL" : "");
	tui_line(4, TUI_NORMAL, "  CIC        %s", ui.cic ? ui.cic : "?");
	tui_line(5, TUI_NORMAL, "  Save type  %s%s%s", ui.save_type ? ui.save_type : "?", ui.save_name[0] ? ", to " : "", ui.save_name);
	if (ui.n64_on)
		tui_line(6, TUI_GOOD, "  N64        on, playing");
	else
		tui_line(6, TUI_NORMAL, "  N64        off: power it on to play");
	if (ui.save_pending)
		tui_line(7, TUI_WARN, "  Save data  changed, saving after 1 s without writes");
	else if (ui.saves)
		tui_line(7, TUI_GOOD, "  Save data  written to the SD card (%lu time%s)", (unsigned long)ui.saves, ui.saves == 1 ? "" : "s");
	else
		tui_line(7, TUI_NORMAL, "  Save data  not changed yet");
	tui_line(8, ui.underruns ? TUI_WARN : TUI_NORMAL, "  Underruns  %lu", (unsigned long)ui.underruns);
	if (ui.bus_valid) {
		const bus_mon_t *b = &ui.bus;
		char n[5][8], sdat[BUS_MSG_LEN];
		int held;

		tui_line(9, TUI_NORMAL, "  Bus        ALE_H %s ALE_L %s ~READ %s ~WRITE %s ROM %s", count(n[0], b->ale & 0xFFFF),
			 count(n[1], b->ale >> 16), count(n[2], b->rw & 0xFFFF), count(n[3], b->rw >> 16),
			 count(n[4], b->rom & 0xFFFF));
		tui_line(10, (b->rom >> 16) || b->glitch ? TUI_WARN : TUI_NORMAL,
			 "  Addresses  first %08lX  last %08lX  word %08lX  ALE noise %s", (unsigned long)b->first_addr,
			 (unsigned long)ui.last_addr, (unsigned long)b->first_data, count(n[0], b->glitch & 0xFFFF));
		held = bus_sdat(b, sdat, sizeof(sdat));
		tui_line(11, held ? TUI_ERROR : TUI_NORMAL, "  S_DAT      %s", sdat);
	} else {
		tui_line(9, TUI_DIM, "  Bus        nothing yet: the N64 hasn't been turned on");
	}
	tui_line(12, TUI_HEADER, "  Messages");
	draw_log(13, rows - 3);
	tui_line(rows - 1, TUI_BAR, ui.dd_status[0] ? " s save now   e eject disk   m or Esc: back to the menu (N64 off first)   ^L redraw" :
		 " s save now   m or Esc: back to the menu (N64 off first)   ^L redraw");

	ui_base_name(ui.rom_name, name, sizeof(name));
	gfx_fill(fb, 0, 0, GFX_W, 0xFF);
	gfx_textn(fb, 0, 1, ui.title[0] ? ui.title : name, GFX_COLS, 1);
	gfx_text(fb, 1, 0, ui.n64_on ? "N64 ON" : "N64 OFF", 0);
	oled_right(fb, 1, ui.save_pending ? "SAVING" : ui.saves ? "SAVED" : "", 0);
	if ((msg = recent_msg(now, 0xFFFFFFFFu, &level)) != NULL)
		gfx_textn(fb, 2, 0, msg + menu_marquee((int)strlen(msg), GFX_COLS, now - msg_ms), GFX_COLS, 0);
	gfx_text(fb, 3, 0, "C:save       L:menu", 0);
}

void ui_draw(uint32_t now_ms, uint8_t *fb)
{
	if (ui.log_seq != seen_seq) {
		seen_seq = ui.log_seq;
		msg_ms = now_ms;
		msg_dismissed = 0;
	}
	if ((int)ui.screen != prev_screen) {
		prev_screen = (int)ui.screen;
		tui_invalidate();
		prev_sel = -1;
	}
	if (ui.menu.sel != prev_sel) {
		prev_sel = ui.menu.sel;
		sel_ms = now_ms;
	}

	tui_begin();
	gfx_clear(fb);
	switch (ui.screen) {
	case UI_MENU:
		draw_menu(now_ms, fb);
		break;
	case UI_LOADING:
		draw_loading(fb);
		break;
	case UI_GAME:
		draw_game(now_ms, fb);
		break;
	}
	tui_end();
}
