// =============================================================================
// fb_cdc_pulse.v - single-pulse clock domain crossing (toggle + 2-FF synchroniser)
// A one-cycle src_pulse flips a toggle in the source domain; the destination sees the
// toggle edge after 2 synchroniser flops and emits exactly one dst_pulse.
// Pulses must be spaced more than ~3 destination cycles apart (true for every user here:
// frame_start, swap_done, underflow/drop request, flush handshakes).
// =============================================================================
module fb_cdc_pulse (
    input      src_clk,
    input      src_rstn,
    input      src_pulse,
    input      dst_clk,
    input      dst_rstn,
    output     dst_pulse
);
    reg src_tgl;
    always @(posedge src_clk or negedge src_rstn)
        if (!src_rstn) src_tgl <= 1'b0;
        else if (src_pulse) src_tgl <= ~src_tgl;

    (* async_reg = "true" *) reg [1:0] sync;
    reg dst_tgl_d;
    always @(posedge dst_clk or negedge dst_rstn)
        if (!dst_rstn) begin sync <= 2'b00; dst_tgl_d <= 1'b0; end
        else begin sync <= {sync[0], src_tgl}; dst_tgl_d <= sync[1]; end

    assign dst_pulse = sync[1] ^ dst_tgl_d;
endmodule

//对，**本质上就是把“检测短脉冲”转换成“检测电平翻转”。**
//过程是：
//1. **源时钟域**：每收到一个脉冲，就翻转一次电平（0→1 或 1→0）。
//2. **跨时钟域**：通过两级寄存器同步这个电平。
//3. **目标时钟域**：检测电平是否发生变化，变化一次就产生一个单周期脉冲。
//**好处：电平会持续保持，不容易像短脉冲那样被目标时钟漏采样。**

//支持快慢时钟域互相转换