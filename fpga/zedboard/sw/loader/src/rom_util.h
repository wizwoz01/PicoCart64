/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Platform independent helpers for the ZedBoard ROM loader.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
	ROM_FMT_UNKNOWN,
	ROM_FMT_Z64,				// big-endian, native
	ROM_FMT_V64,				// 16-bit byte-swapped
	ROM_FMT_N64,				// 32-bit little-endian
} rom_format_t;

// CRC32 of the IPL3 (0x40-0x1000) shipped with CIC 6102/7101 games
#define IPL3_CRC_6102 0x90BB6CB5u

#define ROM_TITLE_LEN 20

const char *rom_format_name(rom_format_t fmt);

// Detects the byte order from the first 4 bytes of the file
rom_format_t rom_detect_format(const uint8_t *first4);

// Converts buf in place to .z64 byte order. len should be a multiple of 4,
// except for the last chunk of a file.
void rom_to_z64(uint8_t * buf, size_t len, rom_format_t fmt);

// True for *.z64, *.v64 and *.n64 (any case)
int rom_has_rom_extension(const char *name);

// "dir/Game.z64" -> "dir/Game.sra". Returns 0 on success, -1 if out is too small.
int rom_save_path(const char *rom_path, char *out, size_t out_len);

// Copies the header title (0x20-0x33) to out (ROM_TITLE_LEN + 1 bytes),
// with non-printable characters replaced and trailing spaces removed.
void rom_title(const uint8_t * z64, char *out);

uint32_t crc32_calc(const uint8_t * buf, size_t len);

// CRC32 of the IPL3 area of a .z64 image (needs at least 0x1000 bytes)
uint32_t rom_ipl3_crc32(const uint8_t * z64);

// SRAM window word <-> N64 SRAM bytes (see cart_regs.v): word = b0<<24 | b1<<16 | b2<<8 | b3
uint32_t sram_pack(const uint8_t * b);
void sram_unpack(uint32_t w, uint8_t * b);
