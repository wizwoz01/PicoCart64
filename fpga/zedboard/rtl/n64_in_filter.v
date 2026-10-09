// SPDX-License-Identifier: BSD-2-Clause
//
// Synchronizer + glitch filter for one asynchronous N64 input.
//
// s: 2-FF synchronized, unfiltered (2 cycles latency)
// q: only changes after N equal samples of s (N + 2 cycles latency)

`default_nettype none

module n64_in_filter #(
	parameter INIT = 1'b0,
	parameter N = 3				// at least 3
) (
	input  wire clk,
	input  wire d,
	output wire s,
	output reg  q = INIT
);

	(* ASYNC_REG = "TRUE" *) reg [1:0] sync = {2{INIT}};
	reg [N-2:0] hist = {(N-1){INIT}};

	always @(posedge clk) begin
		sync <= {sync[0], d};
		hist <= {hist[N-3:0], sync[1]};
		if (sync[1] & (&hist))
			q <= 1'b1;
		else if (~(sync[1] | (|hist)))
			q <= 1'b0;
	end

	assign s = sync[1];

endmodule

`default_nettype wire
