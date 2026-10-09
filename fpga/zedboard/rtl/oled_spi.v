// SPDX-License-Identifier: BSD-2-Clause
//
// SPI link to the ZedBoard's 128x32 OLED (SSD1306, write-only 4-wire SPI with
// CS# tied low on the board).
//
// The PL only moves bytes; the SSD1306 init sequence and addressing commands
// are sent by software (sw/loader/src/main.c), one byte at a time through
// byte_go, or as a whole 512-byte frame from the frame buffer through flush_go.
// Requests that arrive while busy are ignored.
//
// SPI mode 0, MSB first, SCLK = clk / (2 * HALF): data changes on the falling
// edge and the SSD1306 samples it on the rising edge.
//
// ZedBoard power switches: OLED_VDD and OLED_VBAT are active low, so is RES#.
// While VDD is off, every signal pin is held low so that the controller is not
// powered through its inputs.

`default_nettype none

module oled_spi #(
	parameter HALF = 10			// 5 MHz SCLK at 100 MHz (SSD1306 max 10 MHz)
) (
	input  wire        clk,
	input  wire        rst,

	input  wire        vdd_on,
	input  wire        vbat_on,
	input  wire        run,			// release RES#

	input  wire        byte_go,
	input  wire [7:0]  byte_data,
	input  wire        byte_dc,		// 0 = command, 1 = data
	input  wire        flush_go,		// send the frame buffer as data
	output wire        busy,

	// Frame buffer write port: byte 4*addr+j in [8j+7:8j]
	input  wire [6:0]  fb_addr,
	input  wire [31:0] fb_wdata,
	input  wire [3:0]  fb_we,

	output wire        oled_vdd,
	output wire        oled_vbat,
	output wire        oled_res,
	output wire        oled_dc,
	output wire        oled_sclk,
	output wire        oled_sdin
);

	localparam S_IDLE = 3'd0, S_LOAD = 3'd1, S_LOAD2 = 3'd2, S_LO = 3'd3, S_HI = 3'd4, S_END = 3'd5;

	reg [31:0] fb_mem [0:127];
	reg [31:0] fb_rword = 32'h0;
	reg [8:0]  idx = 9'd0;
	integer i;

	always @(posedge clk) begin
		for (i = 0; i < 4; i = i + 1)
			if (fb_we[i])
				fb_mem[fb_addr][i * 8 +: 8] <= fb_wdata[i * 8 +: 8];
		fb_rword <= fb_mem[idx[8:2]];
	end

	wire [7:0] fb_byte = fb_rword[idx[1:0] * 8 +: 8];

	reg [2:0]  state = S_IDLE;
	reg [7:0]  sh = 8'h0;
	reg [2:0]  bitcnt = 3'd0;
	reg [7:0]  div = 8'd0;
	reg        flush = 1'b0;
	reg        sclk = 1'b0, sdin = 1'b0, dc = 1'b0;

	always @(posedge clk) begin
		if (rst || !vdd_on) begin
			state <= S_IDLE;
			flush <= 1'b0;
			sclk <= 1'b0;
			sdin <= 1'b0;
			dc <= 1'b0;
		end else begin
			case (state)
			S_IDLE: begin
				if (byte_go) begin
					sh <= byte_data;
					sdin <= byte_data[7];
					dc <= byte_dc;
					bitcnt <= 3'd7;
					div <= HALF - 1;
					state <= S_LO;
				end else if (flush_go) begin
					idx <= 9'd0;
					dc <= 1'b1;
					flush <= 1'b1;
					state <= S_LOAD;
				end
			end
			// fb_rword follows idx one clock later
			S_LOAD:
				state <= S_LOAD2;
			S_LOAD2: begin
				sh <= fb_byte;
				sdin <= fb_byte[7];
				bitcnt <= 3'd7;
				div <= HALF - 1;
				state <= S_LO;
			end
			S_LO: begin
				if (div == 0) begin
					sclk <= 1'b1;
					div <= HALF - 1;
					state <= S_HI;
				end else begin
					div <= div - 8'd1;
				end
			end
			S_HI: begin
				if (div == 0) begin
					sclk <= 1'b0;
					div <= HALF - 1;
					if (bitcnt == 0) begin
						state <= S_END;
					end else begin
						bitcnt <= bitcnt - 3'd1;
						sh <= {sh[6:0], 1'b0};
						sdin <= sh[6];
						state <= S_LO;
					end
				end else begin
					div <= div - 8'd1;
				end
			end
			// Hold SDIN and D/C for half a clock after the last rising edge
			S_END: begin
				if (div == 0) begin
					if (flush && idx != 9'd511) begin
						idx <= idx + 9'd1;
						state <= S_LOAD;
					end else begin
						flush <= 1'b0;
						state <= S_IDLE;
					end
				end else begin
					div <= div - 8'd1;
				end
			end
			default:
				state <= S_IDLE;
			endcase
		end
	end

	assign busy = (state != S_IDLE);

	assign oled_vdd = ~vdd_on;
	assign oled_vbat = ~(vdd_on & vbat_on);
	assign oled_res = vdd_on & run;
	assign oled_dc = dc;
	assign oled_sclk = sclk;
	assign oled_sdin = sdin;

endmodule

`default_nettype wire
