// =============================================================================
// bb_fifo.v - synchronous first-word-fall-through FIFO (task / command / data FIFOs).
// The memory has a registered read (block RAM); one output register q holds the head entry.
// Capacity = 2**AW entries in total (memory + output register): full = (cnt == 2**AW).
// push must not be asserted while full, pop only while valid.
// Latency: an entry pushed at edge k is valid after edge k+2.
// =============================================================================
module bb_fifo #(
    parameter W  = 8,
    parameter AW = 4
)(
    input              clk,
    input              rstn,
    input              push,
    input  [W-1:0]     din,
    output             full,
    input              pop,
    output reg [W-1:0] dout,
    output reg         valid,
    output [AW:0]      cnt
);
    reg [W-1:0] mem [0:(1<<AW)-1];
    reg [AW:0]  wp, rp;

    wire ld = (wp != rp) && (!valid || pop);       // memory not empty, output register free
    assign cnt  = (wp - rp) + valid;
    assign full = cnt[AW];

    always @(posedge clk) begin
        if (push) mem[wp[AW-1:0]] <= din;
        if (ld)   dout <= mem[rp[AW-1:0]];
    end

    always @(posedge clk or negedge rstn)
        if (!rstn) begin
            wp <= 0; rp <= 0; valid <= 1'b0;
        end else begin
            wp <= wp + push;
            rp <= rp + ld;
            if (ld) valid <= 1'b1; else if (pop) valid <= 1'b0;
        end
endmodule
