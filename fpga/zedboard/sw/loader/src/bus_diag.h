/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Tells from the cart's bus monitor (MON_* registers, see cart_regs.v) how far
 * the N64 got after it was turned on, and which wire to check if it stopped.
 * Platform independent.
 */

#pragma once

#include <stdint.h>

// Bus monitor registers. They are cleared when the N64 is turned on and
// their counts saturate at 0xFFFF.
typedef struct {
	uint32_t ale;				// [15:0] ALE_H falls, [31:16] ALE_L falls
	uint32_t rw;				// [15:0] ~READ pulses, [31:16] ~WRITE pulses
	uint32_t first_addr;		// first address the N64 sent
	uint32_t first_data;		// first two ROM halfwords on AD, first one in [31:16]
	uint32_t rom;				// [15:0] ROM halfwords served, [31:16] of those where
								// AD didn't read back what the cart drove
	uint32_t ad_or;				// OR of all address halves: [31:16] high, [15:0] low
	uint32_t glitch;			// [15:0] ALE pulses ignored as noise, [31:16] underruns
	uint32_t odd;				// [15:0] transfers that read an odd number of halfwords,
								// [31:16] ~READ low pulses under 150 ns
	uint32_t sdat;				// EEPROM line: [15:0] S_DAT falls, [31:16] commands answered
	uint32_t sdat_low;			// [15:0] longest time S_DAT was low, [31:16] low now (us)
	int sdat_high;				// S_DAT level now (PINS)
} bus_mon_t;

// Addresses the N64 sends while booting: the ROM header, then the boot code
// (IPL3) one word at a time, then the first 1 MB of the game in 512-byte pages
#define BUS_BOOT_CODE_ADDRS   1009
#define BUS_GAME_LOAD_ADDRS   (BUS_BOOT_CODE_ADDRS + 2048)

typedef enum {
	BUS_OK,
	BUS_STOPPED,				// the boot stopped, with no sign of a wiring fault
	BUS_FAULT,					// a wire or the Pico looks wrong
} bus_verdict_t;

// One line about the N64's accesses since it was turned on. header is the
// first word of the ROM (big-endian), e.g. 0x80371240.
bus_verdict_t bus_diagnose(const bus_mon_t * m, uint32_t header, char *out, int len);

// A line about noise on the bus and late ROM words, or 0 if there was none
int bus_noise(const bus_mon_t * m, char *out, int len);

// S_DAT, the EEPROM line, is open drain: when it stays low, the PIF stops
// answering and the game freezes at its next controller or save access
#define BUS_SDAT_HELD_US      1000

// One line about S_DAT. Returns 1 if it was held low for BUS_SDAT_HELD_US.
int bus_sdat(const bus_mon_t * m, char *out, int len);

// "20.012345 s  +3.04 us  line fell" for an S_DAT log entry (SDAT_LOG, see
// cart_regs.v); prev is the entry before it, or e itself for the first one
void bus_sdat_event(uint32_t e, uint32_t prev, char *out, int len);

// "ALE_L 60 ns at 12345 us" for a noise log entry (NOISE, see cart_regs.v)
void bus_noise_event(uint32_t e, char *out, int len);

#define BUS_TRACE_LEN  1024		// address phases in the trace
#define BUS_MSG_LEN    120

// Compares the address trace (TRACE_A and TRACE_B, see cart_regs.v) with the
// addresses every boot sends: 0x10000000, then 0x10000040, 0x10000044, ...
// 0x10000FFC. Each AD input that doesn't follow its own line is named, with
// the line it follows instead. n: entries recorded. Writes up to max lines
// into out and returns how many; *clean is set if every input was right.
int bus_trace_check(const uint32_t * a, const uint32_t * b, int n, char out[][BUS_MSG_LEN], int max, int *clean);

// "JB7" for AD12
const char *bus_ad_pin(int ad);

// One transfer from the transfer log (XFER, see cart_regs.v)
typedef struct {
	uint32_t addr;
	uint32_t rw;				// {writes, reads}
	uint32_t sum;				// sum of the halfwords that were on AD
	uint32_t us;				// microseconds after power on
} bus_xfer_t;

// Checks a ROM read against the ROM image (.z64 order, size bytes). Returns
// 1 if the sum matches, 0 if not, -1 if it isn't a plain ROM read.
int bus_xfer_check(const bus_xfer_t * x, const uint8_t * rom, uint32_t size, uint32_t *expected);
