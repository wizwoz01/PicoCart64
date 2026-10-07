/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for rom_util.c:
 *   cc -I../src -o test_rom_util test_rom_util.c ../src/rom_util.c && ./test_rom_util
 */

#include <stdio.h>
#include <string.h>

#include "rom_util.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void make_z64(uint8_t *rom, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		rom[i] = (uint8_t)(i * 7 + 3);
	rom[0] = 0x80;
	rom[1] = 0x37;
	rom[2] = 0x12;
	rom[3] = 0x40;
}

static void test_formats(void)
{
	static uint8_t z64[4096], v64[4096], n64[4096], tmp[4096];
	size_t i;

	make_z64(z64, sizeof(z64));
	for (i = 0; i < sizeof(z64); i += 2) {
		v64[i] = z64[i + 1];
		v64[i + 1] = z64[i];
	}
	for (i = 0; i < sizeof(z64); i += 4) {
		n64[i] = z64[i + 3];
		n64[i + 1] = z64[i + 2];
		n64[i + 2] = z64[i + 1];
		n64[i + 3] = z64[i];
	}

	CHECK(rom_detect_format(z64) == ROM_FMT_Z64);
	CHECK(rom_detect_format(v64) == ROM_FMT_V64);
	CHECK(rom_detect_format(n64) == ROM_FMT_N64);
	CHECK(rom_detect_format((const uint8_t *)"\0\0\0\0") == ROM_FMT_UNKNOWN);

	memcpy(tmp, z64, sizeof(tmp));
	rom_to_z64(tmp, sizeof(tmp), ROM_FMT_Z64);
	CHECK(memcmp(tmp, z64, sizeof(tmp)) == 0);

	memcpy(tmp, v64, sizeof(tmp));
	rom_to_z64(tmp, sizeof(tmp), ROM_FMT_V64);
	CHECK(memcmp(tmp, z64, sizeof(tmp)) == 0);

	// Converted in two chunks, like the loader does
	memcpy(tmp, n64, sizeof(tmp));
	rom_to_z64(tmp, 1024, ROM_FMT_N64);
	rom_to_z64(tmp + 1024, sizeof(tmp) - 1024, ROM_FMT_N64);
	CHECK(memcmp(tmp, z64, sizeof(tmp)) == 0);
}

static void test_names(void)
{
	char out[64];

	CHECK(rom_has_rom_extension("Resident Evil 2 (USA).z64"));
	CHECK(rom_has_rom_extension("a.V64"));
	CHECK(rom_has_rom_extension("b.N64"));
	CHECK(!rom_has_rom_extension("game.sra"));
	CHECK(!rom_has_rom_extension(".z64"));
	CHECK(!rom_has_rom_extension("z64"));

	CHECK(rom_save_path("0:/Resident Evil 2 (USA).z64", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "0:/Resident Evil 2 (USA).sra") == 0);
	CHECK(rom_save_path("0:/dir.x/game", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "0:/dir.x/game.sra") == 0);
	CHECK(rom_save_path("0:/a.z64", out, 9) == 0);
	CHECK(strcmp(out, "0:/a.sra") == 0);
	CHECK(rom_save_path("0:/a.z64", out, 8) == -1);
}

static void test_header(void)
{
	static uint8_t rom[4096];
	char title[ROM_TITLE_LEN + 1];

	make_z64(rom, sizeof(rom));
	memcpy(rom + 0x20, "RESIDENT EVIL II    ", 20);
	rom_title(rom, title);
	CHECK(strcmp(title, "RESIDENT EVIL II") == 0);

	rom[0x20] = 0x82;
	rom_title(rom, title);
	CHECK(title[0] == '?');

	CHECK(crc32_calc((const uint8_t *)"123456789", 9) == 0xCBF43926u);
	CHECK(rom_ipl3_crc32(rom) == crc32_calc(rom + 0x40, 0xFC0));
}

static void test_sram(void)
{
	// Same values as the FPGA testbench: N64 halfwords 0x1234, 0x1335
	const uint8_t b[4] = { 0x12, 0x34, 0x13, 0x35 };
	uint8_t out[4];

	CHECK(sram_pack(b) == 0x12341335u);
	sram_unpack(0xCAFEBABEu, out);
	CHECK(out[0] == 0xCA && out[1] == 0xFE && out[2] == 0xBA && out[3] == 0xBE);
	sram_unpack(sram_pack(b), out);
	CHECK(memcmp(out, b, 4) == 0);
}

int main(void)
{
	test_formats();
	test_names();
	test_header();
	test_sram();

	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
