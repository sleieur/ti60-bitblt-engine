// =============================================================================
// fb_regs_axil.v - Framebuffer control registers on the SoC axiA port
// (0xE1000000, 32-bit AXI4, single-beat register accesses with WSTRB).
// Double-buffer version (CPU_DOUBLE_FRAMEBUFFER_SPEC.md).
//
//  Offset Name            Access  Function
//  0x00   CONTROL         W / R0  bit0 ENABLE: write 1 = commit FB0 and request the display
//                                 start (one pulse, accepted once by fb_page_ctrl); write 0
//                                 has no effect; reads 0
//  0x04   FB0_BASE        R/W     framebuffer 0 byte address, frozen once ENABLE is accepted
//  0x08   FB1_BASE        R/W     framebuffer 1 byte address, frozen once ENABLE is accepted
//  0x0C   (reserved)      R/W     reads 0, writes ignored (former FB2_BASE)
//  0x10   FRAME_SIZE      R       constant FRAME_BYTES (write -> SLVERR, no state change)
//  0x14   STRIDE          R       constant STRIDE_BYTES (write -> SLVERR, no state change)
//  0x18   WR_DONE         W / R0  write bit0=1: one wr_done event (writer page complete)
//  0x1C   FIFO_UNDERFLOW  R/W1C   bit0 sticky, set by underflow_set
//  0x20   WRITE_READY     R       bit0: next writer page released, waiting for WRITE_START
//                                 (write -> SLVERR)
//  0x24   WRITE_START     W / R0  write bit0=1: one write_start event (writer takes the page)
//
//
// BitBlt region (TI60_BITBLT_HW_V1, bitblt_regs.h):
//  0x100  RENDER_MODE     R/W     bit0 0 = CPU, 1 = BitBlt; writable only before the display start
//                                 (!cfg_lock), with an empty queue and an idle engine
//  0x104  OP_FLAGS        R/W     [1:0] op COPY 0 / FILL 1 / KEY 2 / ALPHA 3, bit2 DST_REL, bit3 FRAME_LAST
//  0x108  SRC_ADDR        R/W     0x10C DST_ADDR (absolute mode)   0x110 SIZE  w[15:0] h[31:16]
//  0x114  SRC_STRIDE      R/W     0x118 DST_STRIDE (absolute mode)  0x11C DST_XY x[15:0] y[31:16] (relative)
//  0x120  FILL_COLOR      R/W     0x124 KEY_COLOR (RGB565, [15:0])   0x128 ALPHA [7:0]
//  0x12C  SUBMIT          W / R0  bit0=1: check the shadow registers, push one task snapshot
//                                 (SLVERR and nothing pushed if rejected, ERROR_CODE[3:0] = reason)
//  0x130  QUEUE_LEVEL     R       [7:0] queued tasks, bit8 FULL
//  0x134  STATUS          R       bit0 BUSY, 1 FRAME_WAIT, 2 ERROR, 3 CAN_SUBMIT, 4 HW_MODE
//  0x138  ERROR_CODE      R       [3:0] last submit reject reason, [11:8] execution error (sticky)
//  0x13C  LAST_ACCEPTED_ID R      tasks accepted (= id of the last one, ids start at 1)
//  0x140  LAST_DONE_ID    R       tasks completed (tasks run in order, so = id of the last one)
//  0x144  ERROR_TASK / 0x148 ERROR_ROW   R   task id / row of the execution error
//  0x14C  COMPLETED_COUNT R       same counter as 0x140
//  0x150  FRAME_COUNT     R       frames committed by the BitBlt engine
//  0x154  ERROR_CLEAR     W / R0  bit0=1: clear ERROR_CODE[3:0] (not the execution error)
//  0x158  CAPABILITY      R       0x0001_0565 (version 1, RGB565)
// The offset is decoded as {addr[23:9] != 0, addr[8:0]}: no register is mirrored inside the
// 16 MB axiA window (anything with addr[23:9] != 0 is unmapped).
//
// Commands (ENABLE / WR_DONE / WRITE_START) take effect only when the AXI write is
// accepted and WSTRB[0] is set; whether they are valid in the current state is decided
// by fb_page_ctrl (invalid ones are ignored). In BitBlt mode they are rejected (SLVERR):
// the engine drives fb_page_ctrl itself.
// Additional SLVERR cases (state unchanged): FBx_BASE write after cfg_lock,
// unmapped offset, AWLEN/ARLEN != 0 (all beats are still handshaked).
// Clock: SoC/AXI clock (100 MHz, same as the DDR AXI user clock).
//
// V5 handshake structure (after ZipCPU wb2axip easyaxil.v / skidbuffer.v, OPT_SKIDBUFFER):
//   AW, W and AR each pass through a one-entry skid buffer. AWREADY / WREADY / ARREADY
//   are "buffer empty" flags (registered, no combinational path from BREADY / RREADY).
//   A write executes on the edge where an AW and the last W beat are both available and
//   the B channel is free; BVALID rises in the next cycle. A read captures RDATA on the
//   edge where the AR is available and the R channel is free. With BREADY / RREADY high:
//   one write or one read per clock, B / R one cycle after the handshake.
//   (V4: register write one cycle after the handshake, B two cycles after it, READY low
//   until B / R completed -> one write per 3 clocks, one read per 2 clocks.)
// =============================================================================
module fb_regs_axil #(
    parameter [31:0] FRAME_BYTES  = 32'h001C_2000,   // 1280 x 720 x 2 (RGB565)
    parameter [31:0] STRIDE_BYTES = 32'd2560,        // 1280 x 2
    parameter        ID_W         = 8,              //AXI 的 ARID、RID 等 ID 信号宽度为 8 bit，
    parameter [27:0] RD_LO        = 28'h100_0000,   // BitBlt source window  [RD_LO, DDR_TOP)
    parameter [27:0] WR_LO        = 28'h200_0000,   // absolute destination window [WR_LO, DDR_TOP)
    parameter [28:0] DDR_TOP      = 29'h1000_0000,  // 256 MiB, 28-bit controller address
    parameter        H_ACT        = 1280,
    parameter        V_ACT        = 720,
    parameter        TQ_AW        = 4               // task FIFO depth 2**TQ_AW
)(
    input                 clk,
    input                 rstn,

    // AXI4 slave (axiA)
    input      [ID_W-1:0] s_awid,
    input      [31:0]     s_awaddr,
    input      [7:0]      s_awlen,
    input                 s_awvalid,
    output                s_awready,
    input      [31:0]     s_wdata,
    input      [3:0]      s_wstrb,
    input                 s_wlast,
    input                 s_wvalid,
    output                s_wready,
    output reg [ID_W-1:0] s_bid,
    output reg [1:0]      s_bresp,
    output reg            s_bvalid,
    input                 s_bready,
    input      [ID_W-1:0] s_arid,
    input      [31:0]     s_araddr,
    input      [7:0]      s_arlen,
    input                 s_arvalid,
    output                s_arready,
    output reg [ID_W-1:0] s_rid,
    output reg [31:0]     s_rdata,
    output reg [1:0]      s_rresp,
    output reg            s_rlast,
    output reg            s_rvalid,
    input                 s_rready,

    // to / from the framebuffer logic
    output reg [31:0]     fb0_base,
    output reg [31:0]     fb1_base,
    output reg            enable_cmd,       // one-cycle pulses
    output reg            wr_done,
    output reg            write_start,
    input                 write_ready,      // from fb_page_ctrl
    input                 cfg_lock,         // from fb_page_ctrl: bases frozen
    input                 underflow_set,    // one-cycle pulse from the read path

    // BitBlt engine
    output reg            render_mode,      // 1 = BitBlt drives fb_page_ctrl
    output                task_push,        // one task snapshot into the task FIFO
    output     [152:0]    task_data,        // layout: see bitblt_sched.v
    input                 task_full,
    input      [TQ_AW:0]  task_level,
    input                 bb_busy,          // a task is executing or tasks are queued
    input                 bb_frame_wait,    // frame committed, waiting for the next back page
    input      [3:0]      bb_err,           // execution error code (sticky, 0 = none)
    input      [31:0]     bb_err_task,
    input      [9:0]      bb_err_row,
    input      [31:0]     bb_done_cnt,
    input      [31:0]     bb_frame_cnt
);
    localparam [1:0] OKAY = 2'b00, SLVERR = 2'b10;

    localparam [9:0] A_CONTROL = 10'h00, A_FB0 = 10'h04, A_FB1 = 10'h08, A_RSVD = 10'h0C,
                     A_FSIZE   = 10'h10, A_STRIDE = 10'h14, A_WRDONE = 10'h18, A_UNDERFLOW = 10'h1C,
                     A_WREADY  = 10'h20, A_WSTART = 10'h24,
                     A_MODE    = 10'h100, A_OPF  = 10'h104, A_SRC   = 10'h108, A_DST   = 10'h10C,
                     A_SIZE    = 10'h110, A_SSTR = 10'h114, A_DSTR  = 10'h118, A_XY    = 10'h11C,
                     A_FILL    = 10'h120, A_KEY  = 10'h124, A_ALPHA = 10'h128, A_SUBMIT = 10'h12C,
                     A_QLEVEL  = 10'h130, A_STAT = 10'h134, A_ECODE = 10'h138, A_ACC   = 10'h13C,
                     A_LDONE   = 10'h140, A_ETASK = 10'h144, A_EROW = 10'h148, A_DONE  = 10'h14C,
                     A_FRAMES  = 10'h150, A_ECLR = 10'h154, A_CAP   = 10'h158;
    // submit reject reasons (ERROR_CODE[3:0])
    localparam [3:0] R_PARAM = 4'd1, R_FULL = 4'd2, R_MODE = 4'd3, R_EXEC = 4'd4;

    reg underflow;

    // ------------------------------------------------------------- BitBlt shadow registers
    reg [31:0] opf, src, dst, size, sstr, dstr, xy, fill, key, alpha;
    reg [3:0]  rej;                 // last submit reject reason
    reg [31:0] accepted;            // tasks accepted

    // SUBMIT check of the shadow registers (combinational; all sums wide enough not to wrap)
    wire [1:0]  op    = opf[1:0];
    wire        rel   = opf[2];
    wire [15:0] w     = size[15:0];
    wire [15:0] h     = size[31:16];
    wire [16:0] rbytes = {w, 1'b0};
    wire        empty = (w == 16'd0) || (h == 16'd0);
    wire [32:0] src_end = src + (h - 16'd1) * sstr[15:0] + rbytes;
    wire [32:0] dst_end = dst + (h - 16'd1) * dstr[15:0] + rbytes;
    wire fmt_ok = (opf[31:4] == 28'd0) && (w <= H_ACT) && (h <= V_ACT);
    wire src_ok = (op == 2'd1) ||                                         // FILL reads nothing
                  (!src[0] && !sstr[0] && (sstr[31:16] == 16'd0) && (sstr[16:0] >= rbytes) &&
                   (src >= RD_LO) && (src_end <= DDR_TOP));
    wire dst_ok = rel ? ({1'b0, xy[15:0]} + w <= H_ACT) && ({1'b0, xy[31:16]} + h <= V_ACT)
                      : (!dst[0] && !dstr[0] && (dstr[31:16] == 16'd0) && (dstr[16:0] >= rbytes) &&
                         (dst >= WR_LO) && (dst_end <= DDR_TOP));
    wire [3:0] sub_rej = !render_mode   ? R_MODE  :
                         (bb_err != 0)  ? R_EXEC  :
                         !(fmt_ok && (empty || (src_ok && dst_ok))) ? R_PARAM :
                         task_full      ? R_FULL  : 4'd0;
    // relative destination: byte offset in the back page, stride = one frame line
    wire [27:0] rel_off = xy[31:16] * STRIDE_BYTES + {xy[15:0], 1'b0};
    assign task_data = {op, rel, opf[3], src[27:0], rel ? rel_off : dst[27:0], w[10:0], h[9:0],
                        sstr[15:0], rel ? STRIDE_BYTES[15:0] : dstr[15:0], fill[15:0], key[15:0], alpha[7:0]};

    function [31:0] merge(input [31:0] old_v, input [31:0] new_v, input [3:0] strb);//通常描述组合计算，但调用它的整体电路可以是时序逻辑。有返回值
        merge = { strb[3] ? new_v[31:24] : old_v[31:24],
                  strb[2] ? new_v[23:16] : old_v[23:16],
                  strb[1] ? new_v[15:8]  : old_v[15:8],
                  strb[0] ? new_v[7:0]   : old_v[7:0] };
    endfunction

    function [31:0] rd_value(input [9:0] a);
        case (a)
            A_FB0:       rd_value = fb0_base;
            A_FB1:       rd_value = fb1_base;
            A_FSIZE:     rd_value = FRAME_BYTES;
            A_STRIDE:    rd_value = STRIDE_BYTES;
            A_UNDERFLOW: rd_value = {31'd0, underflow};
            A_WREADY:    rd_value = {31'd0, write_ready};
            A_MODE:      rd_value = {31'd0, render_mode};
            A_OPF:       rd_value = opf;
            A_SRC:       rd_value = src;
            A_DST:       rd_value = dst;
            A_SIZE:      rd_value = size;
            A_SSTR:      rd_value = sstr;
            A_DSTR:      rd_value = dstr;
            A_XY:        rd_value = xy;
            A_FILL:      rd_value = fill;
            A_KEY:       rd_value = key;
            A_ALPHA:     rd_value = alpha;
            A_QLEVEL:    rd_value = {23'd0, task_full, {(7-TQ_AW){1'b0}}, task_level};
            A_STAT:      rd_value = {27'd0, render_mode, render_mode && (bb_err == 0) && !task_full,
                                     bb_err != 0, bb_frame_wait, bb_busy};
            A_ECODE:     rd_value = {20'd0, bb_err, 4'd0, rej};
            A_ACC:       rd_value = accepted;
            A_LDONE,
            A_DONE:      rd_value = bb_done_cnt;
            A_ETASK:     rd_value = bb_err_task;
            A_EROW:      rd_value = {22'd0, bb_err_row};
            A_FRAMES:    rd_value = bb_frame_cnt;
            A_CAP:       rd_value = 32'h0001_0565;
            default:     rd_value = 32'd0;          // CONTROL, reserved, WR_DONE, WRITE_START, SUBMIT, ERROR_CLEAR, unmapped
        endcase
    endfunction

    function rd_mapped(input [9:0] a);
        rd_mapped = (a == A_CONTROL) || (a == A_FB0) || (a == A_FB1) || (a == A_RSVD) ||
                    (a == A_FSIZE) || (a == A_STRIDE) || (a == A_WRDONE) || (a == A_UNDERFLOW) ||
                    (a == A_WREADY) || (a == A_WSTART) ||
                    (a[9:8] == 2'b01 && a[1:0] == 2'b00 && a[7:0] <= A_CAP[7:0]);   // 0x100..0x158
    endfunction
//判断 CPU 读取的寄存器地址是否有效（是否在寄存器地址映射表中）。
//这是一个地址合法性判断函数，返回 1 表示地址已映射，返回 0 表示未映射。
//注意：它只判断地址是否存在，不判断该寄存器是否允许读取。

    // ------------------------------------------------------------- write path
    // AW and W are accepted independently (W may arrive before AW), each into a skid
    // buffer. An incoming beat that can be used in the same cycle passes straight through;
    // otherwise it is held and READY drops for the next cycle.

//缓存主要用于处理 AW 和 W 不同步到达的三种情况：
//
//AW 先到：暂存 AW，等待 W。
//W 先到：暂存 W，等待 AW。
//AW、W 同时到：直接执行写入，不需要暂存（B 通道可用时）。
//
//补充一点： 正常单 Beat 写入时，AW 和 W 不会都因为彼此不同步而暂存；但如果 B 通道堵塞，两者也可能都被暂存。
//
//总结：缓存既解决 AW/W 到达不同步的问题，也用于应对下游反压。
    reg            awq_v;
    reg [ID_W-1:0] awq_id;
    reg [9:0]      awq_off;
    reg [7:0]      awq_len;
    reg            wq_v;
    reg [31:0]     wq_data;
    reg [3:0]      wq_strb;
    reg            wq_last;

    assign s_awready = !awq_v;
    assign s_wready  = !wq_v;

    // AW / W seen by the write logic: the held entry, else the incoming one
    wire            aw_v   = awq_v || s_awvalid;
    wire [ID_W-1:0] aw_id  = awq_v ? awq_id  : s_awid;
    wire [9:0]      aw_off = awq_v ? awq_off : {s_awaddr[23:9] != 0, s_awaddr[8:0]};
    wire [7:0]      aw_len = awq_v ? awq_len : s_awlen;
    wire            w_v    = wq_v || s_wvalid;
    wire [31:0]     w_data = wq_v ? wq_data : s_wdata;
    wire [3:0]      w_strb = wq_v ? wq_strb : s_wstrb;
    wire            w_last = wq_v ? wq_last : s_wlast;

    wire b_free  = !s_bvalid || s_bready;                 // B slot free this cycle
    wire wr_fire = aw_v && w_v && w_last && b_free;       // execute the write on this edge
    wire aw_take = wr_fire;
    wire w_take  = w_v && (!w_last || wr_fire);           // non-last beats (bursts) are just consumed
    // SUBMIT pushes on the single edge that executes the write (B is free then): one push per access
    assign task_push = wr_fire && (aw_off == A_SUBMIT) && (aw_len == 8'd0) && w_strb[0] && w_data[0] &&
                       (sub_rej == 4'd0);

//控制寄存器从机，只支持单 Beat 写入

    reg  [1:0] wr_resp;

    always @(*) begin
        if (aw_len != 8'd0) wr_resp = SLVERR;
        else case (aw_off)
            A_RSVD, A_UNDERFLOW:              wr_resp = OKAY;
            A_CONTROL, A_WRDONE, A_WSTART:    wr_resp = render_mode ? SLVERR : OKAY;   // BitBlt mode: engine only
            A_FB0, A_FB1:                     wr_resp = cfg_lock ? SLVERR : OKAY;
            A_MODE:                           wr_resp = (cfg_lock || bb_busy || task_level != 0) ? SLVERR : OKAY;
            A_OPF, A_SRC, A_DST, A_SIZE, A_SSTR, A_DSTR, A_XY, A_FILL, A_KEY, A_ALPHA, A_ECLR:
                                              wr_resp = OKAY;
            A_SUBMIT:                         wr_resp = (w_strb[0] && w_data[0] && sub_rej != 4'd0) ? SLVERR : OKAY;
            default:                          wr_resp = SLVERR;   // FRAME_SIZE, STRIDE, WRITE_READY, BitBlt status, unmapped
        endcase
    end

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            awq_v <= 1'b0; awq_id <= {ID_W{1'b0}}; awq_off <= 10'd0; awq_len <= 8'd0;
            wq_v <= 1'b0; wq_data <= 32'd0; wq_strb <= 4'd0; wq_last <= 1'b0;
            s_bvalid <= 1'b0; s_bresp <= OKAY; s_bid <= {ID_W{1'b0}};
            fb0_base <= 32'd0; fb1_base <= 32'd0;
            enable_cmd <= 1'b0; wr_done <= 1'b0; write_start <= 1'b0; underflow <= 1'b0;
            render_mode <= 1'b0; rej <= 4'd0; accepted <= 32'd0;
            opf <= 32'd0; src <= 32'd0; dst <= 32'd0; size <= 32'd0; sstr <= 32'd0;
            dstr <= 32'd0; xy <= 32'd0; fill <= 32'd0; key <= 32'd0; alpha <= 32'd0;
        end else begin
            enable_cmd <= 1'b0; wr_done <= 1'b0; write_start <= 1'b0;//脉冲信号

//收到但暂时用不了 → 缓存；收到后能立即使用 → 直接使用；缓存已经使用 → 清除有效标志。
            // skid buffers: hold an accepted beat that is not used in the same cycle
            if (s_awvalid && s_awready && !aw_take) begin
                awq_v <= 1'b1; awq_id <= s_awid; awq_off <= {s_awaddr[23:9] != 0, s_awaddr[8:0]}; awq_len <= s_awlen;
            end else if (aw_take)
                awq_v <= 1'b0;
            if (s_wvalid && s_wready && !w_take) begin
                wq_v <= 1'b1; wq_data <= s_wdata; wq_strb <= s_wstrb; wq_last <= s_wlast;
            end else if (w_take)
                wq_v <= 1'b0;

            // register write and B response
            if (wr_fire) begin
                if (wr_resp == OKAY) begin
                    case (aw_off)
                        A_CONTROL:   if (w_strb[0] && w_data[0]) enable_cmd  <= 1'b1;
                        A_FB0:       fb0_base <= merge(fb0_base, w_data, w_strb);
                        A_FB1:       fb1_base <= merge(fb1_base, w_data, w_strb);
                        A_WRDONE:    if (w_strb[0] && w_data[0]) wr_done     <= 1'b1;
                        A_UNDERFLOW: if (w_strb[0] && w_data[0]) underflow   <= 1'b0;
                        A_WSTART:    if (w_strb[0] && w_data[0]) write_start <= 1'b1;
                        A_MODE:      if (w_strb[0]) render_mode <= w_data[0];
                        A_OPF:       opf   <= merge(opf,   w_data, w_strb);
                        A_SRC:       src   <= merge(src,   w_data, w_strb);
                        A_DST:       dst   <= merge(dst,   w_data, w_strb);
                        A_SIZE:      size  <= merge(size,  w_data, w_strb);
                        A_SSTR:      sstr  <= merge(sstr,  w_data, w_strb);
                        A_DSTR:      dstr  <= merge(dstr,  w_data, w_strb);
                        A_XY:        xy    <= merge(xy,    w_data, w_strb);
                        A_FILL:      fill  <= merge(fill,  w_data, w_strb);
                        A_KEY:       key   <= merge(key,   w_data, w_strb);
                        A_ALPHA:     alpha <= merge(alpha, w_data, w_strb);
                        A_SUBMIT:    if (task_push) accepted <= accepted + 32'd1;
                        A_ECLR:      if (w_strb[0] && w_data[0]) rej <= 4'd0;
                        default: ;                                  // reserved 0x0C: ignored
                    endcase
                end else if (aw_off == A_SUBMIT && aw_len == 8'd0) rej <= sub_rej;
                s_bvalid <= 1'b1; s_bresp <= wr_resp; s_bid <= aw_id;
            end else if (s_bready)
                s_bvalid <= 1'b0;

            // set wins over a simultaneous clear
            if (underflow_set) underflow <= 1'b1;
        end
    end

    // ------------------------------------------------------------- read path
    // AR skid buffer. rd_fire: an AR is available and the R channel is free (no beat
    // pending, or the last beat of the current burst is accepted now). RDATA is captured
    // on that edge; bursts (SLVERR) repeat the beat ARLEN+1 times.
    reg            arq_v;
    reg [ID_W-1:0] arq_id;
    reg [7:0]      arq_len;
    reg [9:0]      arq_off;
    reg [9:0]      r_off;
    reg [7:0]      r_left;   // beats remaining after the current one

    assign s_arready = !arq_v;

    wire            ar_v   = arq_v || s_arvalid;
    wire [ID_W-1:0] ar_id  = arq_v ? arq_id  : s_arid;
    wire [9:0]      ar_off = arq_v ? arq_off : {s_araddr[23:9] != 0, s_araddr[8:0]};
    wire [7:0]      ar_len = arq_v ? arq_len : s_arlen;

    wire r_free  = !s_rvalid || (s_rready && s_rlast);
    wire rd_fire = ar_v && r_free;

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            arq_v <= 1'b0; arq_id <= {ID_W{1'b0}}; arq_off <= 10'd0; arq_len <= 8'd0;
            s_rvalid <= 1'b0; s_rlast <= 1'b0;
            s_rid <= {ID_W{1'b0}}; s_rdata <= 32'd0; s_rresp <= OKAY;
            r_off <= 10'd0; r_left <= 8'd0;
        end else begin
            if (s_arvalid && s_arready && !rd_fire) begin
                arq_v <= 1'b1; arq_id <= s_arid; arq_off <= {s_araddr[23:9] != 0, s_araddr[8:0]}; arq_len <= s_arlen;
            end else if (rd_fire)
                arq_v <= 1'b0;

            if (rd_fire) begin
                s_rvalid <= 1'b1;
                s_rid    <= ar_id; r_off <= ar_off; r_left <= ar_len;
                s_rdata  <= rd_value(ar_off);
                s_rresp  <= ((ar_len != 8'd0) || !rd_mapped(ar_off)) ? SLVERR : OKAY;
                s_rlast  <= (ar_len == 8'd0);
            end else if (s_rvalid && s_rready) begin
                if (s_rlast) begin
                    s_rvalid <= 1'b0; s_rlast <= 1'b0;
                end else begin
                    r_left  <= r_left - 8'd1;// 剩余Beat数减1
                    s_rlast <= (r_left == 8'd1);// 下一拍是不是最后一拍
                    s_rdata <= rd_value(r_off);// 准备下一拍的数据
                    //但你的控制寄存器模块只支持单 Beat（ARLEN=0），所以正常 CPU 读取时，根本不会执行这个 else。
                    //它只是为了处理非法多 Beat 请求，在返回 SLVERR 的同时把整个 Burst 响应完成。
                end
            end
        end
    end
endmodule





