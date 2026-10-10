// SPDX-License-Identifier: BSD-2-Clause
//
// FlashRAM save chip (1 Mbit), in front of the cart's SRAM block RAM.
//
// With flash_mode off, the SRAM port passes straight through. With it on, the
// 128 KB at 0x08000000 behave like the Macronix MX29L1101 that FlashRAM games
// find (the command set and status bits follow ares, n64/cartridge/flash.cpp):
//   0x08010000  command register, written as two halfwords: [31:24] command
//     F0 read array, E1 chip ID, D2 status, B4 load the 128-byte page buffer,
//     A5 program page [9:0] (the page ANDs the buffer in, as flash does),
//     4B sector erase setup (16 KB sector of page [9:0]), 3C chip erase setup,
//     78 erase
//   0x08000000  read: the array (byte addressed), the status (0x8C when ready),
//     or the ID 1111 8001 00C2 001D; written in page buffer mode: the buffer
//     (which reads back as the array: games don't read it)
// Erasing and programming run on the RAM port while the status reads busy.
// The chip returns to read array mode when the N64 is turned on.

`default_nettype none

module flashram (
	input  wire        clk,
	input  wire        rst,
	input  wire        flash_mode,
	input  wire        n64_on,

	// From n64_pi_bus: its SRAM port, halfword index
	input  wire [15:0] pi_addr,
	input  wire [15:0] pi_wdata,
	input  wire        pi_we,
	input  wire        pi_start,		// an address phase in the SRAM range
	output reg  [15:0] pi_rdata,

	// To the SRAM block RAM (cart_regs), 1 clock read latency
	output wire [15:0] ram_addr,
	output wire [15:0] ram_wdata,
	output wire        ram_we,
	input  wire [15:0] ram_rdata
);

	localparam M_READ = 2'd0, M_STATUS = 2'd1, M_ID = 2'd2, M_LOAD = 2'd3;
	localparam E_NONE = 2'd0, E_SECTOR = 2'd1, E_CHIP = 2'd2;
	localparam S_IDLE = 2'd0, S_ERASE = 2'd1, S_READ = 2'd2, S_WRITE = 2'd3;
	localparam CMD_HI = 16'h8000, CMD_LO = 16'h8001;	// 0x08010000, 0x08010002

	reg  [1:0]  mode = M_READ, erase_setup = E_NONE, state = S_IDLE;
	reg  [7:0]  status = 8'h8C;
	reg  [15:0] cmd_hi = 16'h0;
	reg  [2:0]  erase_sector = 3'd0;
	reg  [15:0] start_idx = 16'h0;
	reg  [15:0] page_buf [0:63];
	reg  [16:0] eng_idx = 17'd0, eng_end = 17'd0;
	reg  [5:0]  eng_i = 6'd0;
	reg  [15:0] eng_buf = 16'hFFFF;		// page_buf[eng_i], a clock ahead of the write
	reg         on_d = 1'b0;
	integer i;

	wire        busy = state != S_IDLE;
	wire        cmd_we = flash_mode && pi_we && pi_addr == CMD_LO;
	wire [31:0] cmd = {cmd_hi, pi_wdata};

	// The RAM port: the engine while it runs, else the N64 (writes only
	// outside flash mode: the N64 never writes the array directly)
	assign ram_addr = busy ? eng_idx[15:0] : pi_addr;
	assign ram_we = busy ? state == S_WRITE || state == S_ERASE : pi_we && !flash_mode;
	assign ram_wdata = state == S_ERASE ? 16'hFFFF : state == S_WRITE ? (ram_rdata & eng_buf) : pi_wdata;

	always @(*) begin
		if (!flash_mode)
			pi_rdata = ram_rdata;
		else case (mode)
		M_STATUS: pi_rdata = {8'h00, status};
		M_ID: case (pi_addr[1:0] - start_idx[1:0])
			2'd0: pi_rdata = 16'h1111;
			2'd1: pi_rdata = 16'h8001;
			2'd2: pi_rdata = 16'h00C2;
			default: pi_rdata = 16'h001D;
			endcase
		default: pi_rdata = ram_rdata;
		endcase
	end

	always @(posedge clk) begin
		on_d <= n64_on;
		if (pi_start)
			start_idx <= pi_addr;

		if (rst || !flash_mode || (n64_on && !on_d)) begin
			mode <= M_READ;
			erase_setup <= E_NONE;
			state <= S_IDLE;
			status <= 8'h8C;
			for (i = 0; i < 64; i = i + 1)
				page_buf[i] <= 16'hFFFF;
		end else begin
			// Engine: erase one halfword per clock, program in read/write pairs
			case (state)
			S_ERASE: begin
				eng_idx <= eng_idx + 17'd1;
				if (eng_idx + 17'd1 == eng_end) begin
					state <= S_IDLE;
					status <= 8'h8C;
				end
			end
			S_READ: begin
				eng_buf <= page_buf[eng_i];
				state <= S_WRITE;
			end
			S_WRITE: begin
				eng_idx <= eng_idx + 17'd1;
				eng_i <= eng_i + 6'd1;
				if (eng_i == 6'd63) begin
					state <= S_IDLE;
					status <= 8'h8C;
					for (i = 0; i < 64; i = i + 1)
						page_buf[i] <= 16'hFFFF;
				end else begin
					state <= S_READ;
				end
			end
			default: ;
			endcase

			// N64 writes
			if (pi_we && pi_addr == CMD_HI)
				cmd_hi <= pi_wdata;
			if (pi_we && mode == M_LOAD && pi_addr < CMD_HI)
				page_buf[pi_addr[5:0]] <= pi_wdata;
			if (cmd_we && !busy) begin
				case (cmd[31:24])
				8'hF0: mode <= M_READ;
				8'hE1: mode <= M_ID;
				8'hD2: mode <= M_STATUS;
				8'hB4: mode <= M_LOAD;
				8'h3C: erase_setup <= E_CHIP;
				8'h4B: begin
					erase_setup <= E_SECTOR;
					erase_sector <= cmd[9:7];
				end
				8'h78: if (erase_setup != E_NONE) begin
					mode <= M_STATUS;
					erase_setup <= E_NONE;
					status <= 8'h0E;			// erase busy
					state <= S_ERASE;
					eng_idx <= erase_setup == E_CHIP ? 17'd0 : {1'b0, erase_sector, 13'd0};
					eng_end <= erase_setup == E_CHIP ? 17'h10000 : {1'b0, erase_sector, 13'd0} + 17'h2000;
				end
				8'hA5: begin
					mode <= M_STATUS;
					status <= 8'h0D;			// program busy
					state <= S_READ;
					eng_idx <= {1'b0, cmd[9:0], 6'd0};
					eng_i <= 6'd0;
				end
				default: ;
				endcase
			end
		end
	end

endmodule

`default_nettype wire
