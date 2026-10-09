/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Writes pattern.bin for n64test: mkpattern <file> <size in bytes>
 */

#include <stdio.h>
#include <stdlib.h>

#include "pattern.h"

int main(int argc, char **argv)
{
	static uint8_t buf[65536];
	unsigned long size;
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <file> <size in bytes, multiple of 64 KB>\n", argv[0]);
		return 1;
	}
	size = strtoul(argv[2], NULL, 0);
	if (size == 0 || size % sizeof(buf) != 0) {
		fprintf(stderr, "%s: size must be a multiple of 64 KB\n", argv[0]);
		return 1;
	}
	f = fopen(argv[1], "wb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	for (uint32_t o = 0; o < size; o += sizeof(buf)) {
		for (uint32_t i = 0; i < sizeof(buf); i += 4) {
			uint32_t x = pattern(o + i);

			buf[i] = (uint8_t)(x >> 24);
			buf[i + 1] = (uint8_t)(x >> 16);
			buf[i + 2] = (uint8_t)(x >> 8);
			buf[i + 3] = (uint8_t)x;
		}
		if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
			perror(argv[1]);
			return 1;
		}
	}
	return fclose(f) ? 1 : 0;
}
