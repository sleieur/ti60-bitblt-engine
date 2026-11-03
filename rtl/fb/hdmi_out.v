// =============================================================================
// hdmi_out.v - FPS overlay + DVI/TMDS encoding (pixel clock 74.25 MHz for 720p60).
// Encoder from hdmi_tx_demo_v3 (dvi_encoder/encode). The 10-bit symbols go to the LVDS TX
// blocks (half-rate, 10:1 serialisation, 371.25 MHz fast clock) and are inverted exactly as in
// hdmi_tx_demo_v3/rtl/top.v (board P/N wiring).
// =============================================================================
module hdmi_out (
    input         pix_clk,
    input         pix_rstn,
    input         frame_start,
    input         swap_done,
    input  [23:0] rgb,              // R[23:16] G[15:8] B[7:0]
    input         hs,
    input         vs,
    input         de,
    output [9:0]  tmds_clk_o,
    output [9:0]  tmds_data0_o,
    output [9:0]  tmds_data1_o,
    output [9:0]  tmds_data2_o,
    output [6:0]  render_fps
);
    wire [23:0] ov_rgb;
    wire        ov_hs, ov_vs, ov_de;

    fps_overlay u_fps (
        .clk(pix_clk), .rstn(pix_rstn), .frame_start(frame_start), .swap_done(swap_done),
        .i_rgb(rgb), .i_hs(hs), .i_vs(vs), .i_de(de),
        .o_rgb(ov_rgb), .o_hs(ov_hs), .o_vs(ov_vs), .o_de(ov_de), .render_fps(render_fps));

    wire [9:0] t0, t1, t2, tc;
    dvi_encoder u_dvi (
        .pixelclk(pix_clk), .rstin(~pix_rstn),
        .blue_din(ov_rgb[7:0]), .green_din(ov_rgb[15:8]), .red_din(ov_rgb[23:16]),
        .hsync(ov_hs), .vsync(ov_vs), .de(ov_de),
        .tmds_data0(t0), .tmds_data1(t1), .tmds_data2(t2), .tmds_clk(tc));

    assign tmds_clk_o   = ~tc;
    assign tmds_data0_o = ~t0;
    assign tmds_data1_o = ~t1;
    assign tmds_data2_o = ~t2;
endmodule
