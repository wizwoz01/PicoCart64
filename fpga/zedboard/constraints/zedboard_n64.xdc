# SPDX-License-Identifier: BSD-2-Clause
#
# ZedBoard pins for the N64 cartridge bus. All Pmods are on bank 13 (3.3 V).
#
#   JA: AD0-AD7     JB: AD8-AD15     JC: ALE_L, ALE_H, ~WRITE, ~READ, COLD_RESET
#
# Pmod pin numbers: 1-4 top row, 7-10 bottom row (5/11 = GND, 6/12 = 3V3).
# Never connect the Pmod 3V3 pins to the cartridge.

# JA -> AD0..AD7
set_property PACKAGE_PIN Y11  [get_ports {n64_ad[0]}]
set_property PACKAGE_PIN AA11 [get_ports {n64_ad[1]}]
set_property PACKAGE_PIN Y10  [get_ports {n64_ad[2]}]
set_property PACKAGE_PIN AA9  [get_ports {n64_ad[3]}]
set_property PACKAGE_PIN AB11 [get_ports {n64_ad[4]}]
set_property PACKAGE_PIN AB10 [get_ports {n64_ad[5]}]
set_property PACKAGE_PIN AB9  [get_ports {n64_ad[6]}]
set_property PACKAGE_PIN AA8  [get_ports {n64_ad[7]}]

# JB -> AD8..AD15
set_property PACKAGE_PIN W12  [get_ports {n64_ad[8]}]
set_property PACKAGE_PIN W11  [get_ports {n64_ad[9]}]
set_property PACKAGE_PIN V10  [get_ports {n64_ad[10]}]
set_property PACKAGE_PIN W8   [get_ports {n64_ad[11]}]
set_property PACKAGE_PIN V12  [get_ports {n64_ad[12]}]
set_property PACKAGE_PIN W10  [get_ports {n64_ad[13]}]
set_property PACKAGE_PIN V9   [get_ports {n64_ad[14]}]
set_property PACKAGE_PIN V8   [get_ports {n64_ad[15]}]

# JC -> control (inputs only)
set_property PACKAGE_PIN AB7  [get_ports n64_alel]
set_property PACKAGE_PIN AB6  [get_ports n64_aleh]
set_property PACKAGE_PIN Y4   [get_ports n64_write_n]
set_property PACKAGE_PIN AA4  [get_ports n64_read_n]
set_property PACKAGE_PIN R6   [get_ports n64_cold_reset]

# User LEDs LD0..LD7
set_property PACKAGE_PIN T22  [get_ports {led[0]}]
set_property PACKAGE_PIN T21  [get_ports {led[1]}]
set_property PACKAGE_PIN U22  [get_ports {led[2]}]
set_property PACKAGE_PIN U21  [get_ports {led[3]}]
set_property PACKAGE_PIN V22  [get_ports {led[4]}]
set_property PACKAGE_PIN W22  [get_ports {led[5]}]
set_property PACKAGE_PIN U19  [get_ports {led[6]}]
set_property PACKAGE_PIN U14  [get_ports {led[7]}]

set_property IOSTANDARD LVCMOS33 [get_ports {n64_* led[*]}]

# Keep the cart off the bus while the N64 is unpowered
set_property PULLDOWN TRUE [get_ports n64_cold_reset]

# Gentle edges on the bus outputs to limit ringing on the wires
set_property DRIVE 8 [get_ports {n64_ad[*]}]
set_property SLEW SLOW [get_ports {n64_ad[*]}]

# The N64 bus is asynchronous to the FPGA clock; it is synchronized in n64_in_filter.v
set_false_path -from [get_ports {n64_*}]
set_false_path -to [get_ports {n64_ad[*]}]
set_false_path -to [get_ports {led[*]}]
