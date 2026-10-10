/**
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * 64DD disk images (see dd_disk.h).
 *
 * The disk geometry tables, the system area checks and the LBA to track
 * mapping are ported from ares (mia/medium/nintendo-64dd.cpp and
 * ares/n64/dd/drive.cpp):
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

#include "dd_disk.h"

#define SYS_SECTOR_RETAIL  0xE8
#define SYS_SECTOR_DEV     0xC0

// Bytes per block in each zone (85 sectors); head 1 starts at zone 1
static const uint32_t block_size[9] = {
	0x4D08, 0x47B8, 0x4510, 0x3FC0, 0x3A70, 0x3520, 0x2FD0, 0x2A80, 0x2530
};

// First LBA after each virtual zone, per disk type
static const uint16_t vzone_lba[7][16] = {
	{ 0x0124, 0x0248, 0x035A, 0x047E, 0x05A2, 0x06B4, 0x07C6, 0x08D8, 0x09EA, 0x0AB6, 0x0B82, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC },
	{ 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x06A2, 0x07C6, 0x08D8, 0x09EA, 0x0AFC, 0x0BC8, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC },
	{ 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08C6, 0x09EA, 0x0AFC, 0x0C0E, 0x0CDA, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC },
	{ 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AEA, 0x0C0E, 0x0D20, 0x0DEC, 0x0EB8, 0x0FCA, 0x10DC },
	{ 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0D0E, 0x0E32, 0x0EFE, 0x0FCA, 0x10DC },
	{ 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E, 0x0980, 0x0A92, 0x0BA4, 0x0CB6, 0x0DC8, 0x0EEC, 0x1010, 0x10DC },
	{ 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E, 0x093A, 0x0A4C, 0x0B5E, 0x0C70, 0x0D82, 0x0E94, 0x0FB8, 0x10DC },
};

// Physical zone of each virtual zone, per disk type: 0-7 head 0, 8-15 head 1
static const uint8_t vzone_pzone[7][16] = {
	{ 0x0, 0x1, 0x2, 0x9, 0x8, 0x3, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB, 0xA },
	{ 0x0, 0x1, 0x2, 0x3, 0xA, 0x9, 0x8, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB },
	{ 0x0, 0x1, 0x2, 0x3, 0x4, 0xB, 0xA, 0x9, 0x8, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC },
	{ 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0xC, 0xB, 0xA, 0x9, 0x8, 0x6, 0x7, 0xF, 0xE, 0xD },
	{ 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x7, 0xF, 0xE },
	{ 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0xF },
	{ 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8 },
};

// First track of each zone; for head 1 (8-15) the track its LBAs start from
static const uint16_t track_phys[16] = {
	0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425,
	0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A
};

// Start of each physical zone in the physical layout
static const uint32_t zone_start[16] = {
	0x0000000, 0x05F15E0, 0x0B79D00, 0x10801A0, 0x1523720, 0x1963D80, 0x1D414C0, 0x20BBCE0,
	0x23196E0, 0x28A1E00, 0x2DF5DC0, 0x3299340, 0x36D99A0, 0x3AB70E0, 0x3E31900, 0x4149200
};

static const uint8_t region_magic[2][4] = {
	{ 0xE8, 0x48, 0xD3, 0x16 },		// Japan
	{ 0x22, 0x63, 0xEE, 0x56 },		// USA
};

// The block (in sector_bytes x 85 copies) repeats its first sector
static int repeats(const uint8_t *p, uint32_t sector_bytes)
{
	uint32_t s;

	for (s = 1; s < DD_SECTORS; s++)
		if (memcmp(p, p + s * sector_bytes, sector_bytes) != 0)
			return 0;
	return 1;
}

static int sys_valid(const uint8_t *p, int dev)
{
	static const uint8_t zero[4] = { 0 };

	if (dev ? memcmp(p, zero, 4) != 0 : (memcmp(p, region_magic[0], 4) != 0 && memcmp(p, region_magic[1], 4) != 0))
		return 0;
	if (p[4] != 0x10 || p[5] < 0x10 || p[5] >= 0x17)	// format type, disk type
		return 0;
	if (p[0x18] != 0xFF || p[0x19] != 0xFF || p[0x1A] != 0xFF || p[0x1B] != 0xFF)
		return 0;
	if (p[0x1C] != 0x80)					// load address
		return 0;
	return repeats(p, dev ? SYS_SECTOR_DEV : SYS_SECTOR_RETAIL);
}

int dd_disk_check(const uint8_t *ndd, size_t size, uint8_t *err, dd_disk_info_t *info)
{
	static const int sys_blocks[4] = { 0, 1, 8, 9 };
	int i, retail = 0, dev = 0;
	const uint8_t *id;

	if (size != DD_NDD_SIZE)
		return -1;
	memset(err, 0, DD_BLOCKS);
	memset(info, 0, sizeof(*info));

	// Retail system area in blocks 0, 1, 8 and 9; a retail disk also has an
	// unreadable block 12
	for (i = 0; i < 4; i++) {
		uint32_t off = (uint32_t)sys_blocks[i] * block_size[0];

		err[sys_blocks[i]] = 1;
		if (sys_valid(ndd + off, 0)) {
			err[sys_blocks[i]] = 0;
			if (!retail)
				info->sys_offset = off;
			retail = 1;
		}
	}
	if (retail) {
		err[12] = 1;
		info->region = ndd[info->sys_offset] == region_magic[0][0] ? DD_REGION_JAPAN : DD_REGION_USA;
	} else {
		// Development disks: blocks 2, 3, 10 and 11, stored swapped in pairs
		for (i = 0; i < 4; i++) {
			int b = sys_blocks[i] + 2;
			uint32_t off = (uint32_t)(b ^ 1) * block_size[0];

			err[b] = 1;
			if (sys_valid(ndd + off, 1)) {
				err[b] = 0;
				if (!dev)
					info->sys_offset = off;
				dev = 1;
			}
		}
		if (!dev)
			return -1;
		info->region = DD_REGION_DEV;
	}
	info->type = ndd[info->sys_offset + 5] & 0x0F;

	// Disk ID in blocks 14 and 15
	for (i = 14; i <= 15; i++) {
		err[i] = !repeats(ndd + (uint32_t)i * block_size[0], SYS_SECTOR_RETAIL);
	}
	id = ndd + (err[14] ? 15 : 14) * block_size[0];
	for (i = 0; i < 4; i++)
		info->id[i] = (id[i] >= 0x20 && id[i] < 0x7F) ? (char)id[i] : '?';
	info->id[4] = '\0';
	return 0;
}

void dd_disk_to_phys(const uint8_t *ndd, uint8_t *phys, const dd_disk_info_t *info, dd_block_t *map)
{
	const uint8_t *sys = ndd + info->sys_offset;
	const uint8_t type = info->type < 7 ? info->type : 0;
	uint32_t lba, vzone = 0, pos = 0;
	int i;

	for (i = 0; i < DD_BLOCKS; i++) {
		map[i].phys = 0;
		map[i].ndd = 0xFFFFFFFF;
		map[i].size = 0;
	}

	for (lba = 0; lba < DD_LBAS; lba++) {
		uint32_t pzone, head, lba_vzone, track_start, track, defect, defects, block, bsize, off;

		if (lba >= vzone_lba[type][vzone])
			vzone++;
		pzone = vzone_pzone[type][vzone];
		head = pzone > 7;
		lba_vzone = lba - (vzone ? vzone_lba[type][vzone - 1] : 0);

		track_start = track_phys[head ? pzone - 8 : pzone];
		track = track_phys[pzone];
		if (head)
			track -= lba_vzone >> 1;
		else
			track += lba_vzone >> 1;

		// Skip the zone's defect tracks listed in the system area
		defect = pzone ? sys[8 + pzone - 1] : 0;
		defects = sys[8 + pzone] - defect;
		while (defects && sys[0x20 + defect] + track_start <= track) {
			track++;
			defect++;
			defects--;
		}

		block = ((lba & 3) == 0 || (lba & 3) == 3) ? 0 : 1;
		bsize = block_size[head ? pzone - 7 : pzone];
		off = zone_start[pzone] + (track - track_start) * bsize * 2 + bsize * block;

		if (off + bsize <= DD_PHYS_SIZE && track < DD_TRACKS) {
			memcpy(phys + off, ndd + pos, bsize);
			i = (int)(head * DD_TRACKS * 2 + track * 2 + block);
			map[i].phys = off;
			map[i].ndd = pos;
			map[i].size = (uint16_t)bsize;
		}
		pos += bsize;
	}
}

int dd_block_index(uint16_t track, uint8_t sector)
{
	return (int)(((track >> 12) & 1) * DD_TRACKS * 2 + (track & 0xFFF) * 2 + (sector >= 90));
}

uint32_t dd_sector_offset(uint16_t track, uint8_t sector, uint32_t sector_size)
{
	uint32_t t = track & 0xFFF, head = (track >> 12) & 1, block = sector >= 90, pzone = 0, i;

	for (i = 1; i < 8; i++)
		if (t >= track_phys[i])
			pzone++;
	t -= track_phys[pzone];
	pzone += head ? 8 : 0;

	return zone_start[pzone] + block_size[head ? pzone - 7 : pzone] * (2 * t + block) + (sector % 90) * sector_size;
}

int dd_track_is_rom(uint16_t track, uint8_t type)
{
	uint32_t t = track & 0xFFF, pzone = 0, i;

	for (i = 1; i < 8; i++)
		if (t >= track_phys[i])
			pzone++;
	pzone += (track >> 12) & 1;
	return pzone <= (uint32_t)type + 2;
}

const char *dd_region_name(dd_region_t region)
{
	switch (region) {
	case DD_REGION_JAPAN:
		return "Japan";
	case DD_REGION_USA:
		return "USA";
	default:
		return "development";
	}
}
