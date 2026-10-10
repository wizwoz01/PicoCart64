// SPDX-License-Identifier: BSD-2-Clause
//
// N64 PI (cartridge) bus front-end.
//
// Mirrors sw/n64_pi/n64_pi.pio:
//   - ALE_H and ALE_L rise, AD carries address[31:16]
//   - ALE_H falls (latch high half), AD switches to address[15:0]
//   - ALE_L falls (latch low half)
// Like the PIO, an address only counts after the whole sequence: ALE_H falls
// after at least 7 RCP cycles (112 ns) high, ALE_L after 14 (224 ns), and
// never while ~READ or ~WRITE is low. Shorter ALE pulses are noise picked up
// on the wires and are ignored, so they can't restart the ROM stream.
//   - each ~READ pulse: cart drives one 16-bit word, address += 2
//   - each ~WRITE pulse: N64 drives one 16-bit word, address += 2
//
// Claimed: cartridge ROM (0x10000000-0x1FBFFFFF, unless rom_off) and SRAM
// (0x08000000-0x080FFFFF), and with dd_enable the 64DD: its registers and
// buffers (0x05000000-0x050005FF, dd_asic) and its IPL ROM (0x06000000-
// 0x063FFFFF, streamed like the cartridge ROM from 64 MB above it in DDR).
// Every other address is never driven.
//
// AD is latched from the last synchronized sample taken while the strobe was
// still at its old level, so the value is correct even if the N64 changes AD
// right after the strobe edge.
//
// A bus monitor records what the N64 did since it was last turned on, whether
// or not the cart is enabled, and traces its first 1024 addresses, so the
// loader can tell where a boot stops and which wire is at fault.

`default_nettype none

module n64_pi_bus (
	input  wire        clk,
	input  wire        rst,
	input  wire        enable,
	input  wire        dd_enable,		// answer as the 64DD
	input  wire        rom_off,		// no cartridge: leave 0x10000000-0x1FBFFFFF alone

	// N64 pins
	input  wire [15:0] ad_i,
	(* IOB = "TRUE" *) output reg [15:0] ad_o = 16'h0000,
	(* IOB = "TRUE" *) output reg [15:0] ad_t = 16'hFFFF,	// 1 = high-Z
	input  wire        aleh,
	input  wire        alel,
	input  wire        read_n,
	input  wire        write_n,
	input  wire        cold_reset,

	// ROM word stream (rom_prefetch)
	output reg         rom_start = 1'b0,
	output reg  [26:0] rom_addr = 27'd0,	// bit 26: 64DD IPL ROM
	input  wire [15:0] rom_data,
	input  wire        rom_valid,
	output reg         rom_pop = 1'b0,

	// SRAM port (16-bit word index, 1 cycle read latency)
	output wire [15:0] sram_addr,
	input  wire [15:0] sram_rdata,
	output reg  [15:0] sram_wdata = 16'h0000,
	output reg         sram_we = 1'b0,

	// 64DD registers and buffers (dd_asic): halfword index from 0x05000000
	output reg  [9:0]  dd_idx = 10'd0,
	input  wire [15:0] dd_rdata,
	output reg         dd_rd = 1'b0,		// the halfword at dd_ev_idx was read
	output reg         dd_wr = 1'b0,		// write dd_wdata at dd_ev_idx
	output reg  [9:0]  dd_ev_idx = 10'd0,
	output reg  [15:0] dd_wdata = 16'h0000,

	// Status
	output wire        n64_on,
	output reg  [31:0] last_addr = 32'h0,
	output reg         addr_pulse = 1'b0,
	output reg         rom_access = 1'b0,
	output reg         sram_access = 1'b0,
	output reg         underrun = 1'b0,

	// Bus monitor, cleared when the N64 is turned on. Counts saturate at 0xFFFF.
	output reg  [31:0] mon_ale = 32'h0,		// {ALE_L falls, ALE_H falls}
	output reg  [31:0] mon_rw = 32'h0,		// {~WRITE pulses, ~READ pulses}
	output reg  [31:0] mon_first_addr = 32'h0,	// first address latched
	output reg  [31:0] mon_first_data = 32'h0,	// first two ROM halfwords, read back from AD
	output reg  [31:0] mon_rom = 32'h0,		// {AD differed from what the cart drove, ROM halfwords served}
	output reg  [31:0] mon_ad_or = 32'h0,		// {OR of address high halves, OR of low halves}
	output reg  [31:0] mon_glitch = 32'h0,		// {underruns, ALE pulses ignored}
	output reg  [31:0] mon_odd = 32'h0,		// {~READ low pulses under 150 ns, transfers with an odd number of reads}
	output reg  [15:0] mon_noise_n = 16'h0,		// noise events, the first 16 are in the noise log
	output reg  [31:0] mon_xfer_n = 32'h0,		// transfers finished since power on (see the transfer log)
	output reg  [31:0] mon_xfer_addr = 32'h0,	// the transfer in progress: address
	output reg  [31:0] mon_xfer_rw = 32'h0,		// {writes, reads}
	output reg  [31:0] mon_xfer_sum = 32'h0,	// sum of the halfwords on AD
	output wire [22:0] pins,			// synchronized levels, numbered like the Pico GPIOs

	// Trace of the first 1024 address phases since power on
	input  wire [9:0]  trace_raddr,
	output reg  [31:0] trace_a = 32'h0,		// {high half, low half} as latched
	output reg  [31:0] trace_b = 32'h0,		// low half sampled {0-10 ns, 30-40 ns} after ALE_L fell
	output reg  [31:0] noise_q = 32'h0,		// noise log entry trace_raddr[3:0]: {type, width in clocks, us since power on}
	output reg  [31:0] xlog_q			// transfer log word trace_raddr: entry [9:2], field [1:0]
);

	localparam R_NONE = 2'd0;
	localparam R_ROM  = 2'd1;
	localparam R_SRAM = 2'd2;
	localparam R_DD   = 2'd3;

	wire aleh_s, aleh_q, alel_s, alel_q, rd_s, rd_q, wr_s, wr_q, rst_s, rst_q;

	n64_in_filter #(.INIT(1'b0)) f_aleh (.clk(clk), .d(aleh),       .s(aleh_s), .q(aleh_q));
	n64_in_filter #(.INIT(1'b0)) f_alel (.clk(clk), .d(alel),       .s(alel_s), .q(alel_q));
	// ~READ and ~WRITE need 40 ns at a new level: between words the RCP keeps
	// them high for (RLS + 1) cycles, 64 ns with the usual header (RLS = 3)
	n64_in_filter #(.INIT(1'b1), .N(4)) f_rd (.clk(clk), .d(read_n),  .s(rd_s),   .q(rd_q));
	n64_in_filter #(.INIT(1'b1), .N(4)) f_wr (.clk(clk), .d(write_n), .s(wr_s),   .q(wr_q));
	n64_in_filter #(.INIT(1'b0)) f_rst  (.clk(clk), .d(cold_reset), .s(rst_s),  .q(rst_q));

	reg aleh_d = 1'b0, alel_d = 1'b0, rd_d = 1'b1, wr_d = 1'b1, rd_s_d = 1'b1;

	always @(posedge clk) begin
		aleh_d <= aleh_q;
		alel_d <= alel_q;
		rd_d   <= rd_q;
		wr_d   <= wr_q;
		rd_s_d <= rd_s;
	end

	wire aleh_fall = aleh_d & ~aleh_q;
	wire alel_fall = alel_d & ~alel_q;
	wire rd_rise   = ~rd_d & rd_q;
	wire wr_rise   = ~wr_d & wr_q;

	// Clocks each ALE line has been high; a fall only counts after a real pulse
	localparam ALEH_MIN = 8'd8;		// 80 ns
	localparam ALEL_MIN = 8'd16;		// 160 ns
	reg [7:0] aleh_hi = 8'd0, alel_hi = 8'd0;

	always @(posedge clk) begin
		aleh_hi <= !aleh_q ? 8'd0 : aleh_hi + {7'd0, aleh_hi != 8'hFF};
		alel_hi <= !alel_q ? 8'd0 : alel_hi + {7'd0, alel_hi != 8'hFF};
	end

	wire strobes_idle = rd_q & wr_q;
	wire aleh_ok = aleh_fall && aleh_hi >= ALEH_MIN && strobes_idle;
	wire alel_ok = alel_fall && alel_hi >= ALEL_MIN && strobes_idle;
	wire ale_glitch = (aleh_fall && !aleh_ok) || (alel_fall && !alel_ok);

	// AD capture, aligned with the *_s strobe samples (both 2 FFs deep)
	(* ASYNC_REG = "TRUE" *) reg [15:0] ad_m = 16'h0;
	(* ASYNC_REG = "TRUE" *) reg [15:0] ad_s = 16'h0;
	reg [15:0] hold_h = 16'h0, hold_l = 16'h0, hold_w = 16'h0, hold_r = 16'h0;

	always @(posedge clk) begin
		ad_m <= ad_i;
		ad_s <= ad_m;
		if (aleh_s)
			hold_h <= ad_s;
		if (alel_s)
			hold_l <= ad_s;
		if (!wr_s)
			hold_w <= ad_s;
		if (!rd_s)
			hold_r <= ad_s;
	end

	assign pins = {rst_s, 2'b00, rd_s, wr_s, aleh_s, alel_s, ad_s};

	// Same mapping as sram_resolve_address_shifted() in sw/picocart64_v1/sram.h:
	// 1Mbit contiguous and banked (bits [19:18]) modes.
	function [15:0] sram_resolve(input [31:0] a);
		sram_resolve = {a[16:15] | a[19:18], a[14:1]};
	endfunction

	wire        active = enable & rst_q;
	wire [31:0] addr_full = {last_addr[31:16], hold_l};

	reg  [1:0]  region = R_NONE;
	reg  [15:0] sram_idx = 16'h0;
	reg  [15:0] sram_widx = 16'h0;

	assign sram_addr = sram_we ? sram_widx : sram_idx;
	assign n64_on = rst_q;

	always @(posedge clk) begin
		rom_start   <= 1'b0;
		rom_pop     <= 1'b0;
		sram_we     <= 1'b0;
		dd_rd       <= 1'b0;
		dd_wr       <= 1'b0;
		addr_pulse  <= 1'b0;
		rom_access  <= 1'b0;
		sram_access <= 1'b0;
		underrun    <= 1'b0;

		if (rst || !active) begin
			region <= R_NONE;
		end else begin
			if (aleh_ok)
				last_addr[31:16] <= hold_h;

			if (alel_ok) begin
				last_addr[15:0] <= hold_l;
				addr_pulse <= 1'b1;
				if (addr_full >= 32'h1000_0000 && addr_full <= 32'h1FBF_FFFF && !rom_off) begin
					region <= R_ROM;
					rom_start <= 1'b1;
					rom_addr <= {1'b0, addr_full[25:0]};
					rom_access <= 1'b1;
				end else if (dd_enable && addr_full[31:22] == 10'h018) begin
					region <= R_ROM;		// 64DD IPL ROM
					rom_start <= 1'b1;
					rom_addr <= {5'b10000, addr_full[21:0]};
					rom_access <= 1'b1;
				end else if (dd_enable && addr_full[31:11] == 21'h00A000 && addr_full[10:9] != 2'b11) begin
					region <= R_DD;			// 0x05000000-0x050005FF
					dd_idx <= addr_full[10:1];
				end else if (addr_full[31:20] == 12'h080) begin
					region <= R_SRAM;
					sram_idx <= sram_resolve(addr_full);
					sram_access <= 1'b1;
				end else begin
					region <= R_NONE;
				end
			end

			// Data had to be on the pins when ~READ went high
			if (region == R_ROM && ~rd_s_d && rd_s && !rom_valid)
				underrun <= 1'b1;

			if (rd_rise) begin
				if (region == R_ROM)
					rom_pop <= 1'b1;
				else if (region == R_SRAM)
					sram_idx <= sram_idx + 16'd1;
				else if (region == R_DD) begin
					dd_rd <= 1'b1;
					dd_ev_idx <= dd_idx;
					dd_idx <= dd_idx + 10'd1;
				end
			end

			if (wr_rise) begin
				if (region == R_ROM) begin
					// Writes to ROM are ignored but still advance the address
					rom_pop <= 1'b1;
				end else if (region == R_SRAM) begin
					sram_we <= 1'b1;
					sram_widx <= sram_idx;
					sram_wdata <= hold_w;
					sram_idx <= sram_idx + 16'd1;
				end else if (region == R_DD) begin
					dd_wr <= 1'b1;
					dd_ev_idx <= dd_idx;
					dd_wdata <= hold_w;
					dd_idx <= dd_idx + 10'd1;
				end
			end
		end
	end

	// Drive AD while ~READ is low in a claimed region. Turn on from the filtered
	// strobe, turn off from the raw synchronized sample to release the bus early.
	wire drive = active && (region != R_NONE) && !rd_q && !rd_s;

	always @(posedge clk) begin
		ad_o <= (region == R_ROM) ? rom_data : (region == R_DD) ? dd_rdata : sram_rdata;
		ad_t <= {16{~drive}};
	end

	// Bus monitor. Every boot starts by reading the ROM header at 0x10000000,
	// so the first address and data show whether the wiring works.
	function [15:0] sat_inc(input [15:0] v);
		sat_inc = (v == 16'hFFFF) ? v : v + 16'd1;
	endfunction

	reg        on_d = 1'b0;
	reg [15:0] mon_hi = 16'h0;
	reg        mon_addr_seen = 1'b0;
	reg [1:0]  mon_data_n = 2'd0;

	always @(posedge clk) begin
		on_d <= rst_q;
		if (rst || (rst_q && !on_d)) begin
			mon_ale <= 32'h0;
			mon_rw <= 32'h0;
			mon_first_addr <= 32'h0;
			mon_first_data <= 32'h0;
			mon_rom <= 32'h0;
			mon_ad_or <= 32'h0;
			mon_glitch <= 32'h0;
			mon_hi <= 16'h0;
			mon_addr_seen <= 1'b0;
			mon_data_n <= 2'd0;
		end else if (rst_q) begin
			if (ale_glitch)
				mon_glitch[15:0] <= sat_inc(mon_glitch[15:0]);
			if (underrun)
				mon_glitch[31:16] <= sat_inc(mon_glitch[31:16]);
			if (aleh_ok) begin
				mon_hi <= hold_h;
				mon_ale[15:0] <= sat_inc(mon_ale[15:0]);
				mon_ad_or[31:16] <= mon_ad_or[31:16] | hold_h;
			end
			if (alel_ok) begin
				mon_ale[31:16] <= sat_inc(mon_ale[31:16]);
				mon_ad_or[15:0] <= mon_ad_or[15:0] | hold_l;
				if (!mon_addr_seen) begin
					mon_addr_seen <= 1'b1;
					mon_first_addr <= {mon_hi, hold_l};
				end
			end
			if (rd_rise)
				mon_rw[15:0] <= sat_inc(mon_rw[15:0]);
			if (wr_rise)
				mon_rw[31:16] <= sat_inc(mon_rw[31:16]);

			// The halfword on the pins when ~READ rose, against the one driven
			if (rd_rise && active && region == R_ROM) begin
				mon_rom[15:0] <= sat_inc(mon_rom[15:0]);
				if (hold_r != ad_o)
					mon_rom[31:16] <= sat_inc(mon_rom[31:16]);
				if (mon_data_n == 2'd0)
					mon_first_data[31:16] <= hold_r;
				if (mon_data_n == 2'd1)
					mon_first_data[15:0] <= hold_r;
				if (mon_data_n != 2'd2)
					mon_data_n <= mon_data_n + 2'd1;
			end
		end
	end

	// Noise log. Types: 1 short ALE_H pulse, 2 short ALE_L pulse (both ignored),
	// 3 a transfer read an odd number of halfwords (the boot only reads whole
	// words and pages, so a ~READ glitch), 4 a ~READ low pulse under 150 ns.
	localparam N_ALEH = 4'd1, N_ALEL = 4'd2, N_ODD = 4'd3, N_SHORT = 4'd4;
	reg [31:0] noise_mem [0:15];
	reg [6:0]  us_div = 7'd0;
	reg [19:0] mon_us = 20'd0;
	reg [15:0] xfer_reads = 16'h0;
	reg        xfer_open = 1'b0;
	reg [7:0]  rd_lo = 8'd0;
	reg [3:0]  ev_type;
	reg [7:0]  ev_width;

	always @(*) begin
		ev_type = 4'd0;
		ev_width = 8'd0;
		if (aleh_fall && !aleh_ok) begin
			ev_type = N_ALEH;
			ev_width = aleh_hi;
		end else if (alel_fall && !alel_ok) begin
			ev_type = N_ALEL;
			ev_width = alel_hi;
		end else if (alel_ok && xfer_open && xfer_reads[0]) begin
			ev_type = N_ODD;
			ev_width = xfer_reads[7:0];
		end else if (rd_rise && rd_lo < 8'd15) begin
			ev_type = N_SHORT;
			ev_width = rd_lo;
		end
	end

	always @(posedge clk) begin
		rd_lo <= rd_q ? 8'd0 : rd_lo + {7'd0, rd_lo != 8'hFF};
		if (rst || (rst_q && !on_d)) begin
			us_div <= 7'd0;
			mon_us <= 20'd0;
			xfer_reads <= 16'h0;
			xfer_open <= 1'b0;
			mon_odd <= 32'h0;
			mon_noise_n <= 16'h0;
		end else if (rst_q) begin
			us_div <= (us_div == 7'd99) ? 7'd0 : us_div + 7'd1;
			if (us_div == 7'd99 && mon_us != 20'hFFFFF)
				mon_us <= mon_us + 20'd1;
			if (alel_ok) begin
				if (xfer_open && xfer_reads[0])
					mon_odd[15:0] <= sat_inc(mon_odd[15:0]);
				xfer_reads <= 16'h0;
				xfer_open <= 1'b1;
			end else if (rd_rise) begin
				xfer_reads <= xfer_reads + 16'd1;
			end
			if (rd_rise && rd_lo < 8'd15)
				mon_odd[31:16] <= sat_inc(mon_odd[31:16]);
			if (ev_type != 4'd0) begin
				if (mon_noise_n < 16'd16)
					noise_mem[mon_noise_n[3:0]] <= {ev_type, ev_width, mon_us};
				mon_noise_n <= sat_inc(mon_noise_n);
			end
		end
		noise_q <= noise_mem[trace_raddr[3:0]];
	end

	// Transfer log: the last 256 transfers (an address phase and the reads and
	// writes after it) with the sum of the halfwords that were on AD, so the
	// loader can compare what the N64 got with the ROM. Fields: address,
	// {writes, reads}, sum, microseconds since power on.
	reg [31:0] xlog_addr [0:255];
	reg [31:0] xlog_rw [0:255];
	reg [31:0] xlog_sum [0:255];
	reg [31:0] xlog_time [0:255];
	reg [19:0] xfer_us = 20'd0;
	reg [31:0] xq_addr = 32'h0, xq_rw = 32'h0, xq_sum = 32'h0, xq_time = 32'h0;
	reg [1:0]  xq_sel = 2'd0;
	wire       xlog_we = rst_q && alel_ok && xfer_open;

	always @(posedge clk) begin
		if (rst || (rst_q && !on_d)) begin
			mon_xfer_n <= 32'h0;
			mon_xfer_addr <= 32'h0;
			mon_xfer_rw <= 32'h0;
			mon_xfer_sum <= 32'h0;
			xfer_us <= 20'd0;
		end else if (rst_q) begin
			if (alel_ok) begin
				if (xfer_open)
					mon_xfer_n <= mon_xfer_n + 32'd1;
				mon_xfer_addr <= {mon_hi, hold_l};
				mon_xfer_rw <= 32'h0;
				mon_xfer_sum <= 32'h0;
				xfer_us <= mon_us;
			end else if (rd_rise) begin
				mon_xfer_rw[15:0] <= sat_inc(mon_xfer_rw[15:0]);
				mon_xfer_sum <= mon_xfer_sum + {16'h0, hold_r};
			end else if (wr_rise) begin
				mon_xfer_rw[31:16] <= sat_inc(mon_xfer_rw[31:16]);
				mon_xfer_sum <= mon_xfer_sum + {16'h0, hold_w};
			end
		end
	end

	always @(posedge clk) begin
		if (xlog_we) begin
			xlog_addr[mon_xfer_n[7:0]] <= mon_xfer_addr;
			xlog_rw[mon_xfer_n[7:0]] <= mon_xfer_rw;
			xlog_sum[mon_xfer_n[7:0]] <= mon_xfer_sum;
			xlog_time[mon_xfer_n[7:0]] <= {12'h0, xfer_us};
		end
		xq_addr <= xlog_addr[trace_raddr[9:2]];
		xq_rw <= xlog_rw[trace_raddr[9:2]];
		xq_sum <= xlog_sum[trace_raddr[9:2]];
		xq_time <= xlog_time[trace_raddr[9:2]];
		xq_sel <= trace_raddr[1:0];
	end

	always @(*) begin
		case (xq_sel)
		2'd0: xlog_q = xq_addr;
		2'd1: xlog_q = xq_rw;
		2'd2: xlog_q = xq_sum;
		default: xlog_q = xq_time;
		endcase
	end

	// Address trace. Besides the latched address it keeps AD from the first
	// sample after ALE_L fell and from the cycle the fall is acted on, to
	// tell wiring faults from AD changing around the strobe.
	reg [31:0] trace_mem_a [0:1023];
	reg [31:0] trace_mem_b [0:1023];
	reg [10:0] trace_n = 11'd0;
	reg [15:0] ad_after = 16'h0;
	reg        alel_s_d = 1'b0;
	wire       trace_we = rst_q && alel_ok && !trace_n[10];

	always @(posedge clk) begin
		alel_s_d <= alel_s;
		if (alel_s_d && !alel_s)
			ad_after <= ad_s;
		if (rst || (rst_q && !on_d))
			trace_n <= 11'd0;
		else if (trace_we)
			trace_n <= trace_n + 11'd1;
	end

	always @(posedge clk) begin
		if (trace_we) begin
			trace_mem_a[trace_n[9:0]] <= {mon_hi, hold_l};
			trace_mem_b[trace_n[9:0]] <= {ad_after, ad_s};
		end
		trace_a <= trace_mem_a[trace_raddr];
		trace_b <= trace_mem_b[trace_raddr];
	end

endmodule

`default_nettype wire
