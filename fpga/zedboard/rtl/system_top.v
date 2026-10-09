// SPDX-License-Identifier: BSD-2-Clause
//
// ZedBoard top: Zynq block design (design_1, created by scripts/create_project.tcl)
// plus the bidirectional N64 AD bus buffers, push buttons, OLED, the joybus
// S_DAT line and the CIC type lines (both open drain).

`default_nettype none

module system_top (
	inout  wire [14:0] DDR_addr,
	inout  wire [2:0]  DDR_ba,
	inout  wire        DDR_cas_n,
	inout  wire        DDR_ck_n,
	inout  wire        DDR_ck_p,
	inout  wire        DDR_cke,
	inout  wire        DDR_cs_n,
	inout  wire [3:0]  DDR_dm,
	inout  wire [31:0] DDR_dq,
	inout  wire [3:0]  DDR_dqs_n,
	inout  wire [3:0]  DDR_dqs_p,
	inout  wire        DDR_odt,
	inout  wire        DDR_ras_n,
	inout  wire        DDR_reset_n,
	inout  wire        DDR_we_n,
	inout  wire        FIXED_IO_ddr_vrn,
	inout  wire        FIXED_IO_ddr_vrp,
	inout  wire [53:0] FIXED_IO_mio,
	inout  wire        FIXED_IO_ps_clk,
	inout  wire        FIXED_IO_ps_porb,
	inout  wire        FIXED_IO_ps_srstb,

	inout  wire [15:0] n64_ad,
	input  wire        n64_aleh,
	input  wire        n64_alel,
	input  wire        n64_read_n,
	input  wire        n64_write_n,
	input  wire        n64_cold_reset,

	output wire [7:0]  led,

	input  wire [4:0]  btn,			// BTNC, BTND, BTNL, BTNR, BTNU
	output wire        oled_vdd,
	output wire        oled_vbat,
	output wire        oled_res,
	output wire        oled_dc,
	output wire        oled_sclk,
	output wire        oled_sdin,

	inout  wire        n64_sdat,
	output wire [2:0]  cic_type		// to Pico GPIO26-28
);

	wire [15:0] ad_i, ad_o, ad_t;
	wire        sdat_i, sdat_t;
	wire [2:0]  cic_t;

	genvar i;
	generate
		for (i = 0; i < 16; i = i + 1) begin : g_ad
			IOBUF u_iobuf (
				.IO(n64_ad[i]),
				.I(ad_o[i]),
				.O(ad_i[i]),
				.T(ad_t[i])
			);
		end
	endgenerate

	IOBUF u_sdat (
		.IO(n64_sdat),
		.I(1'b0),
		.O(sdat_i),
		.T(sdat_t)
	);

	generate
		for (i = 0; i < 3; i = i + 1) begin : g_cic
			OBUFT u_obuft (
				.O(cic_type[i]),
				.I(1'b0),
				.T(cic_t[i])
			);
		end
	endgenerate

	design_1_wrapper u_bd (
		.DDR_addr(DDR_addr),
		.DDR_ba(DDR_ba),
		.DDR_cas_n(DDR_cas_n),
		.DDR_ck_n(DDR_ck_n),
		.DDR_ck_p(DDR_ck_p),
		.DDR_cke(DDR_cke),
		.DDR_cs_n(DDR_cs_n),
		.DDR_dm(DDR_dm),
		.DDR_dq(DDR_dq),
		.DDR_dqs_n(DDR_dqs_n),
		.DDR_dqs_p(DDR_dqs_p),
		.DDR_odt(DDR_odt),
		.DDR_ras_n(DDR_ras_n),
		.DDR_reset_n(DDR_reset_n),
		.DDR_we_n(DDR_we_n),
		.FIXED_IO_ddr_vrn(FIXED_IO_ddr_vrn),
		.FIXED_IO_ddr_vrp(FIXED_IO_ddr_vrp),
		.FIXED_IO_mio(FIXED_IO_mio),
		.FIXED_IO_ps_clk(FIXED_IO_ps_clk),
		.FIXED_IO_ps_porb(FIXED_IO_ps_porb),
		.FIXED_IO_ps_srstb(FIXED_IO_ps_srstb),
		.ad_i(ad_i),
		.ad_o(ad_o),
		.ad_t(ad_t),
		.aleh(n64_aleh),
		.alel(n64_alel),
		.read_n(n64_read_n),
		.write_n(n64_write_n),
		.cold_reset(n64_cold_reset),
		.led(led),
		.btn(btn),
		.oled_vdd(oled_vdd),
		.oled_vbat(oled_vbat),
		.oled_res(oled_res),
		.oled_dc(oled_dc),
		.oled_sclk(oled_sclk),
		.oled_sdin(oled_sdin),
		.sdat_i(sdat_i),
		.sdat_t(sdat_t),
		.cic_t(cic_t)
	);

endmodule

`default_nettype wire
