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
#define CART_SRAM            0x20000u
#define CART_SRAM_SIZE       0x20000u

#define CART_ID_MAGIC        0x4E363443u

#define CART_CTRL_ENABLE     (1u << 0)

#define CART_STATUS_N64_ON   (1u << 0)
#define CART_STATUS_DIRTY    (1u << 1)
#define CART_STATUS_UNDERRUN (1u << 2)

#define CART_CLEAR_DIRTY     (1u << 1)
#define CART_CLEAR_UNDERRUN  (1u << 2)

// ROM image in DDR. The default standalone linker script keeps the program
// near the start of DDR, far below this.
#define ROM_DDR_BASE         0x10000000u
#define ROM_MAX_SIZE         (64u * 1024 * 1024)
