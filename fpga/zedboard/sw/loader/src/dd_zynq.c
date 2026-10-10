/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The 64DD on the ZedBoard (see dd_zynq.h).
 */

#include <stdio.h>
#include <string.h>

#include "xparameters.h"
#include "xil_cache.h"
#include "xil_exception.h"
#include "xil_io.h"
#include "xscugic.h"
#include "xtime_l.h"
#include "ff.h"

#include "cart_hw.h"
#include "dd_drive.h"
#include "dd_zynq.h"
#include "rom_util.h"
#include "ui.h"

#define LOAD_CHUNK  (1u << 20)
#define LOG_LEN     256

static XScuGic gic;
static dd_drive_t drive;
static uint8_t err[DD_BLOCKS];
static dd_block_t map[DD_BLOCKS];
static dd_disk_info_t info;
static char disk_path[UI_NAME_LEN + 8], disk_name[UI_NAME_LEN];
static int disk_in;

// Set in the interrupt, cleared by dd_save()
static volatile uint8_t dirty[DD_BLOCKS];
static volatile int dirty_any;
static volatile uint32_t dirty_ms, lost;
static uint32_t tick_ms, on_ms;

// Activity log, written in the interrupt
static struct {
	uint32_t ms;
	uint8_t what;
	uint16_t a, b;
} logbuf[LOG_LEN];
static volatile uint32_t log_n;

static uint32_t ms(void)
{
	XTime t;

	XTime_GetTime(&t);
	return (uint32_t)(t / (COUNTS_PER_SECOND / 1000));
}

// --- dd_asic.v

static void hw_reg(void *ctx, int n, uint16_t v)
{
	(void)ctx;
	Xil_Out32(CART_BASE + CART_DD_REG(n), v);
}

static void hw_flags(void *ctx, uint32_t set, uint32_t clear)
{
	(void)ctx;
	Xil_Out32(CART_BASE + CART_DD_FLAGS, set | (clear << 8));
}

static uint32_t hw_get_flags(void *ctx)
{
	(void)ctx;
	return Xil_In32(CART_BASE + CART_DD_FLAGS) & 0xF;
}

static void hw_put(void *ctx, const uint8_t *src, uint32_t len)
{
	uint32_t i;

	(void)ctx;
	for (i = 0; i + 4 <= len; i += 4)
		Xil_Out32(CART_BASE + CART_DD_SECTOR + i, sram_pack(src + i));
}

static void hw_get(void *ctx, uint8_t *dst, uint32_t len)
{
	uint32_t i;

	(void)ctx;
	for (i = 0; i + 4 <= len; i += 4)
		sram_unpack(Xil_In32(CART_BASE + CART_DD_SECTOR + i), dst + i);
}

static void hw_delay(void *ctx, uint32_t us)
{
	XTime t0, t;

	(void)ctx;
	XTime_GetTime(&t0);
	do
		XTime_GetTime(&t);
	while (t - t0 < (XTime)us * (COUNTS_PER_SECOND / 1000000));
}

static void hw_written(void *ctx, int block)
{
	(void)ctx;
	dirty[block] = 1;
	dirty_ms = ms();
	dirty_any = 1;
}

static void hw_log(void *ctx, int what, uint16_t a, uint16_t b)
{
	uint32_t i = log_n % LOG_LEN;

	(void)ctx;
	logbuf[i].ms = ms();
	logbuf[i].what = (uint8_t)what;
	logbuf[i].a = a;
	logbuf[i].b = b;
	log_n++;
}

static const dd_hw_t hw = {
	NULL, hw_reg, hw_flags, hw_get_flags, hw_put, hw_get, hw_delay, hw_written, hw_log
};

// Events from the N64: the drive answers them here, sector delays included
static void dd_isr(void *ctx)
{
	uint32_t e;

	(void)ctx;
	while ((e = Xil_In32(CART_BASE + CART_DD_EVENT)) & 0x80000000u)
		dd_drive_event(&drive, (int)((e >> 16) & 0x1F), (uint16_t)e);
	if (Xil_In32(CART_BASE + CART_DD_FLAGS) & (1u << 8)) {
		lost++;
		Xil_Out32(CART_BASE + CART_DD_FLAGS, 1u << 16);
	}
}

static void lock(void)
{
	XScuGic_Disable(&gic, CART_DD_IRQ);
}

static void unlock(void)
{
	XScuGic_Enable(&gic, CART_DD_IRQ);
}

// The loader's build time, as the drive's clock starts
static void rtc_from_build(uint8_t bcd[6])
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *date = __DATE__, *time = __TIME__;	// "Oct  9 2026", "16:30:00"
	int month = 1, day, year, i;

	for (i = 0; i < 12; i++)
		if (strncmp(date, months + 3 * i, 3) == 0)
			month = i + 1;
	day = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + date[5] - '0';
	year = (date[9] - '0') * 10 + date[10] - '0';
	bcd[0] = (uint8_t)(((year / 10) << 4) | (year % 10));
	bcd[1] = (uint8_t)(((month / 10) << 4) | (month % 10));
	bcd[2] = (uint8_t)(((day / 10) << 4) | (day % 10));
	bcd[3] = (uint8_t)(((time[0] - '0') << 4) | (time[1] - '0'));
	bcd[4] = (uint8_t)(((time[3] - '0') << 4) | (time[4] - '0'));
	bcd[5] = (uint8_t)(((time[6] - '0') << 4) | (time[7] - '0'));
}

int dd_init(void)
{
	XScuGic_Config *cfg = XScuGic_LookupConfig(XPAR_SCUGIC_0_DEVICE_ID);
	uint8_t rtc[6];

	if (!cfg || XScuGic_CfgInitialize(&gic, cfg, cfg->CpuBaseAddress) != XST_SUCCESS)
		return -1;
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, &gic);
	XScuGic_SetPriorityTriggerType(&gic, CART_DD_IRQ, 0xA0, 0x1);	// level, active high
	if (XScuGic_Connect(&gic, CART_DD_IRQ, dd_isr, NULL) != XST_SUCCESS)
		return -1;

	while (Xil_In32(CART_BASE + CART_DD_EVENT) & 0x80000000u)
		;
	dd_drive_init(&drive, &hw, DD_ID_RETAIL);
	rtc_from_build(rtc);
	dd_drive_set_rtc(&drive, rtc);
	tick_ms = ms();

	XScuGic_Enable(&gic, CART_DD_IRQ);
	Xil_ExceptionEnable();
	return 0;
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int dd_is_disk_name(const char *name)
{
	size_t n = strlen(name);

	return n >= 5 && name[n - 4] == '.' && lower(name[n - 3]) == 'n' && lower(name[n - 2]) == 'd' &&
	       lower(name[n - 1]) == 'd';
}

int dd_is_ipl(const uint8_t *z64)
{
	return z64[0x3B] == 'N' && (z64[0x3C] == 'D' || z64[0x3C] == 'X') && z64[0x3D] == 'D';
}

int dd_load_disk(const char *path, const char *name, void (*progress)(uint32_t done, uint32_t total))
{
	uint8_t *ndd = (uint8_t *)DD_NDD_DDR_BASE, *phys = (uint8_t *)DD_PHYS_DDR_BASE;
	uint32_t size, off = 0;
	FIL f;
	UINT br;

	if (f_open(&f, path, FA_READ) != FR_OK) {
		ui_log(UI_ERROR, "Cannot open %s", name);
		return -1;
	}
	size = f_size(&f);
	if (size != DD_NDD_SIZE) {
		ui_log(UI_ERROR, "%s is %lu bytes; a .ndd disk image has %lu.", name, (unsigned long)size,
		       (unsigned long)DD_NDD_SIZE);
		f_close(&f);
		return -1;
	}
	while (off < size) {
		uint32_t n = size - off < LOAD_CHUNK ? size - off : LOAD_CHUNK;

		if (f_read(&f, ndd + off, n, &br) != FR_OK || br != n) {
			ui_log(UI_ERROR, "Read error in %s at %lu", name, (unsigned long)off);
			f_close(&f);
			return -1;
		}
		off += n;
		if (progress)
			progress(off, size);
	}
	f_close(&f);

	lock();
	if (dd_disk_check(ndd, size, err, &info) != 0) {
		unlock();
		ui_log(UI_ERROR, "%s has no valid 64DD system area: not a disk image.", name);
		return -1;
	}
	memset(phys, 0, DD_PHYS_SIZE);
	dd_disk_to_phys(ndd, phys, &info, map);
	memset((void *)dirty, 0, sizeof(dirty));
	dirty_any = 0;
	dd_drive_insert(&drive, phys, err, info.type);
	unlock();

	strncpy(disk_path, path, sizeof(disk_path) - 1);
	strncpy(disk_name, name, sizeof(disk_name) - 1);
	disk_in = 1;
	ui_log(UI_INFO, "64DD disk %s: %s, type %u, ID %s", name, dd_region_name(info.region), info.type, info.id);
	return 0;
}

void dd_eject(void)
{
	if (!disk_in)
		return;
	dd_save();
	lock();
	dd_drive_insert(&drive, NULL, NULL, 0);
	unlock();
	disk_in = 0;
	ui_log(UI_INFO, "Disk %s out of the drive", disk_name);
}

const char *dd_disk_name(void)
{
	return disk_in ? disk_name : NULL;
}

const dd_disk_info_t *dd_info(void)
{
	return disk_in ? &info : NULL;
}

int dd_load_ipl(cic_t *cic, char *name, int name_len)
{
	uint8_t *dst = (uint8_t *)IPL_DDR_BASE, head[64];
	rom_format_t fmt;
	DIR dir;
	FILINFO fno;
	FIL f;
	UINT br;
	char path[UI_NAME_LEN + 8];
	int found = 0;

	*cic = CIC_UNKNOWN;
	if (f_opendir(&dir, "0:/") != FR_OK)
		return -1;
	while (!found && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
		if ((fno.fattrib & AM_DIR) || !rom_has_rom_extension(fno.fname) || fno.fsize > IPL_MAX_SIZE)
			continue;
		snprintf(path, sizeof(path), "0:/%s", fno.fname);
		if (f_open(&f, path, FA_READ) != FR_OK)
			continue;
		if (f_read(&f, head, sizeof(head), &br) == FR_OK && br == sizeof(head)) {
			fmt = rom_detect_format(head);
			if (fmt != ROM_FMT_UNKNOWN) {
				rom_to_z64(head, sizeof(head), fmt);
				found = dd_is_ipl(head);
			}
		}
		if (found) {
			uint32_t size = f_size(&f);

			f_lseek(&f, 0);
			if (f_read(&f, dst, size, &br) != FR_OK || br != size)
				found = 0;
			else
				rom_to_z64(dst, size, fmt);
			if (size < IPL_MAX_SIZE)
				memset(dst + size, 0, IPL_MAX_SIZE - size);
			snprintf(name, (size_t)name_len, "%s", fno.fname);
		}
		f_close(&f);
	}
	f_closedir(&dir);
	if (!found)
		return -1;

	// HP0 reads DDR directly, past the CPU caches
	Xil_DCacheFlushRange((INTPTR)dst, IPL_MAX_SIZE);
	*cic = cic_detect(dst);
	return 0;
}

void dd_ipl_from_rom(uint32_t size)
{
	if (size > IPL_MAX_SIZE)
		size = IPL_MAX_SIZE;
	memcpy((void *)IPL_DDR_BASE, (const void *)ROM_DDR_BASE, size);
	Xil_DCacheFlushRange((INTPTR)IPL_DDR_BASE, IPL_MAX_SIZE);
}

void dd_console(int on)
{
	if (on)
		on_ms = ms();
	lock();
	while (Xil_In32(CART_BASE + CART_DD_EVENT) & 0x80000000u)
		;
	dd_drive_reset(&drive);
	unlock();
}

int dd_save(void)
{
	FIL f;
	UINT bw;
	int i, n = 0;

	if (!disk_in || !dirty_any)
		return 0;
	if (f_open(&f, disk_path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) {
		ui_log(UI_ERROR, "Cannot write %s: disk changes are not saved", disk_name);
		return 0;
	}
	dirty_any = 0;
	for (i = 0; i < DD_BLOCKS; i++) {
		if (!dirty[i])
			continue;
		dirty[i] = 0;
		if (map[i].ndd == 0xFFFFFFFF)
			continue;
		if (f_lseek(&f, map[i].ndd) != FR_OK ||
		    f_write(&f, (const uint8_t *)DD_PHYS_DDR_BASE + map[i].phys, map[i].size, &bw) != FR_OK || bw != map[i].size) {
			ui_log(UI_ERROR, "Write error in %s", disk_name);
			break;
		}
		n++;
	}
	f_close(&f);
	if (n)
		ui_log(UI_GOOD, "Saved %d changed block%s of %s", n, n == 1 ? "" : "s", disk_name);
	return n;
}

int dd_poll(uint32_t now_ms, uint32_t quiet_ms)
{
	while (now_ms - tick_ms >= 1000) {
		tick_ms += 1000;
		lock();
		dd_drive_rtc_tick(&drive);
		unlock();
	}
	if (dirty_any && now_ms - dirty_ms >= quiet_ms)
		return dd_save();
	return 0;
}

int dd_unsaved(void)
{
	return disk_in && dirty_any;
}

uint32_t dd_events_lost(void)
{
	return lost;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

int dd_disk_match_ipl(void)
{
	static const uint32_t country[2] = { 0xE848D316, 0x2263EE56 };	// Japan, USA
	static const int sys_blocks[4] = { 0, 1, 8, 9 };
	const uint8_t *ipl = (const uint8_t *)IPL_DDR_BASE;
	uint8_t *phys = (uint8_t *)DD_PHYS_DDR_BASE;
	uint32_t have, want, i, j;
	int b, n;

	// NDDE is the US IPL; NDDJ and NDXJ are Japanese
	if (!disk_in || info.region == DD_REGION_DEV || (ipl[0x3E] != 'E' && ipl[0x3E] != 'J'))
		return 0;
	if ((ipl[0x3E] == 'E') == (info.region == DD_REGION_USA))
		return 0;
	have = country[info.region == DD_REGION_USA];
	want = country[ipl[0x3E] == 'E'];

	lock();
	// Each of the 85 copies of the system area (232-byte sectors) starts with it
	for (b = 0; b < 4; b++) {
		uint8_t *p = phys + map[sys_blocks[b]].phys;

		if (map[sys_blocks[b]].ndd == 0xFFFFFFFF)
			continue;
		for (n = 0; n < DD_SECTORS; n++, p += 0xE8)
			if (be32(p) == have)
				put32(p, want);
	}
	// LUI rX, hi then ORI rY, rX, lo in the game's code
	for (i = 0; i + 4 <= DD_PHYS_SIZE; i += 4) {
		uint32_t w = be32(phys + i);
		unsigned rx;

		if ((w & 0xFFE0FFFF) != (0x3C000000u | (have >> 16)))
			continue;
		rx = (w >> 16) & 31;
		for (j = i + 4; j <= i + 24 && j + 4 <= DD_PHYS_SIZE; j += 4) {
			uint32_t w2 = be32(phys + j);

			if ((w2 >> 26) == 0x0D && ((w2 >> 21) & 31) == rx && (w2 & 0xFFFF) == (have & 0xFFFF)) {
				put32(phys + i, (w & 0xFFFF0000u) | (want >> 16));
				put32(phys + j, (w2 & 0xFFFF0000u) | (want & 0xFFFF));
				break;
			}
		}
	}
	info.region = info.region == DD_REGION_USA ? DD_REGION_JAPAN : DD_REGION_USA;
	unlock();
	return 1;
}

int dd_log_lines(void)
{
	return log_n < LOG_LEN ? (int)log_n : LOG_LEN;
}

static const char *cmd_name(int c)
{
	static const char *const names[] = {
		"NOP", "READ_SEEK", "WRITE_SEEK", "RECALIBRATE", "SLEEP", "START", "SET_STANDBY", "SET_SLEEP",
		"CLEAR_CHANGED", "CLEAR_RESET", "READ_VERSION", "SET_DISK_TYPE", "REQUEST_STATUS", "STANDBY",
		"INDEX_LOCK_RETRY", "SET_RTC_YM", "SET_RTC_DH", "SET_RTC_MS", "GET_RTC_YM", "GET_RTC_DH", "GET_RTC_MS",
		"SET_LED",
	};

	if (c < (int)(sizeof(names) / sizeof(names[0])))
		return names[c];
	return c == 0x1B ? "READ_PROGRAM_VERSION" : "unknown";
}

void dd_log_line(int i, char *out, int len)
{
	uint32_t n = log_n, k = (n > LOG_LEN ? n - LOG_LEN : 0) + (uint32_t)i;
	int32_t dt = (int32_t)(logbuf[k % LOG_LEN].ms - on_ms);
	uint32_t t = (uint32_t)(dt < 0 ? -dt : dt);
	uint16_t a = logbuf[k % LOG_LEN].a, b = logbuf[k % LOG_LEN].b;
	char when[16], result[24];

	// Seconds from the last power on (negative: an earlier run)
	snprintf(when, sizeof(when), "%s%4lu.%03lu s", dt < 0 ? "-" : " ", (unsigned long)(t / 1000), (unsigned long)(t % 1000));
	switch (logbuf[k % LOG_LEN].what) {
	case DD_LOG_RESET:
		snprintf(out, (size_t)len, "%s  drive reset", when);
		break;
	case DD_LOG_CMD:
		if (a & 0x80)
			snprintf(result, sizeof(result), "write protected");
		else if (a & 0x7F)
			snprintf(result, sizeof(result), "error %02X", a & 0x7F);
		else
			snprintf(result, sizeof(result), "ok");
		snprintf(out, (size_t)len, "%s  %-16s data %04X  %s", when, cmd_name(a >> 8), b, result);
		break;
	default:
		snprintf(out, (size_t)len, "%s  %s head %u track %u, from sector %u%s", when,
			 (b & 0x100) ? "read " : "write", (a >> 12) & 1, a & 0xFFF, b & 0xFF,
			 (b & 0x200) ? ", both blocks" : "");
		break;
	}
}
