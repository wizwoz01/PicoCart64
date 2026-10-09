/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ZedBoard N64 ROM loader (Vitis standalone application).
 *
 * Shows the ROMs on the SD card in a full screen terminal UI on the USB-UART
 * and on the board's OLED, so a game can be picked from the terminal or with
 * the push buttons. Loads the chosen ROM into DDR, restores its .sra save into
 * the cart SRAM, enables the cart and writes the save back to the SD card
 * whenever the game changes it. After the N64 is turned on, it reports from
 * the cart's bus monitor how far the boot got.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xparameters.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xuartps_hw.h"
#include "xtime_l.h"
#include "sleep.h"
#include "ff.h"

#include "bus_diag.h"
#include "cart_hw.h"
#include "cic.h"
#include "menu.h"
#include "oled_gfx.h"
#include "rom_util.h"
#include "save_db.h"
#include "tui.h"
#include "ui.h"

#define ROM_DIR          "0:/"
#define LAST_FILE        "0:/n64last.txt"
#define CFG_FILE         "0:/n64cfg.txt"
#define MAX_ROMS         256
#define PATH_LEN         (UI_NAME_LEN + 8)
#define LOAD_CHUNK       (1u << 20)
#define POLL_MS          10
#define SAVE_QUIET_MS    1000		// write a save after 1 s without writes from the game
#define BOOT_MS          2000		// the boot reads are over this long after power on
#define STALL_MS         3000		// no cart access this long: the N64 stopped
#define REPEAT_DELAY_MS  400		// held buttons repeat after this...
#define REPEAT_MS        120		// ...this often
#define OLED_SLEEP_MS    (5u * 60 * 1000)	// OLED off after 5 minutes without activity
#define TERM_IDLE_MS     30000		// a key after this long redraws the terminal
#define KEY_QUEUE        32
#define KEY_FLIP         0x300		// BTNL + BTNR

static FATFS fs;
static ui_rom_t roms[MAX_ROMS];
static uint8_t save_buf[CART_SRAM_SIZE];

static uint8_t oled_fb[GFX_FB_SIZE], oled_shown[GFX_FB_SIZE];
static int oled_flip, oled_display_on, oled_stale;
static uint32_t activity_ms, seen_log_seq;

static tui_keydec_t keydec;
static uint32_t uart_rx_ms;
static int keys[KEY_QUEUE], key_head, key_tail;
static uint32_t btn_down_ms[5], btn_rep_ms[5];
static int btn_chord;

static uint32_t cart_rd(uint32_t reg)
{
	return Xil_In32(CART_BASE + reg);
}

static void cart_wr(uint32_t reg, uint32_t val)
{
	Xil_Out32(CART_BASE + reg, val);
}

static uint32_t now_ms(void)
{
	XTime t;

	XTime_GetTime(&t);
	return (uint32_t)(t / (COUNTS_PER_SECOND / 1000));
}

static void uart_write(const char *buf, int len)
{
	while (len-- > 0)
		XUartPs_SendByte(STDOUT_BASEADDRESS, (u8) * buf++);
}

// ---------------------------------------------------------------------------
// OLED (SSD1306 behind oled_spi.v)
// ---------------------------------------------------------------------------

static void oled_wait(void)
{
	int n = 100000;

	while ((cart_rd(CART_REG_OLED_CTRL) & CART_OLED_BUSY) && --n)
		;
}

static void oled_cmd(uint8_t c)
{
	oled_wait();
	cart_wr(CART_REG_OLED_DATA, c);
}

static void oled_cmds(const uint8_t *c, int n)
{
	while (n-- > 0)
		oled_cmd(*c++);
}

// Column and row order. Flipped turns the picture by 180 degrees.
static void oled_orient(void)
{
	oled_cmd(oled_flip ? 0xA1 : 0xA0);
	oled_cmd(oled_flip ? 0xC8 : 0xC0);
	oled_stale = 1;				// the column order applies to new data only
}

static void oled_push(void)
{
	static const uint8_t window[] = { 0x21, 0, GFX_W - 1, 0x22, 0, GFX_H / 8 - 1 };
	uint32_t w;
	int i;

	if (!oled_stale && memcmp(oled_fb, oled_shown, sizeof(oled_fb)) == 0)
		return;
	oled_wait();
	for (i = 0; i < GFX_FB_SIZE; i += 4) {
		memcpy(&w, oled_fb + i, 4);	// byte i in bits [7:0]
		cart_wr(CART_OLED_FB + i, w);
	}
	oled_cmds(window, sizeof(window));
	oled_wait();
	cart_wr(CART_REG_OLED_FLUSH, 1);
	memcpy(oled_shown, oled_fb, sizeof(oled_fb));
	oled_stale = 0;
}

static void oled_set_display(int on)
{
	if (on != oled_display_on) {
		oled_cmd(on ? 0xAF : 0xAE);
		oled_display_on = on;
	}
}

// SSD1306 power-up: VDD, reset, settings, VBAT, 100 ms, display on
static void oled_init(void)
{
	static const uint8_t setup[] = {
		0xAE,					// display off
		0xD5, 0x80,				// clock
		0xA8, GFX_H - 1,		// 32 rows
		0xD3, 0x00,				// no vertical offset
		0x40,					// start line 0
		0x8D, 0x14,				// charge pump on
		0xD9, 0xF1,				// pre-charge period
		0xDA, 0x02,				// sequential COM pins
		0xDB, 0x40,				// VCOMH level
		0x81, 0x8F,				// contrast
		0x20, 0x00,				// horizontal addressing
		0xA4,					// show the RAM contents
		0xA6,					// not inverted
	};

	if (cart_rd(CART_REG_OLED_CTRL) & CART_OLED_VDD) {
		// Left on by a previous run: VBAT off first, then VDD
		oled_cmd(0xAE);
		oled_wait();
		cart_wr(CART_REG_OLED_CTRL, CART_OLED_VDD | CART_OLED_RUN);
		usleep(100 * 1000);
	}
	cart_wr(CART_REG_OLED_CTRL, 0);
	usleep(10 * 1000);
	cart_wr(CART_REG_OLED_CTRL, CART_OLED_VDD);
	usleep(1000);
	cart_wr(CART_REG_OLED_CTRL, CART_OLED_VDD | CART_OLED_RUN);
	usleep(1000);
	oled_cmds(setup, sizeof(setup));
	oled_orient();
	gfx_clear(oled_fb);
	oled_push();
	cart_wr(CART_REG_OLED_CTRL, CART_OLED_VDD | CART_OLED_VBAT | CART_OLED_RUN);
	usleep(100 * 1000);
	oled_display_on = 0;
	oled_set_display(1);
}

// ---------------------------------------------------------------------------
// Settings and last game on the SD card
// ---------------------------------------------------------------------------

static void read_text(const char *path, char *out, int len)
{
	FIL f;
	UINT br = 0;

	out[0] = '\0';
	if (f_open(&f, path, FA_READ) != FR_OK)
		return;
	f_read(&f, out, len - 1, &br);
	f_close(&f);
	out[br] = '\0';
}

static void write_text(const char *path, const char *s)
{
	FIL f;
	UINT bw;

	if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return;
	f_write(&f, s, strlen(s), &bw);
	f_close(&f);
}

static void read_cfg(void)
{
	char buf[64];

	read_text(CFG_FILE, buf, sizeof(buf));
	oled_flip = strstr(buf, "oled_flip=1") != NULL;
}

static void toggle_flip(void)
{
	oled_flip = !oled_flip;
	oled_orient();
	write_text(CFG_FILE, oled_flip ? "oled_flip=1\n" : "oled_flip=0\n");
	ui_log(UI_INFO, "OLED turned %s", oled_flip ? "180 degrees" : "back");
}

// ---------------------------------------------------------------------------
// Input: terminal keys and push buttons in one queue
// ---------------------------------------------------------------------------

static void key_push(int k)
{
	int next = (key_head + 1) % KEY_QUEUE;

	if (next != key_tail) {
		keys[key_head] = k;
		key_head = next;
	}
}

static void poll_uart(uint32_t now)
{
	int k;

	while (XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
		uint8_t c = XUartPs_RecvByte(STDIN_BASEADDRESS);

		// A terminal may have been connected since the last frame
		if (now - uart_rx_ms > TERM_IDLE_MS) {
			tui_invalidate();
			tui_query_size();
		}
		uart_rx_ms = now;
		k = tui_key_feed(&keydec, c);
		if (k == TUI_KEY_SIZE) {
			tui_resize(keydec.param[0], keydec.param[1]);
		} else if (k == TUI_KEY_REDRAW) {
			tui_invalidate();
			tui_query_size();
		} else if (k != TUI_KEY_NONE) {
			key_push(k);
		}
	}
	if (now - uart_rx_ms > 50 && (k = tui_key_idle(&keydec)) != TUI_KEY_NONE)
		key_push(k);
}

static void poll_buttons(uint32_t now)
{
	const uint32_t lr = CART_BTN_L | CART_BTN_R;
	uint32_t b = cart_rd(CART_REG_BUTTONS);
	uint32_t held = b & CART_BTN_ALL;
	uint32_t pressed = (b >> CART_BTN_PRESSED_SHIFT) & CART_BTN_ALL;
	int i;

	if (pressed)
		cart_wr(CART_REG_BUTTONS, pressed << CART_BTN_PRESSED_SHIFT);

	// BTNL + BTNR together turn the OLED, then wait until both are released
	if ((held & lr) == lr) {
		if (!btn_chord)
			key_push(KEY_FLIP);
		btn_chord = 1;
		return;
	}
	if (btn_chord) {
		if (!(held & lr))
			btn_chord = 0;
		return;
	}

	for (i = 0; i < 5; i++) {
		uint32_t m = 1u << i;

		if (pressed & m) {
			key_push(UI_KEY_BTN_C + i);
			btn_down_ms[i] = btn_rep_ms[i] = now;
		} else if ((held & m) && m != CART_BTN_C && now - btn_down_ms[i] >= REPEAT_DELAY_MS && now - btn_rep_ms[i] >= REPEAT_MS) {
			key_push(UI_KEY_BTN_C + i);
			btn_rep_ms[i] = now;
		}
	}
}

// Next key for the current screen. Handles the keys that work everywhere.
static int next_key(uint32_t now)
{
	while (key_tail != key_head) {
		int k = keys[key_tail];
		int button = k >= UI_KEY_BTN_C;

		key_tail = (key_tail + 1) % KEY_QUEUE;
		activity_ms = now;
		// While the OLED sleeps, a button only wakes it; a press also clears a message
		if (button && !oled_display_on)
			continue;
		if (button && k != KEY_FLIP && ui_toast_active(now)) {
			ui_toast_dismiss();
			continue;
		}
		if (k == 'f' || k == 'F' || k == KEY_FLIP) {
			toggle_flip();
			continue;
		}
		return k;
	}
	return TUI_KEY_NONE;
}

static void poll_input(uint32_t now)
{
	ui.n64_on = (cart_rd(CART_REG_STATUS) & CART_STATUS_N64_ON) != 0;
	poll_uart(now);
	poll_buttons(now);
}

// Redraws the terminal and the OLED. The OLED goes dark after a while without
// keys, buttons or messages.
static void refresh(uint32_t now)
{
	int awake;

	if (ui.log_seq != seen_log_seq) {
		seen_log_seq = ui.log_seq;
		activity_ms = now;
	}
	ui_draw(now, oled_fb);
	awake = now - activity_ms < OLED_SLEEP_MS;
	if (awake)
		oled_push();
	oled_set_display(awake);
}

// ---------------------------------------------------------------------------
// ROM list
// ---------------------------------------------------------------------------

static int rom_cmp(const void *a, const void *b)
{
	const char *x = ((const ui_rom_t *)a)->name, *y = ((const ui_rom_t *)b)->name;

	for (;; x++, y++) {
		int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x;
		int cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;

		if (cx != cy || cx == 0)
			return cx - cy;
	}
}

// Reads the ROM list and keeps the selection on `keep` if it is still there
static void scan_roms(int remount, const char *keep)
{
	static char last[UI_NAME_LEN];
	DIR dir;
	FILINFO fno;
	int n = 0, i, sel;

	if (remount || !ui.sd_ok) {
		int flip = oled_flip;

		f_mount(NULL, "0:/", 0);
		ui.sd_ok = f_mount(&fs, "0:/", 1) == FR_OK;
		if (ui.sd_ok)
			read_cfg();
		if (oled_flip != flip)
			oled_orient();
	}
	if (ui.sd_ok && f_opendir(&dir, ROM_DIR) == FR_OK) {
		while (n < MAX_ROMS && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
			if ((fno.fattrib & AM_DIR) || !rom_has_rom_extension(fno.fname))
				continue;
			if (strlen(fno.fname) >= UI_NAME_LEN)
				continue;
			strcpy(roms[n].name, fno.fname);
			roms[n].size = (uint32_t)fno.fsize;
			n++;
		}
		f_closedir(&dir);
	} else {
		ui.sd_ok = 0;
	}
	qsort(roms, n, sizeof(roms[0]), rom_cmp);

	read_text(LAST_FILE, last, sizeof(last));
	last[strcspn(last, "\r\n")] = '\0';
	ui.roms = roms;
	ui.rom_count = n;
	ui.last_idx = -1;
	sel = -1;
	for (i = 0; i < n; i++) {
		if (strcmp(roms[i].name, last) == 0)
			ui.last_idx = i;
		if (keep && strcmp(roms[i].name, keep) == 0)
			sel = i;
	}
	menu_init(&ui.menu, n, sel >= 0 ? sel : ui.last_idx);
}

// Mounts the SD card again, for a card that was swapped or inserted late
static void rescan(void)
{
	static char keep[UI_NAME_LEN];

	strcpy(keep, ui.rom_count ? roms[ui.menu.sel].name : "");
	scan_roms(1, keep);
	if (ui.sd_ok)
		ui_log(UI_INFO, "Found %d ROM%s", ui.rom_count, ui.rom_count == 1 ? "" : "s");
	else
		ui_log(UI_ERROR, "Cannot read the SD card");
}

// Returns the index of the chosen ROM
static int choose_rom(void)
{
	static char keep[UI_NAME_LEN];
	int key;

	keep[0] = '\0';
	if (ui.rom_count > 0)
		strcpy(keep, roms[ui.menu.sel].name);
	scan_roms(0, keep);
	ui.screen = UI_MENU;

	for (;;) {
		uint32_t now = now_ms();

		poll_input(now);
		while ((key = next_key(now)) != TUI_KEY_NONE) {
			switch (key) {
			case TUI_KEY_UP:
			case UI_KEY_BTN_U:
				menu_move(&ui.menu, -1);
				break;
			case TUI_KEY_DOWN:
			case UI_KEY_BTN_D:
				menu_move(&ui.menu, 1);
				break;
			case TUI_KEY_PGUP:
			case TUI_KEY_LEFT:
				menu_move(&ui.menu, -ui_list_height());
				break;
			case TUI_KEY_PGDN:
			case TUI_KEY_RIGHT:
				menu_move(&ui.menu, ui_list_height());
				break;
			case UI_KEY_BTN_L:
				menu_move(&ui.menu, -UI_OLED_LIST_ROWS);
				break;
			case UI_KEY_BTN_R:
				menu_move(&ui.menu, UI_OLED_LIST_ROWS);
				break;
			case TUI_KEY_HOME:
				menu_move(&ui.menu, -ui.rom_count);
				break;
			case TUI_KEY_END:
				menu_move(&ui.menu, ui.rom_count);
				break;
			case 'r':
			case 'R':
				rescan();
				break;
			case TUI_KEY_ENTER:
			case UI_KEY_BTN_C:
				if (ui.rom_count == 0) {
					rescan();
					break;
				}
				if (ui.n64_on) {
					ui_log(UI_WARN, "Turn the N64 off before loading a ROM.");
					break;
				}
				return ui.menu.sel;
			default:
				if (key >= '0' && key <= '9')
					menu_digit(&ui.menu, key - '0', now);
				break;
			}
		}
		menu_tick(&ui.menu, now);
		refresh(now);
		usleep(POLL_MS * 1000);
	}
}

// ---------------------------------------------------------------------------
// Loading and saving
// ---------------------------------------------------------------------------

// One save memory of the cart, kept in sync with a file next to the ROM
typedef struct {
	const char *ext;
	uint32_t window;			// its contents in the register space
	uint32_t max;				// size of the window
	uint32_t dirty;				// STATUS and CLEAR bit
	uint8_t blank;				// contents without a save file
	uint32_t size;				// bytes in the file
	int used;					// kept on the SD card
	int pending;
	uint32_t dirty_ms;
	char path[PATH_LEN];
	char name[UI_NAME_LEN];
} save_mem_t;

static save_mem_t sram = {.ext = ".sra",.window = CART_SRAM,.max = CART_SRAM_SIZE,.dirty = CART_STATUS_DIRTY,.blank = 0x00 };
static save_mem_t eeprom = {.ext = ".eep",.window = CART_EEPROM,.max = CART_EEPROM_SIZE,.dirty = CART_STATUS_EEP_DIRTY,.blank = 0xFF };
static save_type_t save_type;
static cic_t cic;
static uint32_t rom_header;		// first word of the ROM, the first one the N64 reads

static int load_rom(const char *path, const char *name)
{
	uint8_t *dst = (uint8_t *)ROM_DDR_BASE;
	rom_format_t fmt = ROM_FMT_UNKNOWN;
	uint32_t size, off = 0, drawn_ms = 0;
	int unlocked = 0;
	FIL f;
	UINT br;

	ui_log_clear();
	snprintf(ui.rom_name, sizeof(ui.rom_name), "%s", name);
	ui.title[0] = '\0';
	ui.rom_format = NULL;
	ui.rom_size = 0;
	ui.load_done = 0;
	ui.screen = UI_LOADING;

	if (f_open(&f, path, FA_READ) != FR_OK) {
		ui_log(UI_ERROR, "Cannot open %s", name);
		return -1;
	}
	size = f_size(&f);
	if (size < 0x1000 || size > ROM_MAX_SIZE) {
		ui_log(UI_ERROR, "Unsupported ROM size: %lu bytes (max 64 MB)", (unsigned long)size);
		f_close(&f);
		return -1;
	}
	ui.rom_size = size;
	refresh(now_ms());

	while (off < size) {
		uint32_t n = size - off < LOAD_CHUNK ? size - off : LOAD_CHUNK;
		uint32_t now;

		if (f_read(&f, dst + off, n, &br) != FR_OK || br != n) {
			ui_log(UI_ERROR, "Read error at offset %lu", (unsigned long)off);
			f_close(&f);
			return -1;
		}
		if (off == 0) {
			fmt = rom_detect_format(dst);
			if (fmt == ROM_FMT_UNKNOWN) {
				ui_log(UI_ERROR, "Not an N64 ROM (header %02X %02X %02X %02X)", dst[0], dst[1], dst[2], dst[3]);
				f_close(&f);
				return -1;
			}
		}
		rom_to_z64(dst + off, n, fmt);
		off += n;
		ui.load_done = off;
		now = now_ms();
		if (now - drawn_ms >= 200 || off == size) {
			drawn_ms = now;
			refresh(now);
		}
	}
	f_close(&f);

	rom_title(dst, ui.title);
	rom_header = ((uint32_t)dst[0] << 24) | ((uint32_t)dst[1] << 16) | ((uint32_t)dst[2] << 8) | dst[3];
	ui.rom_format = rom_format_name(fmt);
	ui.pal = rom_is_pal(dst);
	cic = cic_detect(dst);
	save_type = save_type_detect(dst);
	ui.cic = cic_name(cic);
	ui.save_type = save_type_name(save_type);

	// Region locks hang a game on a console of another region. Removing one
	// changes the boot code, so its checksum in the header changes too.
	if (cic != CIC_UNKNOWN && size >= 0x101000)
		unlocked = rom_unlock_region(dst, size);
	if (unlocked) {
		uint32_t crc[2];
		int i;

		cic_header_crc(dst, cic, crc);
		for (i = 0; i < 8; i++)
			dst[0x10 + i] = (uint8_t)(crc[i / 4] >> (24 - 8 * (i % 4)));
	}

	// HP0 reads DDR directly, past the CPU caches
	Xil_DCacheFlushRange((INTPTR) dst, size);

	ui_log(UI_INFO, "Loaded \"%s\" (%s, %lu KB)", ui.title, ui.rom_format, (unsigned long)(size >> 10));
	ui_log(UI_INFO, "CIC %s, save type %s", ui.cic, ui.save_type);
	if (unlocked)
		ui_log(UI_INFO, "Removed %d region lock%s from the boot code, so the game also starts on this console.",
		       unlocked, unlocked == 1 ? "" : "s");
	if (cic == CIC_UNKNOWN)
		ui_log(UI_WARN, "Unknown boot code: the Pico answers as a 6102, so the game may not start.");
	if (ui.pal)
		ui_log(UI_INFO, "PAL release: on an NTSC N64 some PAL games output 50 Hz video that some TVs can't show.");
	if (save_type == SAVE_FLASHRAM)
		ui_log(UI_WARN, "This game saves to FlashRAM, which the cart doesn't emulate yet: progress won't be kept.");
	else if (save_type == SAVE_UNKNOWN)
		ui_log(UI_INFO, "Game not in the save list: EEPROM 4 Kbit and SRAM are both available.");
	return 0;
}

// Fills a save memory from its file. grow: a bigger file sets the size.
static void save_restore(save_mem_t *s, int maybe, int grow)
{
	uint32_t i;
	FIL f;
	UINT br = 0;

	s->pending = 0;
	memset(save_buf, s->blank, s->max);
	if ((s->used || maybe) && f_open(&f, s->path, FA_READ) == FR_OK) {
		f_read(&f, save_buf, s->max, &br);
		f_close(&f);
		s->used = 1;
		if (grow && br > s->size)
			s->size = (br + 3) & ~3u;
		ui_log(UI_INFO, "Loaded save %s (%u bytes)", s->name, br);
	} else if (s->used) {
		ui_log(UI_INFO, "No save file yet, it will be created as %s", s->name);
	}

	for (i = 0; i < s->max; i += 4)
		cart_wr(s->window + i, sram_pack(save_buf + i));
}

static void save_store(save_mem_t *s)
{
	uint32_t i;
	FIL f;
	UINT bw = 0;

	s->pending = 0;
	for (i = 0; i < s->size; i += 4)
		sram_unpack(cart_rd(s->window + i), save_buf + i);

	if (f_open(&f, s->path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
		ui_log(UI_ERROR, "Could not create %s", s->name);
		return;
	}
	FRESULT wr = f_write(&f, save_buf, s->size, &bw);
	FRESULT cl = f_close(&f);

	if (wr != FR_OK || cl != FR_OK || bw != s->size) {
		ui_log(UI_ERROR, "Could not write %s", s->name);
	} else {
		ui.saves++;
		ui_log(UI_GOOD, "Saved %s", s->name);
	}
}

static void show_save_names(void)
{
	ui.save_name[0] = '\0';
	if (sram.used)
		strcat(ui.save_name, sram.name);
	if (sram.used && eeprom.used)
		strcat(ui.save_name, " and ");
	if (eeprom.used)
		strcat(ui.save_name, eeprom.name);
}

static void eeprom_config(void)
{
	cart_wr(CART_REG_EEPROM, CART_EEPROM_ENABLE | (eeprom.size > 512 ? CART_EEPROM_16K : 0));
}

// Picks the save memories for the game's save type and fills them from the SD card
static int saves_setup(const char *rom_path, const char *rom_name)
{
	int unknown = save_type == SAVE_UNKNOWN;

	if (rom_save_path(rom_path, sram.ext, sram.path, sizeof(sram.path)) != 0 ||
	    rom_save_path(rom_name, sram.ext, sram.name, sizeof(sram.name)) != 0 ||
	    rom_save_path(rom_path, eeprom.ext, eeprom.path, sizeof(eeprom.path)) != 0 ||
	    rom_save_path(rom_name, eeprom.ext, eeprom.name, sizeof(eeprom.name)) != 0)
		return -1;

	sram.size = (save_type == SAVE_SRAM_96K) ? 96u * 1024 : 32u * 1024;
	sram.used = save_type == SAVE_SRAM || save_type == SAVE_SRAM_96K;
	eeprom.size = (save_type == SAVE_EEPROM_16K) ? 2048 : 512;
	eeprom.used = save_type == SAVE_EEPROM_4K || save_type == SAVE_EEPROM_16K;

	// For games not in the list, an existing file shows what they use
	save_restore(&sram, unknown, 1);
	save_restore(&eeprom, unknown, unknown);
	if (eeprom.size > 512)
		eeprom.size = 2048;

	cart_wr(CART_REG_EEPROM, 0);
	if (eeprom.used || unknown)
		eeprom_config();
	show_save_names();
	return 0;
}

// Follows the game's writes to one save memory
static void save_poll(save_mem_t *s, uint32_t status, uint32_t now)
{
	if (status & s->dirty) {
		cart_wr(CART_REG_CLEAR, s->dirty);
		if (!s->used && save_type == SAVE_UNKNOWN) {
			s->used = 1;
			show_save_names();
		}
		if (s->used) {
			s->pending = 1;
			s->dirty_ms = now;
		}
	} else if (s->pending && now - s->dirty_ms >= SAVE_QUIET_MS) {
		save_store(s);
	}
}

static void bus_read(void)
{
	ui.bus.ale = cart_rd(CART_REG_MON_ALE);
	ui.bus.rw = cart_rd(CART_REG_MON_RW);
	ui.bus.first_addr = cart_rd(CART_REG_MON_FIRST);
	ui.bus.first_data = cart_rd(CART_REG_MON_DATA);
	ui.bus.rom = cart_rd(CART_REG_MON_ROM);
	ui.bus.ad_or = cart_rd(CART_REG_MON_AD_OR);
	ui.bus.glitch = cart_rd(CART_REG_MON_GLITCH);
	ui.bus.odd = cart_rd(CART_REG_MON_ODD);
	ui.bus.sdat = cart_rd(CART_REG_MON_SDAT);
	ui.bus.sdat_low = cart_rd(CART_REG_MON_SDAT_LOW);
	ui.bus.sdat_high = (cart_rd(CART_REG_PINS) & CART_PINS_SDAT) != 0;
	ui.last_addr = cart_rd(CART_REG_LAST_ADDR);
}

// Logs which AD input doesn't follow its line in the trace of the boot addresses
static void trace_report(void)
{
	static uint32_t a[BUS_TRACE_LEN], b[BUS_TRACE_LEN];
	static char lines[3][BUS_MSG_LEN];
	int n = (int)(ui.bus.ale >> 16), i, count, clean;

	if (n > BUS_TRACE_LEN)
		n = BUS_TRACE_LEN;
	for (i = 0; i < n; i++) {
		a[i] = cart_rd(CART_TRACE_A + 4u * (uint32_t)i);
		b[i] = cart_rd(CART_TRACE_B + 4u * (uint32_t)i);
	}
	count = bus_trace_check(a, b, n, lines, 3, &clean);
	for (i = 0; i < count; i++)
		ui_log(clean ? UI_INFO : UI_ERROR, "%s", lines[i]);
}

// Transfer n since power on, from the transfer log (only the last 256 are kept)
static void xfer_read(uint32_t n, bus_xfer_t *x)
{
	uint32_t base = CART_XFER + 16u * (n % CART_XFER_LEN);

	x->addr = cart_rd(base);
	x->rw = cart_rd(base + 4);
	x->sum = cart_rd(base + 8);
	x->us = cart_rd(base + 12);
}

// Compares the logged ROM reads with the ROM. Returns how many were checked.
static int xfer_verify(int *bad, bus_xfer_t *first_bad)
{
	uint32_t n = cart_rd(CART_REG_MON_XFER_N), i, exp;
	uint32_t from = n > CART_XFER_LEN ? n - CART_XFER_LEN : 0;
	int checked = 0, r;
	bus_xfer_t x;

	*bad = 0;
	for (i = from; i < n; i++) {
		xfer_read(i, &x);
		r = bus_xfer_check(&x, (const uint8_t *)ROM_DDR_BASE, ui.rom_size, &exp);
		if (r < 0)
			continue;
		checked++;
		if (!r && (*bad)++ == 0)
			*first_bad = x;
	}
	return checked;
}

// Prints the raw address trace, the noise log, the transfer log and the S_DAT
// log on the terminal, for debugging
static void trace_dump(void)
{
	char line[96];
	int n = (int)(ui.bus.ale >> 16), i;
	uint32_t xn, k, exp;
	bus_xfer_t x;

	if (n > BUS_TRACE_LEN)
		n = BUS_TRACE_LEN;
	snprintf(line, sizeof(line), "\r\n\033[0m--- trace, %d entries ---\r\n", n);
	uart_write(line, (int)strlen(line));
	for (i = 0; i < n; i++) {
		snprintf(line, sizeof(line), "%4d %08lX %08lX\r\n", i, (unsigned long)cart_rd(CART_TRACE_A + 4u * (uint32_t)i),
			 (unsigned long)cart_rd(CART_TRACE_B + 4u * (uint32_t)i));
		uart_write(line, (int)strlen(line));
	}
	n = (int)cart_rd(CART_REG_MON_NOISE);
	snprintf(line, sizeof(line), "--- noise, %d events ---\r\n", n);
	uart_write(line, (int)strlen(line));
	for (i = 0; i < n && i < 16; i++) {
		char ev[40];

		bus_noise_event(cart_rd(CART_NOISE + 4u * (uint32_t)i), ev, sizeof(ev));
		snprintf(line, sizeof(line), "%2d %s\r\n", i, ev);
		uart_write(line, (int)strlen(line));
	}
	xn = cart_rd(CART_REG_MON_XFER_N);
	snprintf(line, sizeof(line), "--- transfers, %lu finished, the last %d kept ---\r\n", (unsigned long)xn,
		 xn < CART_XFER_LEN ? (int)xn : CART_XFER_LEN);
	uart_write(line, (int)strlen(line));
	for (k = xn > CART_XFER_LEN ? xn - CART_XFER_LEN : 0; k <= xn; k++) {
		const char *check;
		char bad[24];
		int r;

		if (k < xn) {
			xfer_read(k, &x);
		} else {
			x.addr = cart_rd(CART_REG_MON_XFER);
			x.rw = cart_rd(CART_REG_MON_XFER + 4);
			x.sum = cart_rd(CART_REG_MON_XFER + 8);
			x.us = 0;
		}
		r = bus_xfer_check(&x, (const uint8_t *)ROM_DDR_BASE, ui.rom_size, &exp);
		snprintf(bad, sizeof(bad), "BAD, ROM %08lX", (unsigned long)exp);
		check = r < 0 ? "" : r ? "ok" : bad;
		snprintf(line, sizeof(line), "%6lu %7lu us %08lX r%-5lu w%-3lu sum %08lX %s%s\r\n", (unsigned long)k,
			 (unsigned long)x.us, (unsigned long)x.addr, (unsigned long)(x.rw & 0xFFFF), (unsigned long)(x.rw >> 16),
			 (unsigned long)x.sum, check, k == xn ? " (in progress)" : "");
		uart_write(line, (int)strlen(line));
	}
	n = (int)cart_rd(CART_REG_SDAT_LOG_N);
	snprintf(line, sizeof(line), "--- S_DAT, %d events ---\r\n", n);
	uart_write(line, (int)strlen(line));
	for (i = 0, k = 0; i < n && i < CART_SDAT_LOG_LEN; i++) {
		uint32_t e = cart_rd(CART_SDAT_LOG + 4u * (uint32_t)i);
		char ev[64];

		bus_sdat_event(e, i ? k : e, ev, sizeof(ev));
		snprintf(line, sizeof(line), "%4d %s\r\n", i, ev);
		uart_write(line, (int)strlen(line));
		k = e;
	}
	uart_write("--- end ---\r\n", 13);
	tui_invalidate();
}

// Once the N64 stops using the cart, logs when, and whether the data it got
// matched the ROM
static void stall_report(uint32_t after_ms)
{
	bus_xfer_t first;
	int bad, checked = xfer_verify(&bad, &first);
	char msg[UI_LOG_LEN];

	ui_log(UI_WARN, "The N64 stopped using the cart %lu ms after power on.", (unsigned long)after_ms);
	if (bad)
		ui_log(UI_ERROR, "%d of the last %d ROM transfers had wrong data, first at %08lX (%lu words).", bad, checked,
		       (unsigned long)first.addr, (unsigned long)(first.rw & 0xFFFF));
	else
		ui_log(UI_INFO, "The last %d ROM transfers all match the ROM.", checked);

	// A held-low EEPROM line stops the PIF, and then the game at its next
	// controller or save access
	bus_read();
	if (bus_sdat(&ui.bus, msg, sizeof(msg)))
		ui_log(UI_ERROR, "S_DAT (EEPROM line) %s", msg);
	else
		ui_log(UI_INFO, "S_DAT (EEPROM line) %s", msg);
}

// Logs how far the N64 got with the boot reads. monitor: PINS showed COLD_RESET
// high while the N64 was on; bitstreams without the bus monitor read 0 there.
static void bus_report(int monitor)
{
	static const ui_level_t level[] = {[BUS_OK] = UI_GOOD,[BUS_STOPPED] = UI_WARN,[BUS_FAULT] = UI_ERROR };
	char msg[UI_LOG_LEN];
	bus_verdict_t v;

	if (!monitor) {
		ui_log(UI_WARN, "This bitstream has no bus monitor, so the boot can't be checked. Rebuild it.");
		return;
	}
	bus_read();
	v = bus_diagnose(&ui.bus, rom_header, msg, sizeof(msg));
	ui_log(level[v], "%s", msg);
	if (bus_noise(&ui.bus, msg, sizeof(msg)))
		ui_log(UI_WARN, "%s", msg);
	if (v != BUS_OK)
		trace_report();
}

// Runs until the user asks for the menu with the N64 off
static void run_game(void)
{
	uint32_t base_underruns = cart_rd(CART_REG_UNDERRUNS);
	uint32_t underrun_ms = 0, on_ms = 0, xfers = 0, xfer_ms = 0;
	int was_on = -1, report_due = 0, monitor = 0, stalled = 0;
	int key;

	ui.saves = 0;
	ui.save_pending = 0;
	ui.underruns = 0;
	ui.bus_valid = 0;
	ui.screen = UI_GAME;
	cart_wr(CART_REG_CLEAR, CART_CLEAR_DIRTY | CART_CLEAR_UNDERRUN | CART_CLEAR_EEP_DIRTY);
	cart_wr(CART_REG_CIC, cic_pico_code(cic));
	cart_wr(CART_REG_CTRL, CART_CTRL_ENABLE);
	ui_log(UI_GOOD, "Cart enabled. Power on the N64.");

	for (;;) {
		uint32_t now = now_ms();
		uint32_t status = cart_rd(CART_REG_STATUS);
		uint32_t u = cart_rd(CART_REG_UNDERRUNS) - base_underruns;
		int on = (status & CART_STATUS_N64_ON) != 0;

		if (on != was_on) {
			if (was_on >= 0)
				ui_log(UI_INFO, on ? "N64 on" : "N64 off");
			if (on) {
				on_ms = now;
				report_due = 1;
				ui.bus_valid = 1;
				xfers = 0;
				xfer_ms = now;
				stalled = 0;
			}
			was_on = on;
		}
		// Note when the N64 stops using the cart for a while
		if (on && monitor) {
			uint32_t x = cart_rd(CART_REG_MON_XFER_N);

			if (x != xfers) {
				xfers = x;
				xfer_ms = now;
				stalled = 0;
			} else if (x && !stalled && now - xfer_ms >= STALL_MS) {
				stall_report(xfer_ms - on_ms);
				stalled = 1;
			}
		}
		if (on && (cart_rd(CART_REG_PINS) & CART_PINS_COLD_RESET))
			monitor = 1;
		if (report_due && (!on || now - on_ms >= BOOT_MS)) {
			bus_report(monitor);
			report_due = 0;
		} else if (ui.bus_valid) {
			bus_read();
		}

		// At most one message a second
		if (u != ui.underruns && now - underrun_ms >= 1000) {
			ui_log(UI_WARN, "%lu ROM words were late (underrun)", (unsigned long)(u - ui.underruns));
			ui.underruns = u;
			underrun_ms = now;
		}

		save_poll(&sram, status, now);
		save_poll(&eeprom, status, now);
		ui.save_pending = sram.pending || eeprom.pending;

		poll_input(now);
		while ((key = next_key(now)) != TUI_KEY_NONE) {
			switch (key) {
			case 's':
			case 'S':
			case UI_KEY_BTN_C:
				if (sram.used)
					save_store(&sram);
				if (eeprom.used)
					save_store(&eeprom);
				if (!sram.used && !eeprom.used)
					ui_log(UI_INFO, "Nothing to save yet");
				break;
			case 't':
			case 'T':
				trace_dump();
				break;
			case 'm':
			case 'M':
			case TUI_KEY_ESC:
			case UI_KEY_BTN_L:
				if (on) {
					ui_log(UI_WARN, "Turn the N64 off before going back to the menu.");
					break;
				}
				if (sram.pending)
					save_store(&sram);
				if (eeprom.pending)
					save_store(&eeprom);
				cart_wr(CART_REG_CTRL, 0);
				cart_wr(CART_REG_EEPROM, 0);
				cart_wr(CART_REG_CIC, CIC_CODE_6102);
				return;
			default:
				break;
			}
		}
		ui.save_pending = sram.pending || eeprom.pending;

		refresh(now);
		usleep(POLL_MS * 1000);
	}
}

int main(void)
{
	static char path[PATH_LEN];
	int idx;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (cart_rd(CART_REG_ID) != CART_ID_MAGIC) {
		printf("\r\nPicoCart64 ZedBoard loader\r\n");
		printf("Cart logic not found at 0x%08lX. Is the bitstream loaded?\r\n", (unsigned long)CART_BASE);
		return 1;
	}
	cart_wr(CART_REG_CTRL, 0);
	cart_wr(CART_REG_EEPROM, 0);
	cart_wr(CART_REG_CIC, CIC_CODE_6102);
	cart_wr(CART_REG_ROM_BASE, ROM_DDR_BASE);

	tui_init(uart_write);
	tui_key_init(&keydec);
	tui_query_size();

	ui.sd_ok = f_mount(&fs, "0:/", 1) == FR_OK;
	if (ui.sd_ok)
		read_cfg();
	oled_init();
	activity_ms = now_ms();

	for (;;) {
		idx = choose_rom();
		snprintf(path, sizeof(path), "%s%s", ROM_DIR, roms[idx].name);
		if (load_rom(path, roms[idx].name) != 0)
			continue;
		write_text(LAST_FILE, roms[idx].name);
		if (saves_setup(path, roms[idx].name) != 0) {
			ui_log(UI_ERROR, "File name too long");
			continue;
		}
		run_game();
	}
}
