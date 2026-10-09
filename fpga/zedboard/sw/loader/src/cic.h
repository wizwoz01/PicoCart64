/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CIC type of a ROM, found from its boot code (IPL3). Platform independent.
 */

#pragma once

#include <stdint.h>

typedef enum {
	CIC_UNKNOWN,
	CIC_6101,
	CIC_6102,					// also 7101
	CIC_7102,
	CIC_6103,					// also 7103
	CIC_6105,					// also 7105
	CIC_6106,					// also 7106
} cic_t;

// Code on the CIC type lines to the Pico (GPIO28..26). 1 = line released, so
// an unconnected Pico reads 7 and emulates the 6102.
#define CIC_CODE_6102 7
#define CIC_CODE_6101 6
#define CIC_CODE_7102 5
#define CIC_CODE_6103 4
#define CIC_CODE_6105 3
#define CIC_CODE_6106 2

// The 48-bit value the PIF compares with the CIC's checksum: IPL2's hash of
// IPL3 (0xFC0 bytes, ROM offset 0x40) with the CIC's seed
uint64_t cic_ipl2_checksum(uint8_t seed, const uint8_t * ipl3);

// CIC for a .z64 image (needs at least 0x1000 bytes)
cic_t cic_detect(const uint8_t * z64);

// "6105/7105", or "unknown"
const char *cic_name(cic_t cic);

// Code to put on the CIC type lines. Unknown CICs get the 6102.
uint8_t cic_pico_code(cic_t cic);

// The header checksum (CRC1 and CRC2, at 0x10 and 0x14) that the boot code
// checks over ROM 0x1000-0x101000 before it starts the game. z64 must hold
// at least 0x101000 bytes. Returns 0, or -1 for an unknown CIC.
int cic_header_crc(const uint8_t * z64, cic_t cic, uint32_t crc[2]);
