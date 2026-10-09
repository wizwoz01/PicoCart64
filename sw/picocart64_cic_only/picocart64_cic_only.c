/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Konrad Beckmann
 */

/*
 * CIC-only firmware.
 *
 * The Pico only answers the CIC handshake on N64_CIC_DCLK / N64_CIC_DIO.
 * The PI bus (N64_AD0..15, ALEL, ALEH, WRITE, READ) is never driven, so an
 * external device (e.g. an FPGA board wired to the same pins) can serve
 * ROM and SRAM instead.
 *
 * The external device picks the CIC type on GPIO26-28 (header J1 of the Lite
 * board). It only pulls these lines low or releases them, and the Pico pulls
 * them up, so an unconnected Pico reads 7 and answers as a 6102/7101.
 *
 * The PIF checks the CIC for as long as the console is on and halts it on a
 * single late or wrong answer, so the CIC runs alone with interrupts off and
 * polls the pins (cic_poll.c), instead of answering from interrupts.
 *
 * The Pico's LED turns on once the PIF has accepted the CIC, then toggles on
 * every command the PIF sends: if it stops changing while the game runs, the
 * PIF has stopped talking to the CIC.
 */

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#include "cic_poll.h"
#include "picocart64_pins.h"

#define CIC_TYPE_PIN0 (26)		/* bit 0, then 27 and 28 */
#define LED_PIN       (25)		/* the Pico's LED, also on J1 pin 1 */

// Seed and checksum for each code on the CIC type lines
static const struct {
	uint8_t seed;
	uint8_t checksum[6];
} cic_types[8] = {
	[0] = { 0x3F, { 0xA5, 0x36, 0xC0, 0xF1, 0xD8, 0x59 } },	// unused: 6102/7101
	[1] = { 0x3F, { 0xA5, 0x36, 0xC0, 0xF1, 0xD8, 0x59 } },	// unused: 6102/7101
	[2] = { 0x85, { 0x2B, 0xBA, 0xD4, 0xE6, 0xEB, 0x74 } },	// 6106/7106
	[3] = { 0x91, { 0x86, 0x18, 0xA4, 0x5B, 0xC2, 0xD3 } },	// 6105/7105
	[4] = { 0x78, { 0x58, 0x6F, 0xD4, 0x70, 0x98, 0x67 } },	// 6103/7103
	[5] = { 0x3F, { 0x44, 0x16, 0x0E, 0xC5, 0xD9, 0xAF } },	// 7102
	[6] = { 0x3F, { 0x45, 0xCC, 0x73, 0xEE, 0x31, 0x7A } },	// 6101
	[7] = { 0x3F, { 0xA5, 0x36, 0xC0, 0xF1, 0xD8, 0x59 } },	// 6102/7101
};

int main(void)
{
	uint32_t code;

	set_sys_clock_khz(CONFIG_CPU_FREQ_MHZ * 1000, true);

	// Every GPIO is a floating input. The PI bus pins must stay high-Z
	// (no pulls) since the external device and the N64 share them.
	for (int i = 0; i <= 28; i++) {
		gpio_init(i);
		gpio_set_dir(i, GPIO_IN);
		gpio_set_pulls(i, false, false);
	}

	// Enable pull up on N64_CIC_DIO since there is no external one.
	gpio_pull_up(N64_CIC_DIO);

	// CIC type lines: open drain from the external device
	for (int i = CIC_TYPE_PIN0; i < CIC_TYPE_PIN0 + 3; i++)
		gpio_pull_up(i);

	// LED off until the PIF accepts the CIC
	gpio_put(LED_PIN, 0);
	gpio_set_dir(LED_PIN, GPIO_OUT);

	// Nothing else runs from here on
	save_and_disable_interrupts();

	// The type lines are set before the N64 is powered on; read them as it
	// leaves reset, right before the handshake.
	while (!gpio_get(N64_COLD_RESET))
		tight_loop_contents();
	code = (gpio_get_all() >> CIC_TYPE_PIN0) & 7;

	// Returns only when the console is off, and then the Pico has no power
	cic_poll_run(cic_types[code].seed, cic_types[code].checksum, CONFIG_REGION_PAL, LED_PIN);

	return 0;
}
