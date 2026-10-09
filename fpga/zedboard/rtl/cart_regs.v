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
//                           [3] EEPROM dirty (written by the N64)
//   0x00010  CLEAR      WO  [1] clear SRAM dirty, [2] clear underrun,
//                           [3] clear EEPROM dirty
//   0x00014  UNDERRUNS  RO  ROM words that were not ready in time
//   0x00018  LAST_ADDR  RO  last address latched from the N64
//   0x0001C  ADDR_COUNT RO  number of address phases seen
//   0x00020  BUTTONS    RW  [4:0] debounced state, [12:8] pressed since cleared
//                           (write 1 to clear). Bits: C, D, L, R, U
//   0x00024  OLED_CTRL  RW  [0] VDD on, [1] VBAT on, [2] release RES#
//                           [8] busy (RO)
//   0x00028  OLED_DATA  WO  send byte [7:0] with D/C = [8] (ignored while busy)
//   0x0002C  OLED_FLUSH WO  send the frame buffer (ignored while busy)
//   0x00030  CIC        RW  [2:0] CIC type for the Pico, open drain: 1 = released
//                           (reads 1 through the Pico's pull-up), 0 = pulled low
//   0x00034  EEPROM     RW  [0] answer on S_DAT, [1] 16 Kbit (else 4 Kbit)
//   Bus monitor, cleared when the N64 is turned on, counted even while the
//   cart is disabled. Counts saturate at 0xFFFF.
//   0x00038  MON_ALE    RO  [15:0] ALE_H falls, [31:16] ALE_L falls
//   0x0003C  MON_RW     RO  [15:0] ~READ pulses, [31:16] ~WRITE pulses
//   0x00040  MON_FIRST  RO  first address latched
//   0x00044  MON_DATA   RO  first two ROM halfwords as read back from AD,
//                           first one in [31:16]
//   0x00048  MON_ROM    RO  [15:0] ROM halfwords served, [31:16] of those
//                           where AD differed from what the cart drove
//   0x0004C  MON_AD_OR  RO  [15:0] OR of all address low halves, [31:16] high
//   0x00050  PINS       RO  live levels: [15:0] AD, [16] ALE_L, [17] ALE_H,
//                           [18] ~WRITE, [19] ~READ, [22] COLD_RESET, [23] S_DAT
//   0x00054  MON_GLITCH RO  [15:0] ALE pulses ignored as noise (too short, or
//                           during ~READ/~WRITE), [31:16] underruns
//   0x00058  MON_ODD    RO  [15:0] transfers that read an odd number of
//                           halfwords, [31:16] ~READ low pulses under 150 ns
//   0x0005C  MON_NOISE  RO  noise events since power on (see the noise log)
//   0x00060  MON_XFER_N RO  transfers finished since power on (see XFER)
//   0x00064  MON_XFER_ADDR RO the transfer in progress: address,
//   0x00068  MON_XFER_RW   RO   {writes, reads},
//   0x0006C  MON_XFER_SUM  RO   sum of the halfwords on AD
//   0x00070  MON_SDAT   RO  [15:0] S_DAT falls, [31:16] EEPROM commands answered
//   0x00074  MON_SDAT_LOW RO [15:0] longest time S_DAT stayed low, [31:16] how
//                           long it has been low now, both in microseconds
//   0x00078  SDAT_LOG_N RO  events in the S_DAT log (up to 1024)
//   0x01000-0x011FF OLED_FB WO 512 bytes, byte 4k+j of the SSD1306 horizontal
//                           addressing order in word k bits [8j+7:8j]
//   0x02000-0x027FF EEPROM RW 2 KB, word k = EEPROM bytes 4k..4k+3 as {b0, b1, b2, b3}
//   0x03000-0x03FFF TRACE_A RO first 1024 address phases since power on (as
//                           many as MON_ALE counted): {high half, low half}
//   0x04000-0x04FFF TRACE_B RO the low half of each, sampled again 0-10 ns
//                           [31:16] and 30-40 ns [15:0] after ALE_L fell
//   0x05000-0x0503F NOISE  RO the first 16 noise events: [31:28] type (1 short
//                           ALE_H, 2 short ALE_L, 3 odd read count, 4 short
//                           ~READ), [27:20] width in clocks or the read count,
//                           [19:0] microseconds since power on
//   0x06000-0x06FFF XFER   RO the last 256 finished transfers, transfer n in
//                           entry n % 256, 4 words each: address, {writes,
//                           reads}, sum of the halfwords on AD, microseconds
//   0x07000-0x07FFF SDAT_LOG RO the first 1024 S_DAT events since power on:
//                           [31:30] 0 line fell, 1 line rose, 2 cart pulls low,
//                           3 cart lets go; [29:0] time in 40 ns units
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
	output wire [15:0] sram_rdata,

	input  wire [4:0]  btn_state,
	input  wire [4:0]  btn_pressed,

	output reg  [2:0]  oled_ctrl = 3'b000,
	input  wire        oled_busy,
	output reg         oled_byte_go = 1'b0,
	output reg  [8:0]  oled_byte = 9'h0,
	output reg         oled_flush_go = 1'b0,
	output wire [6:0]  oled_fb_addr,
	output wire [31:0] oled_fb_wdata,
	output wire [3:0]  oled_fb_we,

	output reg  [2:0]  cic_type = 3'b111,
	output reg         eep_enable = 1'b0,
	output reg         eep_16k = 1'b0,
	input  wire        eep_written,
	output reg         eep_dirty = 1'b0,
	output wire [8:0]  eep_addr,
	output wire [31:0] eep_wdata,
	output wire [3:0]  eep_we,
	input  wire [31:0] eep_rdata,

	input  wire [31:0] mon_ale,
	input  wire [31:0] mon_rw,
	input  wire [31:0] mon_first_addr,
	input  wire [31:0] mon_first_data,
	input  wire [31:0] mon_rom,
	input  wire [31:0] mon_ad_or,
	input  wire [31:0] mon_glitch,
	input  wire [31:0] mon_odd,
	input  wire [15:0] mon_noise_n,
	input  wire [31:0] mon_xfer_n,
	input  wire [31:0] mon_xfer_addr,
	input  wire [31:0] mon_xfer_rw,
	input  wire [31:0] mon_xfer_sum,
	input  wire [31:0] mon_sdat,
	input  wire [31:0] mon_sdat_low,
	input  wire [10:0] sdat_log_n,
	input  wire [31:0] sdat_log_q,
	input  wire [31:0] pins,

	output wire [9:0]  trace_raddr,
	input  wire [31:0] trace_a,
	input  wire [31:0] trace_b,
	input  wire [31:0] noise_q,
	input  wire [31:0] xlog_q
);

	assign s_axi_bresp = 2'b00;
	assign s_axi_rresp = 2'b00;

	reg [31:0] underruns = 32'h0;
	reg [31:0] addr_count = 32'h0;
	reg [4:0]  btn_events = 5'h0;

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

	// Below 0x20000: registers, OLED frame buffer, EEPROM, address trace
	wire        reg_sel = !s_axi_awaddr[17] && s_axi_awaddr[14:12] == 3'b000;
	wire        fb_sel = !s_axi_awaddr[17] && s_axi_awaddr[14:12] == 3'b001;
	wire        eep_sel = !s_axi_awaddr[17] && s_axi_awaddr[14:12] == 3'b010;
	wire [4:0]  btn_clear = (wr_do && reg_sel && s_axi_awaddr[7:2] == 6'h08) ? s_axi_wdata[12:8] : 5'h0;

	// OLED frame buffer at 0x01000 (lives in oled_spi)
	assign oled_fb_addr = s_axi_awaddr[8:2];
	assign oled_fb_wdata = s_axi_wdata;
	assign oled_fb_we = (wr_do && fb_sel) ? s_axi_wstrb : 4'b0000;

	// EEPROM at 0x02000 (lives in joybus_eeprom), read like the SRAM
	assign eep_addr = wr_do ? s_axi_awaddr[10:2] : s_axi_araddr[10:2];
	assign eep_wdata = s_axi_wdata;
	assign eep_we = (wr_do && eep_sel) ? s_axi_wstrb : 4'b0000;

	// Address trace at 0x03000 and 0x04000 (lives in n64_pi_bus), read only
	assign trace_raddr = s_axi_araddr[11:2];

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
			btn_events <= 5'h0;
			cic_type <= 3'b111;
			eep_enable <= 1'b0;
			eep_16k <= 1'b0;
			eep_dirty <= 1'b0;
			oled_ctrl <= 3'b000;
			oled_byte_go <= 1'b0;
			oled_flush_go <= 1'b0;
		end else begin
			s_axi_awready <= 1'b0;
			s_axi_wready <= 1'b0;
			s_axi_arready <= 1'b0;
			oled_byte_go <= 1'b0;
			oled_flush_go <= 1'b0;

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
				if (reg_sel) begin
					case (s_axi_awaddr[7:2])
					6'h01: enable <= s_axi_wdata[0];
					6'h02: rom_base <= {s_axi_wdata[31:7], 7'b0};
					6'h04: begin
						if (s_axi_wdata[1])
							sram_dirty <= 1'b0;
						if (s_axi_wdata[2])
							underrun_seen <= 1'b0;
						if (s_axi_wdata[3])
							eep_dirty <= 1'b0;
					end
					6'h09: oled_ctrl <= s_axi_wdata[2:0];
					6'h0A: begin
						oled_byte <= s_axi_wdata[8:0];
						oled_byte_go <= 1'b1;
					end
					6'h0B: oled_flush_go <= 1'b1;
					6'h0C: cic_type <= s_axi_wdata[2:0];
					6'h0D: begin
						eep_enable <= s_axi_wdata[0];
						eep_16k <= s_axi_wdata[1];
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
			if (eep_written)
				eep_dirty <= 1'b1;
			btn_events <= (btn_events & ~btn_clear) | btn_pressed;

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
				end else if (rd_addr[14:12] == 3'b010) begin
					s_axi_rdata <= eep_rdata;
				end else if (rd_addr[14:12] == 3'b011) begin
					s_axi_rdata <= trace_a;
				end else if (rd_addr[14:12] == 3'b100) begin
					s_axi_rdata <= trace_b;
				end else if (rd_addr[14:12] == 3'b101) begin
					s_axi_rdata <= noise_q;
				end else if (rd_addr[14:12] == 3'b110) begin
					s_axi_rdata <= xlog_q;
				end else if (rd_addr[14:12] == 3'b111) begin
					s_axi_rdata <= sdat_log_q;
				end else if (rd_addr[14:12] != 3'b000) begin
					s_axi_rdata <= 32'h0;
				end else begin
					case (rd_addr[7:2])
					6'h00: s_axi_rdata <= 32'h4E363443;
					6'h01: s_axi_rdata <= {31'h0, enable};
					6'h02: s_axi_rdata <= rom_base;
					6'h03: s_axi_rdata <= {28'h0, eep_dirty, underrun_seen, sram_dirty, n64_on};
					6'h05: s_axi_rdata <= underruns;
					6'h06: s_axi_rdata <= last_addr;
					6'h07: s_axi_rdata <= addr_count;
					6'h08: s_axi_rdata <= {19'h0, btn_events, 3'b000, btn_state};
					6'h09: s_axi_rdata <= {23'h0, oled_busy, 5'h0, oled_ctrl};
					6'h0C: s_axi_rdata <= {29'h0, cic_type};
					6'h0D: s_axi_rdata <= {30'h0, eep_16k, eep_enable};
					6'h0E: s_axi_rdata <= mon_ale;
					6'h0F: s_axi_rdata <= mon_rw;
					6'h10: s_axi_rdata <= mon_first_addr;
					6'h11: s_axi_rdata <= mon_first_data;
					6'h12: s_axi_rdata <= mon_rom;
					6'h13: s_axi_rdata <= mon_ad_or;
					6'h14: s_axi_rdata <= pins;
					6'h15: s_axi_rdata <= mon_glitch;
					6'h16: s_axi_rdata <= mon_odd;
					6'h17: s_axi_rdata <= {16'h0, mon_noise_n};
					6'h18: s_axi_rdata <= mon_xfer_n;
					6'h19: s_axi_rdata <= mon_xfer_addr;
					6'h1A: s_axi_rdata <= mon_xfer_rw;
					6'h1B: s_axi_rdata <= mon_xfer_sum;
					6'h1C: s_axi_rdata <= mon_sdat;
					6'h1D: s_axi_rdata <= mon_sdat_low;
					6'h1E: s_axi_rdata <= {21'h0, sdat_log_n};
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
