// =============================================================================
// bitblt_pix.v - BitBlt pixel unit: compute commands + SRC / DST FIFOs -> write DMA AXI stream.
// 8 RGB565 pixels per 128-bit beat (pixel i = bits [16i+15:16i], lower address first).
//
// One compute command per row ({op, bytes, fill, key, alpha, row}); the command is held until all
// N = ceil(bytes/16) beats of the row entered the pipeline, then the next one is loaded.
// A beat enters when the inputs the op needs are valid (COPY / KEY: SRC, ALPHA: SRC and DST, FILL:
// none) and the pipeline advances; it pops those inputs (src_pop / dst_pop go to the credit counters).
//   COPY   data and keep of SRC
//   FILL   fill colour; keep = row bytes (last beat: low bytes%16 lanes)
//   KEY    SRC data, both keep bits of a pixel cleared where SRC == key (position kept, the beat is
//          still sent, also when all 16 keep bits are 0)
//   ALPHA  per channel (R5 G6 B5) floor((S*a + D*(255-a) + 127) / 255); a=0 gives D, a=255 gives S
// TLAST of the output is the last beat of the row (own beat count). An input TLAST that does not
// match the expected position reports pix_err (data of that task is then not used: no commit).
// Pipeline: input register -> products -> divide/pack -> output, one global advance
// (adv = !out_valid || out_ready): 1 beat per clock, back pressure stops every stage.
// =============================================================================
module bitblt_pix (
    input              clk,
    input              rstn,

    input      [63:0]  cq_dout,
    input              cq_valid,
    output             cq_pop,

    input      [144:0] src_dout,          // {tlast, tkeep[15:0], tdata[127:0]}
    input              src_valid,
    output             src_pop,
    input      [144:0] dst_dout,
    input              dst_valid,
    output             dst_pop,

    output reg [127:0] m_tdata,
    output reg [15:0]  m_tkeep,
    output reg         m_tlast,
    output reg         m_tvalid,
    input              m_tready,

    output reg         pix_err,
    output reg [9:0]   pix_err_row
);
    // ------------------------------------------------------------- current row command
    reg        cmd_v;
    reg [1:0]  op;
    reg [11:0] bytes;
    reg [15:0] fill, key;
    reg [7:0]  alpha;
    reg [9:0]  row;
    reg [7:0]  beat;

    wire [7:0] nb    = (bytes + 12'd15) >> 4;
    wire       lastb = (beat == nb - 8'd1);
    wire [15:0] tail = (bytes[3:0] == 4'd0) ? 16'hFFFF : ~(16'hFFFF << bytes[3:0]);

    wire adv  = !m_tvalid || m_tready;
    wire in_ok = (op == 2'd1) || (src_valid && (op != 2'd3 || dst_valid));
    wire fire = cmd_v && adv && in_ok;

    assign cq_pop  = !cmd_v && cq_valid;
    assign src_pop = fire && (op != 2'd1);
    assign dst_pop = fire && (op == 2'd3);

    always @(posedge clk or negedge rstn)
        if (!rstn) begin
            cmd_v <= 1'b0; op <= 2'd0; bytes <= 12'd0; fill <= 16'd0; key <= 16'd0; alpha <= 8'd0;
            row <= 10'd0; beat <= 8'd0;
        end else if (cq_pop) begin
            cmd_v <= 1'b1; beat <= 8'd0;
            {op, bytes, fill, key, alpha, row} <= cq_dout;
        end else if (fire) begin
            beat <= beat + 8'd1;
            if (lastb) cmd_v <= 1'b0;
        end

    // ------------------------------------------------------------- stage 1: input register
    reg         v1, l1;
    reg [1:0]   op1;
    reg [127:0] s1, d1;
    reg [15:0]  k1, key1;
    reg [7:0]   a1;
    wire        len_bad = (op != 2'd1) && ((src_dout[144] != lastb) || (op == 2'd3 && dst_dout[144] != lastb));

    always @(posedge clk or negedge rstn)
        if (!rstn) begin
            v1 <= 1'b0; l1 <= 1'b0; op1 <= 2'd0; s1 <= 128'd0; d1 <= 128'd0; k1 <= 16'd0; key1 <= 16'd0; a1 <= 8'd0;
            pix_err <= 1'b0; pix_err_row <= 10'd0;
        end else begin
            pix_err <= fire && len_bad;
            if (fire && len_bad) pix_err_row <= row;
            if (adv) begin
                v1 <= fire;
                if (fire) begin
                    l1 <= lastb; op1 <= op; key1 <= key; a1 <= alpha;
                    s1 <= (op == 2'd1) ? {8{fill}} : src_dout[127:0];
                    d1 <= dst_dout[127:0];
                    k1 <= (op == 2'd1) ? (lastb ? tail : 16'hFFFF) : src_dout[143:128];
                end
            end
        end

    // ------------------------------------------------------------- stage 2: products / key mask
    // ALPHA channel value x = S*a + D*(255-a) + 127 (0..16192, fits 14 bits)
    function [13:0] mix(input [5:0] s, input [5:0] d, input [7:0] a);
        mix = s * a + d * (8'd255 - a) + 14'd127;
    endfunction

    reg         v2, l2;
    reg [1:0]   op2;
    reg [127:0] s2;
    reg [15:0]  k2;
    reg [13:0]  xr [0:7], xg [0:7], xb [0:7];
    integer i;

    always @(posedge clk or negedge rstn)
        if (!rstn) begin
            v2 <= 1'b0; l2 <= 1'b0; op2 <= 2'd0; s2 <= 128'd0; k2 <= 16'd0;
        end else if (adv) begin
            v2 <= v1; l2 <= l1; op2 <= op1; s2 <= s1;
            for (i = 0; i < 8; i = i + 1) begin
                k2[2*i +: 2] <= (op1 == 2'd2 && s1[16*i +: 16] == key1) ? 2'b00 : k1[2*i +: 2];
                xr[i] <= mix({1'b0, s1[16*i+11 +: 5]}, {1'b0, d1[16*i+11 +: 5]}, a1);
                xg[i] <= mix(s1[16*i+5 +: 6],        d1[16*i+5 +: 6],        a1);
                xb[i] <= mix({1'b0, s1[16*i    +: 5]}, {1'b0, d1[16*i    +: 5]}, a1);
            end
        end

    // ------------------------------------------------------------- stage 3: divide by 255, output
    function [5:0] div255(input [13:0] x);      // floor(x / 255), exact for x < 65535
        div255 = ({2'b00, x} + 16'd1 + (x >> 8)) >> 8;
    endfunction

    reg [127:0] mixed;
    reg [5:0]   qr, qg, qb;
    always @(*)
        for (i = 0; i < 8; i = i + 1) begin
            qr = div255(xr[i]); qg = div255(xg[i]); qb = div255(xb[i]);
            mixed[16*i +: 16] = {qr[4:0], qg, qb[4:0]};
        end

    always @(posedge clk or negedge rstn)
        if (!rstn) begin
            m_tvalid <= 1'b0; m_tdata <= 128'd0; m_tkeep <= 16'd0; m_tlast <= 1'b0;
        end else if (adv) begin
            m_tvalid <= v2;
            m_tdata  <= (op2 == 2'd3) ? mixed : s2;
            m_tkeep  <= k2;
            m_tlast  <= l2;
        end
endmodule
