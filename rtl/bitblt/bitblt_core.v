// =============================================================================
// bitblt_core.v - BitBlt engine (100 MHz system / DDR AXI clock).
//
//   task FIFO -> bitblt_sched -> ReadCmd FIFO    -> read dispatch -> axi_dma_rd -> TDEST split
//                             -> ComputeCmd FIFO -> bitblt_pix <- SRC FIFO (TDEST 0) / DST FIFO (TDEST 1)
//                             -> WriteCmd FIFO   -> axi_dma_wr descriptor;  bitblt_pix -> axi_dma_wr stream
// DMAs: Y04/AXI_DMA (verilog-axi axi_dma_rd / axi_dma_wr; write DMA masks middle beats with TKEEP),
// 128-bit AXI, 256-beat bursts (never crossing 4 KiB), unaligned addresses, fixed AXI ID 0,
// descriptor tag = row index. Read dispatch: an ALPHA row entry gives two descriptors, DST then SRC.
// TDEST split: tready is the ready of the FIFO selected by this beat's TDEST.
// SRC / DST FIFO entries: {TLAST, TKEEP[15:0], TDATA[127:0]}.
// =============================================================================
module bitblt_core #(
    parameter SRC_AW = 10,               // SRC FIFO 1024 x 145
    parameter DST_AW = 9                 // DST FIFO  512 x 145
)(
    input              clk,
    input              rstn,
    input              cal_done,
    input              hw_mode,

    // task FIFO write side (fb_regs_axil)
    input              task_push,
    input      [152:0] task_data,
    output             task_full,
    output     [4:0]   task_level,

    // fb_page_ctrl
    input      [31:0]  back_base,
    input              write_ready,
    output             hw_enable,
    output             hw_wr_done,
    output             hw_write_start,

    // status
    output             busy,
    output             frame_wait,
    output     [3:0]   err,
    output     [31:0]  err_task,
    output     [9:0]   err_row,
    output     [31:0]  done_cnt,
    output     [31:0]  frame_cnt,

    // AXI4 master, read
    output     [31:0]  m_araddr,
    output     [7:0]   m_arlen,
    output     [2:0]   m_arsize,
    output     [1:0]   m_arburst,
    output             m_arvalid,
    input              m_arready,
    input      [127:0] m_rdata,
    input      [1:0]   m_rresp,
    input              m_rlast,
    input              m_rvalid,
    output             m_rready,
    // AXI4 master, write
    output     [31:0]  m_awaddr,
    output     [7:0]   m_awlen,
    output     [2:0]   m_awsize,
    output     [1:0]   m_awburst,
    output             m_awvalid,
    input              m_awready,
    output     [127:0] m_wdata,
    output     [15:0]  m_wstrb,
    output             m_wlast,
    output             m_wvalid,
    input              m_wready,
    input      [1:0]   m_bresp,
    input              m_bvalid,
    output             m_bready
);
    // ------------------------------------------------------------- FIFOs and scheduler
    wire [152:0] tq_dout;  wire tq_valid, tq_pop;
    wire [79:0]  rq_data, rq_dout;  wire rq_push, rq_full, rq_valid, rq_pop;
    wire [63:0]  cq_data, cq_dout;  wire cq_push, cq_full, cq_valid, cq_pop;
    wire [49:0]  wq_data, wq_dout;  wire wq_push, wq_full, wq_valid, wq_pop;

    bb_fifo #(.W(153), .AW(4)) u_tq (.clk(clk), .rstn(rstn), .push(task_push), .din(task_data), .full(task_full),
        .pop(tq_pop), .dout(tq_dout), .valid(tq_valid), .cnt(task_level));
    bb_fifo #(.W(80), .AW(4)) u_rq (.clk(clk), .rstn(rstn), .push(rq_push), .din(rq_data), .full(rq_full),
        .pop(rq_pop), .dout(rq_dout), .valid(rq_valid), .cnt());
    bb_fifo #(.W(64), .AW(3)) u_cq (.clk(clk), .rstn(rstn), .push(cq_push), .din(cq_data), .full(cq_full),
        .pop(cq_pop), .dout(cq_dout), .valid(cq_valid), .cnt());
    bb_fifo #(.W(50), .AW(3)) u_wq (.clk(clk), .rstn(rstn), .push(wq_push), .din(wq_data), .full(wq_full),
        .pop(wq_pop), .dout(wq_dout), .valid(wq_valid), .cnt());

    wire        rd_st_valid, wr_st_valid, src_pop, dst_pop, pix_err;
    wire [3:0]  rd_st_err, wr_st_err;
    wire [9:0]  rd_st_tag, wr_st_tag, pix_err_row;

    bitblt_sched #(.SRC_DEPTH(1 << SRC_AW), .DST_DEPTH(1 << DST_AW)) u_sched (
        .clk(clk), .rstn(rstn), .cal_done(cal_done), .hw_mode(hw_mode),
        .task_dout(tq_dout), .task_valid(tq_valid), .task_pop(tq_pop),
        .rq_push(rq_push), .rq_data(rq_data), .rq_full(rq_full),
        .cq_push(cq_push), .cq_data(cq_data), .cq_full(cq_full),
        .wq_push(wq_push), .wq_data(wq_data), .wq_full(wq_full),
        .rd_status(rd_st_valid), .rd_status_err(rd_st_err), .rd_status_row(rd_st_tag),
        .wr_status(wr_st_valid), .wr_status_err(wr_st_err), .wr_status_row(wr_st_tag),
        .src_pop(src_pop), .dst_pop(dst_pop), .pix_err(pix_err), .pix_err_row(pix_err_row),
        .back_base(back_base), .write_ready(write_ready),
        .hw_enable(hw_enable), .hw_wr_done(hw_wr_done), .hw_write_start(hw_write_start),
        .busy(busy), .frame_wait(frame_wait), .err(err), .err_task(err_task), .err_row(err_row),
        .done_cnt(done_cnt), .frame_cnt(frame_cnt));

    // ------------------------------------------------------------- read dispatch
    reg  second;                                  // ALPHA entry: DST descriptor accepted, SRC next
    wire two      = rq_dout[79];
    wire rd_dst   = two && !second;
    wire rd_ready;
    assign rq_pop = rq_valid && rd_ready && !rd_dst;
    always @(posedge clk or negedge rstn)
        if (!rstn) second <= 1'b0;
        else if (rq_valid && rd_ready) second <= rd_dst;

    wire [127:0] r_tdata;  wire [15:0] r_tkeep;  wire r_tvalid, r_tready, r_tlast, r_tdest;

    axi_dma_rd #(
        .AXI_DATA_WIDTH(128), .AXI_ADDR_WIDTH(32), .AXI_ID_WIDTH(1), .AXI_MAX_BURST_LEN(256),
        .AXIS_LAST_ENABLE(1), .AXIS_ID_ENABLE(0), .AXIS_DEST_ENABLE(1), .AXIS_DEST_WIDTH(1),
        .AXIS_USER_ENABLE(0), .LEN_WIDTH(16), .TAG_WIDTH(10), .ENABLE_SG(0), .ENABLE_UNALIGNED(1)
    ) u_rd (
        .clk(clk), .rst(!rstn),
        .s_axis_read_desc_addr({4'd0, rd_dst ? rq_dout[49:22] : rq_dout[77:50]}),
        .s_axis_read_desc_len({4'd0, rq_dout[21:10]}), .s_axis_read_desc_tag(rq_dout[9:0]),
        .s_axis_read_desc_id(8'd0), .s_axis_read_desc_dest(rd_dst), .s_axis_read_desc_user(1'b0),
        .s_axis_read_desc_valid(rq_valid), .s_axis_read_desc_ready(rd_ready),
        .m_axis_read_desc_status_tag(rd_st_tag), .m_axis_read_desc_status_error(rd_st_err),
        .m_axis_read_desc_status_valid(rd_st_valid),
        .m_axis_read_data_tdata(r_tdata), .m_axis_read_data_tkeep(r_tkeep), .m_axis_read_data_tvalid(r_tvalid),
        .m_axis_read_data_tready(r_tready), .m_axis_read_data_tlast(r_tlast),
        .m_axis_read_data_tid(), .m_axis_read_data_tdest(r_tdest), .m_axis_read_data_tuser(),
        .m_axi_arid(), .m_axi_araddr(m_araddr), .m_axi_arlen(m_arlen), .m_axi_arsize(m_arsize),
        .m_axi_arburst(m_arburst), .m_axi_arlock(), .m_axi_arcache(), .m_axi_arprot(),
        .m_axi_arvalid(m_arvalid), .m_axi_arready(m_arready),
        .m_axi_rid(1'b0), .m_axi_rdata(m_rdata), .m_axi_rresp(m_rresp), .m_axi_rlast(m_rlast),
        .m_axi_rvalid(m_rvalid), .m_axi_rready(m_rready),
        .enable(1'b1));

    // ------------------------------------------------------------- TDEST split, SRC / DST FIFOs
    wire [144:0] src_dout, dst_dout;  wire src_full, dst_full, src_valid, dst_valid;
    assign r_tready = r_tdest ? !dst_full : !src_full;
    bb_fifo #(.W(145), .AW(SRC_AW)) u_src (.clk(clk), .rstn(rstn), .push(r_tvalid && r_tready && !r_tdest),
        .din({r_tlast, r_tkeep, r_tdata}), .full(src_full), .pop(src_pop), .dout(src_dout), .valid(src_valid), .cnt());
    bb_fifo #(.W(145), .AW(DST_AW)) u_dst (.clk(clk), .rstn(rstn), .push(r_tvalid && r_tready && r_tdest),
        .din({r_tlast, r_tkeep, r_tdata}), .full(dst_full), .pop(dst_pop), .dout(dst_dout), .valid(dst_valid), .cnt());

    // ------------------------------------------------------------- pixel unit
    wire [127:0] w_tdata;  wire [15:0] w_tkeep;  wire w_tlast, w_tvalid, w_tready;
    bitblt_pix u_pix (.clk(clk), .rstn(rstn),
        .cq_dout(cq_dout), .cq_valid(cq_valid), .cq_pop(cq_pop),
        .src_dout(src_dout), .src_valid(src_valid), .src_pop(src_pop),
        .dst_dout(dst_dout), .dst_valid(dst_valid), .dst_pop(dst_pop),
        .m_tdata(w_tdata), .m_tkeep(w_tkeep), .m_tlast(w_tlast), .m_tvalid(w_tvalid), .m_tready(w_tready),
        .pix_err(pix_err), .pix_err_row(pix_err_row));

    // ------------------------------------------------------------- write DMA
    wire wr_ready;
    assign wq_pop = wq_valid && wr_ready;
    axi_dma_wr #(
        .AXI_DATA_WIDTH(128), .AXI_ADDR_WIDTH(32), .AXI_ID_WIDTH(1), .AXI_MAX_BURST_LEN(256),
        .AXIS_LAST_ENABLE(1), .AXIS_ID_ENABLE(0), .AXIS_DEST_ENABLE(0), .AXIS_USER_ENABLE(0),
        .LEN_WIDTH(16), .TAG_WIDTH(10), .ENABLE_SG(0), .ENABLE_UNALIGNED(1)
    ) u_wr (
        .clk(clk), .rst(!rstn),
        .s_axis_write_desc_addr({4'd0, wq_dout[49:22]}), .s_axis_write_desc_len({4'd0, wq_dout[21:10]}),
        .s_axis_write_desc_tag(wq_dout[9:0]), .s_axis_write_desc_valid(wq_valid), .s_axis_write_desc_ready(wr_ready),
        .m_axis_write_desc_status_tag(wr_st_tag), .m_axis_write_desc_status_id(), .m_axis_write_desc_status_dest(),
        .m_axis_write_desc_status_user(), .m_axis_write_desc_status_error(wr_st_err),
        .m_axis_write_desc_status_valid(wr_st_valid),
        .s_axis_write_data_tdata(w_tdata), .s_axis_write_data_tkeep(w_tkeep), .s_axis_write_data_tvalid(w_tvalid),
        .s_axis_write_data_tready(w_tready), .s_axis_write_data_tlast(w_tlast),
        .s_axis_write_data_tid(8'd0), .s_axis_write_data_tdest(8'd0), .s_axis_write_data_tuser(1'b0),
        .m_axi_awid(), .m_axi_awaddr(m_awaddr), .m_axi_awlen(m_awlen), .m_axi_awsize(m_awsize),
        .m_axi_awburst(m_awburst), .m_axi_awlock(), .m_axi_awcache(), .m_axi_awprot(),
        .m_axi_awvalid(m_awvalid), .m_axi_awready(m_awready),
        .m_axi_wdata(m_wdata), .m_axi_wstrb(m_wstrb), .m_axi_wlast(m_wlast), .m_axi_wvalid(m_wvalid),
        .m_axi_wready(m_wready),
        .m_axi_bid(1'b0), .m_axi_bresp(m_bresp), .m_axi_bvalid(m_bvalid), .m_axi_bready(m_bready),
        .enable(1'b1), .abort(1'b0));
endmodule
