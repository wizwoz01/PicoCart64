/**
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * CIC type of a ROM, found from its boot code (IPL3).
 *
 * cic_ipl2_checksum() and the seed/checksum table are ported from ares
 * (mia/medium/nintendo-64.cpp):
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

#include <stddef.h>

#include "cic.h"

static const struct {
	cic_t cic;
	uint8_t seed;
	uint64_t checksum;
	uint8_t code;
	const char *name;
} cics[] = {
	{ CIC_6102, 0x3F, 0xA536C0F1D859ull, CIC_CODE_6102, "6102/7101" },
	{ CIC_6101, 0x3F, 0x45CC73EE317Aull, CIC_CODE_6101, "6101" },
	{ CIC_7102, 0x3F, 0x44160EC5D9AFull, CIC_CODE_7102, "7102" },
	{ CIC_6103, 0x78, 0x586FD4709867ull, CIC_CODE_6103, "6103/7103" },
	{ CIC_6105, 0x91, 0x8618A45BC2D3ull, CIC_CODE_6105, "6105/7105" },
	{ CIC_6106, 0x85, 0x2BBAD4E6EB74ull, CIC_CODE_6106, "6106/7106" },
	{ CIC_DD_JP, 0xDD, 0x32B294E2AB90ull, CIC_CODE_DD_JP, "64DD Japan (8303)" },
	{ CIC_DD_US, 0xDE, 0x05BA2EF0A5F1ull, CIC_CODE_DD_US, "64DD USA" },
};

static uint32_t rotl(uint32_t v, uint32_t s)
{
	s &= 31;
	return s ? (v << s) | (v >> (32 - s)) : v;
}

static uint32_t rotr(uint32_t v, uint32_t s)
{
	s &= 31;
	return s ? (v >> s) | (v << (32 - s)) : v;
}

static uint32_t csum(uint32_t a0, uint32_t a1, uint32_t a2)
{
	uint64_t prod;
	uint32_t diff;

	if (a1 == 0)
		a1 = a2;
	prod = (uint64_t)a0 * a1;
	diff = (uint32_t)(prod >> 32) - (uint32_t)prod;
	return diff ? diff : a0;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

uint64_t cic_ipl2_checksum(uint8_t seed, const uint8_t *ipl3)
{
	uint32_t state[16], buf[4];
	uint32_t data = be32(ipl3), data_next = data, data_last;
	uint32_t init = (0x6C078965u * seed + 1) ^ data;
	uint32_t loop;
	int i;

	ipl3 += 4;
	for (i = 0; i < 16; i++)
		state[i] = init;

	for (loop = 1;; loop++) {
		data_last = data;
		data = data_next;

		state[0] += csum(1007 - loop, data, loop);
		state[1] = csum(state[1], data, loop);
		state[2] ^= data;
		state[3] += csum(data + 5, 0x6C078965u, loop);
		state[9] = (data_last < data) ? csum(state[9], data, loop) : state[9] + data;
		state[4] += rotr(data, data_last & 0x1F);
		state[7] = csum(state[7], rotl(data, data_last & 0x1F), loop);
		state[6] = (data < state[6]) ? (state[3] + state[6]) ^ (data + loop) : (state[4] + data) ^ state[6];
		state[5] += rotl(data, data_last >> 27);
		state[8] = csum(state[8], rotr(data, data_last >> 27), loop);

		if (loop == 1008)
			break;

		data_next = be32(ipl3);
		ipl3 += 4;
		state[15] = csum(csum(state[15], rotl(data, data_last >> 27), loop), rotl(data_next, data >> 27), loop);
		state[14] = csum(csum(state[14], rotr(data, data_last & 0x1F), loop), rotr(data_next, data & 0x1F), loop);
		state[13] += rotr(data, data & 0x1F) + rotr(data_next, data_next & 0x1F);
		state[10] = csum(state[10] + data, data_next, loop);
		state[11] = csum(state[11] ^ data, data_next, loop);
		state[12] += state[8] ^ data;
	}

	for (i = 0; i < 4; i++)
		buf[i] = state[0];

	for (loop = 0; loop < 16; loop++) {
		uint32_t tmp, bit0;

		data = state[loop];
		tmp = buf[0] + rotr(data, data & 0x1F);
		buf[0] = tmp;
		buf[1] = (data < tmp) ? buf[1] + data : csum(buf[1], data, loop);
		bit0 = data & 0x01;
		buf[2] = (((data & 0x02) >> 1) == bit0) ? buf[2] + data : csum(buf[2], data, loop);
		buf[3] = bit0 ? buf[3] ^ data : csum(buf[3], data, loop);
	}

	return (((uint64_t)csum(buf[0], buf[1], 16) << 32) | (buf[3] ^ buf[2])) & 0xFFFFFFFFFFFFull;
}

cic_t cic_detect(const uint8_t *z64)
{
	uint64_t sum = 0;
	int seed = -1;
	size_t i;

	for (i = 0; i < sizeof(cics) / sizeof(cics[0]); i++) {
		if (cics[i].seed != seed) {
			seed = cics[i].seed;
			sum = cic_ipl2_checksum((uint8_t)seed, z64 + 0x40);
		}
		if (sum == cics[i].checksum)
			return cics[i].cic;
	}
	return CIC_UNKNOWN;
}

const char *cic_name(cic_t cic)
{
	size_t i;

	for (i = 0; i < sizeof(cics) / sizeof(cics[0]); i++)
		if (cics[i].cic == cic)
			return cics[i].name;
	return "unknown";
}

// As in n64crc.c (Parasyte): the boot code's checksum
int cic_header_crc(const uint8_t *z64, cic_t cic, uint32_t crc[2])
{
	uint32_t seed, t1, t2, t3, t4, t5, t6, i;

	switch (cic) {
	case CIC_6101:
	case CIC_6102:
	case CIC_7102:
		seed = 0xF8CA4DDC;
		break;
	case CIC_6103:
		seed = 0xA3886759;
		break;
	case CIC_6105:
		seed = 0xDF26F436;
		break;
	case CIC_6106:
		seed = 0x1FEA617A;
		break;
	default:
		return -1;
	}

	t1 = t2 = t3 = t4 = t5 = t6 = seed;
	for (i = 0x1000; i < 0x101000; i += 4) {
		uint32_t d = be32(z64 + i), r = rotl(d, d);

		if (t6 + d < t6)
			t4++;
		t6 += d;
		t3 ^= d;
		t5 += r;
		t2 ^= (t2 > d) ? r : t6 ^ d;
		t1 += (cic == CIC_6105) ? be32(z64 + 0x750 + (i & 0xFF)) ^ d : t5 ^ d;
	}

	if (cic == CIC_6103) {
		crc[0] = (t6 ^ t4) + t3;
		crc[1] = (t5 ^ t2) + t1;
	} else if (cic == CIC_6106) {
		crc[0] = t6 * t4 + t3;
		crc[1] = t5 * t2 + t1;
	} else {
		crc[0] = t6 ^ t4 ^ t3;
		crc[1] = t5 ^ t2 ^ t1;
	}
	return 0;
}

uint8_t cic_pico_code(cic_t cic)
{
	size_t i;

	for (i = 0; i < sizeof(cics) / sizeof(cics[0]); i++)
		if (cics[i].cic == cic)
			return cics[i].code;
	return CIC_CODE_6102;
}
