// =============================================================================
// fb_hdmi_subsys.v - double-buffer framebuffer display subsystem (CPU_DOUBLE_FRAMEBUFFER_SPEC.md)
//   fb_regs_axil   : axiA control registers (sys clock)
//   fb_page_ctrl   : FB0/FB1 ownership: front/back, startup, WR_DONE / WRITE_READY /
//                    WRITE_START handoff, swaps only at safe frame boundaries (sys clock)
//   fb_read_path   : HDMI AXI read master + 8 KiB async FIFO, 128-bit in / 16-bit out (sys -> pixel clock)
//   bitblt_core    : BitBlt engine (task FIFO, scheduler, read / write DMA, pixel unit), TI60_BITBLT_HW_V1
//   axi_mux_3to1   : HDMI (S0) + SoC io_ddrA (S1) + BitBlt (S2) -> DDR controller
//                    (read priority HDMI > CPU > BitBlt, write CPU > BitBlt)
// fb_page_ctrl commands: CPU (fb_regs_axil) in CPU mode, BitBlt engine in BitBlt mode (the other
// source is always 0 in each mode: fb_regs_axil rejects the CPU page commands in BitBlt mode).
//   video_timing   : video timing (positive HS/VS), 720p60 by default
// Framebuffer format (V6): RGB565, 2 bytes per pixel, 8 pixels per 128-bit beat.
// Pixel outputs (pix_rgb/hs/vs/de) are aligned and registered in the pixel clock domain.
// swap_done is also delivered in the pixel domain (pix_swap_done) for FPS counting.
// =============================================================================
module fb_hdmi_subsys #(
    parameter H_ACT = 1280, parameter H_FP = 110, parameter H_SYNC = 40, parameter H_BP = 220,
    parameter V_ACT = 720,  parameter V_FP = 5,   parameter V_SYNC = 5,  parameter V_BP = 20,
    parameter M_ID_W = 4,
    parameter S1_ID_W = 8
)(
    input                 sys_clk,
    input                 sys_rstn,
    input                 pix_clk,
    input                 pix_rstn,          // released only while the HDMI PLL is locked
    input                 cal_done,

    // axiA (SoC AXI4 master, 32-bit) -> registers
    input  [7:0]          a_awid,   input [31:0] a_awaddr, input [7:0] a_awlen, input a_awvalid, output a_awready,
    input  [31:0]         a_wdata,  input [3:0]  a_wstrb,  input a_wlast, input a_wvalid, output a_wready,
    output [7:0]          a_bid,    output [1:0] a_bresp,  output a_bvalid, input a_bready,
    input  [7:0]          a_arid,   input [31:0] a_araddr, input [7:0] a_arlen, input a_arvalid, output a_arready,
    output [7:0]          a_rid,    output [31:0] a_rdata, output [1:0] a_rresp, output a_rlast, output a_rvalid, input a_rready,

    // SoC io_ddrA (S1)
    input  [S1_ID_W-1:0]  s1_awid, input [31:0] s1_awaddr, input [7:0] s1_awlen, input [2:0] s1_awsize, input [1:0] s1_awburst,
    input                 s1_awlock, input [3:0] s1_awcache, input [2:0] s1_awprot, input s1_awvalid, output s1_awready,
    input  [127:0]        s1_wdata, input [15:0] s1_wstrb, input s1_wlast, input s1_wvalid, output s1_wready,
    output [S1_ID_W-1:0]  s1_bid, output [1:0] s1_bresp, output s1_bvalid, input s1_bready,
    input  [S1_ID_W-1:0]  s1_arid, input [31:0] s1_araddr, input [7:0] s1_arlen, input [2:0] s1_arsize, input [1:0] s1_arburst,
    input                 s1_arlock, input [3:0] s1_arcache, input [2:0] s1_arprot, input s1_arvalid, output s1_arready,
    output [S1_ID_W-1:0]  s1_rid, output [127:0] s1_rdata, output [1:0] s1_rresp, output s1_rlast, output s1_rvalid, input s1_rready,

    // DDR controller AXI port
    output [M_ID_W-1:0]   m_awid, output [31:0] m_awaddr, output [7:0] m_awlen, output [2:0] m_awsize, output [1:0] m_awburst,
    output                m_awlock, output [3:0] m_awcache, output [2:0] m_awprot, output m_awvalid, input m_awready,
    output [127:0]        m_wdata, output [15:0] m_wstrb, output m_wlast, output m_wvalid, input m_wready,
    input  [M_ID_W-1:0]   m_bid, input [1:0] m_bresp, input m_bvalid, output m_bready,
    output [M_ID_W-1:0]   m_arid, output [31:0] m_araddr, output [7:0] m_arlen, output [2:0] m_arsize, output [1:0] m_arburst,
    output                m_arlock, output [3:0] m_arcache, output [2:0] m_arprot, output m_arvalid, input m_arready,
    input  [M_ID_W-1:0]   m_rid, input [127:0] m_rdata, input [1:0] m_rresp, input m_rlast, input m_rvalid, output m_rready,

    // pixel domain outputs
    output     [23:0]     pix_rgb,
    output reg            pix_hs,
    output reg            pix_vs,
    output reg            pix_de,
    output                pix_swap_done,     // one pulse per displayed new page
    output                pix_frame_start
);
    localparam PIX_BYTES = 2;                        // RGB565
    localparam NBURST = H_ACT * V_ACT * PIX_BYTES / 4096;   // 720p: 450//这个公式要求帧大小能被 4096 整除，否则整数除法会舍去余数。

    wire [31:0] fb0_base, fb1_base, front_base;
    wire        enable_cmd, wr_done, write_start, cfg_lock, write_ready, display_en;
    wire        sys_frame_start, sys_frame_safe, sys_underflow, swap_done;
    wire [31:0] back_base;
    // BitBlt
    wire         render_mode, task_push, task_full, bb_busy, bb_frame_wait;
    wire [152:0] task_data;
    wire [4:0]   task_level;
    wire [3:0]   bb_err;
    wire [31:0]  bb_err_task, bb_done_cnt, bb_frame_cnt;
    wire [9:0]   bb_err_row;
    wire         hw_enable, hw_wr_done, hw_write_start;

    fb_regs_axil #(.FRAME_BYTES(H_ACT * V_ACT * PIX_BYTES), .STRIDE_BYTES(H_ACT * PIX_BYTES), .ID_W(8)) u_regs (
        .clk(sys_clk), .rstn(sys_rstn),
        .s_awid(a_awid), .s_awaddr(a_awaddr), .s_awlen(a_awlen), .s_awvalid(a_awvalid), .s_awready(a_awready),
        .s_wdata(a_wdata), .s_wstrb(a_wstrb), .s_wlast(a_wlast), .s_wvalid(a_wvalid), .s_wready(a_wready),
        .s_bid(a_bid), .s_bresp(a_bresp), .s_bvalid(a_bvalid), .s_bready(a_bready),
        .s_arid(a_arid), .s_araddr(a_araddr), .s_arlen(a_arlen), .s_arvalid(a_arvalid), .s_arready(a_arready),
        .s_rid(a_rid), .s_rdata(a_rdata), .s_rresp(a_rresp), .s_rlast(a_rlast), .s_rvalid(a_rvalid), .s_rready(a_rready),
        .fb0_base(fb0_base), .fb1_base(fb1_base),
        .enable_cmd(enable_cmd), .wr_done(wr_done), .write_start(write_start),
        .write_ready(write_ready), .cfg_lock(cfg_lock), .underflow_set(sys_underflow),
        .render_mode(render_mode), .task_push(task_push), .task_data(task_data),
        .task_full(task_full), .task_level(task_level), .bb_busy(bb_busy), .bb_frame_wait(bb_frame_wait),
        .bb_err(bb_err), .bb_err_task(bb_err_task), .bb_err_row(bb_err_row),
        .bb_done_cnt(bb_done_cnt), .bb_frame_cnt(bb_frame_cnt));

    fb_page_ctrl u_page (
        .clk(sys_clk), .rstn(sys_rstn),
        .enable_cmd(enable_cmd | hw_enable), .wr_done(wr_done | hw_wr_done), .write_start(write_start | hw_write_start),
        .frame_start(sys_frame_start), .frame_safe(sys_frame_safe),
        .fb0_base(fb0_base), .fb1_base(fb1_base),
        .write_ready(write_ready),
        .display_en(display_en), .cfg_lock(cfg_lock), .swap_done(swap_done), .front_base(front_base),
        .back_base(back_base));

    fb_cdc_pulse u_cdc_swap (.src_clk(sys_clk), .src_rstn(sys_rstn), .src_pulse(swap_done),
                             .dst_clk(pix_clk), .dst_rstn(pix_rstn), .dst_pulse(pix_swap_done));

    // ------------------------------------------------------------ timing
    wire tg_hs, tg_vs, tg_de;
    video_timing #(.H_ACT(H_ACT), .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP),
                   .V_ACT(V_ACT), .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP)) u_timing (
        .clk(pix_clk), .rstn(pix_rstn), .hs(tg_hs), .vs(tg_vs), .de(tg_de));

    // ------------------------------------------------------------ read path
    wire [3:0]   h_arid;  wire [31:0] h_araddr; wire [7:0] h_arlen; wire [2:0] h_arsize; wire [1:0] h_arburst;
    wire         h_arvalid, h_arready, h_rlast, h_rvalid, h_rready;
    wire [127:0] h_rdata;
    wire [23:0]  rp_rgb;

    fb_read_path #(.ID_W(4), .NBURST(NBURST)) u_read (
        .sys_clk(sys_clk), .sys_rstn(sys_rstn), .pix_clk(pix_clk), .pix_rstn(pix_rstn),
        .enable(display_en), .cal_done(cal_done), .front_base(front_base),
        .sys_frame_start(sys_frame_start), .sys_underflow(sys_underflow), .sys_frame_safe(sys_frame_safe),
        .pix_vs(tg_vs), .pix_de(tg_de), .pix_rgb(rp_rgb), .pix_frame_start(pix_frame_start),
        .m_arid(h_arid), .m_araddr(h_araddr), .m_arlen(h_arlen), .m_arsize(h_arsize), .m_arburst(h_arburst),
        .m_arvalid(h_arvalid), .m_arready(h_arready),
        .m_rdata(h_rdata), .m_rlast(h_rlast), .m_rvalid(h_rvalid), .m_rready(h_rready));

    // align timing with the pixel: rp_rgb comes from the FIFO output register (pix_reader) and
    // is valid 1 clock after tg_de, so hs/vs/de are delayed by 1 clock
    assign pix_rgb = rp_rgb;
    always @(posedge pix_clk or negedge pix_rstn)
        if (!pix_rstn) begin
            pix_hs <= 1'b0; pix_vs <= 1'b0; pix_de <= 1'b0;
        end else begin
            pix_hs <= tg_hs; pix_vs <= tg_vs; pix_de <= tg_de;
        end

    // ------------------------------------------------------------ BitBlt engine
    wire [31:0]  b_araddr, b_awaddr;  wire [7:0] b_arlen, b_awlen;  wire [2:0] b_arsize, b_awsize;
    wire [1:0]   b_arburst, b_awburst, b_rresp, b_bresp;
    wire         b_arvalid, b_arready, b_rlast, b_rvalid, b_rready, b_awvalid, b_awready;
    wire         b_wlast, b_wvalid, b_wready, b_bvalid, b_bready;
    wire [127:0] b_rdata, b_wdata;  wire [15:0] b_wstrb;

    bitblt_core u_bitblt (
        .clk(sys_clk), .rstn(sys_rstn), .cal_done(cal_done), .hw_mode(render_mode),
        .task_push(task_push), .task_data(task_data), .task_full(task_full), .task_level(task_level),
        .back_base(back_base), .write_ready(write_ready),
        .hw_enable(hw_enable), .hw_wr_done(hw_wr_done), .hw_write_start(hw_write_start),
        .busy(bb_busy), .frame_wait(bb_frame_wait), .err(bb_err), .err_task(bb_err_task), .err_row(bb_err_row),
        .done_cnt(bb_done_cnt), .frame_cnt(bb_frame_cnt),
        .m_araddr(b_araddr), .m_arlen(b_arlen), .m_arsize(b_arsize), .m_arburst(b_arburst), .m_arvalid(b_arvalid),
        .m_arready(b_arready), .m_rdata(b_rdata), .m_rresp(b_rresp), .m_rlast(b_rlast), .m_rvalid(b_rvalid), .m_rready(b_rready),
        .m_awaddr(b_awaddr), .m_awlen(b_awlen), .m_awsize(b_awsize), .m_awburst(b_awburst), .m_awvalid(b_awvalid),
        .m_awready(b_awready), .m_wdata(b_wdata), .m_wstrb(b_wstrb), .m_wlast(b_wlast), .m_wvalid(b_wvalid),
        .m_wready(b_wready), .m_bresp(b_bresp), .m_bvalid(b_bvalid), .m_bready(b_bready));

    // ------------------------------------------------------------ DDR mux
    axi_mux_3to1 #(.S1_ID_W(S1_ID_W), .AW(32), .DW(128)) u_mux (
        .clk(sys_clk), .rstn(sys_rstn),
        .s0_araddr(h_araddr), .s0_arlen(h_arlen), .s0_arsize(h_arsize), .s0_arburst(h_arburst),
        .s0_arvalid(h_arvalid), .s0_arready(h_arready),
        .s0_rdata(h_rdata), .s0_rresp(), .s0_rlast(h_rlast), .s0_rvalid(h_rvalid), .s0_rready(h_rready),
        .s1_awid(s1_awid), .s1_awaddr(s1_awaddr), .s1_awlen(s1_awlen), .s1_awsize(s1_awsize), .s1_awburst(s1_awburst),
        .s1_awlock(s1_awlock), .s1_awcache(s1_awcache), .s1_awprot(s1_awprot), .s1_awvalid(s1_awvalid), .s1_awready(s1_awready),
        .s1_wdata(s1_wdata), .s1_wstrb(s1_wstrb), .s1_wlast(s1_wlast), .s1_wvalid(s1_wvalid), .s1_wready(s1_wready),
        .s1_bid(s1_bid), .s1_bresp(s1_bresp), .s1_bvalid(s1_bvalid), .s1_bready(s1_bready),
        .s1_arid(s1_arid), .s1_araddr(s1_araddr), .s1_arlen(s1_arlen), .s1_arsize(s1_arsize), .s1_arburst(s1_arburst),
        .s1_arlock(s1_arlock), .s1_arcache(s1_arcache), .s1_arprot(s1_arprot), .s1_arvalid(s1_arvalid), .s1_arready(s1_arready),
        .s1_rid(s1_rid), .s1_rdata(s1_rdata), .s1_rresp(s1_rresp), .s1_rlast(s1_rlast), .s1_rvalid(s1_rvalid), .s1_rready(s1_rready),
        .s2_awaddr(b_awaddr), .s2_awlen(b_awlen), .s2_awsize(b_awsize), .s2_awburst(b_awburst), .s2_awvalid(b_awvalid),
        .s2_awready(b_awready), .s2_wdata(b_wdata), .s2_wstrb(b_wstrb), .s2_wlast(b_wlast), .s2_wvalid(b_wvalid),
        .s2_wready(b_wready), .s2_bresp(b_bresp), .s2_bvalid(b_bvalid), .s2_bready(b_bready),
        .s2_araddr(b_araddr), .s2_arlen(b_arlen), .s2_arsize(b_arsize), .s2_arburst(b_arburst), .s2_arvalid(b_arvalid),
        .s2_arready(b_arready), .s2_rdata(b_rdata), .s2_rresp(b_rresp), .s2_rlast(b_rlast), .s2_rvalid(b_rvalid),
        .s2_rready(b_rready),
        .m_awid(m_awid), .m_awaddr(m_awaddr), .m_awlen(m_awlen), .m_awsize(m_awsize), .m_awburst(m_awburst),
        .m_awlock(m_awlock), .m_awcache(m_awcache), .m_awprot(m_awprot), .m_awvalid(m_awvalid), .m_awready(m_awready),
        .m_wdata(m_wdata), .m_wstrb(m_wstrb), .m_wlast(m_wlast), .m_wvalid(m_wvalid), .m_wready(m_wready),
        .m_bid(m_bid), .m_bresp(m_bresp), .m_bvalid(m_bvalid), .m_bready(m_bready),
        .m_arid(m_arid), .m_araddr(m_araddr), .m_arlen(m_arlen), .m_arsize(m_arsize), .m_arburst(m_arburst),
        .m_arlock(m_arlock), .m_arcache(m_arcache), .m_arprot(m_arprot), .m_arvalid(m_arvalid), .m_arready(m_arready),
        .m_rid(m_rid), .m_rdata(m_rdata), .m_rresp(m_rresp), .m_rlast(m_rlast), .m_rvalid(m_rvalid), .m_rready(m_rready));
endmodule
