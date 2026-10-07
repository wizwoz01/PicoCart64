// SPDX-License-Identifier: BSD-2-Clause
//
// N64 PI (cartridge) bus front-end.
//
// Mirrors sw/n64_pi/n64_pi.pio:
//   - ALE_H and ALE_L rise, AD carries address[31:16]
//   - ALE_H falls (latch high half), AD switches to address[15:0]
//   - ALE_L falls (latch low half)
//   - each ~READ pulse: cart drives one 16-bit word, address += 2
//   - each ~WRITE pulse: N64 drives one 16-bit word, address += 2
//
// Only cartridge ROM (0x10000000-0x1FBFFFFF) and SRAM (0x08000000-0x080FFFFF)
// are claimed. Every other address (64DD etc.) is never driven.
//
// AD is latched from the last synchronized sample taken while the strobe was
// still at its old level, so the value is correct even if the N64 changes AD
// right after the strobe edge.

`default_nettype none

module n64_pi_bus (
	input  wire        clk,
	input  wire        rst,
	input  wire        enable,

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
	output reg  [25:0] rom_addr = 26'd0,
	input  wire [15:0] rom_data,
	input  wire        rom_valid,
	output reg         rom_pop = 1'b0,

	// SRAM port (16-bit word index, 1 cycle read latency)
	output wire [15:0] sram_addr,
	input  wire [15:0] sram_rdata,
	output reg  [15:0] sram_wdata = 16'h0000,
	output reg         sram_we = 1'b0,

	// Status
	output wire        n64_on,
	output reg  [31:0] last_addr = 32'h0,
	output reg         addr_pulse = 1'b0,
	output reg         rom_access = 1'b0,
	output reg         sram_access = 1'b0,
	output reg         underrun = 1'b0
);

	localparam R_NONE = 2'd0;
	localparam R_ROM  = 2'd1;
	localparam R_SRAM = 2'd2;

	wire aleh_s, aleh_q, alel_s, alel_q, rd_s, rd_q, wr_s, wr_q, rst_s, rst_q;

	n64_in_filter #(.INIT(1'b0)) f_aleh (.clk(clk), .d(aleh),       .s(aleh_s), .q(aleh_q));
	n64_in_filter #(.INIT(1'b0)) f_alel (.clk(clk), .d(alel),       .s(alel_s), .q(alel_q));
	n64_in_filter #(.INIT(1'b1)) f_rd   (.clk(clk), .d(read_n),     .s(rd_s),   .q(rd_q));
	n64_in_filter #(.INIT(1'b1)) f_wr   (.clk(clk), .d(write_n),    .s(wr_s),   .q(wr_q));
	n64_in_filter #(.INIT(1'b0)) f_rst  (.clk(clk), .d(cold_reset), .s(rst_s),  .q(rst_q));

	reg aleh_d = 1'b0, alel_d = 1'b0, rd_d = 1'b1, wr_d = 1'b1, rd_s_d = 1'b1;

	always @(posedge clk) begin
		aleh_d <= aleh_q;
		alel_d <= alel_q;
		rd_d   <= rd_q;
		wr_d   <= wr_q;
		rd_s_d <= rd_s;
	end

	wire aleh_rise = ~aleh_d & aleh_q;
	wire aleh_fall = aleh_d & ~aleh_q;
	wire alel_fall = alel_d & ~alel_q;
	wire rd_rise   = ~rd_d & rd_q;
	wire wr_rise   = ~wr_d & wr_q;

	// AD capture, aligned with the *_s strobe samples (both 2 FFs deep)
	(* ASYNC_REG = "TRUE" *) reg [15:0] ad_m = 16'h0;
	(* ASYNC_REG = "TRUE" *) reg [15:0] ad_s = 16'h0;
	reg [15:0] hold_h = 16'h0, hold_l = 16'h0, hold_w = 16'h0;

	always @(posedge clk) begin
		ad_m <= ad_i;
		ad_s <= ad_m;
		if (aleh_s)
			hold_h <= ad_s;
		if (alel_s)
			hold_l <= ad_s;
		if (!wr_s)
			hold_w <= ad_s;
	end

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
		addr_pulse  <= 1'b0;
		rom_access  <= 1'b0;
		sram_access <= 1'b0;
		underrun    <= 1'b0;

		if (rst || !active) begin
			region <= R_NONE;
		end else begin
			if (aleh_rise)
				region <= R_NONE;

			if (aleh_fall)
				last_addr[31:16] <= hold_h;

			if (alel_fall) begin
				last_addr[15:0] <= hold_l;
				addr_pulse <= 1'b1;
				if (addr_full >= 32'h1000_0000 && addr_full <= 32'h1FBF_FFFF) begin
					region <= R_ROM;
					rom_start <= 1'b1;
					rom_addr <= addr_full[25:0];
					rom_access <= 1'b1;
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
				end
			end
		end
	end

	// Drive AD while ~READ is low in a claimed region. Turn on from the filtered
	// strobe, turn off from the raw synchronized sample to release the bus early.
	wire drive = active && (region != R_NONE) && !rd_q && !rd_s;

	always @(posedge clk) begin
		ad_o <= (region == R_ROM) ? rom_data : sram_rdata;
		ad_t <= {16{~drive}};
	end

endmodule

`default_nettype wire
