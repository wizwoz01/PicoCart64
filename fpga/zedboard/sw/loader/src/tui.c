/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Full screen text UI over a serial line.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tui.h"

typedef struct {
	char text[TUI_MAX_COLS + 1];
	uint8_t attr;
	uint8_t valid;
} tui_row_t;

static tui_write_fn tui_wr;
static tui_row_t shown[TUI_MAX_ROWS];	// what the terminal shows
static tui_row_t frame[TUI_MAX_ROWS];	// what is being built
static int rows = 24, cols = 80;
static int full = 1;

static const char *const sgr[] = {
	[TUI_NORMAL] = "\033[0m",
	[TUI_BAR] = "\033[0;1;37;44m",
	[TUI_SELECTED] = "\033[0;30;46m",
	[TUI_HEADER] = "\033[0;1m",
	[TUI_DIM] = "\033[0;2m",
	[TUI_GOOD] = "\033[0;32m",
	[TUI_WARN] = "\033[0;33m",
	[TUI_ERROR] = "\033[0;1;31m",
};

static void put(const char *s)
{
	tui_wr(s, (int)strlen(s));
}

void tui_init(tui_write_fn wr)
{
	tui_wr = wr;
	rows = 24;
	cols = 80;
	full = 1;
}

int tui_rows(void)
{
	return rows;
}

int tui_cols(void)
{
	return cols;
}

void tui_query_size(void)
{
	// Save the cursor, move it as far as it goes, report its position, restore
	put("\0337\033[999;999H\033[6n\0338");
}

void tui_resize(int r, int c)
{
	if (r < TUI_MIN_ROWS || c < TUI_MIN_COLS)
		return;
	if (r > TUI_MAX_ROWS)
		r = TUI_MAX_ROWS;
	if (c > TUI_MAX_COLS)
		c = TUI_MAX_COLS;
	if (r != rows || c != cols) {
		rows = r;
		cols = c;
		full = 1;
	}
}

void tui_invalidate(void)
{
	full = 1;
}

void tui_begin(void)
{
	int r;

	for (r = 0; r < rows; r++) {
		frame[r].text[0] = '\0';
		frame[r].attr = TUI_NORMAL;
	}
}

static void set_line(int row, tui_attr_t attr, const char *s)
{
	char *d;
	int n;

	if (row < 0 || row >= rows)
		return;
	d = frame[row].text;
	for (n = 0; n < cols && s[n]; n++) {
		unsigned char c = (unsigned char)s[n];

		d[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
	}
	d[n] = '\0';
	frame[row].attr = (uint8_t)attr;
}

void tui_line(int row, tui_attr_t attr, const char *fmt, ...)
{
	char buf[TUI_MAX_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	set_line(row, attr, buf);
}

void tui_line_lr(int row, tui_attr_t attr, const char *left, const char *right)
{
	char buf[TUI_MAX_COLS + 1];
	int l = (int)strlen(left), r = (int)strlen(right);

	if (l > cols)
		l = cols;
	memcpy(buf, left, (size_t)l);
	if (r > 0 && l + 1 + r <= cols) {
		memset(buf + l, ' ', (size_t)(cols - l - r));
		memcpy(buf + cols - r, right, (size_t)r);
		l = cols;
	}
	buf[l] = '\0';
	set_line(row, attr, buf);
}

void tui_end(void)
{
	char buf[TUI_MAX_COLS + 48];
	int r;

	if (full) {
		// Reset colors, hide the cursor, no auto-wrap, clear
		put("\033[0m\033[?25l\033[?7l\033[2J");
		for (r = 0; r < TUI_MAX_ROWS; r++)
			shown[r].valid = 0;
		full = 0;
	}

	for (r = 0; r < rows; r++) {
		tui_row_t *f = &frame[r], *s = &shown[r];
		int n, len;

		if (s->valid && s->attr == f->attr && strcmp(s->text, f->text) == 0)
			continue;

		n = snprintf(buf, sizeof(buf), "\033[%d;1H%s%s", r + 1, sgr[f->attr], f->text);
		len = (int)strlen(f->text);
		if (f->attr == TUI_BAR || f->attr == TUI_SELECTED) {
			// Pad so the background covers the line
			for (; len < cols; len++)
				buf[n++] = ' ';
			memcpy(buf + n, "\033[0m", 4);
			n += 4;
		} else {
			memcpy(buf + n, "\033[0m\033[K", 7);
			n += 7;
		}
		tui_wr(buf, n);
		*s = *f;
		s->valid = 1;
	}
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

enum { K_NORMAL, K_ESC, K_CSI, K_SS3 };

void tui_key_init(tui_keydec_t *d)
{
	memset(d, 0, sizeof(*d));
}

static int csi_final(tui_keydec_t *d, uint8_t c)
{
	int p0 = d->nparam > 0 ? d->param[0] : 0;

	switch (c) {
	case 'A':
		return TUI_KEY_UP;
	case 'B':
		return TUI_KEY_DOWN;
	case 'C':
		return TUI_KEY_RIGHT;
	case 'D':
		return TUI_KEY_LEFT;
	case 'H':
		return TUI_KEY_HOME;
	case 'F':
		return TUI_KEY_END;
	case 'R':
		return d->nparam == 2 ? TUI_KEY_SIZE : TUI_KEY_NONE;
	case '~':
		switch (p0) {
		case 1:
		case 7:
			return TUI_KEY_HOME;
		case 4:
		case 8:
			return TUI_KEY_END;
		case 5:
			return TUI_KEY_PGUP;
		case 6:
			return TUI_KEY_PGDN;
		default:
			return TUI_KEY_NONE;
		}
	default:
		return TUI_KEY_NONE;
	}
}

int tui_key_feed(tui_keydec_t *d, uint8_t c)
{
	int cr = d->last_cr;

	d->last_cr = 0;
	switch (d->state) {
	case K_ESC:
		if (c == '[') {
			d->state = K_CSI;
			d->nparam = 0;
			d->param[0] = d->param[1] = 0;
			return TUI_KEY_NONE;
		}
		if (c == 'O') {
			d->state = K_SS3;
			return TUI_KEY_NONE;
		}
		d->state = K_NORMAL;
		if (c == 0x1B) {
			d->state = K_ESC;
			return TUI_KEY_ESC;
		}
		break;				// Alt+key: treat as the key alone

	case K_CSI:
		if (c >= '0' && c <= '9') {
			if (d->nparam == 0)
				d->nparam = 1;
			if (d->param[d->nparam - 1] < 10000)
				d->param[d->nparam - 1] = d->param[d->nparam - 1] * 10 + (c - '0');
			return TUI_KEY_NONE;
		}
		if (c == ';') {
			if (d->nparam == 0)
				d->nparam = 1;
			if (d->nparam < 2)
				d->nparam++;
			return TUI_KEY_NONE;
		}
		if (c >= 0x20 && c < 0x40)
			return TUI_KEY_NONE;	// other parameter or intermediate bytes
		d->state = K_NORMAL;
		return csi_final(d, c);

	case K_SS3:
		d->state = K_NORMAL;
		switch (c) {
		case 'A':
			return TUI_KEY_UP;
		case 'B':
			return TUI_KEY_DOWN;
		case 'C':
			return TUI_KEY_RIGHT;
		case 'D':
			return TUI_KEY_LEFT;
		case 'H':
			return TUI_KEY_HOME;
		case 'F':
			return TUI_KEY_END;
		case 'M':
			return TUI_KEY_ENTER;	// keypad Enter
		default:
			return TUI_KEY_NONE;
		}

	default:
		break;
	}

	switch (c) {
	case 0x1B:
		d->state = K_ESC;
		return TUI_KEY_NONE;
	case '\r':
		d->last_cr = 1;
		return TUI_KEY_ENTER;
	case '\n':
		return cr ? TUI_KEY_NONE : TUI_KEY_ENTER;	// CR LF is one Enter
	case '\b':
	case 0x7F:
		return TUI_KEY_BACKSPACE;
	case 0x0C:
		return TUI_KEY_REDRAW;
	default:
		return (c >= 0x20 && c < 0x7F) ? c : TUI_KEY_NONE;
	}
}

int tui_key_idle(tui_keydec_t *d)
{
	int was_esc = d->state == K_ESC;

	d->state = K_NORMAL;
	d->last_cr = 0;
	return was_esc ? TUI_KEY_ESC : TUI_KEY_NONE;
}
