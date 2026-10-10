/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The 64DD on the ZedBoard: the drive (dd_drive.c) runs in the interrupt of
 * dd_asic.v's events, the disk lives in DDR, and changed blocks go back to
 * the .ndd file on the SD card.
 */

#pragma once

#include <stdint.h>

#include "cic.h"
#include "dd_disk.h"

// Interrupt controller and an empty drive. Returns 0 on success.
int dd_init(void);

// True for *.ndd (any case)
int dd_is_disk_name(const char *name);

// True if the .z64 header is a 64DD IPL ROM's (game code NDD... or NDX...)
int dd_is_ipl(const uint8_t * z64);

// Loads a .ndd into the drive. progress(done, total) is called while reading.
int dd_load_disk(const char *path, const char *name, void (*progress)(uint32_t done, uint32_t total));
void dd_eject(void);

// The disk in the drive, or NULL
const char *dd_disk_name(void);
const dd_disk_info_t *dd_info(void);

// Finds the IPL ROM on the SD card and loads it to IPL_DDR_BASE. Returns 0
// and the IPL's CIC (CIC_DD_US or CIC_DD_JP, or CIC_UNKNOWN).
int dd_load_ipl(cic_t * cic, char *name, int name_len);

// Copies an IPL loaded as a cartridge ROM (at ROM_DDR_BASE) to IPL_DDR_BASE
void dd_ipl_from_rom(uint32_t size);

// The console was turned on or off: the drive resets with it
void dd_console(int on);

// From the main loop: the drive's clock, and changed blocks to the SD card
// after quiet_ms without writes. Returns how many blocks it saved.
int dd_poll(uint32_t now_ms, uint32_t quiet_ms);
int dd_save(void);
int dd_unsaved(void);

// Events dropped because the FIFO was full (should stay 0)
uint32_t dd_events_lost(void);

// The IPL only takes disks of its own region (a US IPL refuses Japanese disks),
// and its boot code checksums the IPL, so the disk changes instead: in the copy
// in DDR, the system area gets the IPL's region code, and so does the game's
// drive library wherever it builds the disk's code. The .ndd isn't changed.
// Returns 1 if the disk was converted.
int dd_disk_match_ipl(void);

// The drive's recent activity, one line at a time, for the 't' dump
int dd_log_lines(void);
void dd_log_line(int i, char *out, int len);
