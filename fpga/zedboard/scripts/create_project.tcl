# SPDX-License-Identifier: BSD-2-Clause
#
# Creates the Vivado project, the Zynq block design and (optionally) the bitstream.
#
#   vivado -mode batch -source scripts/create_project.tcl              ;# project only
#   vivado -mode batch -source scripts/create_project.tcl -tclargs build ;# + bitstream + .xsa
#
# Requires the ZedBoard board files (Vivado: Tools > Vivado Store > Boards > ZedBoard).

set root [file normalize [file dirname [info script]]/..]
set proj_dir $root/vivado

create_project n64cart $proj_dir -part xc7z020clg484-1 -force

set board [lindex [lsort [get_board_parts -quiet *zed*]] end]
if {$board eq ""} {
	error "ZedBoard board files not found. Install them from Tools > Vivado Store > Boards."
}
set_property board_part $board [current_project]

add_files [glob $root/rtl/*.v]
add_files -fileset constrs_1 $root/constraints/zedboard_n64.xdc
update_compile_order -fileset sources_1

# --- Block design ---------------------------------------------------------

create_bd_design design_1

set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
	-config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} $ps

# GP0 for the registers, HP0 (64-bit) for ROM reads, FCLK0 = 100 MHz
set_property -dict [list \
	CONFIG.PCW_USE_M_AXI_GP0 {1} \
	CONFIG.PCW_USE_S_AXI_HP0 {1} \
	CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
	CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
] $ps

create_bd_cell -type module -reference n64_cart_top cart

apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config { \
	Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
	Master {/ps7/M_AXI_GP0} Slave {/cart/s_axi} \
	ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0} \
} [get_bd_intf_pins cart/s_axi]

apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config { \
	Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
	Master {/cart/m_axi} Slave {/ps7/S_AXI_HP0} \
	ddr_seg {Auto} intc_ip {Auto} master_apm {0} \
} [get_bd_intf_pins ps7/S_AXI_HP0]

foreach pin {ad_i ad_o ad_t aleh alel read_n write_n cold_reset led} {
	make_bd_pins_external [get_bd_pins cart/$pin]
	set_property name $pin [get_bd_ports ${pin}_0]
}

assign_bd_address
# Registers at 0x43C00000 (256 KB window: registers + SRAM)
set seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces ps7/Data] -filter {NAME =~ "*cart*"}]
set_property offset 0x43C00000 $seg
set_property range 256K $seg

validate_bd_design
save_bd_design

set bd_file [get_files design_1.bd]
generate_target all $bd_file
add_files -norecurse [make_wrapper -files $bd_file -top]
set_property top system_top [current_fileset]
update_compile_order -fileset sources_1

# --- Build ----------------------------------------------------------------

if {[lindex $argv 0] eq "build"} {
	launch_runs impl_1 -to_step write_bitstream -jobs 4
	wait_on_run impl_1
	if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
		error "Implementation failed, see $proj_dir/n64cart.runs/impl_1"
	}
	write_hw_platform -fixed -include_bit -force $root/n64cart.xsa
	puts "Wrote $root/n64cart.xsa"
}
