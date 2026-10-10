/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for dd_disk.c and dd_drive.c, with a model of dd_asic.v:
 *   cc -I../src -o test_dd test_dd.c ../src/dd_disk.c ../src/dd_drive.c && ./test_dd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dd_disk.h"
#include "dd_drive.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// dd_asic.v as the drive sees it
static struct {
	uint16_t view[32];
	uint32_t flags;
	uint8_t sector[256];
	uint32_t put_len;
	int written_block, written_count;
	uint32_t delay;
} hw;

static void hw_reg(void *ctx, int n, uint16_t v) { (void)ctx; hw.view[n] = v; }
static void hw_flags(void *ctx, uint32_t set, uint32_t clear) { (void)ctx; hw.flags = (hw.flags & ~clear) | set; }
static uint32_t hw_get_flags(void *ctx) { (void)ctx; return hw.flags; }
static void hw_put(void *ctx, const uint8_t *src, uint32_t len) { (void)ctx; memcpy(hw.sector, src, len); hw.put_len = len; }
static void hw_get(void *ctx, uint8_t *dst, uint32_t len) { (void)ctx; memcpy(dst, hw.sector, len); }
static void hw_delay(void *ctx, uint32_t us) { (void)ctx; hw.delay += us; }
static void hw_written(void *ctx, int block) { (void)ctx; hw.written_block = block; hw.written_count++; }

static int log_count;
static void hw_log(void *ctx, int what, uint16_t a, uint16_t b) { (void)ctx; (void)what; (void)a; (void)b; log_count++; }

static const dd_hw_t ops = {
	NULL, hw_reg, hw_flags, hw_get_flags, hw_put, hw_get, hw_delay, hw_written, hw_log
};

// What the N64 does: a register write goes through dd_asic.v first
static void n64_write(dd_drive_t *d, int reg, uint16_t v)
{
	if (reg == DD_REG_STATUS) {
		if (hw.flags & DD_FLAG_BUSY)
			return;		// dd_asic.v drops commands while busy
		hw.flags |= DD_FLAG_BUSY;
	}
	if (reg == DD_REG_BM) {
		if (v & 0x0100)
			hw.flags &= ~DD_FLAG_MECHA;
		if ((v & 0x8000) && (hw.view[DD_REG_STATUS] & 0x0100))
			hw.flags |= DD_FLAG_START;
	}
	dd_drive_event(d, reg, v);
}

static void command(dd_drive_t *d, uint16_t cmd, uint16_t data)
{
	n64_write(d, DD_REG_DATA, data);
	n64_write(d, DD_REG_STATUS, cmd);
	hw.flags &= ~DD_FLAG_MECHA;	// the N64 acknowledges it
}

// Reading STATUS with the BM interrupt set acknowledges it
static void bm_ack(dd_drive_t *d)
{
	CHECK(hw.flags & DD_FLAG_BM);
	hw.flags &= ~DD_FLAG_BM;
	dd_drive_event(d, DD_EV_BM_ACK, 0);
}

static void test_geometry(void)
{
	// Zone 0 head 0: 232-byte sectors, 2 blocks per track
	CHECK(dd_sector_offset(0x0000, 0, 232) == 0);
	CHECK(dd_sector_offset(0x0000, 1, 232) == 232);
	CHECK(dd_sector_offset(0x0000, 90, 232) == 0x4D08);
	CHECK(dd_sector_offset(0x0001, 0, 232) == 2 * 0x4D08);
	CHECK(dd_sector_offset(0x6001, 0, 232) == 2 * 0x4D08);	// on-track bits ignored
	// Head 1 starts at zone 1's sector size, after all of head 0
	CHECK(dd_sector_offset(0x1000, 0, 216) == 0x23196E0);
	CHECK(dd_block_index(0x0000, 0) == 0 && dd_block_index(0x0000, 90) == 1);
	CHECK(dd_block_index(0x0006, 0) == 12 && dd_block_index(0x1000, 0) == DD_TRACKS * 2);
	// Disk type 3: ROM area up to physical zone 5
	CHECK(dd_track_is_rom(0x0000, 3) == 1);
	CHECK(dd_track_is_rom(0x02FB, 3) == 1);		// zone 5
	CHECK(dd_track_is_rom(0x0390, 3) == 0);		// zone 6
	CHECK(dd_track_is_rom(0x1266, 3) == 1);		// head 1, zone 4 + 1
	CHECK(dd_track_is_rom(0x12FB, 3) == 0);
}

static void test_commands(dd_drive_t *d, uint8_t *phys, uint8_t *err)
{
	// No disk: reset state, seeks fail
	CHECK(hw.view[DD_REG_STATUS] == 0x0059);	// reset, motor stopped, heads retracted, changed
	CHECK(hw.view[DD_REG_ID] == DD_ID_RETAIL);
	command(d, 0x01, 0x0000);
	CHECK(hw.view[DD_REG_STATUS] & 0x0002);	// mecha error
	CHECK(!(hw.flags & DD_FLAG_BUSY));

	dd_drive_insert(d, phys, err, 3);
	CHECK(hw.view[DD_REG_STATUS] & 0x0100);	// disk present
	command(d, 0x09, 0x0000);			// clear reset
	CHECK((hw.view[DD_REG_STATUS] & 0x0041) == 0);

	// Version, RTC, unknown command
	command(d, 0x0A, 0x0000);
	CHECK(hw.view[DD_REG_DATA] == 0x0114);
	command(d, 0x0F, 0x9912);
	command(d, 0x10, 0x3123);
	command(d, 0x11, 0x5959);
	dd_drive_rtc_tick(d);
	command(d, 0x12, 0x0000);
	CHECK(hw.view[DD_REG_DATA] == 0x0001);		// 2000-01
	command(d, 0x13, 0x0000);
	CHECK(hw.view[DD_REG_DATA] == 0x0100);		// day 1, 00 h
	command(d, 0x14, 0x0000);
	CHECK(hw.view[DD_REG_DATA] == 0x0000);
	command(d, 0x77, 0x0000);
	CHECK(hw.view[DD_REG_STATUS] & 0x0002);
	command(d, 0x0C, 0x0000);
	CHECK(hw.view[DD_REG_DATA] == 0x0010);		// undefined command

	// Seek: track register, motor on, no write protection for reads
	command(d, 0x01, 0x0005);
	CHECK(hw.view[DD_REG_CUR_TK] == 0x6005);
	CHECK((hw.view[DD_REG_STATUS] & 0x001E) == 0);
	CHECK(hw.flags == 0);
	// Writing to the ROM area is refused
	command(d, 0x02, 0x0005);
	CHECK(hw.view[DD_REG_STATUS] & 0x0006);
	command(d, 0x02, 0x0400);
	CHECK(!(hw.view[DD_REG_STATUS] & 0x0006));
	// Out of range
	command(d, 0x01, 0x05D8);
	CHECK(hw.view[DD_REG_STATUS] & 0x0002);
}

static void test_bm(dd_drive_t *d, uint8_t *phys, uint8_t *err)
{
	uint32_t off;
	int s;

	// Read track 6, block 0 (unreadable on a retail disk), 232-byte sectors
	command(d, 0x01, 0x0006);
	n64_write(d, DD_REG_HOST_SECBYTE, 231);
	hw.delay = 0;
	n64_write(d, DD_REG_BM, 0xC000);		// start, read mode, sector 0
	CHECK(hw.delay >= 100);
	CHECK(hw.flags & DD_FLAG_BM);
	CHECK(hw.put_len == 232);
	off = dd_sector_offset(0x0006, 0, 232);
	CHECK(memcmp(hw.sector, phys + off, 232) == 0);
	CHECK(hw.view[DD_REG_STATUS] & 0x4000);	// sector waiting
	CHECK(hw.view[DD_REG_BM] & 0x0060);		// C1 errors: block 12
	CHECK(hw.view[DD_REG_CUR_SECTOR] == 0x01C3);
	for (s = 1; s < 85; s++) {
		bm_ack(d);
		CHECK(memcmp(hw.sector, phys + off + (uint32_t)s * 232, 232) == 0);
	}
	// Three C2 sectors, then the last one stops the BM
	bm_ack(d);
	CHECK(!(hw.view[DD_REG_STATUS] & 0x5000));
	bm_ack(d);
	bm_ack(d);
	bm_ack(d);
	CHECK(hw.view[DD_REG_STATUS] & 0x1000);	// C2 sector
	CHECK(!(hw.flags & DD_FLAG_START));
	hw.flags &= ~DD_FLAG_BM;
	dd_drive_event(d, DD_EV_BM_ACK, 0);
	CHECK(!(hw.flags & DD_FLAG_BM));		// nothing after the stop

	// Block transfer: both blocks of track 7
	command(d, 0x01, 0x0007);
	n64_write(d, DD_REG_BM, 0xC200);		// start, read mode, both blocks
	CHECK(!(hw.view[DD_REG_BM] & 0x0060));
	for (s = 1; s < 89; s++)
		bm_ack(d);
	CHECK(hw.flags & DD_FLAG_START);		// on to block 1
	bm_ack(d);
	CHECK(hw.view[DD_REG_CUR_SECTOR] == ((91 << 8) | 0xC3));
	CHECK(memcmp(hw.sector, phys + dd_sector_offset(0x0007, 90, 232), 232) == 0);
	for (s = 1; s < 89; s++)
		bm_ack(d);
	CHECK(!(hw.flags & DD_FLAG_START));
	hw.flags &= ~DD_FLAG_BM;

	// Write a RAM track, zone 6: 144-byte sectors
	command(d, 0x02, 0x0400);
	n64_write(d, DD_REG_HOST_SECBYTE, 143);
	n64_write(d, DD_REG_BM, 0x8000);		// start, write mode, sector 0
	CHECK(hw.view[DD_REG_STATUS] & 0x4000);	// asks for sector 0
	CHECK(hw.written_count == 0);
	memset(hw.sector, 0x5A, sizeof(hw.sector));
	bm_ack(d);
	off = dd_sector_offset(0x0400, 0, 144);
	CHECK(phys[off] == 0x5A && phys[off + 143] == 0x5A && phys[off + 144] != 0x5A);
	CHECK(hw.written_count == 1 && hw.written_block == dd_block_index(0x0400, 0));
	for (s = 1; s < 85; s++)
		bm_ack(d);
	CHECK(!(hw.flags & DD_FLAG_START));
	CHECK(!(hw.view[DD_REG_STATUS] & 0x4000));
	hw.flags &= ~DD_FLAG_BM;

	// BM reset stops a transfer: START is cleared by dd_asic.v
	n64_write(d, DD_REG_BM, 0xC000);
	n64_write(d, DD_REG_BM, 0x1000);
	hw.flags &= ~(DD_FLAG_START | DD_FLAG_BM);
	n64_write(d, DD_REG_BM, 0x0000);
	CHECK(!(hw.view[DD_REG_STATUS] & 0x5000));
	dd_drive_event(d, DD_EV_BM_ACK, 0);
	CHECK(!(hw.flags & DD_FLAG_BM));

	// Hard reset
	n64_write(d, DD_REG_HARD_RESET, 0xAAAA);
	CHECK(hw.view[DD_REG_STATUS] & 0x0040);
	CHECK(log_count > 20);
	(void)err;
}

int main(void)
{
	uint8_t *phys = malloc(DD_PHYS_SIZE);
	static uint8_t err[DD_BLOCKS];
	static dd_drive_t d;
	uint32_t i;

	for (i = 0; i < DD_PHYS_SIZE; i++)
		phys[i] = (uint8_t)(i * 7 + (i >> 9));
	err[12] = 1;

	test_geometry();
	dd_drive_init(&d, &ops, DD_ID_RETAIL);
	test_commands(&d, phys, err);
	test_bm(&d, phys, err);
	free(phys);

	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
