To build and flash with a j-link:

```bash

# First of all, don't forget to update submodules
git submodule update --init

# If you have more than 2MB flash, you need to change the SDK file src/rp2_common/pico_standard_link/memmap_copy_to_ram.ld (for now!):
# Change to: `FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 16384k`

cd sw

# Load a rom into sw/picocart64/rom.h
# `--compress` will compress the rom with a simple compression scheme.
./scripts/load_rom.py --compress my_rom.z64

mkdir build
cd build

# Configure cmake. To use NTSC, set -DREGION=NTSC
# If you have more than 2MB flash, you need to change the flash size by adding -DFLASH_SIZE_MB={one of 2,4,8,16} here.

cmake -DREGION=PAL ..

# Build
cmake --build .

# You can now flash it by holding down the BOOT button and resetting the device, then mounting the usb mass storage device, then copy the file picocart64/picocart64.uf2 to the mounted path and run `sync`.

# Alternatively, you can use openocd with a J-Link:

# Requires https://github.com/raspberrypi/openocd
# AUR package if you're on archlinux: https://aur.archlinux.org/packages/openocd-raspberrypi-git
openocd -f interface/jlink.cfg -c "transport select swd; adapter_khz 1000" -f target/rp2040.cfg -c "program ./picocart64/picocart64.elf verify reset exit"

# Note! You *have* to power reset the pico after flashing it with a jlink, otherwise multicore doesn't work properly.
# Alternatively, attach a debugger, `mon reset halt`, `c`, does the trick as well. It seems that openocd's `reset run` doesn't work properly.

```

## CIC-only firmware

`picocart64_cic_only` runs only the CIC emulation and leaves the PI bus pins
(GPIO 0-19) as floating inputs. Use it when an external device (e.g. an FPGA board
wired to the same pins) serves ROM and saves to the N64, as in `fpga/zedboard`.

The console checks the CIC for as long as it is on and halts on a single late
answer, so this firmware doesn't use the interrupt-driven `n64_cic` library:
`cic_poll.c` runs the protocol in a loop that polls the pins with interrupts off,
like the CIC of SummerCart64 (whose interrupt-driven version froze games at random).
It runs at 133 MHz, without FreeRTOS.

`REGION` is the region of your console. The external device picks the CIC type on
GPIO26-28 (header J1), see `fpga/zedboard/README.md`.

```bash
cd sw
mkdir build
cd build
cmake -DREGION=NTSC ..
cmake --build . --target picocart64_cic_only

# Flash picocart64_cic_only/picocart64_cic_only.uf2 the same way as above.
```

With GPIO26-28 unconnected the CIC is emulated as a 6102/7101, which matches most
games. Pulled low by the external device, they select the type:

| GPIO28..26 | CIC |
|------------|-----|
| 7 (open) | 6102/7101 |
| 6 | 6101 |
| 5 | 7102 |
| 4 | 6103/7103 |
| 3 | 6105/7105 |
| 2 | 6106/7106 |

The Pico's LED turns on once the console's PIF has accepted the CIC, then toggles
on every command the PIF sends. If it stays off with the N64 on, the Pico has no
power, doesn't run this firmware, or the PIF rejected the CIC (e.g. wrong `REGION`).
If it stops changing while a game runs, the PIF stopped talking to the CIC: it
also does that when it hangs on a stuck EEPROM line (S_DAT), so check that first
(`fpga/zedboard/README.md`).
