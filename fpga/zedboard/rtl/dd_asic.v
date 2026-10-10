// SPDX-License-Identifier: BSD-2-Clause
//
// The 64DD drive's interface (its ASIC) as the N64 sees it on the PI bus:
//   0x05000000-0x050003FF  C2 buffer (1 KB)
//   0x05000400-0x050004FF  data sector buffer (256 bytes)
//   0x05000500-0x0500057F  ASIC registers, in the upper halfword of each word
//   0x05000580-0x050005BF  MSEQ RAM (64 bytes)
//
// The drive itself is emulated by the loader on the ARM. Every register write
// from the N64 goes into an event FIFO that interrupts the ARM, and the ARM
// sets what each register reads. What the N64 has to see at once happens
// here: a command sets BUSY, starting the buffer manager sets BM START,
// BM_CTL bit 8 clears the MECHA interrupt, a BM reset clears BM START and the
// BM interrupt, and reading STATUS clears the BM interrupt and tells the ARM
// to supply the next sector (as on the real drive, see ares n64/dd).
//
// ARM side, byte offsets in the window:
//   0x0000-0x07FF  buffers: words 0-255 C2, 256-319 data sector, 320-335 MSEQ,
//                  {first halfword, second halfword} as the N64 sees them
//   0x1000-0x107C  register n (N64 0x05000500 + 4n) in [15:0]: what it reads
//   0x1080  FLAGS  R: [0] BUSY, [1] MECHA interrupt, [2] BM interrupt,
//                  [3] BM START, [8] events lost (FIFO overflow)
//                  W: [3:0] set, [11:8] clear, [16] clear events lost
//   0x1084  EVENT  R: takes the oldest event: [31] valid, [20:16] register
//                  (31: STATUS read with the BM interrupt set), [15:0] data
//
// STATUS (register 2) reads with BUSY in bit 7 and the MECHA and BM interrupts
// in bits 9 and 10, BM_STATUS (register 4) with BM START in bit 15.

`default_nettype none

module dd_asic (
	input  wire        clk,
	input  wire        rst,

	// N64 side (n64_pi_bus): halfword index from 0x05000000
	input  wire [9:0]  n64_idx,		// next halfword to read
	output reg  [15:0] n64_rdata = 16'h0,	// the halfword at n64_idx, 2 clocks after it changes
	input  wire        n64_rd,		// a read of the halfword at ev_idx finished
	input  wire        n64_wr,		// write n64_wdata at ev_idx
	input  wire [9:0]  ev_idx,
	input  wire [15:0] n64_wdata,

	// ARM side (cart_regs window), 1 clock read latency
	input  wire [10:0] arm_addr,		// word address in the window
	input  wire        arm_rd,
	input  wire        arm_wr,
	input  wire [31:0] arm_wdata,
	input  wire [3:0]  arm_wstrb,
	output wire [31:0] arm_q,

	output wire        irq_n64,		// MECHA or BM interrupt: drives /INT1
	output wire        irq_arm		// events waiting
);

	localparam R_DATA = 5'd0, R_STATUS = 5'd2, R_BM = 5'd4, R_CUR_SECTOR = 5'd7, R_HARD_RESET = 5'd8;
	localparam R_HOST_SECBYTE = 5'd10, R_C1_S2 = 5'd11, R_SEC_BYTE = 5'd12;
	localparam EV_BM_ACK = 5'd31;

	// --- Buffers: 512 x 32 (336 used), N64 halfwords on port A, ARM words on port B

	reg  [31:0] buf_mem [0:511];
	reg  [31:0] buf_a = 32'h0;
	integer i;

	// Halfword index -> buffer word: C2 0x000-0x1FF, sector 0x200-0x27F, MSEQ 0x2C0-0x2DF
	function [8:0] buf_word(input [9:0] idx);
		if (idx < 10'h200)
			buf_word = {1'b0, idx[8:1]};
		else if (idx < 10'h280)
			buf_word = 9'd256 + {3'd0, idx[6:1]};
		else
			buf_word = 9'd320 + {5'd0, idx[4:1]};
	endfunction

	function is_buf(input [9:0] idx);
		is_buf = idx < 10'h280 || (idx >= 10'h2C0 && idx < 10'h2E0);
	endfunction

	wire [8:0] a_word = n64_wr ? buf_word(ev_idx) : buf_word(n64_idx);
	wire [3:0] a_we = (n64_wr && is_buf(ev_idx)) ? (ev_idx[0] ? 4'b0011 : 4'b1100) : 4'b0000;
	wire       b_sel = arm_addr[10] == 1'b0;	// 0x0000-0x0FFF
	wire [3:0] b_we = (arm_wr && b_sel) ? arm_wstrb : 4'b0000;
	reg  [31:0] buf_b = 32'h0;

	wire [31:0] a_din = {n64_wdata, n64_wdata};

	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (a_we[i])
				buf_mem[a_word][i * 8 +: 8] <= a_din[i * 8 +: 8];
		buf_a <= buf_mem[a_word];
	end

	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (b_we[i])
				buf_mem[arm_addr[8:0]][i * 8 +: 8] <= arm_wdata[i * 8 +: 8];
		buf_b <= buf_mem[arm_addr[8:0]];
	end

	// --- Registers

	reg [15:0] view [0:31];			// what each register reads, set by the ARM
	reg        busy = 1'b0, mecha_irq = 1'b0, bm_irq = 1'b0, bm_start = 1'b0;
	reg        bm_reset = 1'b0, lost = 1'b0;

	initial begin
		for (i = 0; i < 32; i = i + 1)
			view[i] = 16'h0;
		for (i = 0; i < 512; i = i + 1)
			buf_mem[i] = 32'h0;
	end

	wire [4:0] ev_reg = ev_idx[5:1];
	wire       reg_wr = n64_wr && ev_idx[9:6] == 4'hA && !ev_idx[0];	// 0x500-0x57F, upper halves
	wire       status_rd = n64_rd && ev_idx == 10'h284 && bm_irq;		// STATUS at 0x508
	wire       disk_present = view[R_STATUS][8];

	// Event FIFO: {register, data}
	reg  [20:0] fifo [0:31];
	reg  [5:0]  wr_ptr = 6'd0, rd_ptr = 6'd0;
	wire [5:0]  count = wr_ptr - rd_ptr;
	wire        push = (reg_wr && !(ev_reg == R_STATUS && busy)) || status_rd;
	wire [20:0] push_data = status_rd ? {EV_BM_ACK, 16'h0} : {ev_reg, n64_wdata};
	wire        ev_sel = arm_addr == 11'h421;	// 0x1084
	wire        pop = arm_rd && ev_sel && count != 6'd0;

	always @(posedge clk) begin
		if (push && count != 6'd32)
			fifo[wr_ptr[4:0]] <= push_data;
	end

	assign irq_n64 = mecha_irq | bm_irq;
	assign irq_arm = count != 6'd0;

	always @(posedge clk) begin
		if (rst) begin
			busy <= 1'b0;
			mecha_irq <= 1'b0;
			bm_irq <= 1'b0;
			bm_start <= 1'b0;
			bm_reset <= 1'b0;
			lost <= 1'b0;
			wr_ptr <= 6'd0;
			rd_ptr <= 6'd0;
		end else begin
			// ARM
			if (arm_wr && arm_addr[10:5] == 6'b100000)		// 0x1000-0x107C
				view[arm_addr[4:0]] <= arm_wdata[15:0];
			if (arm_wr && arm_addr == 11'h420) begin		// 0x1080 FLAGS
				busy <= arm_wdata[0] | (busy & ~arm_wdata[8]);
				mecha_irq <= arm_wdata[1] | (mecha_irq & ~arm_wdata[9]);
				bm_irq <= arm_wdata[2] | (bm_irq & ~arm_wdata[10]);
				bm_start <= arm_wdata[3] | (bm_start & ~arm_wdata[11]);
				if (arm_wdata[16])
					lost <= 1'b0;
			end
			if (pop)
				rd_ptr <= rd_ptr + 6'd1;

			// N64
			if (push) begin
				if (count != 6'd32)
					wr_ptr <= wr_ptr + 6'd1;
				else
					lost <= 1'b1;
			end
			if (status_rd)
				bm_irq <= 1'b0;
			if (reg_wr) begin
				case (ev_reg)
				R_DATA:
					view[R_DATA] <= n64_wdata;
				R_STATUS:				// ASIC_CMD
					busy <= 1'b1;
				R_BM: begin				// ASIC_BM_CTL
					if (n64_wdata[8])
						mecha_irq <= 1'b0;
					view[R_CUR_SECTOR] <= {n64_wdata[7:0], 8'hC3};
					if (n64_wdata[12]) begin
						bm_reset <= 1'b1;
					end else if (bm_reset) begin
						bm_reset <= 1'b0;
						bm_start <= 1'b0;
						bm_irq <= 1'b0;
					end
					if (n64_wdata[15] && disk_present)
						bm_start <= 1'b1;
				end
				R_HOST_SECBYTE:
					view[R_HOST_SECBYTE] <= {8'h00, n64_wdata[7:0]};
				R_C1_S2:
					view[R_SEC_BYTE] <= n64_wdata;
				default: ;
				endcase
			end
		end
	end

	// --- Read data

	function [15:0] reg_read(input [4:0] n);
		case (n)
		R_STATUS: reg_read = {view[n][15:11], bm_irq, mecha_irq, view[n][8], busy, view[n][6:0]};
		R_BM:     reg_read = {bm_start, view[n][14:0]};
		default:  reg_read = view[n];
		endcase
	endfunction

	reg        sel_buf = 1'b0, sel_half = 1'b0;
	reg [15:0] reg_q = 16'h0;

	always @(posedge clk) begin
		sel_buf <= is_buf(n64_idx);
		sel_half <= n64_idx[0];
		reg_q <= (n64_idx[9:6] == 4'hA && !n64_idx[0]) ? reg_read(n64_idx[5:1]) : 16'h0;
		n64_rdata <= sel_buf ? (sel_half ? buf_a[15:0] : buf_a[31:16]) : reg_q;
	end

	// ARM reads: cart_regs samples arm_q one clock after arm_rd. Buffer words
	// come straight from the RAM output.
	reg [31:0] arm_rdata = 32'h0;
	reg        arm_buf_rd = 1'b0;

	always @(posedge clk) begin
		arm_buf_rd <= b_sel;
		if (arm_addr[10:5] == 6'b100000)
			arm_rdata <= {16'h0, reg_read(arm_addr[4:0])};
		else if (arm_addr == 11'h420)
			arm_rdata <= {23'h0, lost, 4'h0, bm_start, bm_irq, mecha_irq, busy};
		else if (ev_sel)
			arm_rdata <= count != 6'd0 ? {1'b1, 10'h0, fifo[rd_ptr[4:0]]} : 32'h0;
		else
			arm_rdata <= 32'h0;
	end

	assign arm_q = arm_buf_rd ? buf_b : arm_rdata;
endmodule

`default_nettype wire
