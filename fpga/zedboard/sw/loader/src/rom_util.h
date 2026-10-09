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

#define ROM_TITLE_LEN 20

const char *rom_format_name(rom_format_t fmt);

// Detects the byte order from the first 4 bytes of the file
rom_format_t rom_detect_format(const uint8_t *first4);

// Converts buf in place to .z64 byte order. len should be a multiple of 4,
// except for the last chunk of a file.
void rom_to_z64(uint8_t * buf, size_t len, rom_format_t fmt);

// True for *.z64, *.v64 and *.n64 (any case)
int rom_has_rom_extension(const char *name);

// "dir/Game.z64", ".sra" -> "dir/Game.sra". Returns 0 on success, -1 if out is too small.
int rom_save_path(const char *rom_path, const char *ext, char *out, size_t out_len);

// Copies the header title (0x20-0x33) to out (ROM_TITLE_LEN + 1 bytes),
// with non-printable characters replaced and trailing spaces removed.
void rom_title(const uint8_t * z64, char *out);

// True if the header's region code (0x3E) is a PAL one
int rom_is_pal(const uint8_t * z64);

// Removes region locks from the boot code (ROM 0x1000-0x101000): a read of the
// console's TV type (osTvType, 0x80000300) followed by a branch to itself on
// it, which hangs the game on a console of another region. Each such branch
// becomes a NOP. Returns how many were removed; the header checksum then has
// to be updated (cic_header_crc).
int rom_unlock_region(uint8_t * z64, size_t size);

// SRAM window word <-> N64 SRAM bytes (see cart_regs.v): word = b0<<24 | b1<<16 | b2<<8 | b3
uint32_t sram_pack(const uint8_t * b);
void sram_unpack(uint32_t w, uint8_t * b);
