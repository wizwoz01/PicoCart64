/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Platform independent helpers for the ZedBoard ROM loader.
 */

#include <string.h>

#include "rom_util.h"

const char *rom_format_name(rom_format_t fmt)
{
	switch (fmt) {
	case ROM_FMT_Z64:
		return "z64";
	case ROM_FMT_V64:
		return "v64 (byte-swapped)";
	case ROM_FMT_N64:
		return "n64 (little-endian)";
	default:
		return "unknown";
	}
}

// The header's first word is the PI timing: 0x80 first and the latency (0x40)
// last. Games use 0x80371240; the 64DD IPL ROM 0x80270740.
rom_format_t rom_detect_format(const uint8_t *first4)
{
	if (first4[0] == 0x80 && first4[3] == 0x40)
		return ROM_FMT_Z64;
	if (first4[1] == 0x80 && first4[2] == 0x40)
		return ROM_FMT_V64;
	if (first4[3] == 0x80 && first4[0] == 0x40)
		return ROM_FMT_N64;
	return ROM_FMT_UNKNOWN;
}

void rom_to_z64(uint8_t *buf, size_t len, rom_format_t fmt)
{
	size_t i;
	uint8_t t;

	if (fmt == ROM_FMT_V64) {
		for (i = 0; i + 1 < len; i += 2) {
			t = buf[i];
			buf[i] = buf[i + 1];
			buf[i + 1] = t;
		}
	} else if (fmt == ROM_FMT_N64) {
		for (i = 0; i + 3 < len; i += 4) {
			t = buf[i];
			buf[i] = buf[i + 3];
			buf[i + 3] = t;
			t = buf[i + 1];
			buf[i + 1] = buf[i + 2];
			buf[i + 2] = t;
		}
	}
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int rom_has_rom_extension(const char *name)
{
	size_t n = strlen(name);
	char ext[4];

	if (n < 5 || name[n - 4] != '.')
		return 0;
	ext[0] = lower(name[n - 3]);
	ext[1] = lower(name[n - 2]);
	ext[2] = lower(name[n - 1]);
	ext[3] = '\0';
	return strcmp(ext, "z64") == 0 || strcmp(ext, "v64") == 0 || strcmp(ext, "n64") == 0;
}

int rom_save_path(const char *rom_path, const char *ext, char *out, size_t out_len)
{
	const char *slash = strrchr(rom_path, '/');
	const char *dot = strrchr(rom_path, '.');
	size_t base_len;

	if (dot == NULL || (slash != NULL && dot < slash))
		base_len = strlen(rom_path);
	else
		base_len = (size_t)(dot - rom_path);

	if (base_len + strlen(ext) + 1 > out_len)
		return -1;
	memcpy(out, rom_path, base_len);
	strcpy(out + base_len, ext);
	return 0;
}

void rom_title(const uint8_t *z64, char *out)
{
	int i;

	for (i = 0; i < ROM_TITLE_LEN; i++) {
		uint8_t c = z64[0x20 + i];
		out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
	}
	out[ROM_TITLE_LEN] = '\0';
	for (i = ROM_TITLE_LEN - 1; i >= 0 && out[i] == ' '; i--)
		out[i] = '\0';
}

int rom_is_pal(const uint8_t *z64)
{
	return strchr("DFHILPSUWXYZ", z64[0x3E]) != NULL && z64[0x3E] != 0;
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// A conditional branch on register r back to itself: loops while r says so
static int branch_to_self_on(uint32_t w, unsigned r)
{
	unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;

	if ((w & 0xFFFF) != 0xFFFF)
		return 0;
	switch (op) {
	case 0x04:					// BEQ, BNE, BEQL, BNEL
	case 0x05:
	case 0x14:
	case 0x15:
		return (rs == r || rt == r) && rs != rt;
	case 0x06:					// BLEZ, BGTZ, BLEZL, BGTZL
	case 0x07:
	case 0x16:
	case 0x17:
		return rs == r && rt == 0;
	case 0x01:					// BLTZ, BGEZ, BLTZL, BGEZL
		return rs == r && rt <= 3;
	default:
		return 0;
	}
}

int rom_unlock_region(uint8_t *z64, size_t size)
{
	const size_t end = size < 0x101000 ? size : 0x101000;
	size_t i, j, k;
	int n = 0;

	for (i = 0x1000; i + 4 <= end; i += 4) {
		uint32_t w = rd32(z64 + i);
		unsigned base;

		// LUI base, 0x8000, then within 4 instructions LW r, 0x300(base)
		if ((w & 0xFFE0FFFF) != 0x3C008000)
			continue;
		base = (w >> 16) & 31;
		for (j = i + 4; j <= i + 16 && j + 4 <= end; j += 4) {
			unsigned r;

			w = rd32(z64 + j);
			if ((w & 0xFFE0FFFF) != (0x8C000300u | (base << 21)))
				continue;
			r = (w >> 16) & 31;
			// and within 8 more a branch to itself on r: replace it with a NOP
			for (k = j + 4; k <= j + 32 && k + 4 <= end; k += 4) {
				if (branch_to_self_on(rd32(z64 + k), r)) {
					z64[k] = z64[k + 1] = z64[k + 2] = z64[k + 3] = 0;
					n++;
					break;
				}
			}
			break;
		}
	}
	return n;
}

uint32_t sram_pack(const uint8_t *b)
{
	return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3];
}

void sram_unpack(uint32_t w, uint8_t *b)
{
	b[0] = (uint8_t)(w >> 24);
	b[1] = (uint8_t)(w >> 16);
	b[2] = (uint8_t)(w >> 8);
	b[3] = (uint8_t)w;
}
