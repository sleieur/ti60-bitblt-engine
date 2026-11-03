// =============================================================================
// video_timing.v - video timing generator (positive HS/VS), 720p60 by default (V6)
//
// Replaces color_bar_rgb (only its hs/vs/de were used). Line: HS -> HBP -> active -> HFP,
// frame: VS -> VBP -> active -> VFP. The vertical counter advances at the end of each line.
// hs/vs/de are registered: same cycle timing as color_bar_rgb (checked in sim_fb/timing).
// Clock: pixel clock (74.25 MHz for 720p60, 148.5 MHz for 1080p60).
// =============================================================================
module video_timing #(
    parameter H_ACT = 1280, parameter H_FP = 110, parameter H_SYNC = 40, parameter H_BP = 220,
    parameter V_ACT = 720,  parameter V_FP = 5,   parameter V_SYNC = 5,  parameter V_BP = 20
)(
    input      clk,
    input      rstn,
    output reg hs,
    output reg vs,
    output reg de
);
    localparam H_TOT = H_SYNC + H_BP + H_ACT + H_FP;
    localparam V_TOT = V_SYNC + V_BP + V_ACT + V_FP;
    localparam H_DE0 = H_SYNC + H_BP, H_DE1 = H_DE0 + H_ACT;
    localparam V_DE0 = V_SYNC + V_BP, V_DE1 = V_DE0 + V_ACT;

    reg [15:0] h, v;

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            h <= 16'd0; v <= 16'd0;
            hs <= 1'b0; vs <= 1'b0; de <= 1'b0;
        end else begin
            if (h == H_TOT - 1) begin
                h <= 16'd0;
                v <= (v == V_TOT - 1) ? 16'd0 : v + 16'd1;
            end else
                h <= h + 16'd1;
            hs <= (h < H_SYNC);
            vs <= (v < V_SYNC);
            de <= (h >= H_DE0) && (h < H_DE1) && (v >= V_DE0) && (v < V_DE1);
        end
    end
endmodule
