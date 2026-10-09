// SPDX-License-Identifier: BSD-2-Clause
//
// Cartridge EEPROM (4 or 16 Kbit) on the N64 joybus line S_DAT.
//
// S_DAT is open drain with its pull-up in the console. Bits are 4 us, MSB
// first: 0 = 3 us low + 1 us high, 1 = 1 us low + 3 us high. The console ends
// its message with a stop bit of 1 us low, the cart ends its reply with 2 us low.
//
// Commands:
//   0x00 info, 0xFF reset: reply 0x00, 0x80 (4 Kbit) or 0xC0 (16 Kbit), 0x00
//   0x04 read block:       reply the 8 bytes of the block
//   0x05 write block:      8 data bytes follow, reply 0x00
// Other commands are not answered.
//
// The memory port is 512 words; word k holds bytes 4k..4k+3 as {b0, b1, b2, b3}.

`default_nettype none

module joybus_eeprom #(
	parameter US = 100			// clocks per microsecond
) (
	input  wire        clk,
	input  wire        rst,
	input  wire        enable,
	input  wire        size16k,
	input  wire        sdat,		// line level, synchronized
	output reg         sdat_low = 1'b0,	// pull the line low

	input  wire [8:0]  mem_addr,
	input  wire [31:0] mem_wdata,
	input  wire [3:0]  mem_we,
	output reg  [31:0] mem_rdata = 32'h0,

	output reg         written = 1'b0,	// pulse: a block was written
	output reg         access = 1'b0	// pulse: a command was answered
);

	localparam S_IDLE = 4'd0, S_BIT = 4'd1, S_GAP = 4'd2, S_STOP = 4'd3, S_STOP_HI = 4'd4;
	localparam S_TX_LOAD = 4'd5, S_TX_LOAD2 = 4'd6, S_TX_LO = 4'd7, S_TX_HI = 4'd8, S_TX_STOP = 4'd9;
	localparam IDLE_TIME = 10 * US;		// line high this long before a new command
	localparam TIMEOUT = 10 * US;

	reg [31:0] mem [0:511];
	integer i;

	// Port A: joybus, one byte at a time. Byte n of a word is in [31-8n:24-8n].
	reg  [10:0] a_addr = 11'h0;
	reg  [7:0]  a_wdata = 8'h0;
	reg         a_wr = 1'b0;
	reg  [31:0] a_dout = 32'h0;
	wire [1:0]  a_lane = 2'd3 - a_addr[1:0];

	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (a_wr && a_lane == i)
				mem[a_addr[10:2]][i * 8 +: 8] <= a_wdata;
		a_dout <= mem[a_addr[10:2]];
	end

	wire [7:0] a_byte = a_dout[a_lane * 8 +: 8];

	// Port B: registers / ARM
	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (mem_we[i])
				mem[mem_addr][i * 8 +: 8] <= mem_wdata[i * 8 +: 8];
		mem_rdata <= mem[mem_addr];
	end

	reg  [3:0]  state = S_IDLE;
	reg  [15:0] t = 16'h0;
	reg         prev = 1'b1;
	reg  [7:0]  sh = 8'h0;
	reg  [2:0]  nbit = 3'h0;
	reg  [3:0]  nbyte = 4'h0;
	reg  [3:0]  need = 4'h0;
	reg  [7:0]  cmd = 8'h0;
	reg  [7:0]  block = 8'h0;
	reg  [3:0]  ntx = 4'h0;
	reg  [3:0]  txi = 4'h0;

	wire        fall = prev && !sdat;
	wire [7:0]  rx_byte = {sh[6:0], sdat};
	wire [7:0]  blk = size16k ? block : {2'b00, block[5:0]};
	wire [3:0]  cmd_len = (rx_byte == 8'h00 || rx_byte == 8'hFF) ? 4'd1 :
			      (rx_byte == 8'h04) ? 4'd2 :
			      (rx_byte == 8'h05) ? 4'd10 : 4'd0;
	wire [7:0]  info_byte = (txi == 4'd1) ? (size16k ? 8'hC0 : 8'h80) : 8'h00;
	wire [15:0] lo_time = sh[7] ? US : 3 * US;
	wire [15:0] hi_time = sh[7] ? 3 * US : US;

	always @(posedge clk) begin
		prev <= sdat;
		a_wr <= 1'b0;
		written <= 1'b0;
		access <= 1'b0;

		if (rst || !enable) begin
			state <= S_IDLE;
			t <= 16'h0;
			sdat_low <= 1'b0;
		end else begin
			case (state)
			// Wait for a quiet line, then the first falling edge
			S_IDLE: begin
				if (fall && t >= IDLE_TIME) begin
					t <= 16'h0;
					nbit <= 3'h0;
					nbyte <= 4'h0;
					state <= S_BIT;
				end else if (!sdat) begin
					t <= 16'h0;
				end else if (t != 16'hFFFF) begin
					t <= t + 16'h1;
				end
			end
			// Sample 2 us after the falling edge
			S_BIT: begin
				t <= t + 16'h1;
				if (t == 2 * US) begin
					sh <= rx_byte;
					nbit <= nbit + 3'h1;
					state <= S_GAP;
					if (nbit == 3'h7) begin
						nbyte <= nbyte + 4'h1;
						if (nbyte == 4'h0) begin
							cmd <= rx_byte;
							need <= cmd_len;
							if (cmd_len == 4'd0)
								state <= S_IDLE;
							else if (cmd_len == 4'd1)
								state <= S_STOP;
						end else begin
							if (nbyte == 4'h1)
								block <= rx_byte;
							else begin
								a_addr <= {blk, nbyte[2:0] - 3'd2};
								a_wdata <= rx_byte;
								a_wr <= 1'b1;
							end
							if (nbyte + 4'h1 == need)
								state <= S_STOP;
						end
					end
				end
			end
			S_GAP: begin
				t <= t + 16'h1;
				if (fall) begin
					t <= 16'h0;
					state <= S_BIT;
				end else if (t > TIMEOUT) begin
					t <= 16'h0;
					state <= S_IDLE;
				end
			end
			// Console stop bit, then answer 2 us after it ends
			S_STOP: begin
				t <= t + 16'h1;
				if (fall) begin
					t <= 16'h0;
					state <= S_STOP_HI;
				end else if (t > TIMEOUT) begin
					t <= 16'h0;
					state <= S_IDLE;
				end
			end
			S_STOP_HI: begin
				t <= t + 16'h1;
				if (!prev && sdat) begin
					t <= 16'h0;
				end else if (!sdat && t > TIMEOUT) begin
					t <= 16'h0;
					state <= S_IDLE;
				end else if (sdat && t == 2 * US) begin
					ntx <= (cmd == 8'h04) ? 4'd8 : (cmd == 8'h05) ? 4'd1 : 4'd3;
					txi <= 4'h0;
					written <= (cmd == 8'h05);
					access <= 1'b1;
					state <= S_TX_LOAD;
				end
			end
			// Reply
			S_TX_LOAD: begin
				a_addr <= {blk, txi[2:0]};
				t <= 16'h0;
				state <= S_TX_LOAD2;
			end
			S_TX_LOAD2: begin
				t <= t + 16'h1;
				if (t == 16'h2) begin
					sh <= (cmd == 8'h04) ? a_byte : (cmd == 8'h05) ? 8'h00 : info_byte;
					nbit <= 3'h0;
					sdat_low <= 1'b1;
					t <= 16'h0;
					state <= S_TX_LO;
				end
			end
			S_TX_LO: begin
				t <= t + 16'h1;
				if (t == lo_time - 16'h1) begin
					sdat_low <= 1'b0;
					t <= 16'h0;
					state <= S_TX_HI;
				end
			end
			S_TX_HI: begin
				t <= t + 16'h1;
				if (t == hi_time - 16'h1) begin
					t <= 16'h0;
					if (nbit == 3'h7) begin
						txi <= txi + 4'h1;
						if (txi + 4'h1 == ntx) begin
							sdat_low <= 1'b1;
							state <= S_TX_STOP;
						end else begin
							state <= S_TX_LOAD;
						end
					end else begin
						nbit <= nbit + 3'h1;
						sh <= {sh[6:0], 1'b0};
						sdat_low <= 1'b1;
						state <= S_TX_LO;
					end
				end
			end
			S_TX_STOP: begin
				t <= t + 16'h1;
				if (t == 2 * US - 1) begin
					sdat_low <= 1'b0;
					t <= 16'h0;
					state <= S_IDLE;
				end
			end
			default:
				state <= S_IDLE;
			endcase
		end
	end

endmodule

`default_nettype wire
