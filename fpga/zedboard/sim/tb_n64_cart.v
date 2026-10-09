// SPDX-License-Identifier: BSD-2-Clause
//
// Testbench for n64_cart_top.
//
// Models:
//   - N64 PI bus master with real PI timings (62.5 MHz RCP clock)
//   - DDR behind the HP port: AXI slave with random latency and rvalid gaps
//   - ARM accesses through AXI4-Lite
//   - SSD1306 SPI receiver with setup/hold checks, bouncing push buttons
//   - console side of the joybus S_DAT line (EEPROM), with the console's pull-up
// Checks data, bus contention, AXI burst legality, underruns, buttons, OLED,
// EEPROM commands and timing, and the CIC type lines.

`timescale 1ns / 1ps
`default_nettype none

module tb_n64_cart;

	localparam T_PI = 16;			// RCP cycle, ns
	localparam ROM_BASE = 32'h1000_0000;

	// Domain 1 (ROM) with the default header 0x80371240: LAT=0x40 PWD=0x12 RLS=0x03
	localparam D1_LAT = 8'h40, D1_PWD = 8'h12, D1_RLS = 8'h03;
	// Domain 2 (SRAM), typical values set by libultra
	localparam D2_LAT = 8'h05, D2_PWD = 8'h0C, D2_RLS = 8'h02;

	reg clk = 0;
	reg aresetn = 0;
	always #5 clk = ~clk;			// 100 MHz

	integer errors = 0;

	// ------------------------------------------------------------------
	// N64 side
	// ------------------------------------------------------------------
	reg        aleh = 0, alel = 0, read_n = 1, write_n = 1, cold_reset = 0;
	reg        n64_drive = 0;
	reg [15:0] n64_ad = 16'h0;
	wire [15:0] ad_bus;
	wire [15:0] ad_o, ad_t;

	assign ad_bus = n64_drive ? n64_ad : 16'hzzzz;
	genvar gi;
	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : g_ad
			assign ad_bus[gi] = ad_t[gi] ? 1'bz : ad_o[gi];
		end
	endgenerate

	// Another device fighting the cart on AD3; its stronger drive wins
	reg pico_ad3 = 0;
	assign (supply0, supply1) ad_bus[3] = pico_ad3 ? 1'b0 : 1'bz;

	// Bus contention monitor
	always @(*) begin
		if (n64_drive && ad_t !== 16'hFFFF && aresetn) begin
			$display("[%0t] ERROR: bus contention (N64 and FPGA both drive AD)", $time);
			errors = errors + 1;
		end
	end

	// ------------------------------------------------------------------
	// AXI4-Lite master
	// ------------------------------------------------------------------
	reg  [17:0] awaddr = 0, araddr = 0;
	reg         awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
	reg  [31:0] wdata = 0;
	reg  [3:0]  wstrb = 0;
	wire        awready, wready, bvalid, arready, rvalid;
	wire [1:0]  bresp, rresp;
	wire [31:0] rdata;

	task axil_write_strb(input [17:0] a, input [31:0] d, input [3:0] strb);
		begin
			@(posedge clk);
			awaddr <= a; awvalid <= 1; wdata <= d; wstrb <= strb; wvalid <= 1; bready <= 1;
			@(posedge clk);
			while (!(awready && wready)) @(posedge clk);
			awvalid <= 0; wvalid <= 0;
			while (!bvalid) @(posedge clk);
			@(posedge clk);
			bready <= 0;
		end
	endtask

	task axil_write(input [17:0] a, input [31:0] d);
		axil_write_strb(a, d, 4'hF);
	endtask

	task axil_read(input [17:0] a, output [31:0] d);
		begin
			@(posedge clk);
			araddr <= a; arvalid <= 1; rready <= 1;
			@(posedge clk);
			while (!arready) @(posedge clk);
			arvalid <= 0;
			while (!rvalid) @(posedge clk);
			d = rdata;
			@(posedge clk);
			rready <= 0;
		end
	endtask

	// ------------------------------------------------------------------
	// DDR model: AXI4 read slave
	// ------------------------------------------------------------------
	wire [31:0] m_araddr;
	wire [7:0]  m_arlen;
	wire [2:0]  m_arsize, m_arprot;
	wire [1:0]  m_arburst;
	wire [3:0]  m_arcache;
	wire        m_arvalid, m_rready;
	reg         m_arready = 0;
	reg  [63:0] m_rdata = 0;
	reg         m_rlast = 0, m_rvalid = 0;

	// Deterministic ROM contents: byte at ROM offset o
	function [7:0] rom_byte(input [31:0] o);
		rom_byte = (o[7:0] * 8'd31) ^ o[15:8] ^ (o[23:16] + 8'd7) ^ {6'b0, o[25:24]};
	endfunction

	function [15:0] rom_hw(input [31:0] o);
		rom_hw = {rom_byte(o), rom_byte(o + 1)};
	endfunction

	integer bursts = 0;
	integer k;
	reg [31:0] b_addr;
	reg [7:0]  b_len;

	initial begin : ddr_model
		integer lat, beat;
		forever begin
			@(posedge clk);
			m_arready <= 1;
			@(posedge clk);
			while (!m_arvalid) @(posedge clk);
			b_addr = m_araddr;
			b_len = m_arlen;
			m_arready <= 0;
			bursts = bursts + 1;
			if (b_len > 15 || m_arsize != 3 || m_arburst != 1 || b_addr[2:0] != 0 ||
			    (b_addr[11:0] + (b_len + 1) * 8) > 4096) begin
				$display("[%0t] ERROR: illegal burst addr=%08h len=%0d", $time, b_addr, b_len);
				errors = errors + 1;
			end
			// HP port read latency, 100..300 ns
			lat = 10 + ($random & 15) + ($random & 7);
			repeat (lat) @(posedge clk);
			for (beat = 0; beat <= b_len; beat = beat + 1) begin
				// Occasional gaps
				while (($random & 7) == 0) begin
					m_rvalid <= 0;
					@(posedge clk);
				end
				for (k = 0; k < 8; k = k + 1)
					m_rdata[k * 8 +: 8] <= rom_byte(b_addr - ROM_BASE + beat * 8 + k);
				m_rvalid <= 1;
				m_rlast <= (beat == b_len);
				@(posedge clk);
				while (!m_rready) @(posedge clk);
			end
			m_rvalid <= 0;
			m_rlast <= 0;
		end
	end

	// ------------------------------------------------------------------
	// OLED: SSD1306 serial receiver. Samples SDIN on SCLK rising edges, MSB
	// first, D/C with the last bit. Needs 15 ns setup/hold and 20 ns pulses.
	// ------------------------------------------------------------------
	wire       oled_vdd, oled_vbat, oled_res, oled_dc, oled_sclk, oled_sdin;
	reg  [7:0] spi_sh = 0;
	reg  [8:0] spi_log [0:1023];
	integer    spi_bits = 0, spi_n = 0;
	time       t_rise = 0, t_change = 0, t_edge = 0;

	always @(posedge oled_sclk) begin
		if (oled_res && $time - t_change < 15) begin
			$display("[%0t] ERROR: OLED SDIN/DC setup time", $time);
			errors = errors + 1;
		end
		t_rise = $time;
		spi_sh = {spi_sh[6:0], oled_sdin};
		spi_bits = spi_bits + 1;
		if (spi_bits == 8) begin
			spi_log[spi_n] = {oled_dc, spi_sh};
			spi_n = spi_n + 1;
			spi_bits = 0;
		end
	end

	always @(oled_sdin or oled_dc) begin
		if (oled_res && $time - t_rise < 15) begin
			$display("[%0t] ERROR: OLED SDIN/DC hold time", $time);
			errors = errors + 1;
		end
		t_change = $time;
	end

	always @(oled_sclk) begin
		if (oled_res && $time - t_edge < 20) begin
			$display("[%0t] ERROR: OLED SCLK pulse too short", $time);
			errors = errors + 1;
		end
		t_edge = $time;
	end

	always @(negedge oled_res)
		spi_bits = 0;

	function [7:0] fb_byte(input integer i);
		fb_byte = (i * 7 + 1) ^ (i >> 8);
	endfunction

	function [31:0] fb_word(input integer w);
		integer b;
		begin
			for (b = 0; b < 4; b = b + 1)
				fb_word[b * 8 +: 8] = fb_byte(w * 4 + b);
		end
	endfunction

	// ------------------------------------------------------------------
	// Joybus (S_DAT): open drain, pulled up by the console
	// ------------------------------------------------------------------
	localparam JB_US = 1000;		// ns
	reg        pif_low = 0;
	wire       sdat_t;
	wire [2:0] cic_t;
	wire       sdat_line = !(pif_low || !sdat_t);
	reg  [7:0] jb_tx [0:15];
	reg  [7:0] jb_rx [0:15];
	integer    jb_n, jb_first;
	reg        jb_stop_ok;

	task jb_bit(input b);
		begin
			pif_low = 1;
			#((b ? 1 : 3) * JB_US);
			pif_low = 0;
			#((b ? 3 : 1) * JB_US);
		end
	endtask

	task jb_byte(input [7:0] v);
		integer k;
		begin
			for (k = 7; k >= 0; k = k - 1)
				jb_bit(v[k]);
		end
	endtask

	// Reply: 1 us low = 1, 3 us low = 0, 2 us low = stop bit
	task jb_receive;
		integer w, lo, bits;
		reg [7:0] v;
		begin
			jb_n = 0;
			jb_stop_ok = 0;
			bits = 0;
			v = 0;
			w = 0;
			while (sdat_line && w < 50000) begin
				#10;
				w = w + 10;
			end
			jb_first = w;
			while (!jb_stop_ok && w < 50000) begin
				lo = 0;
				while (!sdat_line && lo < 10000) begin
					#10;
					lo = lo + 10;
				end
				if (lo >= 1700 && lo <= 2300) begin
					jb_stop_ok = 1;
				end else begin
					if (lo >= 700 && lo <= 1300) begin
						v = {v[6:0], 1'b1};
					end else if (lo >= 2700 && lo <= 3300) begin
						v = {v[6:0], 1'b0};
					end else begin
						$display("[%0t] ERROR: joybus reply pulse of %0d ns", $time, lo);
						errors = errors + 1;
					end
					bits = bits + 1;
					if (bits % 8 == 0) begin
						jb_rx[jb_n] = v;
						jb_n = jb_n + 1;
					end
					w = 0;
					while (sdat_line && w < 10000) begin
						#10;
						w = w + 10;
					end
					if (w >= 10000) begin
						$display("[%0t] ERROR: joybus reply without a stop bit", $time);
						errors = errors + 1;
						w = 50000;
					end
				end
			end
		end
	endtask

	// Sends jb_tx[0..n-1] and the stop bit, then collects the reply
	task jb_xfer(input integer n);
		integer k;
		begin
			#15000;
			for (k = 0; k < n; k = k + 1)
				jb_byte(jb_tx[k]);
			pif_low = 1;
			#(1 * JB_US);
			pif_low = 0;
			#1;
			jb_receive;
		end
	endtask

	// ------------------------------------------------------------------
	// DUT
	// ------------------------------------------------------------------
	wire [7:0] led;
	reg  [4:0] btn = 5'h0;

	// 200 ns button samples instead of 1 ms
	n64_cart_top #(
		.BTN_TICK(20)
	) dut (
		.aclk(clk),
		.aresetn(aresetn),
		.s_axi_awaddr(awaddr),
		.s_axi_awprot(3'b0),
		.s_axi_awvalid(awvalid),
		.s_axi_awready(awready),
		.s_axi_wdata(wdata),
		.s_axi_wstrb(wstrb),
		.s_axi_wvalid(wvalid),
		.s_axi_wready(wready),
		.s_axi_bresp(bresp),
		.s_axi_bvalid(bvalid),
		.s_axi_bready(bready),
		.s_axi_araddr(araddr),
		.s_axi_arprot(3'b0),
		.s_axi_arvalid(arvalid),
		.s_axi_arready(arready),
		.s_axi_rdata(rdata),
		.s_axi_rresp(rresp),
		.s_axi_rvalid(rvalid),
		.s_axi_rready(rready),
		.m_axi_araddr(m_araddr),
		.m_axi_arlen(m_arlen),
		.m_axi_arsize(m_arsize),
		.m_axi_arburst(m_arburst),
		.m_axi_arcache(m_arcache),
		.m_axi_arprot(m_arprot),
		.m_axi_arvalid(m_arvalid),
		.m_axi_arready(m_arready),
		.m_axi_rdata(m_rdata),
		.m_axi_rresp(2'b00),
		.m_axi_rlast(m_rlast),
		.m_axi_rvalid(m_rvalid),
		.m_axi_rready(m_rready),
		.ad_i(ad_bus),
		.ad_o(ad_o),
		.ad_t(ad_t),
		.aleh(aleh),
		.alel(alel),
		.read_n(read_n),
		.write_n(write_n),
		.cold_reset(cold_reset),
		.led(led),
		.btn(btn),
		.oled_vdd(oled_vdd),
		.oled_vbat(oled_vbat),
		.oled_res(oled_res),
		.oled_dc(oled_dc),
		.oled_sclk(oled_sclk),
		.oled_sdin(oled_sdin),
		.sdat_i(sdat_line),
		.sdat_t(sdat_t),
		.cic_t(cic_t)
	);

	// ------------------------------------------------------------------
	// N64 PI master tasks
	// ------------------------------------------------------------------

	// Address phase: ALE_H high for 7 RCP cycles, ALE_L falls 7 cycles after
	// it, the shortest the RCP makes. AD changes only 3 ns after each ALE edge,
	// to check that the FPGA latches the value from before the edge.
	task pi_addr(input [31:0] a);
		begin
			n64_drive = 1;
			n64_ad = a[31:16];
			aleh = 1;
			alel = 1;
			#(7 * T_PI);
			aleh = 0;
			#3 n64_ad = a[15:0];
			#(7 * T_PI - 3);
			alel = 0;
			#3 n64_drive = 0;
			n64_ad = 16'hxxxx;
		end
	endtask

	// Burst read of n halfwords; the RCP samples AD at the end of each pulse.
	reg [15:0] rd_buf [0:1023];

	task pi_read(input integer n, input [7:0] lat, input [7:0] pwd, input [7:0] rls);
		integer j;
		begin
			#((lat + 1) * T_PI - 3);
			for (j = 0; j < n; j = j + 1) begin
				read_n = 0;
				#((pwd + 1) * T_PI);
				rd_buf[j] = ad_bus;
				read_n = 1;
				#((rls + 1) * T_PI);
			end
			#(2 * T_PI);
		end
	endtask

	// Like pi_read, with noise on the ALE lines: a 60 ns ALE_L pulse while
	// ~READ is low before read 2, another between reads 4 and 5, and a 60 ns
	// ALE_H pulse before read 7. All must be ignored.
	task pi_read_noisy(input integer n, input [7:0] lat, input [7:0] pwd, input [7:0] rls);
		integer j;
		begin
			#((lat + 1) * T_PI - 3);
			for (j = 0; j < n; j = j + 1) begin
				read_n = 0;
				if (j == 2) begin
					#(4 * T_PI) alel = 1;
					#60 alel = 0;
					#((pwd + 1) * T_PI - 4 * T_PI - 60);
				end else if (j == 7) begin
					#(4 * T_PI) aleh = 1;
					#60 aleh = 0;
					#((pwd + 1) * T_PI - 4 * T_PI - 60);
				end else begin
					#((pwd + 1) * T_PI);
				end
				rd_buf[j] = ad_bus;
				read_n = 1;
				if (j == 4) begin
					alel = 1;
					#((rls + 1) * T_PI) alel = 0;
				end else begin
					#((rls + 1) * T_PI);
				end
			end
			#(2 * T_PI);
		end
	endtask

	// Like pi_read, with a 60 ns high glitch on ~READ 4 RCP cycles into read 3
	task pi_read_rdglitch(input integer n, input [7:0] lat, input [7:0] pwd, input [7:0] rls);
		integer j;
		begin
			#((lat + 1) * T_PI - 3);
			for (j = 0; j < n; j = j + 1) begin
				read_n = 0;
				if (j == 3) begin
					#(4 * T_PI) read_n = 1;
					#60 read_n = 0;
					#((pwd + 1) * T_PI - 4 * T_PI - 60);
				end else begin
					#((pwd + 1) * T_PI);
				end
				rd_buf[j] = ad_bus;
				read_n = 1;
				#((rls + 1) * T_PI);
			end
			#(2 * T_PI);
		end
	endtask

	task pi_write(input integer n, input [15:0] first, input [7:0] lat, input [7:0] pwd, input [7:0] rls);
		integer j;
		begin
			#((lat + 1) * T_PI - 3);
			for (j = 0; j < n; j = j + 1) begin
				n64_drive = 1;
				n64_ad = first + j * 16'h0101;
				write_n = 0;
				#((pwd + 1) * T_PI);
				write_n = 1;
				#4 n64_drive = 0;
				n64_ad = 16'hxxxx;
				#((rls + 1) * T_PI - 4);
			end
			#(2 * T_PI);
		end
	endtask

	// ------------------------------------------------------------------
	// Checks
	// ------------------------------------------------------------------
	task check_rom(input [31:0] pi_addr_v, input integer n, input [7:0] lat);
		integer j;
		integer bad;
		begin
			pi_addr(pi_addr_v);
			pi_read(n, lat, D1_PWD, D1_RLS);
			bad = 0;
			for (j = 0; j < n; j = j + 1) begin
				if (rd_buf[j] !== rom_hw(pi_addr_v - 32'h1000_0000 + j * 2)) begin
					if (bad < 4)
						$display("  ROM %08h: got %04h expected %04h", pi_addr_v + j * 2, rd_buf[j],
							 rom_hw(pi_addr_v - 32'h1000_0000 + j * 2));
					bad = bad + 1;
				end
			end
			if (bad) begin
				$display("FAIL ROM read %08h x%0d (LAT=%02h): %0d bad words", pi_addr_v, n, lat, bad);
				errors = errors + 1;
			end else begin
				$display("ok   ROM read %08h x%0d (LAT=%02h)", pi_addr_v, n, lat);
			end
		end
	endtask

	task check_undriven(input [31:0] a, input [7:0] lat, input [7:0] pwd, input [7:0] rls, input [255:0] what);
		begin
			pi_addr(a);
			pi_read(4, lat, pwd, rls);
			if (rd_buf[0] !== 16'hzzzz || rd_buf[3] !== 16'hzzzz) begin
				$display("FAIL %0s: bus driven (%04h)", what, rd_buf[0]);
				errors = errors + 1;
			end else begin
				$display("ok   %0s: bus not driven", what);
			end
		end
	endtask

	task expect32(input [31:0] got, input [31:0] exp, input [255:0] what);
		begin
			if (got !== exp) begin
				$display("FAIL %0s: got %08h expected %08h", what, got, exp);
				errors = errors + 1;
			end else begin
				$display("ok   %0s = %08h", what, got);
			end
		end
	endtask

	task expect1(input got, input exp, input [255:0] what);
		begin
			if (got !== exp) begin
				$display("FAIL %0s: got %b expected %b", what, got, exp);
				errors = errors + 1;
			end
		end
	endtask

	task oled_wait_idle;
		integer polls;
		reg [31:0] v;
		begin
			polls = 0;
			axil_read(18'h00024, v);
			while ((v & 32'h100) && polls < 10000) begin
				#500;
				axil_read(18'h00024, v);
				polls = polls + 1;
			end
			if (v & 32'h100) begin
				$display("FAIL OLED stays busy");
				errors = errors + 1;
			end
		end
	endtask

	task expect16(input [15:0] got, input [15:0] exp, input [255:0] what);
		begin
			if (got !== exp) begin
				$display("FAIL %0s: got %04h expected %04h", what, got, exp);
				errors = errors + 1;
			end else begin
				$display("ok   %0s = %04h", what, got);
			end
		end
	endtask

	reg [31:0] r;
	integer j, nbad;
	reg [31:0] r2;
	time t0;

	initial begin
		$dumpfile("tb_n64_cart.vcd");
		$dumpvars(0, tb_n64_cart);

		repeat (10) @(posedge clk);
		aresetn = 1;
		repeat (10) @(posedge clk);

		axil_read(18'h00000, r);
		expect32(r, 32'h4E363443, "ID");

		// Not enabled yet: the cart must stay off the bus
		cold_reset = 1;
		#1000;
		check_undriven(32'h1000_0000, D1_LAT, D1_PWD, D1_RLS, "ROM before enable");

		// The bus monitor counts while the cart is disabled
		axil_read(18'h00038, r);
		expect32(r, 32'h0001_0001, "MON_ALE before enable");
		axil_read(18'h0003C, r);
		expect32(r, 32'h0000_0004, "MON_RW before enable");
		axil_read(18'h00040, r);
		expect32(r, 32'h1000_0000, "MON_FIRST");
		axil_read(18'h00048, r);
		expect32(r, 32'h0, "MON_ROM before enable");

		axil_write(18'h00008, ROM_BASE);
		axil_write(18'h00004, 32'h1);
		axil_read(18'h0000C, r);
		expect32(r & 1, 1, "STATUS.n64_on");

		// ROM header and IPL3 area, like the PIF/IPL3 boot reads
		check_rom(32'h1000_0000, 32, D1_LAT);
		axil_read(18'h00044, r);
		expect32(r, {rom_hw(0), rom_hw(2)}, "MON_DATA (header)");
		axil_read(18'h00048, r);
		expect32(r, 32'd32, "MON_ROM after the header");
		check_rom(32'h1000_0040, 512, D1_LAT);
		axil_read(18'h00040, r);
		expect32(r, 32'h1000_0000, "MON_FIRST kept");
		axil_read(18'h0004C, r);
		expect32(r, 32'h1000_0040, "MON_AD_OR");
		// Address trace, starting with the read before enable
		axil_read(18'h03000, r);
		expect32(r, 32'h1000_0000, "TRACE_A[0]");
		axil_read(18'h03008, r);
		expect32(r, 32'h1000_0040, "TRACE_A[2]");
		axil_read(18'h04008, r);
		$display("info TRACE_B[2] = %08h (the testbench releases AD 3 ns after ALE_L)", r);
		axil_read(18'h00038, r);
		expect32(r, 32'h0003_0003, "MON_ALE");
		axil_read(18'h0003C, r);
		expect32(r, 32'd548, "MON_RW");
		// Idle bus: ALE low, ~WRITE, ~READ, COLD_RESET and S_DAT high
		axil_read(18'h00050, r);
		expect32(r & 32'hFFFF_0000, 32'h00CC_0000, "PINS control lines");
		// Unaligned start and crossing a 4 KB boundary
		check_rom(32'h1000_0FF2, 300, D1_LAT);
		check_rom(32'h1012_3456, 3, D1_LAT);
		// Top of a 64 MB ROM
		check_rom(32'h13FF_FFC0, 32, D1_LAT);
		// New address right after a short read, while a burst is still in flight
		for (j = 0; j < 8; j = j + 1)
			check_rom(32'h1100_0000 + j * 32'h0001_2346, 2 + j, D1_LAT);
		// Faster LAT to see the margin
		check_rom(32'h1234_5678, 64, 8'h20);

		// Writes to ROM are ignored
		pi_addr(32'h1000_0100);
		pi_write(4, 16'hDEAD, D1_LAT, D1_PWD, D1_RLS);
		check_rom(32'h1000_0100, 4, D1_LAT);

		// 64DD / other regions are never claimed
		check_undriven(32'h0500_0000, D2_LAT, D2_PWD, D2_RLS, "64DD regs 0x05000000");
		check_undriven(32'h0600_0000, D1_LAT, D1_PWD, D1_RLS, "64DD IPL 0x06000000");
		check_undriven(32'h1FC0_0000, D1_LAT, D1_PWD, D1_RLS, "PIF area 0x1FC00000");

		// SRAM: N64 writes then reads back
		axil_read(18'h0000C, r);
		expect32(r & 2, 0, "STATUS.sram_dirty before write");
		pi_addr(32'h0800_0000);
		pi_write(16, 16'h1234, D2_LAT, D2_PWD, D2_RLS);
		pi_addr(32'h0800_0000);
		pi_read(16, D2_LAT, D2_PWD, D2_RLS);
		r = 0;
		for (j = 0; j < 16; j = j + 1)
			if (rd_buf[j] !== 16'h1234 + j * 16'h0101)
				r = r + 1;
		expect32(r, 0, "SRAM N64 write/read mismatches");
		axil_read(18'h0000C, r);
		expect32(r & 2, 2, "STATUS.sram_dirty after write");

		// SRAM as seen by the ARM: {b0,b1,b2,b3}
		axil_read(18'h20000, r);
		expect32(r, 32'h12341335, "SRAM word 0 via AXI");

		// ARM writes, N64 reads
		axil_write(18'h20040, 32'hCAFEBABE);
		pi_addr(32'h0800_0040);
		pi_read(2, D2_LAT, D2_PWD, D2_RLS);
		expect16(rd_buf[0], 16'hCAFE, "SRAM 0x08000040 via N64");
		expect16(rd_buf[1], 16'hBABE, "SRAM 0x08000042 via N64");

		// Banked SRAM: bank 1 at 0x08040000 -> SRAM byte offset 0x8000
		pi_addr(32'h0804_0000);
		pi_write(2, 16'hA55A, D2_LAT, D2_PWD, D2_RLS);
		axil_read(18'h28000, r);
		expect32(r, 32'hA55AA65B, "banked SRAM via AXI");

		// Clear dirty
		axil_write(18'h00010, 32'h2);
		axil_read(18'h0000C, r);
		expect32(r & 2, 0, "STATUS.sram_dirty after clear");

		// N64 powered off: cart must release the bus
		cold_reset = 0;
		#1000;
		check_undriven(32'h1000_0000, D1_LAT, D1_PWD, D1_RLS, "ROM with COLD_RESET low");

		axil_read(18'h00014, r);
		expect32(r, 0, "UNDERRUNS");
		axil_read(18'h0001C, r);
		$display("info address phases=%0d AXI bursts=%0d", r, bursts);

		// Turning the N64 on clears the bus monitor
		cold_reset = 1;
		#1000;
		axil_read(18'h00038, r);
		expect32(r, 32'h0, "MON_ALE after power on");
		axil_read(18'h00040, r);
		expect32(r, 32'h0, "MON_FIRST after power on");

		// Another driver holding AD3 low during ROM reads: counted as mismatches
		pico_ad3 = 1;
		pi_addr(32'h1000_0000);
		pi_read(16, D1_LAT, D1_PWD, D1_RLS);
		pico_ad3 = 0;
		axil_read(18'h00044, r);
		expect32(r, {rom_hw(0), rom_hw(2)} & ~32'h0008_0008, "MON_DATA with AD3 held low");
		nbad = 0;
		for (j = 0; j < 16; j = j + 1)
			nbad = nbad + ((rom_hw(j * 2) >> 3) & 1);
		axil_read(18'h00048, r);
		expect32(r, {nbad[15:0], 16'd16}, "MON_ROM with AD3 held low");
		// The trace starts over at power on
		pi_addr(32'h1000_1234);
		pi_read(2, D1_LAT, D1_PWD, D1_RLS);
		axil_read(18'h03004, r);
		expect32(r, 32'h1000_1234, "TRACE_A[1] after power on");

		// Noise on ALE_L and ALE_H during a burst doesn't restart the stream
		axil_read(18'h00014, r);
		nbad = r;
		pi_addr(32'h1000_2000);
		pi_read_noisy(12, D1_LAT, D1_PWD, D1_RLS);
		r = 0;
		for (j = 0; j < 12; j = j + 1)
			if (rd_buf[j] !== rom_hw(32'h2000 + j * 2))
				r = r + 1;
		expect32(r, 0, "ROM words wrong with ALE noise");
		axil_read(18'h00054, r);
		expect32(r, 32'h0000_0003, "MON_GLITCH 3 ignored, no underr");
		axil_read(18'h00014, r);
		expect32(r - nbad, 0, "UNDERRUNS with ALE noise");
		axil_read(18'h00038, r);
		expect32(r, 32'h0003_0003, "MON_ALE real phases only");
		// Noise log: ALE_L, ALE_L, ALE_H pulses of 6 clocks
		axil_read(18'h0005C, r);
		expect32(r, 32'd3, "MON_NOISE events");
		axil_read(18'h05000, r);
		expect32(r & 32'hF000_0000, 32'h2000_0000, "NOISE[0] type ALE_L");
		axil_read(18'h05008, r);
		expect32(r & 32'hF000_0000, 32'h1000_0000, "NOISE[2] type ALE_H");
		$display("info NOISE[2] = %08h (width %0d clocks, at %0d us)", r, r[27:20], r[19:0]);

		// A 60 ns glitch on ~READ splits a read in two: the cart can't tell,
		// but the next transfer shows an odd read count
		pi_addr(32'h1000_3000);
		pi_read_rdglitch(8, D1_LAT, D1_PWD, D1_RLS);
		pi_addr(32'h1000_4000);
		pi_read(2, D1_LAT, D1_PWD, D1_RLS);
		axil_read(18'h00058, r);
		expect32(r, 32'h0001_0001, "MON_ODD after a ~READ glitch");
		axil_read(18'h0005C, r);
		expect32(r, 32'd5, "MON_NOISE with ~READ glitch");

		// Transfer log since this power on: 10000000, 10001234, 10002000,
		// 10003000 finished, 10004000 in progress
		axil_read(18'h00060, r);
		expect32(r, 32'd4, "MON_XFER_N");
		axil_read(18'h06010, r);
		expect32(r, 32'h1000_1234, "XFER[1] address");
		axil_read(18'h06014, r);
		expect32(r, 32'd2, "XFER[1] reads");
		axil_read(18'h06018, r);
		expect32(r, rom_hw(32'h1234) + rom_hw(32'h1236), "XFER[1] sum");
		axil_read(18'h06034, r);
		expect32(r, 32'd9, "XFER[3] reads with ~READ glitch");
		axil_read(18'h00064, r);
		expect32(r, 32'h1000_4000, "MON_XFER_ADDR");
		axil_read(18'h00068, r);
		expect32(r, 32'd2, "MON_XFER_RW");
		axil_read(18'h0006C, r);
		expect32(r, rom_hw(32'h4000) + rom_hw(32'h4002), "MON_XFER_SUM");
		cold_reset = 0;
		#1000;

		// Push buttons: bouncing press of BTNU (bit 4)
		axil_read(18'h00020, r);
		expect32(r, 32'h0, "BUTTONS idle");
		for (j = 0; j < 5; j = j + 1) begin
			btn[4] = ~btn[4];
			#137;
		end
		btn[4] = 1;
		#3000;
		axil_read(18'h00020, r);
		expect32(r, 32'h1010, "BUTTONS BTNU held");
		btn[4] = 0;
		#3000;
		axil_read(18'h00020, r);
		expect32(r, 32'h1000, "BUTTONS BTNU released, press kept");
		axil_write(18'h00020, 32'h1000);
		axil_read(18'h00020, r);
		expect32(r, 32'h0, "BUTTONS after clear");
		// A 1 us glitch on BTNC is shorter than 8 samples
		btn[0] = 1;
		#1000;
		btn[0] = 0;
		#3000;
		axil_read(18'h00020, r);
		expect32(r, 32'h0, "BUTTONS glitch ignored");

		// OLED: everything off and low after reset
		expect1(oled_vdd, 1, "OLED VDD off");
		expect1(oled_vbat, 1, "OLED VBAT off");
		expect1(oled_res, 0, "OLED RES# low");
		expect1(oled_sclk | oled_sdin | oled_dc, 0, "OLED signals low while off");
		axil_write(18'h00024, 32'h1);
		expect1(oled_vdd, 0, "OLED VDD on");
		expect1(oled_res, 0, "OLED still in reset");
		axil_write(18'h00024, 32'h5);
		expect1(oled_res, 1, "OLED RES# released");
		expect1(oled_vbat, 1, "OLED VBAT still off");
		axil_write(18'h00024, 32'h7);
		expect1(oled_vbat, 0, "OLED VBAT on");

		// Command byte, and a second one sent while busy is dropped
		spi_n = 0;
		axil_write(18'h00028, 32'h0AE);
		axil_read(18'h00024, r);
		expect32(r, 32'h107, "OLED_CTRL busy");
		axil_write(18'h00028, 32'h08D);
		oled_wait_idle;
		expect32(spi_n, 1, "OLED bytes after command");
		expect32(spi_log[0], 32'h0AE, "OLED command byte with D/C=0");
		axil_write(18'h00028, 32'h1C3);
		oled_wait_idle;
		expect32(spi_log[1], 32'h1C3, "OLED data byte with D/C=1");

		// Frame buffer, with one byte-masked write
		for (j = 0; j < 128; j = j + 1)
			axil_write(18'h01000 + j * 4, fb_word(j));
		axil_write_strb(18'h01014, 32'hFFFFA5FF, 4'b0010);
		spi_n = 0;
		t0 = $time;
		axil_write(18'h0002C, 32'h1);
		oled_wait_idle;
		$display("info OLED frame took %0d us", ($time - t0) / 1000);
		expect32(spi_n, 512, "OLED frame bytes");
		r = 0;
		for (j = 0; j < 512; j = j + 1)
			if (spi_log[j] !== {1'b1, (j == 21) ? 8'hA5 : fb_byte(j)}) begin
				if (r < 4)
					$display("  OLED byte %0d: got %03h", j, spi_log[j]);
				r = r + 1;
			end
		expect32(r, 0, "OLED frame mismatches");

		axil_write(18'h00024, 32'h0);
		expect1(oled_vdd & oled_vbat, 1, "OLED powered off");
		expect1(oled_res | oled_sclk | oled_sdin | oled_dc, 0, "OLED signals low after power off");

		// CIC type lines: released after reset (Pico pull-ups read 7 = 6102)
		expect32(cic_t, 32'h7, "CIC lines released");
		axil_write(18'h00030, 32'h3);
		expect32(cic_t, 32'h3, "CIC type 3 (6105)");
		axil_read(18'h00030, r);
		expect32(r, 32'h3, "CIC register");

		// The console talks to the EEPROM while it is on (and the S_DAT
		// monitor counts only then)
		cold_reset = 1;
		#1000;

		// EEPROM disabled: no reply
		jb_tx[0] = 8'h00;
		jb_xfer(1);
		expect32(jb_n, 0, "EEPROM off: reply bytes");

		// 4 Kbit: info
		axil_write(18'h00034, 32'h1);
		jb_xfer(1);
		expect32(jb_n, 3, "EEPROM 4K info: reply bytes");
		expect32({jb_rx[0], jb_rx[1], jb_rx[2]}, 32'h008000, "EEPROM 4K info");
		expect1(jb_stop_ok, 1, "EEPROM reply stop bit");
		$display("info EEPROM reply starts %0d ns after the console stop bit ends", jb_first);
		if (jb_first > 6000) begin
			$display("FAIL EEPROM reply too late");
			errors = errors + 1;
		end

		// Write block 5
		jb_tx[0] = 8'h05;
		jb_tx[1] = 8'h05;
		for (j = 0; j < 8; j = j + 1)
			jb_tx[2 + j] = 8'hA0 + j;
		jb_xfer(10);
		expect32(jb_n, 1, "EEPROM write: reply bytes");
		expect32(jb_rx[0], 32'h00, "EEPROM write status");
		axil_read(18'h0000C, r);
		expect32(r & 8, 8, "STATUS.eeprom_dirty after write");
		axil_read(18'h02028, r);
		expect32(r, 32'hA0A1A2A3, "EEPROM block 5 word 0 via AXI");
		axil_read(18'h0202C, r);
		expect32(r, 32'hA4A5A6A7, "EEPROM block 5 word 1 via AXI");

		// ARM writes block 7, the console reads it
		axil_write(18'h02038, 32'h11223344);
		axil_write(18'h0203C, 32'h55667788);
		jb_tx[0] = 8'h04;
		jb_tx[1] = 8'h07;
		jb_xfer(2);
		expect32(jb_n, 8, "EEPROM read: reply bytes");
		expect32({jb_rx[0], jb_rx[1], jb_rx[2], jb_rx[3]}, 32'h11223344, "EEPROM read block 7 bytes 0-3");
		expect32({jb_rx[4], jb_rx[5], jb_rx[6], jb_rx[7]}, 32'h55667788, "EEPROM read block 7 bytes 4-7");

		// 4 Kbit has 64 blocks: block 0x45 is block 5
		jb_tx[1] = 8'h45;
		jb_xfer(2);
		expect32({jb_rx[0], jb_rx[7]}, 32'hA0A7, "EEPROM 4K block 0x45 = block 5");

		// 16 Kbit: reset command, and block 0x45 is its own block
		axil_write(18'h00034, 32'h3);
		jb_tx[0] = 8'hFF;
		jb_xfer(1);
		expect32({jb_rx[0], jb_rx[1], jb_rx[2]}, 32'h00C000, "EEPROM 16K reset/info");
		jb_tx[0] = 8'h05;
		jb_tx[1] = 8'h45;
		for (j = 0; j < 8; j = j + 1)
			jb_tx[2 + j] = 8'hB0 + j;
		jb_xfer(10);
		jb_tx[0] = 8'h04;
		jb_tx[1] = 8'h05;
		jb_xfer(2);
		expect32({jb_rx[0], jb_rx[7]}, 32'hA0A7, "EEPROM 16K block 5 kept");
		axil_read(18'h0222C, r);
		expect32(r, 32'hB4B5B6B7, "EEPROM 16K block 0x45 word 1 via AXI");

		// Unknown command: no reply
		jb_tx[0] = 8'h06;
		jb_xfer(1);
		expect32(jb_n, 0, "EEPROM unknown command: reply bytes");

		// S_DAT monitor: 7 commands answered, and the line was never low
		// longer than a 3 us bit
		axil_read(18'h00070, r);
		expect32(r >> 16, 7, "MON_SDAT commands answered");
		if ((r & 32'hFFFF) < 100) begin
			$display("FAIL MON_SDAT falls: %0d", r & 32'hFFFF);
			errors = errors + 1;
		end
		axil_read(18'h00074, r);
		expect32(r >> 16, 0, "MON_SDAT_LOW low now");
		if ((r & 32'hFFFF) < 2 || (r & 32'hFFFF) > 3) begin
			$display("FAIL MON_SDAT_LOW longest low: %0d us", r & 32'hFFFF);
			errors = errors + 1;
		end

		// S_DAT event log: starts with the console's first bit, times go up,
		// and the cart's replies show as the cart pulling the line low
		axil_read(18'h00078, r);
		if (r < 100) begin
			$display("FAIL SDAT_LOG_N: %0d", r);
			errors = errors + 1;
		end
		axil_read(18'h07000, r);
		expect32(r >> 30, 0, "SDAT_LOG[0] line fell");
		axil_read(18'h07004, r2);
		expect32(r2 >> 30, 1, "SDAT_LOG[1] line rose");
		if ((r2 & 32'h3FFFFFFF) <= (r & 32'h3FFFFFFF)) begin
			$display("FAIL SDAT_LOG times don't go up");
			errors = errors + 1;
		end
		nbad = 0;
		for (j = 0; j < 128; j = j + 1) begin
			axil_read(18'h07000 + 4 * j, r);
			if ((r >> 30) == 2)
				nbad = nbad + 1;
		end
		if (nbad == 0) begin
			$display("FAIL SDAT_LOG has no cart replies in its first 128 events");
			errors = errors + 1;
		end

		axil_write(18'h00010, 32'h8);
		axil_read(18'h0000C, r);
		expect32(r & 8, 0, "STATUS.eeprom_dirty after clear");

		if (errors == 0)
			$display("\nPASS");
		else
			$display("\nFAILED with %0d errors", errors);
		$finish;
	end

	initial begin
		#20_000_000;
		$display("TIMEOUT");
		$finish;
	end

endmodule

`default_nettype wire
