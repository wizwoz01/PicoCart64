/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Save type of a game, from the ID in its ROM header. Platform independent.
 */

#pragma once

#include <stdint.h>

typedef enum {
	SAVE_UNKNOWN,				// not in the table
	SAVE_NONE,
	SAVE_EEPROM_4K,
	SAVE_EEPROM_16K,
	SAVE_SRAM,					// 32 KB
	SAVE_SRAM_96K,				// 3 banks of 32 KB
	SAVE_FLASHRAM,
} save_type_t;

// Save type of a .z64 image (needs the header)
save_type_t save_type_detect(const uint8_t * z64);

const char *save_type_name(save_type_t type);
