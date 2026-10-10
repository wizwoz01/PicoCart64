/**
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * The 64DD drive (see dd_drive.h).
 *
 * The command set, the register bits and the buffer manager follow ares
 * (ares/n64/dd/controller.cpp, drive.cpp, io.cpp and rtc.cpp):
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *
 *   Permission to use, copy, modify, and/or distribute this software for any
 *   purpose with or without fee is hereby granted, provided that the above
 *   copyright notice and this permission notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <string.h>

#include "dd_drive.h"

enum {
	CMD_NOP = 0x00,
	CMD_READ_SEEK = 0x01,
	CMD_WRITE_SEEK = 0x02,
	CMD_RECALIBRATE = 0x03,
	CMD_SLEEP = 0x04,
	CMD_START = 0x05,
	CMD_SET_STANDBY = 0x06,
	CMD_SET_SLEEP = 0x07,
	CMD_CLEAR_CHANGED = 0x08,
	CMD_CLEAR_RESET = 0x09,
	CMD_READ_VERSION = 0x0A,
	CMD_SET_DISK_TYPE = 0x0B,
	CMD_REQUEST_STATUS = 0x0C,
	CMD_STANDBY = 0x0D,
	CMD_INDEX_LOCK_RETRY = 0x0E,
	CMD_SET_RTC_YM = 0x0F,
	CMD_SET_RTC_DH = 0x10,
	CMD_SET_RTC_MS = 0x11,
	CMD_GET_RTC_YM = 0x12,
	CMD_GET_RTC_DH = 0x13,
	CMD_GET_RTC_MS = 0x14,
	CMD_SET_LED = 0x15,
	CMD_READ_PROGRAM_VERSION = 0x1B,
};

// REQUEST_STATUS bits
#define ERR_SELF_DIAGNOSTIC  (1u << 0)
#define ERR_UNDEFINED_CMD    (1u << 4)
#define ERR_INVALID_PARAM    (1u << 5)

// The drive answers faster than a real one, but the N64 copies a sector out
// of the buffer after it acknowledges the BM interrupt, so the next sector
// has to wait (ares waits ~400 us; a real disk passes a sector every ~170 us)
#define CMD_US          100
#define SEEK_US         200
#define SEEK_TRACK_US   2
#define BM_FIRST_US     500
#define BM_NEXT_US      200

#define SECTORS_PER_BLOCK  90		// 85 user sectors, 4 C2 sectors, 1 gap
#define USER_SECTORS       85

static void views(dd_drive_t *d)
{
	const dd_hw_t *hw = d->hw;
	uint16_t status = (uint16_t)(d->st.disk_changed | (d->st.mecha_error << 1) | (d->st.write_protect << 2) |
				     (d->st.head_retracted << 3) | (d->st.motor_stopped << 4) | (d->st.reset << 6) |
				     (d->st.disk_present << 8) | (d->bm.error << 11) | (d->st.request_c2 << 12) |
				     (d->st.request_user << 14));
	uint16_t bm = (uint16_t)((d->bm.c1_single << 5) | (d->bm.c1_double << 6) | (d->bm.block_transfer << 8) |
				 (d->bm.error << 10));

	hw->reg(hw->ctx, DD_REG_DATA, d->data);
	hw->reg(hw->ctx, DD_REG_STATUS, status);
	hw->reg(hw->ctx, DD_REG_CUR_TK, d->track);
	hw->reg(hw->ctx, DD_REG_BM, bm);
	hw->reg(hw->ctx, DD_REG_ERR_SECTOR, (uint16_t)(!d->st.disk_present << 10));
	hw->reg(hw->ctx, DD_REG_CUR_SECTOR, (uint16_t)((d->sector << 8) | 0xC3));
	hw->reg(hw->ctx, DD_REG_HOST_SECBYTE, d->sector_size_buf);
	hw->reg(hw->ctx, DD_REG_SEC_BYTE, (uint16_t)((d->sector_block << 8) | d->sector_size));
	hw->reg(hw->ctx, DD_REG_ID, d->id);
}

static void motor_active(dd_drive_t *d)
{
	d->st.head_retracted = 0;
	d->st.motor_stopped = 0;
}

static void motor_standby(dd_drive_t *d)
{
	d->st.head_retracted = 1;
	d->st.motor_stopped = 0;
}

static void motor_stop(dd_drive_t *d)
{
	d->st.head_retracted = 1;
	d->st.motor_stopped = 1;
}

void dd_drive_init(dd_drive_t *d, const dd_hw_t *hw, uint16_t id)
{
	static const uint8_t epoch[6] = { 0x99, 0x01, 0x01, 0x00, 0x00, 0x00 };

	memset(d, 0, sizeof(*d));
	d->hw = hw;
	d->id = id;
	memcpy(d->rtc, epoch, sizeof(d->rtc));
	dd_drive_reset(d);
}

void dd_drive_insert(dd_drive_t *d, uint8_t *phys, const uint8_t *err, uint8_t disk_type)
{
	d->disk = phys;
	d->err = err;
	d->disk_type = disk_type;
	d->st.disk_present = phys != NULL;
	d->st.disk_changed = 1;
	if (!phys) {
		d->hw->flags(d->hw->ctx, 0, DD_FLAG_START);
		motor_stop(d);
	}
	views(d);
}

void dd_drive_reset(dd_drive_t *d)
{
	const dd_hw_t *hw = d->hw;
	static const uint8_t zero[256];

	memset(&d->st, 0, sizeof(d->st));
	memset(&d->bm, 0, sizeof(d->bm));
	d->data = 0;
	d->track = 0;
	d->sector = 0;
	d->sector_size_buf = 0;
	d->sector_size = 0;
	d->sector_block = 0;
	d->errors = 0;
	d->seek = 0;
	d->st.reset = 1;
	d->st.disk_changed = 1;
	d->st.disk_present = d->disk != NULL;
	motor_stop(d);
	hw->flags(hw->ctx, 0, DD_FLAG_BUSY | DD_FLAG_MECHA | DD_FLAG_BM | DD_FLAG_START);
	hw->sector_put(hw->ctx, zero, sizeof(zero));
	views(d);
	if (hw->log)
		hw->log(hw->ctx, DD_LOG_RESET, 0, 0);
}

static void mecha_response(dd_drive_t *d, uint32_t delay)
{
	d->hw->delay_us(d->hw->ctx, delay);
	if (d->seek) {
		d->seek = 0;
		if (d->st.disk_present)
			motor_active(d);
		else
			motor_stop(d);
	}
	views(d);
	d->hw->flags(d->hw->ctx, DD_FLAG_MECHA, DD_FLAG_BUSY);
}

static uint32_t seek_to(dd_drive_t *d, uint16_t track)
{
	uint32_t from = d->track & 0xFFF, to = track & 0xFFF;

	d->track = track | 0x6000;
	d->seek = 1;
	motor_active(d);
	return SEEK_US + SEEK_TRACK_US * (from > to ? from - to : to - from);
}

static void command(dd_drive_t *d, uint16_t cmd)
{
	uint32_t delay = CMD_US;
	uint8_t prev_errors = d->errors;
	uint16_t param = d->data;

	d->errors = 0;
	d->st.mecha_error = 0;
	d->st.write_protect = 0;

	switch (cmd) {
	case CMD_NOP:
		break;
	case CMD_READ_SEEK:
	case CMD_WRITE_SEEK:
		if (!d->disk) {
			d->errors |= ERR_SELF_DIAGNOSTIC;
		} else if ((d->data >> 12) > 1 || (d->data & 0xFFF) > 0x5D7) {
			d->errors |= ERR_INVALID_PARAM;
		} else {
			delay = seek_to(d, d->data);
			if (cmd == CMD_WRITE_SEEK)
				d->st.write_protect = (uint8_t)dd_track_is_rom(d->track, d->disk_type);
		}
		break;
	case CMD_RECALIBRATE:
	case CMD_START:
		if (d->disk)
			delay = seek_to(d, 0);
		else
			d->errors |= ERR_SELF_DIAGNOSTIC;
		break;
	case CMD_SLEEP:
		motor_stop(d);
		break;
	case CMD_SET_STANDBY:
	case CMD_SET_SLEEP:
	case CMD_SET_LED:
		break;			// the emulated drive never spins down
	case CMD_CLEAR_CHANGED:
		d->st.disk_changed = 0;
		break;
	case CMD_CLEAR_RESET:
		d->st.disk_changed = 0;
		d->st.reset = 0;
		break;
	case CMD_READ_VERSION:
		d->data = (d->data & 1) ? 0x5300 : 0x0114;
		break;
	case CMD_SET_DISK_TYPE:
		d->type_set = d->data & 0x0F;
		break;
	case CMD_REQUEST_STATUS:
		d->data = prev_errors;		// why the previous command failed
		break;
	case CMD_STANDBY:
		if (d->disk)
			motor_standby(d);
		else
			d->errors |= ERR_SELF_DIAGNOSTIC;
		break;
	case CMD_INDEX_LOCK_RETRY:
		if (!d->disk)
			d->errors |= ERR_SELF_DIAGNOSTIC;
		break;
	case CMD_SET_RTC_YM:
	case CMD_SET_RTC_DH:
	case CMD_SET_RTC_MS:
		d->rtc[(cmd - CMD_SET_RTC_YM) * 2] = (uint8_t)(d->data >> 8);
		d->rtc[(cmd - CMD_SET_RTC_YM) * 2 + 1] = (uint8_t)d->data;
		break;
	case CMD_GET_RTC_YM:
	case CMD_GET_RTC_DH:
	case CMD_GET_RTC_MS:
		d->data = (uint16_t)((d->rtc[(cmd - CMD_GET_RTC_YM) * 2] << 8) | d->rtc[(cmd - CMD_GET_RTC_YM) * 2 + 1]);
		break;
	case CMD_READ_PROGRAM_VERSION:
		d->data = 0x0003;
		break;
	default:
		d->errors |= ERR_UNDEFINED_CMD;
		break;
	}
	if (d->errors || d->st.write_protect)
		d->st.mecha_error = 1;
	if (d->hw->log)
		d->hw->log(d->hw->ctx, DD_LOG_CMD, (uint16_t)((cmd << 8) | d->errors | (d->st.write_protect << 7)), param);
	mecha_response(d, delay);
}

// The next sector for the buffer manager, then the BM interrupt
static void bm_request(dd_drive_t *d)
{
	const dd_hw_t *hw = d->hw;
	uint32_t bytes = (uint32_t)d->sector_size_buf + 1;
	uint8_t block = d->sector >= SECTORS_PER_BLOCK, n = (uint8_t)(d->sector % SECTORS_PER_BLOCK);

	if (!(hw->get_flags(hw->ctx) & DD_FLAG_START) || !d->disk)
		return;				// stopped or reset meanwhile

	d->st.request_user = 0;
	d->st.request_c2 = 0;
	d->bm.error = 0;
	d->bm.c1_single = 0;
	d->bm.c1_double = 0;

	if (d->bm.read_mode) {
		if (d->err[dd_block_index(d->track, d->sector)]) {
			d->bm.c1_single = 1;	// unreadable: copy protection checks this
			d->bm.c1_double = 1;
		}
		if (n < USER_SECTORS) {
			hw->sector_put(hw->ctx, d->disk + dd_sector_offset(d->track, d->sector, bytes), bytes);
			d->st.request_user = 1;
			d->sector++;
		} else if (n < USER_SECTORS + 3) {
			d->sector++;		// C2 sectors
		} else {
			d->st.request_c2 = 1;	// the last C2 sector
			if (d->bm.block_transfer) {
				d->bm.block_transfer = 0;
				d->sector = block ? 0 : SECTORS_PER_BLOCK;
			} else {
				hw->flags(hw->ctx, 0, DD_FLAG_START);
			}
		}
	} else {
		// Each interrupt writes the sector the N64 put in the buffer for the
		// previous one, and asks for the next
		if (n <= USER_SECTORS) {
			if (n > 0) {
				uint8_t prev = (uint8_t)(d->sector - 1);

				hw->sector_get(hw->ctx, d->disk + dd_sector_offset(d->track, prev, bytes), bytes);
				if (hw->written)
					hw->written(hw->ctx, dd_block_index(d->track, prev));
			}
			d->st.request_user = 1;
		}
		if (n >= USER_SECTORS) {
			if (d->bm.block_transfer) {
				d->bm.block_transfer = 0;
				d->sector = block ? 0 : SECTORS_PER_BLOCK;
			} else {
				d->st.request_user = 0;
				hw->flags(hw->ctx, 0, DD_FLAG_START);
			}
		}
		d->sector++;
	}
	views(d);
	hw->flags(hw->ctx, DD_FLAG_BM, 0);
}

static void bm_ctl(dd_drive_t *d, uint16_t v)
{
	d->bm.reset |= (v >> 12) & 1;
	d->bm.read_mode = (v >> 14) & 1;
	d->bm.block_transfer = (v >> 9) & 1;
	d->sector = (uint8_t)v;
	if (!((v >> 12) & 1) && d->bm.reset) {
		// End of a BM reset (dd_asic.v cleared START and the BM interrupt)
		d->bm.reset = 0;
		d->bm.error = 0;
		d->st.request_user = 0;
		d->st.request_c2 = 0;
	}
	views(d);
	if (((v >> 15) & 1) && d->disk) {
		if (d->hw->log)
			d->hw->log(d->hw->ctx, DD_LOG_BM, d->track,
				   (uint16_t)(d->sector | (d->bm.read_mode << 8) | (d->bm.block_transfer << 9)));
		d->hw->delay_us(d->hw->ctx, BM_FIRST_US);
		bm_request(d);
	}
}

void dd_drive_event(dd_drive_t *d, int reg, uint16_t data)
{
	switch (reg) {
	case DD_REG_DATA:
		d->data = data;
		break;
	case DD_REG_STATUS:			// ASIC_CMD
		command(d, data);
		break;
	case DD_REG_BM:				// ASIC_BM_CTL
		bm_ctl(d, data);
		break;
	case DD_REG_SEQ:
		d->bm.micro_enable = (data >> 14) & 1;
		break;
	case DD_REG_HARD_RESET:
		if (data == 0xAAAA)
			dd_drive_reset(d);
		break;
	case DD_REG_HOST_SECBYTE:
		d->sector_size_buf = (uint8_t)data;
		break;
	case DD_REG_C1_S2:
		d->sector_size = (uint8_t)data;
		d->sector_block = (uint8_t)(data >> 8);
		break;
	case DD_EV_BM_ACK:
		d->hw->delay_us(d->hw->ctx, BM_NEXT_US);
		bm_request(d);
		break;
	default:
		break;
	}
}

void dd_drive_set_rtc(dd_drive_t *d, const uint8_t bcd[6])
{
	memcpy(d->rtc, bcd, sizeof(d->rtc));
}

static uint8_t bcd_inc(uint8_t v)
{
	v++;
	if ((v & 0x0F) > 9)
		v = (uint8_t)((v & 0xF0) + 0x10);
	if ((v & 0xF0) > 0x90)
		v = 0;
	return v;
}

static uint8_t days_in_month(uint8_t month_bcd, uint8_t year_bcd)
{
	static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int month = (month_bcd >> 4) * 10 + (month_bcd & 0x0F);
	int year = (year_bcd >> 4) * 10 + (year_bcd & 0x0F);

	if (month < 1 || month > 12)
		return 31;
	return (uint8_t)(days[month - 1] + (month == 2 && year % 4 == 0));
}

void dd_drive_rtc_tick(dd_drive_t *d)
{
	uint8_t *r = d->rtc, dim;

	r[5] = bcd_inc(r[5]);
	if (r[5] < 0x60)
		return;
	r[5] = 0;
	r[4] = bcd_inc(r[4]);
	if (r[4] < 0x60)
		return;
	r[4] = 0;
	r[3] = bcd_inc(r[3]);
	if (r[3] < 0x24)
		return;
	r[3] = 0;
	dim = days_in_month(r[1], r[0]);
	r[2] = bcd_inc(r[2]);
	if (r[2] <= (uint8_t)(((dim / 10) << 4) | (dim % 10)))
		return;
	r[2] = 1;
	r[1] = bcd_inc(r[1]);
	if (r[1] <= 0x12)
		return;
	r[1] = 1;
	r[0] = bcd_inc(r[0]);
}
