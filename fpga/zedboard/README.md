# PicoCart64 + ZedBoard: ROMs up to 64 MB

The ZedBoard serves cartridge ROM and SRAM to the N64 from its DDR3. The Pico on
the PicoCart64-lite board only runs the CIC (`sw/picocart64_cic_only`).

```
N64 cart slot ── PicoCart64-lite v1.4 ──┬── Pico (CIC only: GP20/GP21/GP22)
                                        └── wires ── ZedBoard Pmods JA/JB/JC
```

## Wiring

Solder wires to the Pico's pins on the Lite board. Add a 33-47 Ω series resistor
on each AD line, close to the ZedBoard end. Keep wires 15 cm or shorter and run
ground wires alongside them.

| Signal      | Pico GPIO | Pico pin | ZedBoard |
|-------------|-----------|----------|----------|
| AD0-AD3     | GP0-GP3   | 1, 2, 4, 5 | JA1-JA4 |
| AD4-AD7     | GP4-GP7   | 6, 7, 9, 10 | JA7-JA10 |
| AD8-AD11    | GP8-GP11  | 11, 12, 14, 15 | JB1-JB4 |
| AD12-AD15   | GP12-GP15 | 16, 17, 19, 20 | JB7-JB10 |
| ALE_L       | GP16      | 21 | JC1 |
| ALE_H       | GP17      | 22 | JC2 |
| ~WRITE      | GP18      | 24 | JC3 |
| ~READ       | GP19      | 25 | JC4 |
| COLD_RESET  | GP22      | 29 | JC7 |
| GND         | GND       | 3, 8, 13, 18, 23, 28 | JA5, JB5, JC5, JA11, JB11, JC11 |

Never connect the Pmod 3V3 pins (6 and 12) to the cart.

**Power order:** turn the ZedBoard on first and the N64 off first. Running the N64
while the ZedBoard is unpowered back-feeds the FPGA through its pins.

The cart only drives AD while COLD_RESET is high, CTRL.enable is set, and the N64
is reading a ROM (`0x10000000-0x1FBFFFFF`) or SRAM (`0x08000000-0x080FFFFF`) address.

## LEDs

| LED | Meaning |
|-----|---------|
| LD0 | Cart enabled by software |
| LD1 | N64 powered (COLD_RESET high) |
| LD2 | ROM access |
| LD3 | SRAM access |
| LD4 | ROM data was late at least once (underrun) |
| LD5 | SRAM written since last clear |

## Registers (AXI GP0, base 0x43C00000)

| Offset | Name | |
|--------|------|-|
| 0x00000 | ID | RO, `0x4E363443` |
| 0x00004 | CTRL | bit 0: enable |
| 0x00008 | ROM_BASE | DDR address of the `.z64` image, 128-byte aligned (default `0x10000000`) |
| 0x0000C | STATUS | bit 0: N64 on, bit 1: SRAM dirty, bit 2: underrun seen |
| 0x00010 | CLEAR | write bit 1: clear SRAM dirty, bit 2: clear underrun |
| 0x00014 | UNDERRUNS | count |
| 0x00018 | LAST_ADDR | last N64 bus address |
| 0x0001C | ADDR_COUNT | address phases seen |
| 0x20000-0x3FFFF | SRAM | 128 KB. Word k = N64 SRAM bytes 4k..4k+3 as `b0<<24 \| b1<<16 \| b2<<8 \| b3` |

The loader (`sw/loader`) uses these registers to start a game:
1. copies the ROM to DDR at ROM_BASE in `.z64` byte order, converting `.v64`/`.n64` files,
2. flushes the data cache for that range, because HP0 reads bypass the cache,
3. writes the `.sra` save into the SRAM window,
4. sets CTRL.enable = 1, then you power on the N64.

## Simulation

Needs [Icarus Verilog](https://bleyer.org/icarus/).

```bash
cd fpga/zedboard/sim
iverilog -g2005 -s tb_n64_cart -o tb.vvp tb_n64_cart.v ../rtl/*.v
vvp -n tb.vvp
```

The testbench uses the N64's real bus timing and a DDR model with 100-300 ns
latency. It checks ROM reads (unaligned, across 4 KB boundaries, at the top of
64 MB, and with back-to-back addresses), SRAM in both directions, banked SRAM,
regions the cart must ignore, bus contention, AXI burst rules and underruns.

## Building the bitstream

Needs Vivado (the free edition supports the ZedBoard's XC7Z020) and the ZedBoard
board files.

```bash
cd fpga/zedboard
vivado -mode batch -source scripts/create_project.tcl -tclargs build
```

This writes `n64cart.xsa` for Vitis, where the bare-metal loader is built.

## Loader software

`sw/loader/src` is a Vitis standalone (bare-metal) app for the Cortex-A9.

**Set up in Vitis:**
1. Create a platform from `n64cart.xsa`: OS *standalone*, processor `ps7_cortexa9_0`.
2. In the platform's BSP settings, add the **xilffs** library and set `use_lfn` to 1
   (long file names). Without it, only 8.3 names are listed.
3. Create an empty C application on that platform and add the four files in `sw/loader/src`.
4. Build and run it over JTAG (boot mode jumpers JP7-JP11 all to GND), or make a
   `BOOT.BIN` (FSBL + bitstream + app) with *Create Boot Image*, copy it to the SD
   card and set JP9 and JP10 to 3V3 for SD boot. Check the jumpers against the
   ZedBoard user guide.

**SD card:** FAT32, ROMs (`.z64`, `.v64`, `.n64`) in the root folder.

**Using it:** connect to the USB-UART port (J14) at 115200 8N1.
1. Pick a ROM by number. Enter picks the last game played.
2. Wait for "Cart enabled", then power on the N64.
3. Saves are written to `<rom name>.sra` next to the ROM, 1 second after the
   game stops writing SRAM. Press `s` to save immediately.
4. To change games: power off the N64, then press `m`.

The loader warns if a ROM's boot code isn't the 6102/7101 type, since the Pico
only emulates that CIC.

**Saves:** only SRAM games save, such as Resident Evil 2.
EEPROM games (they use the serial bus, which isn't wired) and FlashRAM games
(Majora's Mask, Paper Mario...) run, but their saves are lost.

**Unit tests** for the platform independent part (any C compiler):

```bash
cd fpga/zedboard/sw/loader/test
cc -I../src -o test_rom_util test_rom_util.c ../src/rom_util.c && ./test_rom_util
```
