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

rom_format_t rom_detect_format(const uint8_t *first4)
{
	if (first4[0] == 0x80 && first4[1] == 0x37 && first4[2] == 0x12 && first4[3] == 0x40)
		return ROM_FMT_Z64;
	if (first4[0] == 0x37 && first4[1] == 0x80 && first4[2] == 0x40 && first4[3] == 0x12)
		return ROM_FMT_V64;
	if (first4[0] == 0x40 && first4[1] == 0x12 && first4[2] == 0x37 && first4[3] == 0x80)
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

int rom_save_path(const char *rom_path, char *out, size_t out_len)
{
	const char *slash = strrchr(rom_path, '/');
	const char *dot = strrchr(rom_path, '.');
	size_t base_len;

	if (dot == NULL || (slash != NULL && dot < slash))
		base_len = strlen(rom_path);
	else
		base_len = (size_t)(dot - rom_path);

	if (base_len + 5 > out_len)
		return -1;
	memcpy(out, rom_path, base_len);
	memcpy(out + base_len, ".sra", 5);
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

uint32_t crc32_calc(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

uint32_t rom_ipl3_crc32(const uint8_t *z64)
{
	return crc32_calc(z64 + 0x40, 0x1000 - 0x40);
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
