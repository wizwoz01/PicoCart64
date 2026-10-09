/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Diagnosis from the cart's bus monitor.
 */

#include <stdio.h>
#include <string.h>

#include "bus_diag.h"

#define FIRST_ADDR 0x10000000u		// the ROM header is always read first

const char *bus_ad_pin(int ad)
{
	static const char *const pins[16] = {
		"JA1", "JD7", "JA3", "JA4", "JA7", "JD8", "JA9", "JA10",
		"JB1", "JB2", "JB3", "JB4", "JB7", "JB8", "JB9", "JB10",
	};

	return ad >= 0 && ad < 16 ? pins[ad] : "?";
}

// Pico pin of ADn (GPn)
static int pico_pin(int ad)
{
	static const uint8_t pins[16] = { 1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15, 16, 17, 19, 20 };

	return pins[ad & 15];
}

// "AD3 (JA4), AD12 (JB7)" for the set bits of mask, at most 4 of them
static void ad_list(uint32_t mask, char *out, int len)
{
	int ad, n = 0, used = 0;

	out[0] = '\0';
	for (ad = 0; ad < 16 && used < len; ad++) {
		if (!(mask & (1u << ad)))
			continue;
		if (n == 4) {
			snprintf(out + used, (size_t)(len - used), ", ...");
			return;
		}
		used += snprintf(out + used, (size_t)(len - used), "%sAD%d (%s)", n ? ", " : "", ad, bus_ad_pin(ad));
		n++;
	}
}

bus_verdict_t bus_diagnose(const bus_mon_t *m, uint32_t header, char *out, int len)
{
	const uint32_t aleh = m->ale & 0xFFFF, alel = m->ale >> 16;
	const uint32_t rd = m->rw & 0xFFFF, wr = m->rw >> 16;
	const uint32_t served = m->rom & 0xFFFF, bad = m->rom >> 16;
	const uint32_t diff = m->first_addr ^ FIRST_ADDR;
	uint32_t seen = (m->ad_or >> 16) | (m->ad_or & 0xFFFF), expect = 0;
	char lines[64];

	if (!aleh && !alel && !rd && !wr) {
		snprintf(out, (size_t)len, "No bus activity: the N64 stopped before reading the cart. "
			 "Check the Pico's LED (CIC), the cart's seating and JC wires.");
		return BUS_FAULT;
	}
	if (!alel) {
		snprintf(out, (size_t)len, "ALE_H toggles but ALE_L never does: check the ALE_L wire (JC1).");
		return BUS_FAULT;
	}
	if (!aleh) {
		snprintf(out, (size_t)len, "ALE_L toggles but ALE_H never does: check the ALE_H wire (JC2).");
		return BUS_FAULT;
	}
	if (!rd) {
		snprintf(out, (size_t)len, wr ? "~WRITE pulses but no ~READ: ~READ and ~WRITE look swapped (JC4, JC3)." :
			 "Addresses but no ~READ pulses: check the ~READ wire (JC4).");
		return BUS_FAULT;
	}
	if ((m->first_addr & 0xFFFF) == (FIRST_ADDR >> 16) && (m->first_addr >> 16) != (FIRST_ADDR >> 16)) {
		snprintf(out, (size_t)len, "First address %08lX: ALE_L and ALE_H look swapped (JC1, JC2).",
			 (unsigned long)m->first_addr);
		return BUS_FAULT;
	}
	if (diff) {
		ad_list((diff >> 16) | (diff & 0xFFFF), lines, sizeof(lines));
		snprintf(out, (size_t)len, "First address %08lX, not %08lX: check %s.", (unsigned long)m->first_addr,
			 (unsigned long)FIRST_ADDR, lines);
		return BUS_FAULT;
	}
	if (!served) {
		snprintf(out, (size_t)len, "The N64 read the ROM header but the cart didn't answer. Is it enabled?");
		return BUS_FAULT;
	}
	if (bad) {
		snprintf(out, (size_t)len, "%lu ROM reads didn't match what the cart drove: the Pico or a touching AD wire "
			 "also drives the bus.", (unsigned long)bad);
		return BUS_FAULT;
	}
	if (served >= 2 && m->first_data != header) {
		snprintf(out, (size_t)len, "The ROM header went out as %08lX instead of %08lX: DDR or cache problem.",
			 (unsigned long)m->first_data, (unsigned long)header);
		return BUS_FAULT;
	}

	// Every AD line carries a 1 at some point of the boot reads
	if (alel >= BUS_GAME_LOAD_ADDRS)
		expect = 0xFFFF;
	else if (alel >= BUS_BOOT_CODE_ADDRS)
		expect = 0x1FFC;
	if (expect & ~seen) {
		ad_list(expect & ~seen, lines, sizeof(lines));
		snprintf(out, (size_t)len, "Never high in any address: %s. Check those wires.", lines);
		return BUS_FAULT;
	}

	if (alel < BUS_BOOT_CODE_ADDRS) {
		snprintf(out, (size_t)len, "The N64 stopped while reading the boot code (%lu of %d addresses). "
			 "Check the AD wires and the Pico.", (unsigned long)alel, BUS_BOOT_CODE_ADDRS);
		return BUS_STOPPED;
	}
	if (alel < BUS_GAME_LOAD_ADDRS) {
		snprintf(out, (size_t)len, "Boot code read, then the N64 stopped (%lu addresses): the CIC check failed. "
			 "Wrong CIC, or AD data errors.", (unsigned long)alel);
		return BUS_STOPPED;
	}
	if (alel < BUS_GAME_LOAD_ADDRS + 64) {
		snprintf(out, (size_t)len, "The game's first 1 MB loaded, then the N64 stopped (%lu addresses): its checksum "
			 "failed, the data had errors.", (unsigned long)alel);
		return BUS_STOPPED;
	}
	snprintf(out, (size_t)len, "Bus OK: the N64 runs the game (%lu%s addresses, %lu%s ROM reads, header %08lX).",
		 (unsigned long)alel, alel == 0xFFFF ? "+" : "", (unsigned long)served, served == 0xFFFF ? "+" : "",
		 (unsigned long)m->first_data);
	return BUS_OK;
}

int bus_sdat(const bus_mon_t *m, char *out, int len)
{
	const uint32_t falls = m->sdat & 0xFFFF, cmds = m->sdat >> 16;
	const uint32_t longest = m->sdat_low & 0xFFFF, now = m->sdat_low >> 16;
	const uint32_t held = now > longest ? now : longest;
	const char *level = m->sdat_high ? "high" : "LOW";

	if (held >= BUS_SDAT_HELD_US) {
		snprintf(out, (size_t)len, "%s, held low for %lu%s us after %lu%s falls, %lu EEPROM commands answered: "
			 "the PIF hangs on it", level, (unsigned long)held, held == 0xFFFF ? "+" : "", (unsigned long)falls,
			 falls == 0xFFFF ? "+" : "", (unsigned long)cmds);
		return 1;
	}
	if (!falls)
		snprintf(out, (size_t)len, "%s, no EEPROM traffic yet", level);
	else
		snprintf(out, (size_t)len, "%s, %lu%s EEPROM commands answered, %lu%s falls, longest low %lu us", level,
			 (unsigned long)cmds, cmds == 0xFFFF ? "+" : "", (unsigned long)falls, falls == 0xFFFF ? "+" : "",
			 (unsigned long)longest);
	return 0;
}

void bus_sdat_event(uint32_t e, uint32_t prev, char *out, int len)
{
	static const char *const what[] = { "line fell", "line rose", "cart pulls low", "cart lets go" };
	const uint64_t ns = (uint64_t)(e & 0x3FFFFFFF) * 40, dt = ns - (uint64_t)(prev & 0x3FFFFFFF) * 40;

	snprintf(out, (size_t)len, "%lu.%06lu s  +%lu.%02lu us  %s", (unsigned long)(ns / 1000000000u),
		 (unsigned long)(ns / 1000 % 1000000), (unsigned long)(dt / 1000), (unsigned long)(dt % 1000 / 10), what[e >> 30]);
}

int bus_noise(const bus_mon_t *m, char *out, int len)
{
	const uint32_t ignored = m->glitch & 0xFFFF, late = m->glitch >> 16;
	const uint32_t odd = m->odd & 0xFFFF, shorts = m->odd >> 16;

	if (odd || shorts) {
		snprintf(out, (size_t)len, "~READ noise: %lu transfers read an odd number of words, %lu ~READ pulses were "
			 "too short. Check JC4 and ground.", (unsigned long)odd, (unsigned long)shorts);
		return 1;
	}
	if (late) {
		snprintf(out, (size_t)len, "%lu ROM words were late, so the N64 read wrong data; %lu ALE pulses were "
			 "ignored as noise.", (unsigned long)late, (unsigned long)ignored);
		return 1;
	}
	if (ignored) {
		snprintf(out, (size_t)len, "Ignored %lu%s short pulses on ALE_L/ALE_H as noise. Check the ground wires.",
			 (unsigned long)ignored, ignored == 0xFFFF ? "+" : "");
		return 1;
	}
	return 0;
}

void bus_noise_event(uint32_t e, char *out, int len)
{
	static const char *const types[] = { "?", "ALE_H pulse", "ALE_L pulse", "odd read count", "short ~READ" };
	uint32_t type = e >> 28, width = (e >> 20) & 0xFF, us = e & 0xFFFFF;

	if (type == 3)
		snprintf(out, (size_t)len, "%s %lu at %lu us", types[type], (unsigned long)width, (unsigned long)us);
	else
		snprintf(out, (size_t)len, "%s %lu ns at %lu us", type < 5 ? types[type] : "?", (unsigned long)width * 10,
			 (unsigned long)us);
}

// Address the boot sends in trace entry k
static uint32_t boot_addr(int k)
{
	return k == 0 ? FIRST_ADDR : FIRST_ADDR + 0x40 + 4u * (uint32_t)(k - 1);
}

static int boot_bit(int k, int ad)
{
	return (int)((boot_addr(k) >> ad) & 1);
}

int bus_trace_check(const uint32_t *a, const uint32_t *b, int n, char out[][BUS_MSG_LEN], int max, int *clean)
{
	const int m = n < BUS_BOOT_CODE_ADDRS ? n : BUS_BOOT_CODE_ADDRS;
	int before = 0, after = 0, late = 0, lines = 0, used, found = 0;
	uint32_t hi = 0;
	char item[48];
	int j, k;

	*clean = 0;
	if (m < 2 || max < 1)
		return 0;

	// The low half sampled before ALE_L fell (the one the cart uses), and twice after
	for (k = 0; k < m; k++) {
		uint32_t e = boot_addr(k) & 0xFFFF;

		before += (a[k] & 0xFFFF) != e;
		after += (b[k] >> 16) != e;
		late += (b[k] & 0xFFFF) != e;
		hi |= (a[k] >> 16) ^ (FIRST_ADDR >> 16);
	}
	if ((before || after || late) && lines < max)
		snprintf(out[lines++], BUS_MSG_LEN, "Trace of %d boot addresses: low half wrong in %d sampled before ALE_L fell, "
			 "%d just after, %d 35 ns later.", m, before, after, late);
	if (hi && lines < max)
		snprintf(out[lines++], BUS_MSG_LEN, "Address high half differs from 1000 in bits %04lX.", (unsigned long)hi);
	if (lines >= max)
		return lines;

	// Which line each AD input follows instead of its own
	used = snprintf(out[lines], BUS_MSG_LEN, "Wiring:");
	for (j = 0; j < 16; j++) {
		int err = 0, ones = 0, best = -1, best_miss = m + 1, i;

		for (k = 0; k < m; k++) {
			int o = (int)((a[k] >> j) & 1);

			ones += o;
			err += o != boot_bit(k, j);
		}
		if (!err)
			continue;
		for (i = 2; i < 12; i++) {
			int miss = 0;

			if (i == j)
				continue;
			for (k = 0; k < m; k++)
				miss += (int)((a[k] >> j) & 1) != boot_bit(k, i);
			if (miss < best_miss) {
				best_miss = miss;
				best = i;
			}
		}
		if (ones == 0 || ones == m)
			snprintf(item, sizeof(item), "%s (AD%d) stuck at %d", bus_ad_pin(j), j, ones ? 1 : 0);
		else if (best_miss <= m / 32)
			snprintf(item, sizeof(item), "%s (AD%d) follows AD%d (Pico pin %d)", bus_ad_pin(j), j, best, pico_pin(best));
		else
			snprintf(item, sizeof(item), "%s (AD%d) wrong in %d of %d", bus_ad_pin(j), j, err, m);
		if (used + (int)strlen(item) + 6 >= BUS_MSG_LEN) {
			snprintf(out[lines] + used, (size_t)(BUS_MSG_LEN - used), " ...");
			found++;
			break;
		}
		used += snprintf(out[lines] + used, (size_t)(BUS_MSG_LEN - used), "%s %s", found ? "," : "", item);
		found++;
	}
	if (found)
		return lines + 1;
	*clean = lines == 0;
	snprintf(out[lines], BUS_MSG_LEN, "Trace of %d boot addresses: every AD input follows its own line.", m);
	return lines + 1;
}

int bus_xfer_check(const bus_xfer_t *x, const uint8_t *rom, uint32_t size, uint32_t *expected)
{
	const uint32_t reads = x->rw & 0xFFFF, writes = x->rw >> 16;
	uint32_t off, sum = 0, i;

	if (x->addr < FIRST_ADDR || x->addr > 0x1FBFFFFFu || !reads || writes || reads == 0xFFFF)
		return -1;
	off = x->addr - FIRST_ADDR;
	if (off + 2 * reads > size)
		return -1;
	for (i = 0; i < reads; i++, off += 2)
		sum += ((uint32_t)rom[off] << 8) | rom[off + 1];
	*expected = sum;
	return sum == x->sum;
}

