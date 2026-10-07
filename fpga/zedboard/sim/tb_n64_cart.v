// SPDX-License-Identifier: BSD-2-Clause
//
// Testbench for n64_cart_top.
//
// Models:
//   - N64 PI bus master with real PI timings (62.5 MHz RCP clock)
//   - DDR behind the HP port: AXI slave with random latency and rvalid gaps
//   - ARM accesses through AXI4-Lite
// Checks data, bus contention, AXI burst legality and underruns.

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

	task axil_write(input [17:0] a, input [31:0] d);
		begin
			@(posedge clk);
			awaddr <= a; awvalid <= 1; wdata <= d; wstrb <= 4'hF; wvalid <= 1; bready <= 1;
			@(posedge clk);
			while (!(awready && wready)) @(posedge clk);
			awvalid <= 0; wvalid <= 0;
			while (!bvalid) @(posedge clk);
			@(posedge clk);
			bready <= 0;
		end
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
	// DUT
	// ------------------------------------------------------------------
	wire [7:0] led;

	n64_cart_top dut (
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
		.led(led)
	);

	// ------------------------------------------------------------------
	// N64 PI master tasks
	// ------------------------------------------------------------------

	// Address phase. AD changes only 3 ns after each ALE edge, to check that
	// the FPGA latches the value from before the edge.
	task pi_addr(input [31:0] a);
		begin
			n64_drive = 1;
			n64_ad = a[31:16];
			aleh = 1;
			alel = 1;
			#(4 * T_PI);
			aleh = 0;
			#3 n64_ad = a[15:0];
			#(4 * T_PI - 3);
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
	integer j;

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

		axil_write(18'h00008, ROM_BASE);
		axil_write(18'h00004, 32'h1);
		axil_read(18'h0000C, r);
		expect32(r & 1, 1, "STATUS.n64_on");

		// ROM header and IPL3 area, like the PIF/IPL3 boot reads
		check_rom(32'h1000_0000, 32, D1_LAT);
		check_rom(32'h1000_0040, 512, D1_LAT);
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
