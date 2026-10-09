# SPDX-License-Identifier: BSD-2-Clause
#
# ZedBoard pins for the N64 cartridge bus. All Pmods are on bank 13 (3.3 V).
#
#   JA: AD0-AD7     JB: AD8-AD15     JC: ALE_L, ALE_H, ~WRITE, ~READ, COLD_RESET
#   except AD1 on JD7 and AD5 on JD8: JA2 and JA8 didn't make reliable contact
#   with the jumpers on the board this was brought up on.
#
#   JD: S_DAT (EEPROM joybus), CIC type bits 0-2 to the Pico (all open drain)
#
# Also the on-board OLED (bank 13) and push buttons BTNC/D/L/R/U (bank 34).
#
# Pmod pin numbers: 1-4 top row, 7-10 bottom row (5/11 = GND, 6/12 = 3V3).
# Never connect the Pmod 3V3 pins to the cartridge.

# JA -> AD0..AD7
set_property PACKAGE_PIN Y11  [get_ports {n64_ad[0]}]
set_property PACKAGE_PIN W6   [get_ports {n64_ad[1]}]  ;# JD7
set_property PACKAGE_PIN Y10  [get_ports {n64_ad[2]}]
set_property PACKAGE_PIN AA9  [get_ports {n64_ad[3]}]
set_property PACKAGE_PIN AB11 [get_ports {n64_ad[4]}]
set_property PACKAGE_PIN W5   [get_ports {n64_ad[5]}]  ;# JD8
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

# JD -> N64_S_DAT (Lite board J2 pin 1), CIC type -> Pico GPIO26-28 (J1 pins 2-4)
set_property PACKAGE_PIN V7   [get_ports n64_sdat]
set_property PACKAGE_PIN W7   [get_ports {cic_type[0]}]
set_property PACKAGE_PIN V5   [get_ports {cic_type[1]}]
set_property PACKAGE_PIN V4   [get_ports {cic_type[2]}]

# OLED (SSD1306, 128x32). VDD and VBAT switches are active low.
set_property PACKAGE_PIN AA12 [get_ports oled_sdin]
set_property PACKAGE_PIN AB12 [get_ports oled_sclk]
set_property PACKAGE_PIN U10  [get_ports oled_dc]
set_property PACKAGE_PIN U9   [get_ports oled_res]
set_property PACKAGE_PIN U11  [get_ports oled_vbat]
set_property PACKAGE_PIN U12  [get_ports oled_vdd]

# Push buttons, active high: btn[0..4] = BTNC, BTND, BTNL, BTNR, BTNU
set_property PACKAGE_PIN P16  [get_ports {btn[0]}]
set_property PACKAGE_PIN R16  [get_ports {btn[1]}]
set_property PACKAGE_PIN N15  [get_ports {btn[2]}]
set_property PACKAGE_PIN R18  [get_ports {btn[3]}]
set_property PACKAGE_PIN T18  [get_ports {btn[4]}]

set_property IOSTANDARD LVCMOS33 [get_ports {n64_* led[*] oled_* cic_type[*]}]
# Bank 34 runs at VADJ, 2.5 V with jumper J18 in its default position
set_property IOSTANDARD LVCMOS25 [get_ports {btn[*]}]

# Keep the cart off the bus while the N64 is unpowered
set_property PULLDOWN TRUE [get_ports n64_cold_reset]

# S_DAT (EEPROM joybus) is open drain, and the cart pulls it up, like a
# controller pulls up its own line (SummerCart64 does the same). Without a
# pull-up the PIF hung at the first EEPROM command, freezing EEPROM games at
# their first save access. The internal pull-up is weak (10-50 kOhm); a
# 2.2-4.7 kOhm resistor from S_DAT to the cart's 3.3 V (the Pico's 3V3 pin)
# gives cleaner edges over long wires.
set_property PULLUP TRUE [get_ports n64_sdat]

# Weak, slow bus outputs: less ringing on the wires, and less ground bounce
# when all 16 AD lines switch at once
set_property DRIVE 4 [get_ports {n64_ad[*] n64_sdat cic_type[*]}]
set_property SLEW SLOW [get_ports {n64_ad[*] n64_sdat cic_type[*]}]

# The OLED shares bank 13 with the Pmods: keep its edges slow and weak too
set_property DRIVE 4 [get_ports {oled_*}]
set_property SLEW SLOW [get_ports {oled_*}]

# The N64 bus is asynchronous to the FPGA clock; it is synchronized in n64_in_filter.v
set_false_path -from [get_ports {n64_*}]
set_false_path -to [get_ports {n64_ad[*] n64_sdat cic_type[*]}]
set_false_path -to [get_ports {led[*]}]
# Buttons are synchronized in btn_debounce.v, the OLED SPI runs at 5 MHz
set_false_path -from [get_ports {btn[*]}]
set_false_path -to [get_ports {oled_*}]
