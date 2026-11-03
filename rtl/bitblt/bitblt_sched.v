// =============================================================================
// bitblt_sched.v - BitBlt task scheduler, row issue, completion tracking and frame control.
//
// Task FIFO entry (fb_regs_axil.task_data, 153 bits, MSB first):
//   {op[1:0], rel, frame_last, src[27:0], dst[27:0], w[10:0], h[9:0],
//    src_stride[15:0], dst_stride[15:0], fill[15:0], key[15:0], alpha[7:0]}
//   rel: dst = byte offset inside the back page (stride 2560); else absolute DDR address.
//
// Tasks run strictly one after the other: a task is popped only when the previous one is complete.
// Inside a task, rows are issued back to back (no wait for any write status) whenever
//   - the read, compute and write command FIFOs each have a free entry (one entry per row in each,
//     pushed in the same cycle -> a row group is always complete), and
//   - the SRC / DST data FIFOs have room for the whole row on top of everything already reserved
//     (credit: +N at issue, -1 per beat consumed by the pixel unit; N = beats of the row).
// Read commands: FILL none, COPY / KEY one (SRC), ALPHA one entry -> two descriptors, DST then SRC.
// Task complete: all rows issued, every read and write descriptor status returned, all reserved
// input beats consumed (so the pixel pipeline and both data FIFOs are empty for this task).
// DMA status pulses have no ready and are counted directly. An error (DMA status != 0 or a row
// length / TLAST mismatch reported by the pixel unit) is sticky: no further rows are issued, the
// task never completes and the frame is not committed; recovery is a system reset.
//
// Frame control (BitBlt mode, drives fb_page_ctrl instead of the CPU):
//   own = the engine owns the back page. After reset it owns FB0 (fb_page_ctrl: front=1, back=FB0).
//   A task starts only while own. A completed frame_last task commits the frame: the first one
//   with enable_cmd (display start), later ones with wr_done; then own=0 (no further task starts).
//   fb_page_ctrl swaps at the next safe frame boundary and sets write_ready; the engine then takes
//   the new back page with one write_start (which clears write_ready) and continues.
//   Relative destinations are resolved with back_base when a task starts (the page is fixed while
//   own=1), so tasks queued for the next frame never use the old page.
// =============================================================================
module bitblt_sched #(
    parameter SRC_DEPTH = 1024,          // data FIFO capacities (beats)
    parameter DST_DEPTH = 512
)(
    input             clk,
    input             rstn,
    input             cal_done,
    input             hw_mode,

    // task FIFO
    input      [152:0] task_dout,
    input             task_valid,
    output            task_pop,

    // command FIFOs (push when !full)
    output            rq_push,           // {need_dst, need_src, src[27:0], dst[27:0], bytes[11:0], row[9:0]}
    output     [79:0] rq_data,
    input             rq_full,
    output            cq_push,           // {op[1:0], bytes[11:0], fill[15:0], key[15:0], alpha[7:0], row[9:0]}
    output     [63:0] cq_data,
    input             cq_full,
    output            wq_push,           // {dst[27:0], bytes[11:0], row[9:0]}
    output     [49:0] wq_data,
    input             wq_full,

    // progress
    input             rd_status,         // one pulse per read descriptor (DMA status valid)
    input      [3:0]  rd_status_err,
    input      [9:0]  rd_status_row,
    input             wr_status,
    input      [3:0]  wr_status_err,
    input      [9:0]  wr_status_row,
    input             src_pop,           // one input beat consumed by the pixel unit
    input             dst_pop,
    input             pix_err,           // row length / TLAST mismatch (pulse)
    input      [9:0]  pix_err_row,

    // fb_page_ctrl
    input      [31:0] back_base,
    input             write_ready,
    output reg        hw_enable,
    output reg        hw_wr_done,
    output reg        hw_write_start,

    // status
    output            busy,
    output            frame_wait,
    output reg [3:0]  err,               // 0 none, 1 read, 2 write, 3 row length / TLAST
    output reg [31:0] err_task,
    output reg [9:0]  err_row,
    output reg [31:0] done_cnt,
    output reg [31:0] frame_cnt
);
    // ------------------------------------------------------------- current task
    reg        run;                      // a task is executing
    reg        own, first;
    reg [1:0]  op;
    reg        last;
    reg [27:0] src_a, dst_a;             // address of the next row
    reg [11:0] bytes;
    reg [7:0]  nb;                       // beats per row
    reg [9:0]  h, row;
    reg [15:0] ss, ds, fill, key;
    reg [7:0]  alpha;
    reg [10:0] src_alloc;                // reserved input beats
    reg [9:0]  dst_alloc;
    reg [9:0]  rd_pend, wr_pend;         // descriptors issued, status not yet returned

    wire need_src = (op != 2'd1);
    wire need_dst = (op == 2'd3);

    wire [1:0]  t_op    = task_dout[152:151];
    wire        t_rel   = task_dout[150];
    wire        t_last  = task_dout[149];
    wire [27:0] t_src   = task_dout[148:121];
    wire [27:0] t_dst   = task_dout[120:93];
    wire [10:0] t_w     = task_dout[92:82];
    wire [9:0]  t_h     = task_dout[81:72];
    wire [11:0] t_bytes = {t_w, 1'b0};

    assign task_pop = !run && task_valid && cal_done && hw_mode && own;
    wire   t_empty  = (t_w == 11'd0) || (t_h == 10'd0);

    wire issue = run && (err == 4'd0) && (row != h) && !rq_full && !cq_full && !wq_full &&
                 (!need_src || ({1'b0, src_alloc} + nb <= SRC_DEPTH)) &&
                 (!need_dst || ({1'b0, dst_alloc} + nb <= DST_DEPTH));
    wire task_done = run && (row == h) && (rd_pend == 10'd0) && (wr_pend == 10'd0) &&
                     (src_alloc == 11'd0) && (dst_alloc == 10'd0) && (err == 4'd0);
    wire retire = (task_pop && t_empty) || task_done;      // empty tasks complete when popped
    wire commit = retire && (task_pop ? t_last : last);    // frame_last task completed

    assign rq_push = issue && need_src;
    assign rq_data = {need_dst, need_src, src_a, dst_a, bytes, row};
    assign cq_push = issue;
    assign cq_data = {op, bytes, fill, key, alpha, row};
    assign wq_push = issue;
    assign wq_data = {dst_a, bytes, row};

    assign busy       = run || task_valid;
    assign frame_wait = !own;

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            run <= 1'b0; own <= 1'b1; first <= 1'b1;
            op <= 2'd0; last <= 1'b0; src_a <= 28'd0; dst_a <= 28'd0; bytes <= 12'd0; nb <= 8'd0;
            h <= 10'd0; row <= 10'd0; ss <= 16'd0; ds <= 16'd0; fill <= 16'd0; key <= 16'd0; alpha <= 8'd0;
            src_alloc <= 11'd0; dst_alloc <= 10'd0; rd_pend <= 10'd0; wr_pend <= 10'd0;
            hw_enable <= 1'b0; hw_wr_done <= 1'b0; hw_write_start <= 1'b0;
            err <= 4'd0; err_task <= 32'd0; err_row <= 10'd0; done_cnt <= 32'd0; frame_cnt <= 32'd0;
        end else begin
            hw_enable <= 1'b0; hw_wr_done <= 1'b0; hw_write_start <= 1'b0;

            // ---------------------------------------------------------- task start / end
            if (task_pop && !t_empty) begin
                run <= 1'b1; row <= 10'd0;
                {op, last} <= {t_op, t_last};
                src_a <= t_src;
                dst_a <= t_rel ? back_base[27:0] + t_dst : t_dst;
                bytes <= t_bytes; nb <= (t_bytes + 12'd15) >> 4; h <= t_h;
                {ss, ds, fill, key, alpha} <= task_dout[71:0];
            end
            if (task_done) run <= 1'b0;
            if (retire) done_cnt <= done_cnt + 32'd1;
            if (commit) begin
                own <= 1'b0; first <= 1'b0; frame_cnt <= frame_cnt + 32'd1;
                if (first) hw_enable <= 1'b1; else hw_wr_done <= 1'b1;
            end
            if (!own && write_ready) begin own <= 1'b1; hw_write_start <= 1'b1; end

            // ---------------------------------------------------------- rows
            if (issue) begin
                row <= row + 10'd1;
                src_a <= src_a + ss;
                dst_a <= dst_a + ds;
            end
            src_alloc <= src_alloc + ((issue && need_src) ? nb : 8'd0) - src_pop;
            dst_alloc <= dst_alloc + ((issue && need_dst) ? nb : 8'd0) - dst_pop;
            rd_pend   <= rd_pend + ((issue && need_src) ? (need_dst ? 10'd2 : 10'd1) : 10'd0) - rd_status;
            wr_pend   <= wr_pend + issue - wr_status;

            // ---------------------------------------------------------- errors (first one kept)
            if (err == 4'd0) begin
                if (rd_status && rd_status_err != 4'd0) begin err <= 4'd1; err_row <= rd_status_row; end
                else if (wr_status && wr_status_err != 4'd0) begin err <= 4'd2; err_row <= wr_status_row; end
                else if (pix_err) begin err <= 4'd3; err_row <= pix_err_row; end
                err_task <= done_cnt + 32'd1;
            end
        end
    end
endmodule
