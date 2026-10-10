/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Register map of the N64 cart in the PL, see fpga/zedboard/rtl/cart_regs.v
 */

#pragma once

#define CART_BASE            0x43C00000u

#define CART_REG_ID          0x00000u
#define CART_REG_CTRL        0x00004u
#define CART_REG_ROM_BASE    0x00008u
#define CART_REG_STATUS      0x0000Cu
#define CART_REG_CLEAR       0x00010u
#define CART_REG_UNDERRUNS   0x00014u
#define CART_REG_LAST_ADDR   0x00018u
#define CART_REG_ADDR_COUNT  0x0001Cu
#define CART_REG_BUTTONS     0x00020u
#define CART_REG_OLED_CTRL   0x00024u
#define CART_REG_OLED_DATA   0x00028u
#define CART_REG_OLED_FLUSH  0x0002Cu
#define CART_REG_CIC         0x00030u
#define CART_REG_EEPROM      0x00034u
// Bus monitor, cleared when the N64 is turned on (bus_diag.h)
#define CART_REG_MON_ALE     0x00038u
#define CART_REG_MON_RW      0x0003Cu
#define CART_REG_MON_FIRST   0x00040u
#define CART_REG_MON_DATA    0x00044u
#define CART_REG_MON_ROM     0x00048u
#define CART_REG_MON_AD_OR   0x0004Cu
#define CART_REG_PINS        0x00050u
#define CART_REG_MON_GLITCH  0x00054u
#define CART_REG_MON_ODD     0x00058u
#define CART_REG_MON_NOISE   0x0005Cu
#define CART_REG_MON_XFER_N  0x00060u	// transfers finished, then the one in progress:
#define CART_REG_MON_XFER    0x00064u	// address, {writes, reads}, sum
#define CART_REG_MON_SDAT    0x00070u	// S_DAT, see bus_diag.h
#define CART_REG_MON_SDAT_LOW 0x00074u
#define CART_REG_SDAT_LOG_N  0x00078u
#define CART_OLED_FB         0x01000u
#define CART_EEPROM          0x02000u
#define CART_EEPROM_SIZE     0x00800u
#define CART_TRACE_A         0x03000u	// address trace, see bus_diag.h
#define CART_TRACE_B         0x04000u
#define CART_NOISE           0x05000u	// first 16 noise events
#define CART_XFER            0x06000u	// last 256 transfers, 4 words each
#define CART_XFER_LEN        256
#define CART_SDAT_LOG        0x07000u	// first 1024 S_DAT events, see bus_diag.h
#define CART_SDAT_LOG_LEN    1024
// 64DD (dd_asic.v): buffers, what each ASIC register reads, flags, events
#define CART_DD_C2           0x08000u
#define CART_DD_SECTOR       0x08400u
#define CART_DD_MSEQ         0x08500u
#define CART_DD_REG(n)       (0x09000u + 4u * (n))
#define CART_DD_FLAGS        0x09080u	// [3:0] read / set, [11:8] clear, [8] read: events lost
#define CART_DD_EVENT        0x09084u	// [31] valid, [20:16] register, [15:0] data
#define CART_DD_IRQ          61		// IRQ_F2P[0]: events waiting
#define CART_SRAM            0x20000u
#define CART_SRAM_SIZE       0x20000u

#define CART_ID_MAGIC        0x4E363443u

#define CART_CTRL_ENABLE     (1u << 0)
#define CART_CTRL_DD         (1u << 1)	// answer as the 64DD
#define CART_CTRL_ROM_OFF    (1u << 2)	// no cartridge
#define CART_CTRL_FLASH      (1u << 3)	// the SRAM is a FlashRAM chip

#define CART_STATUS_N64_ON   (1u << 0)
#define CART_STATUS_DIRTY    (1u << 1)
#define CART_STATUS_UNDERRUN (1u << 2)
#define CART_STATUS_EEP_DIRTY (1u << 3)

#define CART_CLEAR_DIRTY     (1u << 1)
#define CART_CLEAR_UNDERRUN  (1u << 2)
#define CART_CLEAR_EEP_DIRTY (1u << 3)

// PINS: levels numbered like the Pico GPIOs, S_DAT in bit 23
#define CART_PINS_COLD_RESET (1u << 22)
#define CART_PINS_SDAT       (1u << 23)

// EEPROM
#define CART_EEPROM_ENABLE   (1u << 0)
#define CART_EEPROM_16K      (1u << 1)

// BUTTONS: [4:0] held, [12:8] pressed since last cleared (write 1 to clear)
#define CART_BTN_C           (1u << 0)
#define CART_BTN_D           (1u << 1)
#define CART_BTN_L           (1u << 2)
#define CART_BTN_R           (1u << 3)
#define CART_BTN_U           (1u << 4)
#define CART_BTN_ALL         0x1Fu
#define CART_BTN_PRESSED_SHIFT 8

// OLED_CTRL
#define CART_OLED_VDD        (1u << 0)
#define CART_OLED_VBAT       (1u << 1)
#define CART_OLED_RUN        (1u << 2)	// RES# released
#define CART_OLED_BUSY       (1u << 8)

// OLED_DATA: byte in [7:0], D/C in bit 8
#define CART_OLED_DC         (1u << 8)

// ROM image in DDR. The default standalone linker script keeps the program
// near the start of DDR, far below this.
#define ROM_DDR_BASE         0x10000000u
#define ROM_MAX_SIZE         (64u * 1024 * 1024)

// 64DD: its IPL ROM right after the ROM (the cart streams it from there), the
// disk laid out physically, and the .ndd file while it is converted
#define IPL_DDR_BASE         (ROM_DDR_BASE + ROM_MAX_SIZE)
#define IPL_MAX_SIZE         (4u * 1024 * 1024)
#define DD_PHYS_DDR_BASE     0x14400000u
#define DD_NDD_DDR_BASE      0x18800000u
