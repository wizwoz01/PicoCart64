/**
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * The 64DD drive, behind the ASIC registers of dd_asic.v: commands, the
 * buffer manager that moves sectors between the disk and the sector buffer,
 * and the drive's clock. Platform independent: the hardware is reached
 * through dd_hw_t.
 */

#pragma once

#include <stdint.h>

#include "dd_disk.h"

// dd_asic.v's flags
#define DD_FLAG_BUSY   (1u << 0)
#define DD_FLAG_MECHA  (1u << 1)		// MECHA interrupt
#define DD_FLAG_BM     (1u << 2)		// buffer manager interrupt
#define DD_FLAG_START  (1u << 3)		// buffer manager running

// ASIC registers (N64 0x05000500 + 4n) and events
enum {
	DD_REG_DATA = 0,
	DD_REG_STATUS = 2,			// writes: ASIC_CMD
	DD_REG_CUR_TK = 3,
	DD_REG_BM = 4,				// reads BM_STATUS, writes BM_CTL
	DD_REG_ERR_SECTOR = 5,
	DD_REG_SEQ = 6,
	DD_REG_CUR_SECTOR = 7,
	DD_REG_HARD_RESET = 8,
	DD_REG_HOST_SECBYTE = 10,
	DD_REG_C1_S2 = 11,
	DD_REG_SEC_BYTE = 12,
	DD_REG_ID = 16,
	DD_EV_BM_ACK = 31,			// STATUS read with the BM interrupt set
};

#define DD_ID_RETAIL       3		// retail drives and the US development IPL
#define DD_ID_DEVELOPMENT  4		// Japanese development drives (NDXJ IPL)

// What dd_hw_t.log reports
enum {
	DD_LOG_RESET,				// a, b: 0
	DD_LOG_CMD,				// a: command << 8 | errors, b: the parameter (ASIC_DATA)
	DD_LOG_BM,				// buffer manager started: a: track, b: sector | read << 8 | both blocks << 9
};

typedef struct {
	void *ctx;
	void (*reg)(void *ctx, int n, uint16_t value);		// what register n reads
	void (*flags)(void *ctx, uint32_t set, uint32_t clear);
	uint32_t (*get_flags)(void *ctx);
	void (*sector_put)(void *ctx, const uint8_t *src, uint32_t len);	// to the sector buffer
	void (*sector_get)(void *ctx, uint8_t *dst, uint32_t len);		// from it
	void (*delay_us)(void *ctx, uint32_t us);
	void (*written)(void *ctx, int block);			// a block of the disk changed
	void (*log)(void *ctx, int what, uint16_t a, uint16_t b);	// optional
} dd_hw_t;

typedef struct {
	const dd_hw_t *hw;
	uint8_t *disk;				// physical image (dd_disk_to_phys), NULL: no disk
	const uint8_t *err;			// unreadable blocks
	uint8_t disk_type;			// from the disk's system area: its ROM area
	uint16_t id;

	uint16_t data;
	uint16_t track;				// [11:0] track, [12] head, [14:13] on track
	uint8_t sector;
	uint8_t sector_size_buf;		// bytes per sector - 1 (HOST_SECBYTE)
	uint8_t sector_size, sector_block;	// C1_S2
	uint8_t type_set;			// SET_DISK_TYPE

	struct {
		uint8_t request_user, request_c2, reset, disk_changed, disk_present;
		uint8_t write_protect, mecha_error, head_retracted, motor_stopped;
	} st;
	struct {
		uint8_t reset, error, block_transfer, read_mode, c1_single, c1_double, micro_enable;
	} bm;
	uint8_t errors;				// REQUEST_STATUS bits
	uint8_t seek;				// a seek is waiting for its response

	uint8_t rtc[6];				// BCD: year, month, day, hour, minute, second
} dd_drive_t;

void dd_drive_init(dd_drive_t * d, const dd_hw_t * hw, uint16_t id);

// Puts a disk in the drive (phys and err from dd_disk), or takes it out (NULL)
void dd_drive_insert(dd_drive_t * d, uint8_t * phys, const uint8_t * err, uint8_t disk_type);

// Drive reset, as at power on or ASIC_HARD_RESET
void dd_drive_reset(dd_drive_t * d);

// One event from the FIFO: a register write, or DD_EV_BM_ACK
void dd_drive_event(dd_drive_t * d, int reg, uint16_t data);

// The clock: set it (BCD), and advance it by a second
void dd_drive_set_rtc(dd_drive_t * d, const uint8_t bcd[6]);
void dd_drive_rtc_tick(dd_drive_t * d);
