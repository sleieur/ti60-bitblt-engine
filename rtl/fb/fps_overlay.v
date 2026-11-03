// =============================================================================
// fps_overlay.v - render FPS counter and "FPS: NN" overlay (pixel clock domain).
// render_fps = number of swap_done pulses (new pages shown) in each window of 60 frame
// starts (= 1 s at 60 Hz). Pages change only at a frame boundary, so there is at most one
// swap_done per frame and render_fps <= 60 (no saturation needed). Not visible to the CPU.
// The text is drawn at the top-left of the active area with a 5x7 font, each character
// cell 8x8 font pixels, scaled x4 (32x32 screen pixels per character), black text on a transparent background
// (pixels of the box that are not part of a glyph keep the original image).
// Only the RGB sent to the TMDS encoder is changed; the framebuffer is untouched.
// Inputs hs/vs/de/rgb must be aligned; outputs are aligned and delayed by one clock.
// =============================================================================
module fps_overlay #(
    parameter X0 = 16,                 // top-left of the text box (active-area pixels)
    parameter Y0 = 16
)(
    input             clk,
    input             rstn,
    input             frame_start,     // 1 pulse per frame (VSYNC rising edge)
    input             swap_done,       // 1 pulse per new page shown
    input      [23:0] i_rgb,
    input             i_hs,
    input             i_vs,
    input             i_de,
    output reg [23:0] o_rgb,
    output reg        o_hs,
    output reg        o_vs,
    output reg        o_de,
    output reg [6:0]  render_fps
);
    localparam NCHAR = 7;              // "FPS: NN"

    // ------------------------------------------------------------ FPS counter
    reg [5:0] win_frames;
    reg [6:0] swaps;
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin win_frames <= 6'd0; swaps <= 7'd0; render_fps <= 7'd0; end
        else begin
            if (frame_start && win_frames == 6'd59) begin
                win_frames <= 6'd0;
                render_fps <= swaps + swap_done;
                swaps      <= 7'd0;
            end else begin
                if (frame_start) win_frames <= win_frames + 6'd1;
                if (swap_done)   swaps      <= swaps + 7'd1;
            end
        end
    end

    wire [3:0] tens = render_fps / 7'd10;
    wire [3:0] ones = render_fps % 7'd10;

    // ------------------------------------------------------------ active-area position
    reg [11:0] x, y;
    reg        de_d;
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin x <= 12'd0; y <= 12'd0; de_d <= 1'b0; end
        else begin
            de_d <= i_de;
            if (frame_start) begin x <= 12'd0; y <= 12'd0; end
            else if (i_de) x <= x + 12'd1;
            else begin
                x <= 12'd0;
                if (de_d) y <= y + 12'd1;       // end of an active line
            end
        end
    end

    // ------------------------------------------------------------ glyphs
    // code: 0-9 digits, 10 'F', 11 'P', 12 'S', 13 ':', 14 ' '
    function [4:0] glyph(input [3:0] code, input [2:0] row);
        reg [34:0] g;
        begin
            case (code)
                4'd0:  g = 35'b01110_10001_10011_10101_11001_10001_01110;
                4'd1:  g = 35'b00100_01100_00100_00100_00100_00100_01110;
                4'd2:  g = 35'b01110_10001_00001_00010_00100_01000_11111;
                4'd3:  g = 35'b11111_00010_00100_00010_00001_10001_01110;
                4'd4:  g = 35'b00010_00110_01010_10010_11111_00010_00010;
                4'd5:  g = 35'b11111_10000_11110_00001_00001_10001_01110;
                4'd6:  g = 35'b00110_01000_10000_11110_10001_10001_01110;
                4'd7:  g = 35'b11111_00001_00010_00100_01000_01000_01000;
                4'd8:  g = 35'b01110_10001_10001_01110_10001_10001_01110;
                4'd9:  g = 35'b01110_10001_10001_01111_00001_00010_01100;
                4'd10: g = 35'b11111_10000_10000_11110_10000_10000_10000;
                4'd11: g = 35'b11110_10001_10001_11110_10000_10000_10000;
                4'd12: g = 35'b01111_10000_10000_01110_00001_00001_11110;
                4'd13: g = 35'b00000_01100_01100_00000_01100_01100_00000;
                default: g = 35'd0;
            endcase
            glyph = (row == 3'd7) ? 5'd0 : g[34 - row * 5 -: 5];
        end
    endfunction

    wire [11:0] dx = x - X0;
    wire [11:0] dy = y - Y0;
    wire        in_box = i_de && (x >= X0) && (x < X0 + NCHAR * 32) && (y >= Y0) && (y < Y0 + 32);
    wire [2:0]  ch  = dx[7:5];         // character index (32 px per character, dx < 224 in the box)
    wire [2:0]  col = dx[4:2];         // font column 0..7 (x4 scale)
    wire [2:0]  row = dy[4:2];         // font row 0..7
    reg  [3:0]  code;
    always @(*) begin
        case (ch)
            3'd0: code = 4'd10;    // F
            3'd1: code = 4'd11;    // P
            3'd2: code = 4'd12;    // S
            3'd3: code = 4'd13;    // :
            3'd4: code = 4'd14;    // space
            3'd5: code = tens;
            default: code = ones;
        endcase
    end
    wire [4:0] bits = glyph(code, row);
    wire       on   = (col < 3'd5) && bits[3'd4 - col];

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin o_rgb <= 24'd0; o_hs <= 1'b0; o_vs <= 1'b0; o_de <= 1'b0; end
        else begin
            o_hs <= i_hs; o_vs <= i_vs; o_de <= i_de;
            o_rgb <= (in_box && on) ? 24'h000000 : i_rgb;
        end
    end
endmodule
