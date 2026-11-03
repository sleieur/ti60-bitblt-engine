// =============================================================================
// pix_reader.v - pixel-clock side of the framebuffer read path.
// Reads the 16-bit side of pix_fifo (Efinity efx_fifo_top: 128-bit write, 16-bit read,
// little endian, so bits [15:0] of a written word = lowest address = leftmost pixel come
// out first) and outputs one pixel per DE cycle. The framebuffer is RGB565
// (R[15:11] G[10:5] B[4:0]); it is expanded to RGB888 by repeating the top bits
// (0x1F -> 0xFF, 0x3F -> 0xFF, so white stays white).
// Replaces async_fifo_128 + pix_unpack_128to32: the FIFO itself does the 8:1 unpacking,
// so no partial word is ever held outside the FIFO.
//
// frame_drop (1 after reset): output black, no pixel consumption.
//   - cleared only by `resume` from the read master (frame boundary, all clean).
//   - set on the FIFO IP underflow_o (a pixel read while the FIFO was empty, reported one
//     clock after the read): drop_req pulse to the read master (also counted as
//     FIFO_UNDERFLOW).
//     While displaying, the FIFO is read on every DE cycle without looking at empty_o;
//     the IP ignores a read of an empty FIFO (underflow protection) and flags it.
//   While dropped the FIFO is read every cycle whenever not empty (independent of DE) and
//   the data is discarded. The read master resumes only when nothing is in flight and its
//   FIFO count is 0, so no flush handshake is needed.
// DE is the only pixel consume enable. The FIFO read is registered (standard mode), so
// pix_rgb is valid one clock after the corresponding de input (the caller delays hs/vs/de
// by one clock). pix_rgb = FIFO output register, black when that read underflowed or the
// frame is dropped (frame_drop is set one clock after the underflow, so one more read may
// already be issued; its pixel is blanked by frame_drop).
// =============================================================================
module pix_reader (
    input              clk,
    input              rstn,
    input              de,
    // FIFO read side (16 bit, RGB565)
    input      [15:0]  fifo_rdata,
    input              fifo_empty,
    input              fifo_underflow,  // FIFO IP underflow_o (pulse, one clock after the read)
    output             fifo_rd,
    // handshake with hdmi_rd_master (pulses, already in this clock domain)
    input              resume,
    output reg         drop_req,
    // pixel out
    output     [23:0]  pix_rgb
);
    reg         frame_drop;
    reg         take_d;
    
    //take 是读取并显示，drain 是读取并丢弃。
    wire take  = de && !frame_drop;                     // 正常显示 consume one pixel
    wire drain = frame_drop && !fifo_empty;//丢帧后清理 FIFO，不受 de 限制。
    assign fifo_rd = take || drain;//丢帧状态下仍然会读取 FIFO，但目的是清空旧数据，而不是显示。

    wire [23:0] rgb888 = {fifo_rdata[15:11], fifo_rdata[15:13],     // R
                          fifo_rdata[10:5],  fifo_rdata[10:9],      // G
                          fifo_rdata[4:0],   fifo_rdata[4:2]};      // B
    assign pix_rgb = (take_d && !fifo_underflow && !frame_drop) ? rgb888 : 24'd0;

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            frame_drop <= 1'b1; take_d <= 1'b0; drop_req <= 1'b0;
        end else begin
            drop_req <= 1'b0;
            take_d <= take;

            if (!frame_drop) begin
                if (fifo_underflow) begin          // that read found the FIFO empty
                    frame_drop <= 1'b1; drop_req <= 1'b1;
                end
            end else if (resume) frame_drop <= 1'b0;
        end
    end
endmodule
