// =============================================================================
// hdmi_rd_master.v - HDMI framebuffer AXI4 read master (AR/R only), SoC/DDR AXI clock.
//
// Every burst: ARLEN=255, ARSIZE=4 (16 B), INCR, ARID=RD_ID -> 4 KiB, 4 KiB aligned.
// MAX_RD_OUTSTANDING = 1. NBURST bursts per frame (1280x720x2 / 4096 = 450, RGB565), then stop.
// The frame size must be a multiple of 4 KiB (every burst is a full 4 KiB).
// AR issue condition: live && outstanding==0 && burst_idx<NBURST && fifo_wcount < REFILL_WORDS
//   (4 KiB = 256 words of the 8 KiB FIFO), which also guarantees >= 4 KiB free -> no overflow.
//   After frame_start the FIFO is empty, so this rule issues one burst (4 KiB prefetch),
//   then one burst each time the content drops below 4 KiB (>= ~27 us of 720p RGB565
//   pixels left, against a few us to refill).
//
// Frame / drop protocol (pixel side = pix_reader):
//   live=0 after reset. At frame_start (front_base valid one cycle later):
//     live      : restart at burst 0 of the new front_base (fs_err if FIFO/outstanding/
//                 burst count not clean - assertion only).
//   frame_safe (combinational, sampled by fb_page_ctrl at frame_start): the old frame is
//     completely finished - no burst in flight or pending, FIFO empty, and either the live
//     frame has issued all its bursts, or (not live) cal_done is set.
//     Pages may only be swapped / released at such a boundary.
//     not live  : become live (and send `resume` to the pixel side) only if this
//                 boundary was safe and enable (display started),
//                 cal_done, outstanding==0 and FIFO word count==0; otherwise this
//                 frame is skipped (black) and the check is repeated at the next frame_start.
//   drop_req (pixel side read the FIFO empty during active video): live=0, no more AR,
//     R beats of the in-flight burst are discarded (not written to the FIFO). The pixel side
//     keeps reading the FIFO empty while dropped; no handshake is needed for that:
//     outstanding==0 (nothing more is written) and fifo_wcount==0 (never below the true
//     fill level) together mean the FIFO really is empty.
// RRESP / RID are not checked (single ID, OKAY expected); a burst ends on RLAST.
// =============================================================================
module hdmi_rd_master #(
    parameter        ID_W         = 4,
    parameter [ID_W-1:0] RD_ID    = 0,
    parameter        NBURST       = 450,
    parameter        FIFO_AW      = 9,
    parameter        REFILL_WORDS = 256
)(
    input                 clk,
    input                 rstn,
    input                 enable,           // display enable (fb_page_ctrl: start done)
    input                 cal_done,
    input                 frame_start,      // 1-cycle pulse, already in this clock domain
    input      [31:0]     front_base,       // valid the cycle after frame_start
    input                 drop_req,         // pulse from the pixel side (underflow)
    output reg            resume,           // pulse: this frame is live, pixel side may show it
    output                frame_safe,       // safe frame boundary (see above)

    // FIFO write side
    input      [FIFO_AW:0] fifo_wcount,
    output                fifo_wr,
    output     [127:0]    fifo_wdata,

    // AXI4 read address / data
    output     [ID_W-1:0] m_arid,
    output reg [31:0]     m_araddr,
    output     [7:0]      m_arlen,
    output     [2:0]      m_arsize,
    output     [1:0]      m_arburst,
    output reg            m_arvalid,
    input                 m_arready,
    input      [127:0]    m_rdata,
    input                 m_rlast,
    input                 m_rvalid,
    output                m_rready
);
    assign m_arid    = RD_ID;
    assign m_arlen   = 8'd255;
    assign m_arsize  = 3'd4;
    assign m_arburst = 2'b01;
    assign m_rready  = 1'b1;               // space for the whole burst is reserved at AR time

    reg        live;                        // reading the current frame
    reg        outstanding;                 // one burst in flight
    reg [11:0] burst_idx;                   // bursts issued in this frame
    reg        fs_d;
    reg        fs_safe;                     // frame_safe sampled at frame_start
    reg [31:0] base;

    wire r_hs = m_rvalid;                  // m_rready is constant 1//r_hs 表示 AXI R 通道握手成功（Read Handshake）。
    assign fifo_wr    = r_hs && live;      // dropped frames: in-flight data is discarded
    assign fifo_wdata = m_rdata;

//clean_idle：live=0，可能是刚复位，此时 DDR 不一定完成校准，所以必须检查 cal_done=1。
//clean_live：live=1，说明此前已经通过 cal_done 检查，进入了正常运行状态，因此不再重复检查
    wire clean_live = live && !outstanding && !m_arvalid && (fifo_wcount == 0) && (burst_idx == NBURST);
    wire clean_idle = !live && cal_done && !outstanding && !m_arvalid && (fifo_wcount == 0);
    assign frame_safe = (clean_live || clean_idle) && !drop_req;

    wire can_issue = live && !drop_req && !outstanding && !m_arvalid && !fs_d && !frame_start &&//在帧切换期间暂停发起 AXI 读请求，避免读到旧帧地址。fs_d=1更新 base（新帧首地址）清零 burst_idx
                     (burst_idx < NBURST) && (fifo_wcount < REFILL_WORDS);//仅支持1个outstanding        
    wire start_live = fs_safe && enable && clean_idle;   // not live -> live at this boundary：：取消复位//从丢帧恢复

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            fs_d <= 1'b0; fs_safe <= 1'b0; base <= 32'd0; live <= 1'b0; resume <= 1'b0;
            m_arvalid <= 1'b0; m_araddr <= 32'd0; outstanding <= 1'b0; burst_idx <= 12'd0;
        end else begin
            resume <= 1'b0;
            fs_d  <= frame_start;//hdmi  时序帧开始 delay 对齐时序
            if (frame_start) fs_safe <= frame_safe;

            // ---------------------------------------------------------- frame boundary
            // live: next frame from burst 0; not live: start only if start_live, else skip (black)
            //live:HDMI 读取通路处于正常运行模式”的标志
            if (fs_d && !drop_req && (live || start_live)) begin
                base <= front_base; burst_idx <= 12'd0;
                if (start_live) begin live <= 1'b1; resume <= 1'b1; end
                //start_live和live  不并存（clean_idle）
            end

            // ---------------------------------------------------------- AR
            if (can_issue) begin
                m_arvalid <= 1'b1;
                m_araddr  <= base + {burst_idx, 12'd0};
            end
            if (m_arvalid && m_arready) begin
                m_arvalid   <= 1'b0;
                outstanding <= 1'b1;
                burst_idx   <= burst_idx + 1'b1;
            end

            // ---------------------------------------------------------- R
            if (r_hs && m_rlast) outstanding <= 1'b0;   // AXI end of burst is RLAST

            // ---------------------------------------------------------- drop
            if (drop_req) live <= 1'b0; //停止发起新的 DDR 读请求
        end
    end
endmodule



