/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ZedBoard N64 ROM loader (Vitis standalone application).
 *
 * Lists ROMs on the SD card over the USB-UART, loads the chosen one into DDR,
 * restores its .sra save into the cart SRAM, enables the cart and writes the
 * save back to the SD card whenever the game changes it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xparameters.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xuartps_hw.h"
#include "sleep.h"
#include "ff.h"

#include "cart_hw.h"
#include "rom_util.h"

#define ROM_DIR        "0:/"
#define LAST_FILE      "0:/n64last.txt"
#define MAX_ROMS       256
#define NAME_LEN       256
#define PATH_LEN       (NAME_LEN + 8)
#define LOAD_CHUNK     (1u << 20)
#define POLL_MS        50
#define SAVE_QUIET_MS  1000		// write the save after 1 s without SRAM writes
#define SAVE_MIN_SIZE  (32u * 1024)	// 256 Kbit SRAM, the common size

static FATFS fs;
static char rom_names[MAX_ROMS][NAME_LEN];
static int rom_count;
static uint8_t sram_buf[CART_SRAM_SIZE];

static uint32_t cart_rd(uint32_t reg)
{
	return Xil_In32(CART_BASE + reg);
}

static void cart_wr(uint32_t reg, uint32_t val)
{
	Xil_Out32(CART_BASE + reg, val);
}

static int uart_getc_nonblock(void)
{
	if (!XUartPs_IsReceiveData(STDIN_BASEADDRESS))
		return -1;
	return XUartPs_RecvByte(STDIN_BASEADDRESS);
}

static void read_line(char *buf, int len)
{
	int n = 0;

	for (;;) {
		char c = (char)XUartPs_RecvByte(STDIN_BASEADDRESS);

		if (c == '\r' || c == '\n') {
			printf("\r\n");
			break;
		} else if ((c == '\b' || c == 0x7F) && n > 0) {
			n--;
			printf("\b \b");
		} else if (c >= 0x20 && c < 0x7F && n < len - 1) {
			buf[n++] = c;
			printf("%c", c);
		}
	}
	buf[n] = '\0';
}

static int name_cmp(const void *a, const void *b)
{
	const char *x = a, *y = b;

	for (;; x++, y++) {
		int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x;
		int cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;

		if (cx != cy || cx == 0)
			return cx - cy;
	}
}

static void scan_roms(void)
{
	DIR dir;
	FILINFO fno;

	rom_count = 0;
	if (f_opendir(&dir, ROM_DIR) != FR_OK)
		return;
	while (rom_count < MAX_ROMS && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
		if ((fno.fattrib & AM_DIR) || !rom_has_rom_extension(fno.fname))
			continue;
		if (strlen(fno.fname) >= NAME_LEN)
			continue;
		strcpy(rom_names[rom_count++], fno.fname);
	}
	f_closedir(&dir);
	qsort(rom_names, rom_count, NAME_LEN, name_cmp);
}

static void read_last(char *out, int len)
{
	FIL f;
	UINT br = 0;

	out[0] = '\0';
	if (f_open(&f, LAST_FILE, FA_READ) != FR_OK)
		return;
	f_read(&f, out, len - 1, &br);
	f_close(&f);
	out[br] = '\0';
	out[strcspn(out, "\r\n")] = '\0';
}

static void write_last(const char *name)
{
	FIL f;
	UINT bw;

	if (f_open(&f, LAST_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return;
	f_write(&f, name, strlen(name), &bw);
	f_close(&f);
}

// Returns the index of the chosen ROM
static int choose_rom(void)
{
	static char last[NAME_LEN];
	char line[16];
	int last_idx = -1;
	int i;

	read_last(last, sizeof(last));

	for (;;) {
		scan_roms();
		printf("\r\n=== N64 ROMs on the SD card ===\r\n");
		if (rom_count == 0) {
			printf("No .z64/.v64/.n64 files in the root folder. Press Enter to rescan.\r\n");
			read_line(line, sizeof(line));
			continue;
		}
		for (i = 0; i < rom_count; i++) {
			printf("%3d) %s\r\n", i + 1, rom_names[i]);
			if (strcmp(rom_names[i], last) == 0)
				last_idx = i;
		}
		if (last_idx >= 0)
			printf("Select a ROM [Enter = %s]: ", rom_names[last_idx]);
		else
			printf("Select a ROM: ");

		read_line(line, sizeof(line));
		if (line[0] == '\0' && last_idx >= 0)
			return last_idx;
		i = atoi(line);
		if (i >= 1 && i <= rom_count)
			return i - 1;
		printf("Invalid choice.\r\n");
	}
}

static int load_rom(const char *path, uint32_t *size_out)
{
	uint8_t *dst = (uint8_t *)ROM_DDR_BASE;
	rom_format_t fmt = ROM_FMT_UNKNOWN;
	char title[ROM_TITLE_LEN + 1];
	uint32_t size, off = 0;
	FIL f;
	UINT br;

	if (f_open(&f, path, FA_READ) != FR_OK) {
		printf("Cannot open %s\r\n", path);
		return -1;
	}
	size = f_size(&f);
	if (size < 0x1000 || size > ROM_MAX_SIZE) {
		printf("Unsupported ROM size: %lu bytes (max 64 MB)\r\n", (unsigned long)size);
		f_close(&f);
		return -1;
	}

	while (off < size) {
		uint32_t n = size - off < LOAD_CHUNK ? size - off : LOAD_CHUNK;

		if (f_read(&f, dst + off, n, &br) != FR_OK || br != n) {
			printf("\r\nRead error at offset %lu\r\n", (unsigned long)off);
			f_close(&f);
			return -1;
		}
		if (off == 0) {
			fmt = rom_detect_format(dst);
			if (fmt == ROM_FMT_UNKNOWN) {
				printf("Not an N64 ROM (unknown header %02X %02X %02X %02X)\r\n", dst[0], dst[1], dst[2], dst[3]);
				f_close(&f);
				return -1;
			}
		}
		rom_to_z64(dst + off, n, fmt);
		off += n;
		printf("\rLoading %lu / %lu KB", (unsigned long)(off >> 10), (unsigned long)(size >> 10));
	}
	f_close(&f);

	// HP0 reads DDR directly, past the CPU caches
	Xil_DCacheFlushRange((INTPTR) dst, size);

	rom_title(dst, title);
	printf("\r\nTitle: %s  (%s, %lu KB)\r\n", title, rom_format_name(fmt), (unsigned long)(size >> 10));
	if (rom_ipl3_crc32(dst) != IPL3_CRC_6102)
		printf("WARNING: this ROM does not use the 6102/7101 CIC. The Pico emulates 6102/7101, so it will not boot.\r\n");

	*size_out = size;
	return 0;
}

// Fills the cart SRAM from the save file. Returns the number of bytes to save.
static uint32_t sram_restore(const char *save_path)
{
	uint32_t save_size = SAVE_MIN_SIZE;
	uint32_t i;
	FIL f;
	UINT br = 0;

	memset(sram_buf, 0, sizeof(sram_buf));
	if (f_open(&f, save_path, FA_READ) == FR_OK) {
		f_read(&f, sram_buf, sizeof(sram_buf), &br);
		f_close(&f);
		printf("Loaded save %s (%u bytes)\r\n", save_path, br);
		if (br > save_size)
			save_size = (br + 3) & ~3u;
	} else {
		printf("No save file yet, it will be created as %s\r\n", save_path);
	}

	for (i = 0; i < CART_SRAM_SIZE; i += 4)
		cart_wr(CART_SRAM + i, sram_pack(sram_buf + i));

	return save_size;
}

static void sram_store(const char *save_path, uint32_t save_size)
{
	uint32_t i;
	FIL f;
	UINT bw = 0;

	for (i = 0; i < save_size; i += 4)
		sram_unpack(cart_rd(CART_SRAM + i), sram_buf + i);

	if (f_open(&f, save_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
		printf("ERROR: could not create %s\r\n", save_path);
		return;
	}
	FRESULT wr = f_write(&f, sram_buf, save_size, &bw);
	FRESULT cl = f_close(&f);

	if (wr != FR_OK || cl != FR_OK || bw != save_size)
		printf("ERROR: could not write %s\r\n", save_path);
	else
		printf("Saved %s\r\n", save_path);
}

static void wait_n64_off(void)
{
	if (!(cart_rd(CART_REG_STATUS) & CART_STATUS_N64_ON))
		return;
	printf("Turn the N64 off first...\r\n");
	while (cart_rd(CART_REG_STATUS) & CART_STATUS_N64_ON)
		usleep(100 * 1000);
}

// Runs until the user asks for the menu with the N64 off
static void run_game(const char *save_path, uint32_t save_size)
{
	int quiet_ms = -1;		// >= 0 while a save is pending
	int was_on = -1;
	uint32_t underruns = 0;

	cart_wr(CART_REG_CLEAR, CART_CLEAR_DIRTY | CART_CLEAR_UNDERRUN);
	cart_wr(CART_REG_CTRL, CART_CTRL_ENABLE);
	printf("\r\nCart enabled. Power on the N64.\r\n");
	printf("Keys: s = save now, m = back to the menu (N64 must be off)\r\n");

	for (;;) {
		uint32_t status = cart_rd(CART_REG_STATUS);
		uint32_t u = cart_rd(CART_REG_UNDERRUNS);
		int on = (status & CART_STATUS_N64_ON) != 0;
		int c = uart_getc_nonblock();

		if (on != was_on) {
			printf(on ? "N64 on\r\n" : "N64 off\r\n");
			was_on = on;
		}

		if (u != underruns) {
			printf("WARNING: %lu ROM words were late (underrun)\r\n", (unsigned long)(u - underruns));
			underruns = u;
		}

		if (status & CART_STATUS_DIRTY) {
			cart_wr(CART_REG_CLEAR, CART_CLEAR_DIRTY);
			quiet_ms = 0;
		} else if (quiet_ms >= 0) {
			quiet_ms += POLL_MS;
			if (quiet_ms >= SAVE_QUIET_MS) {
				sram_store(save_path, save_size);
				quiet_ms = -1;
			}
		}

		if (c == 's' || c == 'S') {
			sram_store(save_path, save_size);
			quiet_ms = -1;
		} else if (c == 'm' || c == 'M') {
			if (on) {
				printf("Turn the N64 off before going back to the menu.\r\n");
			} else {
				if (quiet_ms >= 0)
					sram_store(save_path, save_size);
				cart_wr(CART_REG_CTRL, 0);
				return;
			}
		}

		usleep(POLL_MS * 1000);
	}
}

int main(void)
{
	static char path[PATH_LEN];
	static char save_path[PATH_LEN];
	uint32_t rom_size, save_size;
	int idx;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("\r\nPicoCart64 ZedBoard loader\r\n");

	if (cart_rd(CART_REG_ID) != CART_ID_MAGIC) {
		printf("Cart logic not found at 0x%08lX. Is the bitstream loaded?\r\n", (unsigned long)CART_BASE);
		return 1;
	}
	cart_wr(CART_REG_CTRL, 0);
	cart_wr(CART_REG_ROM_BASE, ROM_DDR_BASE);

	if (f_mount(&fs, "0:/", 1) != FR_OK) {
		printf("Cannot mount the SD card (FAT32 expected)\r\n");
		return 1;
	}

	for (;;) {
		wait_n64_off();
		idx = choose_rom();
		snprintf(path, sizeof(path), "%s%s", ROM_DIR, rom_names[idx]);
		if (load_rom(path, &rom_size) != 0)
			continue;
		write_last(rom_names[idx]);
		if (rom_save_path(path, save_path, sizeof(save_path)) != 0)
			continue;
		save_size = sram_restore(save_path);
		run_game(save_path, save_size);
	}
}
