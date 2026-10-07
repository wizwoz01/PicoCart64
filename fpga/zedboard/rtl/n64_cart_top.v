// SPDX-License-Identifier: BSD-2-Clause
//
// N64 cartridge served from Zynq DDR.
//
// Packaged as a Vivado block design module reference:
//   s_axi : AXI4-Lite registers + SRAM (from M_AXI_GP0), see cart_regs.v
//   m_axi : AXI4 read-only master for ROM data (to S_AXI_HP0)
// The AD bus is exposed as ad_i/ad_o/ad_t and gets IOBUFs in system_top.v.

`default_nettype none

module n64_cart_top (
	(* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
	(* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi:m_axi, ASSOCIATED_RESET aresetn, FREQ_HZ 100000000" *)
	input  wire        aclk,
	(* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
	(* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
	input  wire        aresetn,

	input  wire [17:0] s_axi_awaddr,
	input  wire [2:0]  s_axi_awprot,
	input  wire        s_axi_awvalid,
	output wire        s_axi_awready,
	input  wire [31:0] s_axi_wdata,
	input  wire [3:0]  s_axi_wstrb,
	input  wire        s_axi_wvalid,
	output wire        s_axi_wready,
	output wire [1:0]  s_axi_bresp,
	output wire        s_axi_bvalid,
	input  wire        s_axi_bready,
	input  wire [17:0] s_axi_araddr,
	input  wire [2:0]  s_axi_arprot,
	input  wire        s_axi_arvalid,
	output wire        s_axi_arready,
	output wire [31:0] s_axi_rdata,
	output wire [1:0]  s_axi_rresp,
	output wire        s_axi_rvalid,
	input  wire        s_axi_rready,

	output wire [31:0] m_axi_araddr,
	output wire [7:0]  m_axi_arlen,
	output wire [2:0]  m_axi_arsize,
	output wire [1:0]  m_axi_arburst,
	output wire [3:0]  m_axi_arcache,
	output wire [2:0]  m_axi_arprot,
	output wire        m_axi_arvalid,
	input  wire        m_axi_arready,
	input  wire [63:0] m_axi_rdata,
	input  wire [1:0]  m_axi_rresp,
	input  wire        m_axi_rlast,
	input  wire        m_axi_rvalid,
	output wire        m_axi_rready,

	input  wire [15:0] ad_i,
	output wire [15:0] ad_o,
	output wire [15:0] ad_t,
	input  wire        aleh,
	input  wire        alel,
	input  wire        read_n,
	input  wire        write_n,
	input  wire        cold_reset,

	output wire [7:0]  led
);

	wire rst = ~aresetn;

	wire        enable;
	wire [31:0] rom_base;
	wire        rom_start, rom_pop, rom_valid;
	wire [25:0] rom_addr;
	wire [15:0] rom_data;
	wire [15:0] sram_addr, sram_rdata, sram_wdata;
	wire        sram_we;
	wire        n64_on, addr_pulse, rom_access, sram_access, underrun;
	wire [31:0] last_addr;
	wire        sram_dirty, underrun_seen;

	n64_pi_bus u_bus (
		.clk(aclk),
		.rst(rst),
		.enable(enable),
		.ad_i(ad_i),
		.ad_o(ad_o),
		.ad_t(ad_t),
		.aleh(aleh),
		.alel(alel),
		.read_n(read_n),
		.write_n(write_n),
		.cold_reset(cold_reset),
		.rom_start(rom_start),
		.rom_addr(rom_addr),
		.rom_data(rom_data),
		.rom_valid(rom_valid),
		.rom_pop(rom_pop),
		.sram_addr(sram_addr),
		.sram_rdata(sram_rdata),
		.sram_wdata(sram_wdata),
		.sram_we(sram_we),
		.n64_on(n64_on),
		.last_addr(last_addr),
		.addr_pulse(addr_pulse),
		.rom_access(rom_access),
		.sram_access(sram_access),
		.underrun(underrun)
	);

	rom_prefetch u_rom (
		.clk(aclk),
		.rst(rst),
		.rom_base(rom_base),
		.start(rom_start),
		.start_addr(rom_addr),
		.pop(rom_pop),
		.data(rom_data),
		.valid(rom_valid),
		.m_axi_araddr(m_axi_araddr),
		.m_axi_arlen(m_axi_arlen),
		.m_axi_arsize(m_axi_arsize),
		.m_axi_arburst(m_axi_arburst),
		.m_axi_arcache(m_axi_arcache),
		.m_axi_arprot(m_axi_arprot),
		.m_axi_arvalid(m_axi_arvalid),
		.m_axi_arready(m_axi_arready),
		.m_axi_rdata(m_axi_rdata),
		.m_axi_rresp(m_axi_rresp),
		.m_axi_rlast(m_axi_rlast),
		.m_axi_rvalid(m_axi_rvalid),
		.m_axi_rready(m_axi_rready)
	);

	cart_regs u_regs (
		.clk(aclk),
		.rst(rst),
		.s_axi_awaddr(s_axi_awaddr),
		.s_axi_awprot(s_axi_awprot),
		.s_axi_awvalid(s_axi_awvalid),
		.s_axi_awready(s_axi_awready),
		.s_axi_wdata(s_axi_wdata),
		.s_axi_wstrb(s_axi_wstrb),
		.s_axi_wvalid(s_axi_wvalid),
		.s_axi_wready(s_axi_wready),
		.s_axi_bresp(s_axi_bresp),
		.s_axi_bvalid(s_axi_bvalid),
		.s_axi_bready(s_axi_bready),
		.s_axi_araddr(s_axi_araddr),
		.s_axi_arprot(s_axi_arprot),
		.s_axi_arvalid(s_axi_arvalid),
		.s_axi_arready(s_axi_arready),
		.s_axi_rdata(s_axi_rdata),
		.s_axi_rresp(s_axi_rresp),
		.s_axi_rvalid(s_axi_rvalid),
		.s_axi_rready(s_axi_rready),
		.enable(enable),
		.rom_base(rom_base),
		.n64_on(n64_on),
		.underrun(underrun),
		.last_addr(last_addr),
		.addr_pulse(addr_pulse),
		.sram_dirty(sram_dirty),
		.underrun_seen(underrun_seen),
		.sram_addr(sram_addr),
		.sram_wdata(sram_wdata),
		.sram_we(sram_we),
		.sram_rdata(sram_rdata)
	);

	// Stretch access pulses to ~84 ms so they are visible on the LEDs
	reg [22:0] rom_led = 0, sram_led = 0;

	always @(posedge aclk) begin
		if (rom_access)
			rom_led <= {23{1'b1}};
		else if (rom_led != 0)
			rom_led <= rom_led - 1'b1;
		if (sram_access)
			sram_led <= {23{1'b1}};
		else if (sram_led != 0)
			sram_led <= sram_led - 1'b1;
	end

	// LD0 enabled, LD1 N64 on, LD2 ROM activity, LD3 SRAM activity,
	// LD4 underrun seen, LD5 SRAM dirty
	assign led = {2'b00, sram_dirty, underrun_seen, sram_led != 0, rom_led != 0, n64_on, enable};

endmodule

`default_nettype wire
