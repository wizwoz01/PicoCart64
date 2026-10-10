// CIC side of the PIF-CIC protocol for the RP2040, polling the pins.
//
// Follows the CIC firmware of SummerCart64 (sw/cic/cic.c), which is based on
// https://github.com/jago85/UltraCIC_C. SummerCart64 stopped random freezes
// by moving this protocol out of interrupt handlers into a loop that waits on
// the clock directly; this file does the same: the caller runs it with
// interrupts off and nothing else running.

// MIT License

// Copyright (c) 2019 Jan Goldacker
// Copyright (c) 2022-2025 Mateusz Faderewski

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "cic_poll.h"
#include "picocart64_pins.h"

enum {
	CMD_COMPARE = 0,
	CMD_DIE = 1,
	CMD_X105 = 2,
	CMD_SOFT_RESET = 3,
};

static uint8_t cic_ram[32];

// Initial RAM, two nibbles per byte: NTSC, PAL
static const uint8_t cic_ram_init[2][16] = { {
		0xE0, 0x9A, 0x18, 0x5A, 0x13, 0xE1, 0x0D, 0xEC, 0x0B, 0x14, 0xF8, 0xB5, 0x7C, 0xD6, 0x1E, 0x98}, {
		0xE0, 0x4F, 0x51, 0x21, 0x71, 0x98, 0x57, 0x5A, 0x0B, 0x12, 0x3F, 0x82, 0x71, 0x98, 0x11, 0x5C}
};

static int cic_led = -1;

// The PIF's clock phases last microseconds: a level only counts after three
// equal reads in a row, so short noise from nearby wires can't fake an edge.
static void __time_critical_func(cic_clk_wait) (bool level)
{
	int n = 0;

	while (n < 3) {
		if (gpio_get(N64_CIC_DCLK) == level)
			n++;
		else
			n = 0;
	}
}

// DIO by majority of three reads
static bool __time_critical_func(cic_dq_get) (void)
{
	int n = gpio_get(N64_CIC_DIO);

	n += gpio_get(N64_CIC_DIO);
	n += gpio_get(N64_CIC_DIO);
	return n >= 2;
}

// DIO is open drain: pull it low, or let the pull-up take it high
static void __time_critical_func(cic_dq_set) (bool value)
{
	if (value) {
		gpio_set_dir(N64_CIC_DIO, GPIO_IN);
	} else {
		gpio_put(N64_CIC_DIO, 0);
		gpio_set_dir(N64_CIC_DIO, GPIO_OUT);
	}
}

static uint8_t __time_critical_func(cic_read_bit) (void)
{
	uint8_t value;

	cic_clk_wait(false);
	value = cic_dq_get() ? 1 : 0;
	cic_clk_wait(true);
	return value;
}

static void __time_critical_func(cic_write_bit) (uint8_t value)
{
	cic_clk_wait(false);
	cic_dq_set(value);
	cic_clk_wait(true);
	cic_dq_set(true);
}

static uint8_t cic_read(int length)
{
	uint8_t data = 0;

	for (int i = 0; i < length; i++)
		data = ((data << 1) | cic_read_bit());
	return data;
}

static void cic_write(uint8_t data, int length)
{
	for (int i = (length - 1); i >= 0; i--)
		cic_write_bit(data & (1 << i));
}

static void cic_write_ram_nibbles(uint8_t index)
{
	do {
		cic_write(cic_ram[index++], 4);
	} while ((index & 0x0F) != 0);
}

static void cic_encode_round(uint8_t index)
{
	uint8_t data = cic_ram[index++];

	do {
		data = ((((data + 1) & 0x0F) + cic_ram[index]) & 0x0F);
		cic_ram[index++] = data;
	} while ((index & 0x0F) != 0);
}

// 64DD: the first ID bit belongs to the drive and reads high when no cartridge
// CIC pulls it low. Returns 0 if a cartridge CIC did: the console boots the
// cartridge then, and this CIC has to stay quiet.
static int cic_dd_bit_free(void)
{
	int n = 0;

	cic_clk_wait(false);
	while (n < 3) {
		if (gpio_get(N64_CIC_DCLK)) {
			n++;
		} else {
			n = 0;
			if (!cic_dq_get())
				return 0;
		}
	}
	return 1;
}

static void cic_write_id(int pal, int dd)
{
	uint8_t id = ((0 << 3) |	// CIC type: low = cartridge, high = 64DD
		      (pal << 2) |	// Region: low = NTSC/MPAL, high = PAL
		      0x01);		// Constant bit pattern

	// As the 64DD's CIC, leave the type bit high and send the other three
	if (dd) {
		if (!cic_dd_bit_free())
			while (true)
				tight_loop_contents();
		cic_write(id, 3);
	} else {
		cic_write(id, 4);
	}
}

static void cic_write_seed(uint8_t seed)
{
	cic_ram[10] = 0x0B;
	cic_ram[11] = 0x05;
	cic_ram[12] = (seed >> 4);
	cic_ram[13] = seed;
	cic_ram[14] = (seed >> 4);
	cic_ram[15] = seed;
	cic_encode_round(10);
	cic_encode_round(10);
	cic_write_ram_nibbles(10);
}

static void cic_write_checksum(const uint8_t checksum[6])
{
	for (int i = 0; i < 4; i++)
		cic_ram[i] = 0;
	for (int i = 0; i < 6; i++) {
		cic_ram[(i * 2) + 4] = ((checksum[i] >> 4) & 0x0F);
		cic_ram[(i * 2) + 5] = (checksum[i] & 0x0F);
	}
	cic_encode_round(0);
	cic_encode_round(0);
	cic_encode_round(0);
	cic_encode_round(0);
	cic_read(1);
	cic_write_ram_nibbles(0);
}

static void cic_init_ram(int pal)
{
	for (int i = 0; i < 16; i++) {
		uint8_t value = cic_ram_init[pal][i];

		cic_ram[(i * 2)] = ((value >> 4) & 0x0F);
		cic_ram[(i * 2) + 1] = (value & 0x0F);
	}
	cic_ram[1] = cic_read(4);
	cic_ram[17] = cic_read(4);
}

static void cic_exchange_bytes(uint8_t *a, uint8_t *b)
{
	uint8_t tmp = *a;

	*a = *b;
	*b = tmp;
}

static void cic_round(uint8_t *m)
{
	uint8_t a, b, x;

	x = m[15];
	a = x;

	do {
		b = 1;
		a += (m[b] + 1);
		m[b] = a;
		b++;
		a += (m[b] + 1);
		cic_exchange_bytes(&a, &m[b]);
		m[b] = ~(m[b]);
		b++;
		a &= 0x0F;
		a += ((m[b] & 0x0F) + 1);
		if (a < 16) {
			cic_exchange_bytes(&a, &m[b]);
			b++;
		}
		a += m[b];
		m[b] = a;
		b++;
		a += m[b];
		cic_exchange_bytes(&a, &m[b]);
		b++;
		a &= 0x0F;
		a += 8;
		if (a < 16)
			a += m[b];
		cic_exchange_bytes(&a, &m[b]);
		b++;
		do {
			a += (m[b] + 1);
			m[b] = a;
			b++;
			b &= 0x0F;
		} while (b != 0);
		a = (x + 15);
		x = (a & 0x0F);
	} while (x != 15);
}

static void cic_compare_mode(int pal)
{
	uint8_t index;

	cic_round(&cic_ram[16]);
	cic_round(&cic_ram[16]);
	cic_round(&cic_ram[16]);

	index = (cic_ram[23] & 0x0F);
	if (index == 0)
		index = 1;
	index |= (1 << 4);

	do {
		cic_read(1);
		cic_write(cic_ram[index], 1);
		if (pal)
			index -= 1;
		else
			index += 1;
	} while (index & 0x0F);
}

static void cic_x105_algorithm(uint8_t *payload)
{
	uint8_t a = 5;
	uint8_t carry = 1;

	for (int i = 0; i < 30; ++i) {
		if (!(payload[i] & (1 << 0)))
			a += 8;
		if (!(a & (1 << 1)))
			a += 4;
		a = ((a + payload[i]) & 0x0F);
		payload[i] = a;
		if (!carry)
			a += 7;
		a = ((a + payload[i]) & 0x0F);
		a = (a + payload[i] + carry);
		if (a >= 16) {
			carry = 1;
			a -= 16;
		} else {
			carry = 0;
		}
		a = (~(a) & 0x0F);
		payload[i] = a;
	}
}

static void cic_x105_mode(void)
{
	uint8_t payload[30];

	cic_write(0xAA, 8);
	for (int i = 0; i < 30; i++)
		payload[i] = cic_read(4);
	cic_x105_algorithm(payload);
	cic_write(0, 1);
	for (int i = 0; i < 30; i++)
		cic_write(payload[i], 4);
}

// Reset button: answer 500 ms after the PIF asks, like a real CIC
static void cic_soft_reset(void)
{
	cic_clk_wait(false);
	busy_wait_ms(500);
	cic_write(0, 1);
}

static void cic_led_set(bool on)
{
	if (cic_led >= 0)
		gpio_put(cic_led, on);
}

static void cic_led_toggle(void)
{
	if (cic_led >= 0)
		gpio_xor_mask(1u << cic_led);
}

void cic_poll_run(uint8_t seed, const uint8_t checksum[6], int pal, int dd, int led_pin)
{
	cic_led = led_pin;
	pal = pal ? 1 : 0;

	gpio_set_dir(N64_CIC_DIO, GPIO_IN);

	cic_write_id(pal, dd);
	cic_write_seed(seed);
	cic_write_checksum(checksum);
	cic_init_ram(pal);
	cic_led_set(true);

	while (true) {
		switch (cic_read(2)) {
		case CMD_COMPARE:
			cic_compare_mode(pal);
			break;
		case CMD_X105:
			cic_x105_mode();
			break;
		case CMD_SOFT_RESET:
			cic_soft_reset();
			break;
		case CMD_DIE:
		default:
			// Stop answering until the console is turned off
			while (true)
				tight_loop_contents();
		}
		cic_led_toggle();
	}
}
