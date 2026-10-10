/**
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * 64DD disk images. A .ndd file holds the disk's blocks in LBA order; the
 * drive addresses them by head, track and sector. dd_disk lays the blocks out
 * physically, the way ares (and MAME) store a disk, and maps each physical
 * block back to the .ndd for saving. Platform independent.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define DD_NDD_SIZE      0x3DEC800u	// .ndd: every LBA, in order
#define DD_PHYS_SIZE     0x435B0C0u	// both sides, every track, by head and track
#define DD_LBAS          0x10DC
#define DD_TRACKS        1175		// per side
#define DD_BLOCKS        (DD_TRACKS * 2 * 2)	// sides x tracks x 2 blocks
#define DD_SECTORS       85		// user sectors per block

typedef enum {
	DD_REGION_JAPAN,
	DD_REGION_USA,
	DD_REGION_DEV,
} dd_region_t;

typedef struct {
	dd_region_t region;
	uint8_t type;				// disk type 0-6: where the ROM area ends
	char id[5];				// disk ID, e.g. "EFZE"
	uint32_t sys_offset;			// the system area used, in the .ndd
} dd_disk_info_t;

// One physical block: where it is in each layout (ndd = 0xFFFFFFFF: none)
typedef struct {
	uint32_t phys;
	uint32_t ndd;
	uint16_t size;
} dd_block_t;

// Checks a .ndd image and fills err[DD_BLOCKS]: 1 for a block that doesn't
// read, as on a real disk (the boot code checks this as copy protection).
// Returns 0 if it is a disk image the drive can use.
int dd_disk_check(const uint8_t * ndd, size_t size, uint8_t * err, dd_disk_info_t * info);

// Lays the image out physically into phys (DD_PHYS_SIZE bytes, zeroed first)
// and fills map[DD_BLOCKS].
void dd_disk_to_phys(const uint8_t * ndd, uint8_t * phys, const dd_disk_info_t * info, dd_block_t * map);

// The drive's track register: [11:0] track, [12] head
// Index of the block holding a sector (0-89 block 0, 90-179 block 1), for
// err[] and map[]
int dd_block_index(uint16_t track, uint8_t sector);

// Offset in phys of a sector of sector_size bytes
uint32_t dd_sector_offset(uint16_t track, uint8_t sector, uint32_t sector_size);

// 1 if the track is in the disk's ROM area (read only) for this disk type
int dd_track_is_rom(uint16_t track, uint8_t type);

// "Japan", "USA" or "development"
const char *dd_region_name(dd_region_t region);
