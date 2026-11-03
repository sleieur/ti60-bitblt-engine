// =============================================================================
// fb_read_path.v - HDMI framebuffer read path:
//   hdmi_rd_master (sys clock) -> pix_fifo (8 KiB, 128-bit write / 16-bit read) -> pix_reader (pixel clock)
// pix_fifo = Efinity efx_fifo_top IP (par/ddr_demo_ti60/ip/pix_fifo, gen_pix_fifo.py): asynchronous,
// 512 x 128 bit write side, 4096 x 16 bit read side (RGB565), little endian (wdata[15:0] read first).
// plus all clock-domain crossings between them:
//   pixel -> sys : frame_start (VSYNC rising edge), drop_req (underflow)
//   sys -> pixel : resume
// Positive-polarity VSYNC is expected; frame_start = vs & ~vs_d (start of the VSYNC pulse,
// inside vertical blanking). pix_rgb is valid one pixel clock after `de`.
// sys_* outputs are in the sys clock domain (page control, registers).
// Internal state (live, burst count, frame_drop ...) is not exported; testbenches read it
// hierarchically (u_rd.*, u_reader.*).
// =============================================================================
module fb_read_path #(
    parameter ID_W   = 4,
    parameter NBURST = 450
)(
    input              sys_clk,
    input              sys_rstn,
    input              pix_clk,
    input              pix_rstn,

    // sys domain control
    input              enable,
    input              cal_done,
    input      [31:0]  front_base,        // from fb_page_ctrl, valid cycle after sys_frame_start
    output             sys_frame_start,   // to fb_page_ctrl
    output             sys_underflow,     // to fb_regs_axil.underflow_set
    output             sys_frame_safe,    // to fb_page_ctrl: safe frame boundary (with sys_frame_start)

    // pixel domain timing in, pixel out
    input              pix_vs,
    input              pix_de,
    output     [23:0]  pix_rgb,
    output             pix_frame_start,

    // AXI4 read master
    output [ID_W-1:0]  m_arid,
    output [31:0]      m_araddr,
    output [7:0]       m_arlen,
    output [2:0]       m_arsize,
    output [1:0]       m_arburst,
    output             m_arvalid,
    input              m_arready,
    input  [127:0]     m_rdata,
    input              m_rlast,
    input              m_rvalid,
    output             m_rready
);
    // ------------------------------------------------------------ frame start (pixel)
    reg vs_d;
    always @(posedge pix_clk or negedge pix_rstn)
        if (!pix_rstn) vs_d <= 1'b0; else vs_d <= pix_vs;
    assign pix_frame_start = pix_vs && !vs_d;

    // ------------------------------------------------------------ CDC
    wire pix_drop_req, sys_drop_req, sys_resume, pix_resume;

    fb_cdc_pulse u_cdc_fs   (.src_clk(pix_clk), .src_rstn(pix_rstn), .src_pulse(pix_frame_start),
                             .dst_clk(sys_clk), .dst_rstn(sys_rstn), .dst_pulse(sys_frame_start));
    fb_cdc_pulse u_cdc_drop (.src_clk(pix_clk), .src_rstn(pix_rstn), .src_pulse(pix_drop_req),
                             .dst_clk(sys_clk), .dst_rstn(sys_rstn), .dst_pulse(sys_drop_req));
    fb_cdc_pulse u_cdc_rs   (.src_clk(sys_clk), .src_rstn(sys_rstn), .src_pulse(sys_resume),
                             .dst_clk(pix_clk), .dst_rstn(pix_rstn), .dst_pulse(pix_resume));
    assign sys_underflow = sys_drop_req;   // one underflow event = one drop request

    // ------------------------------------------------------------ FIFO
    // wcount (write domain, 128-bit words) = words written minus the synchronised read
    // pointer / 4: a partly read word still counts, and the read pointer lags, so it is
    // never below the true fill level (conservative for "issue another burst").
    // Resets: the IP reset synchronisers are bypassed, sys_rstn / pix_rstn are already
    // synchronised in their own domains. The FIFO is never reset during operation; the
    // underflow flush empties it by reading.
    wire [9:0]   fifo_wcount;
    wire         fifo_wr, fifo_rd, fifo_empty;
    wire         fifo_uflow;               // underflow_o -> pix_reader
    wire [127:0] fifo_wdata;
    wire [15:0]  fifo_rdata;

    pix_fifo u_fifo (
        .a_wr_rst_i(!sys_rstn), .a_rd_rst_i(!pix_rstn), .rst_busy(),
        .wr_clk_i(sys_clk), .wr_en_i(fifo_wr), .wdata(fifo_wdata),
        .full_o(), .wr_datacount_o(fifo_wcount), .overflow_o(),
        .rd_clk_i(pix_clk), .rd_en_i(fifo_rd), .rdata(fifo_rdata),
        .empty_o(fifo_empty), .rd_datacount_o(), .underflow_o(fifo_uflow));

    // ------------------------------------------------------------ read master
    hdmi_rd_master #(.ID_W(ID_W), .RD_ID({ID_W{1'b0}}), .NBURST(NBURST), .FIFO_AW(9), .REFILL_WORDS(256)) u_rd (
        .clk(sys_clk), .rstn(sys_rstn), .enable(enable), .cal_done(cal_done),
        .frame_start(sys_frame_start), .front_base(front_base),
        .drop_req(sys_drop_req), .resume(sys_resume),
        .frame_safe(sys_frame_safe),
        .fifo_wcount(fifo_wcount), .fifo_wr(fifo_wr), .fifo_wdata(fifo_wdata),
        .m_arid(m_arid), .m_araddr(m_araddr), .m_arlen(m_arlen), .m_arsize(m_arsize), .m_arburst(m_arburst),
        .m_arvalid(m_arvalid), .m_arready(m_arready),
        .m_rdata(m_rdata), .m_rlast(m_rlast), .m_rvalid(m_rvalid), .m_rready(m_rready));

    // ------------------------------------------------------------ pixel side
    pix_reader u_reader (
        .clk(pix_clk), .rstn(pix_rstn), .de(pix_de),
        .fifo_rdata(fifo_rdata), .fifo_empty(fifo_empty), .fifo_underflow(fifo_uflow), .fifo_rd(fifo_rd),
        .resume(pix_resume), .drop_req(pix_drop_req),
        .pix_rgb(pix_rgb));
endmodule
