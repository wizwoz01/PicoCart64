/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Test pattern shared by mkpattern.c (on the PC) and n64test.c (on the N64).
 */

#pragma once

#include <stdint.h>

// The 32-bit word at byte offset o of pattern.bin, stored big-endian. Every
// word differs from its neighbours in about half its bits, so a stuck, swapped
// or shorted AD line, or a halfword read from the wrong address, shows up.
static inline uint32_t pattern(uint32_t o)
{
	uint32_t x = o * 0x9E3779B1u;

	x ^= x >> 15;
	x *= 0x85EBCA77u;
	x ^= x >> 13;
	return x;
}
