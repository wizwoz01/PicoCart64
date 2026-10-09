/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Full screen text UI over a serial line, for any ANSI/VT100 terminal
 * (picocom, minicom, screen, PuTTY, Tera Term...). Platform independent.
 *
 * Each frame is built line by line between tui_begin() and tui_end(), which
 * sends only the lines that changed, so moving the cursor costs a few bytes
 * even at 115200 baud.
 */

#pragma once

#include <stdint.h>

#define TUI_MAX_ROWS 60
#define TUI_MAX_COLS 200
#define TUI_MIN_ROWS 12
#define TUI_MIN_COLS 40

typedef enum {
	TUI_NORMAL,
	TUI_BAR,					// title and key bars
	TUI_SELECTED,
	TUI_HEADER,
	TUI_DIM,
	TUI_GOOD,
	TUI_WARN,
	TUI_ERROR,
} tui_attr_t;

typedef void (*tui_write_fn)(const char *buf, int len);

// Starts at 24x80 and clears the terminal on the first frame
void tui_init(tui_write_fn wr);
int tui_rows(void);
int tui_cols(void);

// Asks the terminal for its size. The answer comes back as TUI_KEY_SIZE.
void tui_query_size(void);
void tui_resize(int rows, int cols);

// Clears and redraws the whole terminal on the next tui_end()
void tui_invalidate(void);

void tui_begin(void);
// Sets the text of a line. Control and non-ASCII characters show as '?'.
void tui_line(int row, tui_attr_t attr, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
// Line with `right` aligned to the right edge (dropped if it does not fit)
void tui_line_lr(int row, tui_attr_t attr, const char *left, const char *right);
void tui_end(void);

// Keys decoded from the terminal: printable ASCII as is, plus these
enum {
	TUI_KEY_NONE = 0,
	TUI_KEY_UP = 0x100,
	TUI_KEY_DOWN,
	TUI_KEY_LEFT,
	TUI_KEY_RIGHT,
	TUI_KEY_PGUP,
	TUI_KEY_PGDN,
	TUI_KEY_HOME,
	TUI_KEY_END,
	TUI_KEY_ENTER,
	TUI_KEY_BACKSPACE,
	TUI_KEY_ESC,
	TUI_KEY_REDRAW,				// Ctrl-L
	TUI_KEY_SIZE,				// size report, see tui_keydec_t.param
};

typedef struct {
	int state;
	int param[2];
	int nparam;
	int last_cr;
} tui_keydec_t;

void tui_key_init(tui_keydec_t * d);
// Feeds one received byte. Returns a key, or TUI_KEY_NONE if more bytes are needed.
// For TUI_KEY_SIZE, param[0] and param[1] hold the rows and columns.
int tui_key_feed(tui_keydec_t * d, uint8_t c);
// Call when no byte arrived for ~50 ms: a lone ESC becomes TUI_KEY_ESC
int tui_key_idle(tui_keydec_t * d);
