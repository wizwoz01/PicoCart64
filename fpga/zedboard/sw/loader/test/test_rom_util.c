/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for rom_util.c, cic.c and save_db.c:
 *   cc -I../src -o test_rom_util test_rom_util.c ../src/rom_util.c ../src/cic.c ../src/save_db.c && ./test_rom_util
 */

#include <stdio.h>
#include <string.h>

#include "cic.h"
#include "rom_util.h"
#include "save_db.h"

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

	CHECK(rom_save_path("0:/Resident Evil 2 (USA).z64", ".sra", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "0:/Resident Evil 2 (USA).sra") == 0);
	CHECK(rom_save_path("0:/dir.x/game", ".sra", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "0:/dir.x/game.sra") == 0);
	CHECK(rom_save_path("0:/a.z64", ".eep", out, 9) == 0);
	CHECK(strcmp(out, "0:/a.eep") == 0);
	CHECK(rom_save_path("0:/a.z64", ".eep", out, 8) == -1);
	CHECK(rom_save_path("Conker.z64", ".eep", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "Conker.eep") == 0);
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

	rom[0x3E] = 'E';
	CHECK(!rom_is_pal(rom));
	rom[0x3E] = 'J';
	CHECK(!rom_is_pal(rom));
	rom[0x3E] = 'P';
	CHECK(rom_is_pal(rom));
	rom[0x3E] = 'D';
	CHECK(rom_is_pal(rom));
	rom[0x3E] = 0;
	CHECK(!rom_is_pal(rom));
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

static void test_cic(void)
{
	static uint8_t rom[0x1000];
	size_t i;

	// Reference values from the ares implementation
	for (i = 0; i < 0xFC0; i++)
		rom[0x40 + i] = (uint8_t)(i * 7 + 3);
	CHECK(cic_ipl2_checksum(0x3F, rom + 0x40) == 0x25B12A317036ull);
	CHECK(cic_ipl2_checksum(0x91, rom + 0x40) == 0x16BD3E39F1BBull);

	CHECK(cic_detect(rom) == CIC_UNKNOWN);
	CHECK(strcmp(cic_name(CIC_UNKNOWN), "unknown") == 0);
	CHECK(cic_pico_code(CIC_UNKNOWN) == CIC_CODE_6102);
	CHECK(strcmp(cic_name(CIC_6105), "6105/7105") == 0);
	CHECK(cic_pico_code(CIC_6105) == 3);
	CHECK(cic_pico_code(CIC_6102) == 7);
	CHECK(cic_pico_code(CIC_6101) == 6);
	CHECK(cic_pico_code(CIC_7102) == 5);
	CHECK(cic_pico_code(CIC_6103) == 4);
	CHECK(cic_pico_code(CIC_6106) == 2);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void test_header_crc(void)
{
	static uint8_t rom[0x101000];
	uint32_t crc[2];

	// Reference values from n64crc.c's algorithm, which matches the headers of
	// real ROMs for each CIC
	make_z64(rom, sizeof(rom));
	CHECK(cic_header_crc(rom, CIC_6102, crc) == 0 && crc[0] == 0xF6C453DE && crc[1] == 0x6D3C908C);
	CHECK(cic_header_crc(rom, CIC_6101, crc) == 0 && crc[0] == 0xF6C453DE && crc[1] == 0x6D3C908C);
	CHECK(cic_header_crc(rom, CIC_6103, crc) == 0 && crc[0] == 0xA592715B && crc[1] == 0x6CC2672E);
	CHECK(cic_header_crc(rom, CIC_6105, crc) == 0 && crc[0] == 0xDD10FA38 && crc[1] == 0xC11B9C1E);
	CHECK(cic_header_crc(rom, CIC_6106, crc) == 0 && crc[0] == 0x5141AE92 && crc[1] == 0x66E19326);
	CHECK(cic_header_crc(rom, CIC_UNKNOWN, crc) == -1);
}

static void test_region_lock(void)
{
	static uint8_t rom[0x2000];
	// Resident Evil 2 (Europe): hangs unless osTvType is PAL
	static const uint32_t lock[] = {
		0x3C028000,				// lui   v0, 0x8000
		0x8C420300,				// lw    v0, 0x300(v0)
		0x10400003,				// beqz  v0, +3
		0x00000000,
		0x1440FFFF,				// bnez  v0, .
		0x00000000,
	};
	size_t i;

	memset(rom, 0, sizeof(rom));
	for (i = 0; i < 6; i++)
		put32(rom + 0x1100 + 4 * i, lock[i]);
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 1);
	CHECK(memcmp(rom + 0x1110, "\0\0\0\0", 4) == 0);
	CHECK(rom[0x1100] == 0x3C && rom[0x1108] == 0x10);
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 0);

	// Not locks: another address, a loop on another register, an
	// unconditional loop, or a branch elsewhere
	put32(rom + 0x1104, 0x8C420304);	// lw v0, 0x304(v0)
	put32(rom + 0x1110, 0x1440FFFF);
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 0);
	put32(rom + 0x1104, 0x8C420300);
	put32(rom + 0x1110, 0x1460FFFF);	// bnez v1, .
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 0);
	put32(rom + 0x1110, 0x1000FFFF);	// b .
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 0);
	put32(rom + 0x1110, 0x1440FFFE);	// bnez v0, .-4
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 0);

	// Other branch types on the register, and a load through another base
	put32(rom + 0x1100, 0x3C088000);	// lui   t0, 0x8000
	put32(rom + 0x1104, 0x8D030300);	// lw    v1, 0x300(t0)
	put32(rom + 0x1110, 0x0461FFFF);	// bgez  v1, .
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 1 && rom[0x1110] == 0);
	put32(rom + 0x1110, 0x1C60FFFF);	// bgtz  v1, .
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 1);

	// Only the boot code counts: nothing before 0x1000
	memset(rom, 0, sizeof(rom));
	for (i = 0; i < 6; i++)
		put32(rom + 0x800 + 4 * i, lock[i]);
	CHECK(rom_unlock_region(rom, sizeof(rom)) == 0);
}

static save_type_t save_of(const char *id, char region, uint8_t revision)
{
	static uint8_t rom[0x40];

	memcpy(rom + 0x3B, id, 3);
	rom[0x3E] = (uint8_t)region;
	rom[0x3F] = revision;
	return save_type_detect(rom);
}

static void test_save_db(void)
{
	CHECK(save_of("NSM", 'E', 0) == SAVE_EEPROM_4K);	// Super Mario 64
	CHECK(save_of("NFU", 'E', 0) == SAVE_EEPROM_16K);	// Conker's Bad Fur Day
	CHECK(save_of("NRE", 'P', 0) == SAVE_SRAM);	// Resident Evil 2
	CHECK(save_of("CZL", 'J', 0) == SAVE_SRAM);	// Zelda: Ocarina of Time (J)
	CHECK(save_of("NZS", 'E', 0) == SAVE_FLASHRAM);	// Zelda: Majora's Mask
	CHECK(save_of("CDZ", 'J', 0) == SAVE_SRAM_96K);	// Dezaemon 3D
	CHECK(save_of("NK4", 'J', 0) == SAVE_SRAM);	// Kirby 64, early Japanese
	CHECK(save_of("NK4", 'E', 0) == SAVE_EEPROM_16K);
	CHECK(save_of("ND3", 'J', 0) == SAVE_EEPROM_16K);	// Castlevania (J)
	CHECK(save_of("ND3", 'E', 0) == SAVE_NONE);
	CHECK(save_of("NZZ", 'E', 0) == SAVE_UNKNOWN);
	CHECK(strcmp(save_type_name(SAVE_EEPROM_16K), "EEPROM 16 Kbit") == 0);
}

int main(void)
{
	test_formats();
	test_names();
	test_header();
	test_sram();
	test_cic();
	test_header_crc();
	test_region_lock();
	test_save_db();

	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
