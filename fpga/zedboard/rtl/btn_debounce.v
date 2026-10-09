// SPDX-License-Identifier: BSD-2-Clause
//
// Synchronizer and debouncer for the ZedBoard push buttons (active high).
// A button changes state once it has read the same for 8 ticks in a row
// (8 ms with the default 1 ms tick). pressed pulses for one clock on each
// debounced press.

`default_nettype none

module btn_debounce #(
	parameter N = 5,
	parameter TICK = 100000			// clocks per sample
) (
	input  wire         clk,
	input  wire [N-1:0] btn_in,
	output wire [N-1:0] state,
	output wire [N-1:0] pressed
);

	(* ASYNC_REG = "TRUE" *) reg [N-1:0] sync0 = 0, sync1 = 0;
	reg [31:0]  div = 0;
	reg         tick = 1'b0;
	reg [N-1:0] state_d = 0;

	always @(posedge clk) begin
		sync0 <= btn_in;
		sync1 <= sync0;
		tick <= (div == TICK - 1);
		div <= (div == TICK - 1) ? 32'd0 : div + 32'd1;
		state_d <= state;
	end

	genvar i;
	generate
		for (i = 0; i < N; i = i + 1) begin : g_btn
			reg [7:0] hist = 8'h00;
			reg       st = 1'b0;

			always @(posedge clk) begin
				if (tick) begin
					hist <= {hist[6:0], sync1[i]};
					if (&hist)
						st <= 1'b1;
					else if (~|hist)
						st <= 1'b0;
				end
			end

			assign state[i] = st;
		end
	endgenerate

	assign pressed = state & ~state_d;

endmodule

`default_nettype wire
