/**
 * SPDX-License-Identifier: MIT
 *
 * CIC side of the PIF-CIC protocol, polling the pins (see cic_poll.c).
 */

#pragma once

#include <stdint.h>

/**
 * @brief Answers the PIF until the console is turned off. Call with
 * interrupts disabled, once COLD_RESET is high.
 *
 * @param seed     The CIC type's seed
 * @param checksum The CIC type's IPL3 checksum, most significant byte first
 * @param pal      1 for a PAL console, 0 for NTSC/MPAL
 * @param led_pin  GPIO of an LED that turns on after the handshake and
 *                 toggles on every command, or -1
 */
void cic_poll_run(uint8_t seed, const uint8_t checksum[6], int pal, int led_pin);
