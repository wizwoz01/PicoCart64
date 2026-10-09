/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Cart test for the ZedBoard cart, running on the N64 (libdragon).
 *
 * Shows on the TV what the console receives from the cart, and whether it
 * keeps running:
 *   - ROM: pattern.bin (made by mkpattern.c) read by DMA in 32 KB blocks, a
 *     megabyte per pass; by single CPU reads; and by short DMAs that start on
 *     any halfword, the ways games read. Wrong bits are mapped to the AD line
 *     that carried them.
 *   - SRAM at 0x08000000: written and read back.
 *   - The PIF, which also checks the CIC: asked every 20 ms, with a timeout,
 *     so the console doesn't hang when it stops answering. Only empty requests
 *     for the first 10 s, then controller 1 reads, and the cart EEPROM from
 *     20 s, so the time the PIF stops shows what stopped it.
 * The clock and the pass counter move while the console runs. When the PIF
 * stops answering, the cart tests go on for 3 s, then stop with three marker
 * reads that the ZedBoard's transfer log shows (MARK_ADDR).
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "pattern.h"

#define PI_REG(off)      (*(volatile uint32_t *)(0xA4600000u + (off)))
#define ROM_HEADER       0x80371240u
#define SRAM_ADDR        0x08000000u
#define SRAM_TEST_SIZE   (32 * 1024)
#define CHUNK            (32 * 1024)
#define DMA_PER_PASS     (1024 * 1024)
#define MAX_ERRS         4

#define PIF_POLL_MS      20
#define PIF_TIMEOUT_MS   200
#define CTRL_FROM_MS     10000		// controller reads from here
#define EEPROM_FROM_MS   20000		// EEPROM tests from here
#define EEPROM_EVERY_MS  5000
#define STOP_AFTER_MS    3000		// cart tests stop this long after the PIF

// Markers, inside pattern.bin: MARK_ADDR, then MARK_ADDR + 4 * (1 + request
// kind), then MARK_ADDR + 0x10000 + 4 * (ms the PIF stopped / 10)
#define MARK_ADDR        0x13E00000u

typedef struct {
	const char *name, *tag;
	uint32_t errors;
	uint32_t bits;			// wrong bits, folded onto the AD lines
	uint64_t bytes;			// bytes checked
} test_t;

typedef struct {
	const char *tag;
	uint32_t addr, exp, got;
} err_t;

enum { PIF_NOP, PIF_CTRL, PIF_EEP_INFO, PIF_EEP_WRITE, PIF_EEP_READ };

static const char *const pif_kinds[] = {
	"an empty request (PIF only)", "a controller 1 read", "an EEPROM info command",
	"an EEPROM write", "an EEPROM read",
};

static test_t t_dma = { "DMA 32 KB blocks", "DMA" };
static test_t t_cpu = { "CPU single reads", "CPU" };
static test_t t_half = { "DMA any halfword", "HALF" };
static test_t t_sram = { "SRAM write/read", "SRAM" };
static test_t *const tests[] = { &t_dma, &t_cpu, &t_half, &t_sram };

static err_t errs[MAX_ERRS];
static int nerrs;
static uint32_t buf[CHUNK / 4] __attribute__((aligned(16)));
static uint32_t header, pat_addr, pat_size, dma_pos, sweeps;
static uint32_t pass, rng = 1;
static volatile int reset_irq;

// PIF
static volatile int pif_answered;
static uint8_t pif_out[JOYBUS_BLOCK_SIZE];
static uint32_t pif_replies, pif_last_ms, pif_next_ms, pif_max_us;
static int pif_dead, pif_dead_kind, cart_stopped;
static uint32_t pif_dead_ms, pif_late_ms;
static int ctrl = -1;			// -1 not asked yet, 0 none, 1 connected
static uint32_t eeprom_next_ms = EEPROM_FROM_MS;
static uint32_t eeprom_ok, eeprom_bad;
static const char *eeprom_text = "not tested yet (starts at 20 s)";

static uint32_t now_ms(void)
{
	return (uint32_t)get_ticks_ms();
}

static uint32_t rand32(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// The halfword at even byte offset o of pattern.bin
static uint16_t pattern16(uint32_t o)
{
	uint32_t w = pattern(o & ~3u);

	return (o & 2) ? (uint16_t)w : (uint16_t)(w >> 16);
}

static inline void check(test_t *t, uint32_t addr, uint32_t exp, uint32_t got)
{
	if (exp == got)
		return;
	t->errors++;
	// A word crosses the bus as two halfwords: bit n travels on AD(n % 16)
	t->bits |= ((exp ^ got) | ((exp ^ got) >> 16)) & 0xFFFF;
	if (nerrs < MAX_ERRS)
		errs[nerrs++] = (err_t){ t->tag, addr, exp, got };
}

static void clear_counters(void)
{
	for (int i = 0; i < 4; i++) {
		tests[i]->errors = 0;
		tests[i]->bits = 0;
		tests[i]->bytes = 0;
	}
	nerrs = 0;
	eeprom_ok = 0;
	eeprom_bad = 0;
}

static void pif_callback(uint64_t *out, void *ctx)
{
	(void)ctx;
	memcpy(pif_out, out, JOYBUS_BLOCK_SIZE);
	pif_answered = 1;
}

// One command for joybus channel ch (0-3 controllers, 4 cart EEPROM), or an
// empty request when ntx is 0
static void pif_block(uint8_t *in, int ch, const uint8_t *tx, int ntx, int nrx)
{
	int i = 0;

	memset(in, 0, JOYBUS_BLOCK_SIZE);
	if (ntx) {
		i = ch;				// a 0x00 skips a channel
		in[i++] = ntx;
		in[i++] = nrx;
		memcpy(&in[i], tx, ntx);
		i += ntx;
		memset(&in[i], 0xFF, nrx);
		i += nrx;
	}
	in[i] = 0xFE;
	in[JOYBUS_BLOCK_SIZE - 1] = 0x01;
}

// Runs a block through the PIF and waits at most PIF_TIMEOUT_MS. The first
// time the PIF doesn't answer, it is marked as stopped and isn't asked again.
static int pif_exec(const uint8_t *in, int kind)
{
	uint64_t t0 = get_ticks_us();
	uint32_t us;

	if (pif_dead)
		return -1;
	pif_answered = 0;
	joybus_exec_async(in, pif_callback, NULL);
	while (!pif_answered) {
		if (get_ticks_us() - t0 > PIF_TIMEOUT_MS * 1000) {
			pif_dead = 1;
			pif_dead_kind = kind;
			pif_dead_ms = (uint32_t)(t0 / 1000);
			return -1;
		}
	}
	us = (uint32_t)(get_ticks_us() - t0);
	if (us > pif_max_us)
		pif_max_us = us;
	pif_replies++;
	pif_last_ms = now_ms();
	return 0;
}

// An EEPROM command: 0 answered, -1 no EEPROM answered, -2 the PIF stopped
static int eeprom_cmd(const uint8_t *tx, int ntx, uint8_t *rx, int nrx, int kind)
{
	uint8_t in[JOYBUS_BLOCK_SIZE];

	pif_block(in, 4, tx, ntx, nrx);
	if (pif_exec(in, kind) != 0)
		return -2;
	if (pif_out[5] & 0xC0)
		return -1;
	memcpy(rx, &pif_out[6 + ntx], nrx);
	return 0;
}

static void test_eeprom(void)
{
	static const uint8_t info_cmd[1] = { 0x00 };
	static const uint8_t read_cmd[2] = { 0x04, 0x01 };
	static char text[48];
	uint8_t wr[10], info[3], rd[8];
	int r;

	r = eeprom_cmd(info_cmd, 1, info, 3, PIF_EEP_INFO);
	if (r == -1)
		eeprom_text = "nothing answered (no EEPROM)";
	if (r)
		return;
	wr[0] = 0x05;
	wr[1] = 0x01;
	for (int i = 2; i < 10; i++)
		wr[i] = (uint8_t)rand32();
	if (eeprom_cmd(wr, 10, info + 2, 1, PIF_EEP_WRITE) != 0)
		return;
	wait_ms(20);			// a real EEPROM's write time
	if (eeprom_cmd(read_cmd, 2, rd, 8, PIF_EEP_READ) != 0)
		return;
	if (memcmp(&wr[2], rd, sizeof(rd)) == 0)
		eeprom_ok++;
	else
		eeprom_bad++;
	snprintf(text, sizeof(text), "%s: write+read ok %lu, wrong %lu", info[1] == 0xC0 ? "16 Kbit" : "4 Kbit",
		 (unsigned long)eeprom_ok, (unsigned long)eeprom_bad);
	eeprom_text = text;
}

static void pif_poll(void)
{
	uint8_t in[JOYBUS_BLOCK_SIZE];
	uint32_t t = now_ms();

	if (pif_dead) {
		if (pif_answered && !pif_late_ms)
			pif_late_ms = t;
		return;
	}
	if (t < pif_next_ms)
		return;
	pif_next_ms = t + PIF_POLL_MS;

	if (t < CTRL_FROM_MS) {
		pif_block(in, 0, NULL, 0, 0);
		pif_exec(in, PIF_NOP);
	} else {
		static const uint8_t read_buttons[1] = { 0x01 };

		pif_block(in, 0, read_buttons, 1, 4);
		if (pif_exec(in, PIF_CTRL) == 0) {
			ctrl = !(pif_out[1] & 0xC0);
			if (ctrl && (pif_out[3] & 0x10))	// START
				clear_counters();
		}
	}
	if (t >= eeprom_next_ms) {
		eeprom_next_ms = t + EEPROM_EVERY_MS;
		test_eeprom();
	}
}

// Cart tests run until STOP_AFTER_MS after the PIF stopped, then end with the
// markers for the transfer log
static int cart_on(void)
{
	pif_poll();
	if (!pif_dead || now_ms() - pif_dead_ms < STOP_AFTER_MS)
		return 1;
	if (!cart_stopped) {
		cart_stopped = 1;
		io_read(MARK_ADDR);
		io_read(MARK_ADDR + 4 * (1 + pif_dead_kind));
		io_read(MARK_ADDR + 0x10000 + 4 * ((pif_dead_ms / 10) & 0x3FFF));
	}
	return 0;
}

// The next megabyte of the pattern, 32 KB per DMA
static void test_dma(void)
{
	for (uint32_t done = 0; done < DMA_PER_PASS && pat_size && cart_on(); done += CHUNK) {
		data_cache_hit_writeback_invalidate(buf, CHUNK);
		dma_read(buf, pat_addr + dma_pos, CHUNK);
		for (uint32_t i = 0; i < CHUNK / 4; i++)
			check(&t_dma, pat_addr + dma_pos + i * 4, pattern(dma_pos + i * 4), buf[i]);
		t_dma.bytes += CHUNK;
		dma_pos += CHUNK;
		if (dma_pos >= pat_size) {
			dma_pos = 0;
			sweeps++;
		}
	}
}

// One 32-bit CPU read per word: an address phase each time
static void test_cpu(void)
{
	for (int i = 0; i < 1024 && pat_size; i++) {
		uint32_t o = (rand32() % (pat_size / 4)) * 4;

		if ((i & 255) == 0 && !cart_on())
			return;
		check(&t_cpu, pat_addr + o, pattern(o), io_read(pat_addr + o));
		t_cpu.bytes += 4;
	}
}

// Short DMAs starting on any halfword, like games streaming sound and data
static void test_halfword_dma(void)
{
	const uint16_t *half = (const uint16_t *)buf;

	for (int k = 0; k < 64 && pat_size && cart_on(); k++) {
		uint32_t len = (rand32() % 2048 + 1) * 2;
		uint32_t o = (rand32() % ((pat_size - len) / 2)) * 2;

		data_cache_hit_writeback_invalidate(buf, len);
		dma_read(buf, pat_addr + o, len);
		for (uint32_t i = 0; i < len / 2; i++)
			check(&t_half, pat_addr + o + i * 2, pattern16(o + i * 2), half[i]);
		t_half.bytes += len;
	}
}

static void test_sram(void)
{
	uint32_t seed = rand32() & ~3u;

	if (!cart_on())
		return;
	for (uint32_t o = 0; o < SRAM_TEST_SIZE; o += 4)
		io_write(SRAM_ADDR + o, pattern(o ^ seed));
	pif_poll();
	for (uint32_t o = 0; o < SRAM_TEST_SIZE; o += 4)
		check(&t_sram, SRAM_ADDR + o, pattern(o ^ seed), io_read(SRAM_ADDR + o));
	pif_poll();

	// dma_read() only reaches the ROM
	data_cache_hit_writeback_invalidate(buf, SRAM_TEST_SIZE);
	dma_read_async(buf, SRAM_ADDR, SRAM_TEST_SIZE);
	dma_wait();
	for (uint32_t i = 0; i < SRAM_TEST_SIZE / 4; i++)
		check(&t_sram, SRAM_ADDR + i * 4, pattern((i * 4) ^ seed), buf[i]);
	t_sram.bytes += 2 * SRAM_TEST_SIZE;
}

// The PIF raises this 0.5 s before it resets the console (reset button)
static void on_reset(void)
{
	reset_irq = 1;
}

static const char *ad_lines(uint32_t bits)
{
	static char s[18];
	int n = 0;

	for (int b = 15; b >= 0; b--) {
		s[n++] = (bits >> b) & 1 ? 'X' : '.';
		if (b == 8)
			s[n++] = ' ';
	}
	s[n] = '\0';
	return s;
}

static void show(const char *step)
{
	static const char *const tv[] = { "PAL", "NTSC", "MPAL" };
	uint32_t ms = now_ms();

	pif_poll();
	console_clear();
	printf("ZedBoard cart test        time %lu.%lu s        pass %lu\n", (unsigned long)(ms / 1000),
	       (unsigned long)(ms / 100 % 10), (unsigned long)pass);
	printf("Boot %s   TV %s   Now: %s\n", sys_reset_type() == RESET_WARM ? "WARM" : "cold",
	       tv[get_tv_type() % 3], cart_stopped ? "cart tests stopped" : step);
	if (!pif_dead) {
		printf("PIF  answering: %lu replies, slowest %lu us\n", (unsigned long)pif_replies,
		       (unsigned long)pif_max_us);
	} else {
		printf("PIF  STOPPED ANSWERING at %lu.%02lu s, during\n", (unsigned long)(pif_dead_ms / 1000),
		       (unsigned long)(pif_dead_ms / 10 % 100));
		printf("     %s (last reply %lu.%02lu s)\n", pif_kinds[pif_dead_kind],
		       (unsigned long)(pif_last_ms / 1000), (unsigned long)(pif_last_ms / 10 % 100));
		if (pif_late_ms)
			printf("     ...then answered late, at %lu.%lu s\n", (unsigned long)(pif_late_ms / 1000),
			       (unsigned long)(pif_late_ms / 100 % 10));
	}
	printf("Plan: 0-10 s PIF only, then controller 1, from 20 s EEPROM\n");
	printf("Controller 1  %s\n", ctrl < 0 ? "not asked yet" : ctrl ? "connected" : "not found");
	printf("EEPROM        %s\n", eeprom_text);
	if (reset_irq)
		printf("RESET from the PIF: the console restarts in 0.5 s\n");
	printf("\n");

	printf("ROM header %08lX %s   PI LAT %02lX PWD %02lX PGS %lX RLS %lX\n", (unsigned long)header,
	       header == ROM_HEADER ? "ok" : "WRONG", (unsigned long)(PI_REG(0x14) & 0xFF),
	       (unsigned long)(PI_REG(0x18) & 0xFF), (unsigned long)(PI_REG(0x1C) & 0xF),
	       (unsigned long)(PI_REG(0x20) & 0x3));
	if (pat_size)
		printf("pattern.bin at %08lX, %lu KB, sweep %lu at %lu%%\n\n", (unsigned long)pat_addr,
		       (unsigned long)(pat_size >> 10), (unsigned long)sweeps,
		       (unsigned long)((dma_pos >> 10) * 100 / (pat_size >> 10)));
	else
		printf("pattern.bin NOT FOUND: is the whole ROM file on the SD?\n\n");

	printf("Test              Errors  Checked KB  Wrong AD lines 15..0\n");
	for (int i = 0; i < 4; i++)
		printf("%-16s %7lu %11lu  %s\n", tests[i]->name, (unsigned long)tests[i]->errors,
		       (unsigned long)(tests[i]->bytes >> 10), ad_lines(tests[i]->bits));

	printf("\nFirst errors  test  address   expected  received\n");
	for (int i = 0; i < nerrs; i++)
		printf("              %-4s  %08lX  %08lX  %08lX\n", errs[i].tag, (unsigned long)errs[i].addr,
		       (unsigned long)errs[i].exp, (unsigned long)errs[i].got);
	if (!nerrs)
		printf("              none\n");

	printf("\nTime stopped: the console froze. WARM boot without pressing\n");
	printf("reset: the PIF reset the console. START clears the counters.\n");
	console_render();
}

int main(void)
{
	console_init();
	console_set_render_mode(RENDER_MANUAL);
	register_RESET_handler(on_reset);

	// SRAM timing, as games set it
	PI_REG(0x24) = 0x05;
	PI_REG(0x28) = 0x0C;
	PI_REG(0x2C) = 0x0D;
	PI_REG(0x30) = 0x02;

	header = io_read(0x10000000);
	rng = (uint32_t)get_ticks() | 1;
	if (dfs_init(DFS_DEFAULT_LOCATION) == DFS_ESUCCESS) {
		pat_addr = dfs_rom_addr("pattern.bin");
		if (pat_addr)
			pat_size = (uint32_t)dfs_rom_size("pattern.bin") & ~(CHUNK - 1u);
	}

	while (1) {
		show("DMA 32 KB blocks");
		test_dma();
		show("CPU single reads");
		test_cpu();
		show("DMA any halfword");
		test_halfword_dma();
		show("SRAM write/read");
		test_sram();
		pass++;
	}
}
