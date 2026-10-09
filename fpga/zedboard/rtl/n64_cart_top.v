// SPDX-License-Identifier: BSD-2-Clause
//
// N64 cartridge served from Zynq DDR.
//
// Packaged as a Vivado block design module reference:
//   s_axi : AXI4-Lite registers + SRAM (from M_AXI_GP0), see cart_regs.v
//   m_axi : AXI4 read-only master for ROM data (to S_AXI_HP0)
// The AD bus is exposed as ad_i/ad_o/ad_t and gets IOBUFs in system_top.v.
// The ZedBoard push buttons and OLED are reached through the same registers,
// as are the cart EEPROM on S_DAT and the CIC type lines to the Pico.

`default_nettype none

module n64_cart_top #(
	parameter BTN_TICK = 100000		// button sample period in clocks (1 ms)
) (
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

	output wire [7:0]  led,

	input  wire [4:0]  btn,			// C, D, L, R, U
	output wire        oled_vdd,
	output wire        oled_vbat,
	output wire        oled_res,
	output wire        oled_dc,
	output wire        oled_sclk,
	output wire        oled_sdin,

	input  wire        sdat_i,			// joybus S_DAT, open drain
	output wire        sdat_t,
	output wire [2:0]  cic_t			// open drain: 1 = released
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
	wire [4:0]  btn_state, btn_pressed;
	wire [2:0]  oled_ctrl;
	wire        oled_busy, oled_byte_go, oled_flush_go;
	wire [8:0]  oled_byte;
	wire [6:0]  oled_fb_addr;
	wire [31:0] oled_fb_wdata;
	wire [3:0]  oled_fb_we;
	wire        eep_enable, eep_16k, eep_written, eep_dirty, eep_access, sdat, sdat_low;
	reg         sdat_d = 1'b1, sdat_on_d = 1'b0;
	reg  [6:0]  sdat_div = 7'd0;
	reg  [15:0] sdat_falls = 16'h0, sdat_cmds = 16'h0;	// S_DAT monitor, see below
	reg  [15:0] sdat_low_us = 16'h0, sdat_low_max = 16'h0;
	reg  [10:0] sdat_log_n = 11'd0;		// S_DAT event log, see below
	reg  [31:0] sdat_log_q = 32'h0;
	wire [8:0]  eep_addr;
	wire [31:0] eep_wdata, eep_rdata;
	wire [3:0]  eep_we;
	wire [31:0] mon_ale, mon_rw, mon_first_addr, mon_first_data, mon_rom, mon_ad_or, mon_glitch, mon_odd, noise_q;
	wire [15:0] mon_noise_n;
	wire [31:0] mon_xfer_n, mon_xfer_addr, mon_xfer_rw, mon_xfer_sum, xlog_q;
	wire [22:0] pins;
	wire [9:0]  trace_raddr;
	wire [31:0] trace_a, trace_b;

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
		.underrun(underrun),
		.mon_ale(mon_ale),
		.mon_rw(mon_rw),
		.mon_first_addr(mon_first_addr),
		.mon_first_data(mon_first_data),
		.mon_rom(mon_rom),
		.mon_ad_or(mon_ad_or),
		.mon_glitch(mon_glitch),
		.mon_odd(mon_odd),
		.mon_noise_n(mon_noise_n),
		.mon_xfer_n(mon_xfer_n),
		.mon_xfer_addr(mon_xfer_addr),
		.mon_xfer_rw(mon_xfer_rw),
		.mon_xfer_sum(mon_xfer_sum),
		.pins(pins),
		.trace_raddr(trace_raddr),
		.trace_a(trace_a),
		.trace_b(trace_b),
		.noise_q(noise_q),
		.xlog_q(xlog_q)
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
		.sram_rdata(sram_rdata),
		.btn_state(btn_state),
		.btn_pressed(btn_pressed),
		.oled_ctrl(oled_ctrl),
		.oled_busy(oled_busy),
		.oled_byte_go(oled_byte_go),
		.oled_byte(oled_byte),
		.oled_flush_go(oled_flush_go),
		.oled_fb_addr(oled_fb_addr),
		.oled_fb_wdata(oled_fb_wdata),
		.oled_fb_we(oled_fb_we),
		.cic_type(cic_t),
		.eep_enable(eep_enable),
		.eep_16k(eep_16k),
		.eep_written(eep_written),
		.eep_dirty(eep_dirty),
		.eep_addr(eep_addr),
		.eep_wdata(eep_wdata),
		.eep_we(eep_we),
		.eep_rdata(eep_rdata),
		.mon_ale(mon_ale),
		.mon_rw(mon_rw),
		.mon_first_addr(mon_first_addr),
		.mon_first_data(mon_first_data),
		.mon_rom(mon_rom),
		.mon_ad_or(mon_ad_or),
		.mon_glitch(mon_glitch),
		.mon_odd(mon_odd),
		.mon_noise_n(mon_noise_n),
		.mon_xfer_n(mon_xfer_n),
		.mon_xfer_addr(mon_xfer_addr),
		.mon_xfer_rw(mon_xfer_rw),
		.mon_xfer_sum(mon_xfer_sum),
		.mon_sdat({sdat_cmds, sdat_falls}),
		.mon_sdat_low({sdat_low_us, sdat_low_max}),
		.sdat_log_n(sdat_log_n),
		.sdat_log_q(sdat_log_q),
		.pins({8'h00, sdat, pins}),
		.trace_raddr(trace_raddr),
		.trace_a(trace_a),
		.trace_b(trace_b),
		.noise_q(noise_q),
		.xlog_q(xlog_q)
	);

	// Joybus bits are at least 1 us long: ignore anything shorter than 160 ns
	n64_in_filter #(
		.INIT(1'b1),
		.N(16)
	) u_sdat_filter (
		.clk(aclk),
		.d(sdat_i),
		.s(),
		.q(sdat)
	);

	joybus_eeprom u_eeprom (
		.clk(aclk),
		.rst(rst),
		.enable(eep_enable),
		.size16k(eep_16k),
		.sdat(sdat),
		.sdat_low(sdat_low),
		.mem_addr(eep_addr),
		.mem_wdata(eep_wdata),
		.mem_we(eep_we),
		.mem_rdata(eep_rdata),
		.written(eep_written),
		.access(eep_access)
	);

	assign sdat_t = ~sdat_low;

	// S_DAT monitor, cleared when the N64 is turned on: falling edges, EEPROM
	// commands answered, and how long the line stays low. The PIF hangs when
	// the line doesn't come back high (no pull-up, or something holding it).
	always @(posedge aclk) begin
		sdat_d <= sdat;
		sdat_on_d <= n64_on;
		sdat_div <= (sdat_div == 7'd99) ? 7'd0 : sdat_div + 7'd1;
		if (rst || (n64_on && !sdat_on_d)) begin
			sdat_falls <= 16'h0;
			sdat_cmds <= 16'h0;
			sdat_low_us <= 16'h0;
			sdat_low_max <= 16'h0;
		end else if (n64_on) begin
			if (sdat_d && !sdat && sdat_falls != 16'hFFFF)
				sdat_falls <= sdat_falls + 16'h1;
			if (eep_access && sdat_cmds != 16'hFFFF)
				sdat_cmds <= sdat_cmds + 16'h1;
			if (sdat)
				sdat_low_us <= 16'h0;
			else if (sdat_div == 7'd99 && sdat_low_us != 16'hFFFF)
				sdat_low_us <= sdat_low_us + 16'h1;
			if (sdat_low_us > sdat_low_max)
				sdat_low_max <= sdat_low_us;
		end
	end

	// S_DAT event log: the first 1024 changes of the line and of the cart's own
	// drive since the N64 was turned on, to see the waveform around a hang.
	// Entry: [31:30] event (0 line fell, 1 line rose, 2 cart pulls low, 3 cart
	// lets go), [29:0] time since power on in 40 ns units (43 s).
	reg [31:0] sdat_log [0:1023];
	reg [29:0] sdat_t40 = 30'd0;
	reg [1:0]  sdat_t40_div = 2'd0;
	reg        sdat_low_d = 1'b0, drive_pend = 1'b0, drive_pend_on = 1'b0;
	reg        log_we = 1'b0;
	reg [31:0] log_wdata = 32'h0;

	wire line_ev = sdat_d != sdat;
	wire drive_ev = sdat_low_d != sdat_low;

	always @(posedge aclk) begin
		sdat_low_d <= sdat_low;
		log_we <= 1'b0;
		if (rst || (n64_on && !sdat_on_d)) begin
			sdat_t40 <= 30'd0;
			sdat_t40_div <= 2'd0;
			sdat_log_n <= 11'd0;
			drive_pend <= 1'b0;
		end else if (n64_on) begin
			sdat_t40_div <= sdat_t40_div + 2'd1;
			if (sdat_t40_div == 2'd3 && sdat_t40 != 30'h3FFFFFFF)
				sdat_t40 <= sdat_t40 + 30'd1;
			// A drive change in the same clock as a line change waits one clock
			if (drive_ev && line_ev) begin
				drive_pend <= 1'b1;
				drive_pend_on <= sdat_low;
			end
			if (sdat_log_n != 11'd1024 && (line_ev || drive_ev || drive_pend)) begin
				log_we <= 1'b1;
				if (line_ev)
					log_wdata <= {1'b0, sdat, sdat_t40};
				else if (drive_pend)
					log_wdata <= {1'b1, !drive_pend_on, sdat_t40};
				else
					log_wdata <= {1'b1, !sdat_low, sdat_t40};
				if (!line_ev)
					drive_pend <= 1'b0;
				sdat_log_n <= sdat_log_n + 11'd1;
			end
		end
	end

	always @(posedge aclk) begin
		if (log_we)
			sdat_log[sdat_log_n[9:0] - 10'd1] <= log_wdata;
		sdat_log_q <= sdat_log[trace_raddr];
	end

	btn_debounce #(
		.N(5),
		.TICK(BTN_TICK)
	) u_btn (
		.clk(aclk),
		.btn_in(btn),
		.state(btn_state),
		.pressed(btn_pressed)
	);

	oled_spi u_oled (
		.clk(aclk),
		.rst(rst),
		.vdd_on(oled_ctrl[0]),
		.vbat_on(oled_ctrl[1]),
		.run(oled_ctrl[2]),
		.byte_go(oled_byte_go),
		.byte_data(oled_byte[7:0]),
		.byte_dc(oled_byte[8]),
		.flush_go(oled_flush_go),
		.busy(oled_busy),
		.fb_addr(oled_fb_addr),
		.fb_wdata(oled_fb_wdata),
		.fb_we(oled_fb_we),
		.oled_vdd(oled_vdd),
		.oled_vbat(oled_vbat),
		.oled_res(oled_res),
		.oled_dc(oled_dc),
		.oled_sclk(oled_sclk),
		.oled_sdin(oled_sdin)
	);

	// Stretch access pulses to ~84 ms so they are visible on the LEDs
	reg [22:0] rom_led = 0, sram_led = 0, eep_led = 0;

	always @(posedge aclk) begin
		if (rom_access)
			rom_led <= {23{1'b1}};
		else if (rom_led != 0)
			rom_led <= rom_led - 1'b1;
		if (sram_access)
			sram_led <= {23{1'b1}};
		else if (sram_led != 0)
			sram_led <= sram_led - 1'b1;
		if (eep_access)
			eep_led <= {23{1'b1}};
		else if (eep_led != 0)
			eep_led <= eep_led - 1'b1;
	end

	// LD0 enabled, LD1 N64 on, LD2 ROM activity, LD3 SRAM activity,
	// LD4 underrun seen, LD5 SRAM dirty, LD6 EEPROM activity, LD7 EEPROM dirty
	assign led = {eep_dirty, eep_led != 0, sram_dirty, underrun_seen, sram_led != 0, rom_led != 0, n64_on, enable};

endmodule

`default_nettype wire
