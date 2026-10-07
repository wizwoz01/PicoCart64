// SPDX-License-Identifier: BSD-2-Clause
//
// AXI4-Lite register block + 128 KB cartridge SRAM.
//
// Byte offsets:
//   0x00000  ID         RO  0x4E363443 ("N64C")
//   0x00004  CTRL       RW  [0] enable: respond on the N64 bus
//   0x00008  ROM_BASE   RW  DDR physical address of the .z64 image (128-byte aligned)
//   0x0000C  STATUS     RO  [0] N64 powered (COLD_RESET high)
//                           [1] SRAM dirty (written by the N64)
//                           [2] ROM underrun seen
//   0x00010  CLEAR      WO  [1] clear SRAM dirty, [2] clear underrun
//   0x00014  UNDERRUNS  RO  ROM words that were not ready in time
//   0x00018  LAST_ADDR  RO  last address latched from the N64
//   0x0001C  ADDR_COUNT RO  number of address phases seen
//   0x20000-0x3FFFF SRAM RW  32-bit word k holds N64 SRAM bytes 4k..4k+3 as
//                           {b0, b1, b2, b3} in [31:0] (big-endian in a LE word)

`default_nettype none

module cart_regs (
	input  wire        clk,
	input  wire        rst,

	input  wire [17:0] s_axi_awaddr,
	input  wire [2:0]  s_axi_awprot,
	input  wire        s_axi_awvalid,
	output reg         s_axi_awready = 1'b0,
	input  wire [31:0] s_axi_wdata,
	input  wire [3:0]  s_axi_wstrb,
	input  wire        s_axi_wvalid,
	output reg         s_axi_wready = 1'b0,
	output wire [1:0]  s_axi_bresp,
	output reg         s_axi_bvalid = 1'b0,
	input  wire        s_axi_bready,
	input  wire [17:0] s_axi_araddr,
	input  wire [2:0]  s_axi_arprot,
	input  wire        s_axi_arvalid,
	output reg         s_axi_arready = 1'b0,
	output reg  [31:0] s_axi_rdata = 32'h0,
	output wire [1:0]  s_axi_rresp,
	output reg         s_axi_rvalid = 1'b0,
	input  wire        s_axi_rready,

	output reg         enable = 1'b0,
	output reg  [31:0] rom_base = 32'h1000_0000,
	input  wire        n64_on,
	input  wire        underrun,
	input  wire [31:0] last_addr,
	input  wire        addr_pulse,
	output reg         sram_dirty = 1'b0,
	output reg         underrun_seen = 1'b0,

	input  wire [15:0] sram_addr,
	input  wire [15:0] sram_wdata,
	input  wire        sram_we,
	output wire [15:0] sram_rdata
);

	assign s_axi_bresp = 2'b00;
	assign s_axi_rresp = 2'b00;

	reg [31:0] underruns = 32'h0;
	reg [31:0] addr_count = 32'h0;

	// SRAM: true dual port, 32K x 32 with byte enables
	reg [31:0] sram_mem [0:32767];

	// Port A: N64, 16-bit view. Even halfword index -> [31:16].
	wire [14:0] a_addr = sram_addr[15:1];
	wire [3:0]  a_we = sram_we ? (sram_addr[0] ? 4'b0011 : 4'b1100) : 4'b0000;
	wire [31:0] a_din = {sram_wdata, sram_wdata};
	reg  [31:0] a_dout = 32'h0;
	reg         a_half = 1'b0;
	integer i;

	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (a_we[i])
				sram_mem[a_addr][i * 8 +: 8] <= a_din[i * 8 +: 8];
		a_dout <= sram_mem[a_addr];
		a_half <= sram_addr[0];
	end

	assign sram_rdata = a_half ? a_dout[15:0] : a_dout[31:16];

	// Port B: AXI. Ready is raised one cycle after valid, the access happens
	// on the handshake cycle and the response follows it.
	reg         rd_wait = 1'b0;
	reg  [17:0] rd_addr = 18'h0;
	wire        wr_go = s_axi_awvalid && s_axi_wvalid && !s_axi_awready && !s_axi_bvalid;
	wire        wr_do = s_axi_awready && s_axi_awvalid && s_axi_wready && s_axi_wvalid;
	wire        rd_go = s_axi_arvalid && !s_axi_arready && !s_axi_rvalid && !rd_wait && !wr_do;
	wire [14:0] b_addr = wr_do ? s_axi_awaddr[16:2] : s_axi_araddr[16:2];
	wire [3:0]  b_we = (wr_do && s_axi_awaddr[17]) ? s_axi_wstrb : 4'b0000;
	reg  [31:0] b_dout = 32'h0;

	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (b_we[i])
				sram_mem[b_addr][i * 8 +: 8] <= s_axi_wdata[i * 8 +: 8];
		b_dout <= sram_mem[b_addr];
	end

	always @(posedge clk) begin
		if (rst) begin
			s_axi_awready <= 1'b0;
			s_axi_wready <= 1'b0;
			s_axi_bvalid <= 1'b0;
			s_axi_arready <= 1'b0;
			s_axi_rvalid <= 1'b0;
			rd_wait <= 1'b0;
			enable <= 1'b0;
			sram_dirty <= 1'b0;
			underrun_seen <= 1'b0;
			underruns <= 32'h0;
			addr_count <= 32'h0;
		end else begin
			s_axi_awready <= 1'b0;
			s_axi_wready <= 1'b0;
			s_axi_arready <= 1'b0;

			if (underrun) begin
				underruns <= underruns + 32'd1;
				underrun_seen <= 1'b1;
			end
			if (addr_pulse)
				addr_count <= addr_count + 32'd1;

			// Write
			if (wr_go) begin
				s_axi_awready <= 1'b1;
				s_axi_wready <= 1'b1;
			end
			if (wr_do) begin
				s_axi_bvalid <= 1'b1;
				if (!s_axi_awaddr[17]) begin
					case (s_axi_awaddr[7:2])
					6'h01: enable <= s_axi_wdata[0];
					6'h02: rom_base <= {s_axi_wdata[31:7], 7'b0};
					6'h04: begin
						if (s_axi_wdata[1])
							sram_dirty <= 1'b0;
						if (s_axi_wdata[2])
							underrun_seen <= 1'b0;
					end
					default: ;
					endcase
				end
			end
			if (s_axi_bvalid && s_axi_bready)
				s_axi_bvalid <= 1'b0;

			// Set wins over clear
			if (sram_we)
				sram_dirty <= 1'b1;

			// Read: accept, wait one cycle for the SRAM, then respond
			if (rd_go) begin
				s_axi_arready <= 1'b1;
				rd_addr <= s_axi_araddr;
				rd_wait <= 1'b1;
			end
			if (rd_wait) begin
				rd_wait <= 1'b0;
				s_axi_rvalid <= 1'b1;
				if (rd_addr[17]) begin
					s_axi_rdata <= b_dout;
				end else begin
					case (rd_addr[7:2])
					6'h00: s_axi_rdata <= 32'h4E363443;
					6'h01: s_axi_rdata <= {31'h0, enable};
					6'h02: s_axi_rdata <= rom_base;
					6'h03: s_axi_rdata <= {29'h0, underrun_seen, sram_dirty, n64_on};
					6'h05: s_axi_rdata <= underruns;
					6'h06: s_axi_rdata <= last_addr;
					6'h07: s_axi_rdata <= addr_count;
					default: s_axi_rdata <= 32'h0;
					endcase
				end
			end
			if (s_axi_rvalid && s_axi_rready)
				s_axi_rvalid <= 1'b0;
		end
	end

endmodule

`default_nettype wire
