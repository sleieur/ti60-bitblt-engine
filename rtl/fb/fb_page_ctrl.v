// =============================================================================
// fb_page_ctrl.v - double-buffer page ownership (CPU_DOUBLE_FRAMEBUFFER_SPEC.md)
//
// Two framebuffers FB0 / FB1. State:
//   front_page    page shown by HDMI (changes only at a safe frame boundary)
//   back page     page of the writer (= ~front_page, not a separate signal)
//   startup_wait  1 after reset, cleared once by the first successful start
//   pending_valid writer page fully committed, waiting for a safe frame boundary
//                 (before the start: ENABLE accepted = FB0 committed; this is also the
//                 latched start request, no separate flag is needed)
//   write_ready   next writer page released, waiting for WRITE_START
// Writer owns the back page  <=>  !pending_valid && !write_ready.
// front_page / startup_wait / pending_valid are internal; testbenches read them hierarchically.
//
// Reset: front=1, back=0, startup_wait=1, pending=0, write_ready=0
//        (the CPU owns FB0; nothing is displayed or read).
// Commands are not checked: software must follow the protocol
// (ENABLE once while startup_wait, WRITE_START only when write_ready,
//  WR_DONE only after WRITE_START).
// enable_cmd  (CONTROL.ENABLE): pending_valid=1 (FB0 committed and frozen, start requested).
// wr_done     pending_valid=1.
// write_start write_ready=0 (writer takes the back page).
// frame_start with frame_safe (old frame reads finished, FIFO empty, underflow
//             flush done - reported by hdmi_rd_master):
//             startup_wait: if pending_valid -> front=0, back=1,
//                           startup_wait=0, pending=0, write_ready=1
//             running:      if pending_valid -> swap front/back, pending=0,
//                           write_ready=1; otherwise the front page is repeated.
//             Every displayed new page (start or swap) gives one swap_done pulse.
// frame_start without frame_safe: nothing changes (no swap, no release).
// A command in the same cycle as frame_start is evaluated after the frame boundary
// (the boundary uses the old state, the command takes effect for the next one).
// display_en = !startup_wait enables the HDMI read master.
// cfg_lock = pending_valid || !startup_wait: FB0/FB1_BASE are frozen from the accepted ENABLE.
// back_base = base of the back page (TI60_BITBLT_HW_V1: used by the BitBlt engine, no state change).
// Clock: SoC/AXI clock (100 MHz). All inputs are in this clock domain.
// =============================================================================
module fb_page_ctrl (
    input             clk,
    input             rstn,
    input             enable_cmd,
    input             wr_done,
    input             write_start,
    input             frame_start,
    input             frame_safe,
    input      [31:0] fb0_base,
    input      [31:0] fb1_base,
    output reg        write_ready,
    output            display_en,
    output            cfg_lock,
    output reg        swap_done,        // one-cycle pulse when a new page is shown
    output     [31:0] front_base,
    output     [31:0] back_base         // page of the writer (BitBlt relative destinations)
);
    reg front_page, startup_wait, pending_valid;

    assign display_en = !startup_wait;//首次启动完成后置 1，允许 HDMI 读取 DDR
    assign cfg_lock   = pending_valid || !startup_wait;
    assign front_base = front_page ? fb1_base : fb0_base;
    assign back_base  = front_page ? fb0_base : fb1_base;

    wire boundary = frame_start && frame_safe;
    
//`boundary` 表示"这里是一个可以安全换页的帧边界"。它只在 `frame_start` 和 `frame_safe` 同时为 1 的那一拍有效：
//- **`frame_start`**：HDMI 每一帧开始时（VSYNC 上升沿，处在垂直消隐期）产生的单拍脉冲，已经同步到 100 MHz 域。它只说明"新的一帧开始了"。
//- **`frame_safe`**：读主机给出的判断，表示上一帧已经彻底结束，旧页不再被使用。具体条件是：没有在途或待发的读请求，FIFO 已经读空；正常显示时，本帧全部突发（720p RGB565 为 450 笔）已经发出；发生下溢后，在途读已经结束、FIFO 已经读空，并且 DDR 已校准。
//只有 `boundary` 为 1 时，页状态机才会做三件事：首次启动显示、交换前后台页、释放旧页（置 WRITE_READY）。如果帧开始了但还不安全（例如下溢后还没清空完），这一帧就不换页、不释放页，等到下一个安全的帧边界再处理。这样可以保证 CPU 永远不会去写一个还在被 HDMI 读取的页。

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            front_page <= 1'b1; startup_wait <= 1'b1;
            pending_valid <= 1'b0; write_ready <= 1'b0; swap_done <= 1'b0;
        end else begin
            swap_done <= 1'b0;

            // ---------------------------------------------------- frame boundary (old state)
            if (boundary) begin
                if (startup_wait) begin
                    if (pending_valid) begin
                        front_page <= 1'b0; startup_wait <= 1'b0;
                        pending_valid <= 1'b0; write_ready <= 1'b1; swap_done <= 1'b1;
                    end
                end else if (pending_valid) begin
                    front_page <= ~front_page;
                    pending_valid <= 1'b0; write_ready <= 1'b1; swap_done <= 1'b1;
                end
            end

            // ---------------------------------------------------- commands (old state)
            if (enable_cmd || wr_done) pending_valid <= 1'b1;
            if (write_start)           write_ready   <= 1'b0;
        end
    end
endmodule


//assign write_start==write_ready&&pending_valid