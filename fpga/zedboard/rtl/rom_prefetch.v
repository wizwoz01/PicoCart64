// SPDX-License-Identifier: BSD-2-Clause
//
// Streams cartridge ROM halfwords out of DDR through a read-only AXI4 master.
//
// On `start` the FIFO is flushed and fetching restarts at start_addr. Bursts
// end on 128-byte boundaries (max 16 beats, legal for the AXI3 HP ports, and
// never crossing 4 KB). Only one burst is outstanding; if a new address
// arrives mid-burst, the rest of that burst is discarded.
//
// The ROM image in DDR is in .z64 (big-endian) byte order. The halfword at
// N64 offset 2n is {byte[2n], byte[2n+1]}, the same as swap8() in the
// RP2040 firmware.
//
// rom_base must be 128-byte aligned.

`default_nettype none

module rom_prefetch #(
	parameter FIFO_AW = 5			// 32 x 64-bit beats
) (
	input  wire        clk,
	input  wire        rst,

	input  wire [31:0] rom_base,
	input  wire        start,
	input  wire [25:0] start_addr,
	input  wire        pop,
	output wire [15:0] data,
	output wire        valid,

	output reg  [31:0] m_axi_araddr = 32'h0,
	output reg  [7:0]  m_axi_arlen = 8'h0,
	output wire [2:0]  m_axi_arsize,
	output wire [1:0]  m_axi_arburst,
	output wire [3:0]  m_axi_arcache,
	output wire [2:0]  m_axi_arprot,
	output reg         m_axi_arvalid = 1'b0,
	input  wire        m_axi_arready,
	input  wire [63:0] m_axi_rdata,
	input  wire [1:0]  m_axi_rresp,
	input  wire        m_axi_rlast,
	input  wire        m_axi_rvalid,
	output wire        m_axi_rready
);

	localparam DEPTH = 1 << FIFO_AW;

	assign m_axi_arsize  = 3'd3;		// 8 bytes per beat
	assign m_axi_arburst = 2'b01;		// INCR
	assign m_axi_arcache = 4'b0011;		// normal, non-cacheable, bufferable
	assign m_axi_arprot  = 3'b000;
	assign m_axi_rready  = 1'b1;

	reg [63:0] mem [0:DEPTH-1];
	reg [FIFO_AW:0] wr_ptr = 0, rd_ptr = 0;
	reg [1:0] hw = 2'd0;			// halfword index within the head beat
	reg active = 1'b0;
	reg busy = 1'b0;			// a burst is outstanding (AR issued, RLAST not seen)
	reg discard = 1'b0;			// drop the rest of the outstanding burst
	reg [25:3] fetch_beat = 0;		// next 8-byte beat to request

	wire [FIFO_AW:0] count = wr_ptr - rd_ptr;
	wire [FIFO_AW:0] free = DEPTH - count;
	wire [4:0] burst_beats = 5'd16 - {1'b0, fetch_beat[6:3]};

	wire push = m_axi_rvalid && !discard && !start;

	always @(posedge clk) begin
		if (push)
			mem[wr_ptr[FIFO_AW-1:0]] <= m_axi_rdata;
	end

	always @(posedge clk) begin
		if (rst) begin
			wr_ptr <= 0;
			rd_ptr <= 0;
			hw <= 2'd0;
			active <= 1'b0;
			busy <= 1'b0;
			discard <= 1'b0;
			m_axi_arvalid <= 1'b0;
		end else begin
			if (m_axi_arvalid && m_axi_arready)
				m_axi_arvalid <= 1'b0;

			if (push)
				wr_ptr <= wr_ptr + 1'b1;

			if (m_axi_rvalid && m_axi_rlast) begin
				busy <= 1'b0;
				discard <= 1'b0;
			end

			if (pop && valid && !start) begin
				hw <= hw + 2'd1;
				if (hw == 2'd3)
					rd_ptr <= rd_ptr + 1'b1;
			end

			if (start) begin
				active <= 1'b1;
				wr_ptr <= 0;
				rd_ptr <= 0;
				hw <= start_addr[2:1];
				fetch_beat <= start_addr[25:3];
				if (busy && !(m_axi_rvalid && m_axi_rlast))
					discard <= 1'b1;
			end else if (active && !busy && free >= burst_beats) begin
				m_axi_araddr <= rom_base + {fetch_beat, 3'b000};
				m_axi_arlen <= burst_beats - 5'd1;
				m_axi_arvalid <= 1'b1;
				busy <= 1'b1;
				fetch_beat <= fetch_beat + burst_beats;
			end
		end
	end

	wire [63:0] head = mem[rd_ptr[FIFO_AW-1:0]];
	wire [15:0] hw_le = head[hw * 16 +: 16];

	assign data = {hw_le[7:0], hw_le[15:8]};
	assign valid = (count != 0);

endmodule

`default_nettype wire
