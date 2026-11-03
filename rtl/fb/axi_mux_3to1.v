// =============================================================================
// axi_mux_3to1.v - 3 masters -> 1 DDR controller AXI4 port (single clock).
// Extends axi_mux_2to1.v (V6) with the BitBlt engine:
//   S0 = HDMI read master (AR/R)   S1 = SoC io_ddrA (AW/W/B + AR/R)   S2 = BitBlt DMA (AW/W/B + AR/R)
//
// Controller ID (4 bit): HDMI 0, BitBlt 1, CPU {1, cpu_id[2:0]} (8..15; the CPU uses ID 0, see P0).
// R and B are routed beat by beat on the full ID (no assumption on order or interleaving).
// Read : fixed priority S0 > S1 > S2. Idle: chosen combinationally; if the AR is not accepted in that
//        cycle the choice is locked until its handshake (a later higher-priority request cannot take
//        a presented address away).
// Write: fixed priority S1 > S2 on AW, locked the same way. The owner of every accepted AW is pushed
//        into a 4-entry FIFO; W beats are taken only from the owner at its head until WLAST (bursts are
//        never mixed, W never runs ahead of its AW). AW is not accepted while the owner FIFO is full.
// Read and write paths are independent.
// =============================================================================
module axi_mux_3to1 #(
    parameter S1_ID_W = 8,
    parameter AW      = 32,
    parameter DW      = 128
)(
    input                  clk,
    input                  rstn,

    // S0: HDMI, read only (ID 0)
    input  [AW-1:0]        s0_araddr,
    input  [7:0]           s0_arlen,
    input  [2:0]           s0_arsize,
    input  [1:0]           s0_arburst,
    input                  s0_arvalid,
    output                 s0_arready,
    output [DW-1:0]        s0_rdata,
    output [1:0]           s0_rresp,
    output                 s0_rlast,
    output                 s0_rvalid,
    input                  s0_rready,

    // S1: SoC
    input  [S1_ID_W-1:0]   s1_awid,
    input  [AW-1:0]        s1_awaddr,
    input  [7:0]           s1_awlen,
    input  [2:0]           s1_awsize,
    input  [1:0]           s1_awburst,
    input                  s1_awlock,
    input  [3:0]           s1_awcache,
    input  [2:0]           s1_awprot,
    input                  s1_awvalid,
    output                 s1_awready,
    input  [DW-1:0]        s1_wdata,
    input  [DW/8-1:0]      s1_wstrb,
    input                  s1_wlast,
    input                  s1_wvalid,
    output                 s1_wready,
    output [S1_ID_W-1:0]   s1_bid,
    output [1:0]           s1_bresp,
    output                 s1_bvalid,
    input                  s1_bready,
    input  [S1_ID_W-1:0]   s1_arid,
    input  [AW-1:0]        s1_araddr,
    input  [7:0]           s1_arlen,
    input  [2:0]           s1_arsize,
    input  [1:0]           s1_arburst,
    input                  s1_arlock,
    input  [3:0]           s1_arcache,
    input  [2:0]           s1_arprot,
    input                  s1_arvalid,
    output                 s1_arready,
    output [S1_ID_W-1:0]   s1_rid,
    output [DW-1:0]        s1_rdata,
    output [1:0]           s1_rresp,
    output                 s1_rlast,
    output                 s1_rvalid,
    input                  s1_rready,

    // S2: BitBlt (single ID)
    input  [AW-1:0]        s2_awaddr,
    input  [7:0]           s2_awlen,
    input  [2:0]           s2_awsize,
    input  [1:0]           s2_awburst,
    input                  s2_awvalid,
    output                 s2_awready,
    input  [DW-1:0]        s2_wdata,
    input  [DW/8-1:0]      s2_wstrb,
    input                  s2_wlast,
    input                  s2_wvalid,
    output                 s2_wready,
    output [1:0]           s2_bresp,
    output                 s2_bvalid,
    input                  s2_bready,
    input  [AW-1:0]        s2_araddr,
    input  [7:0]           s2_arlen,
    input  [2:0]           s2_arsize,
    input  [1:0]           s2_arburst,
    input                  s2_arvalid,
    output                 s2_arready,
    output [DW-1:0]        s2_rdata,
    output [1:0]           s2_rresp,
    output                 s2_rlast,
    output                 s2_rvalid,
    input                  s2_rready,

    // M: DDR controller
    output [3:0]           m_awid,
    output [AW-1:0]        m_awaddr,
    output [7:0]           m_awlen,
    output [2:0]           m_awsize,
    output [1:0]           m_awburst,
    output                 m_awlock,
    output [3:0]           m_awcache,
    output [2:0]           m_awprot,
    output                 m_awvalid,
    input                  m_awready,
    output [DW-1:0]        m_wdata,
    output [DW/8-1:0]      m_wstrb,
    output                 m_wlast,
    output                 m_wvalid,
    input                  m_wready,
    input  [3:0]           m_bid,
    input  [1:0]           m_bresp,
    input                  m_bvalid,
    output                 m_bready,
    output [3:0]           m_arid,
    output [AW-1:0]        m_araddr,
    output [7:0]           m_arlen,
    output [2:0]           m_arsize,
    output [1:0]           m_arburst,
    output                 m_arlock,
    output [3:0]           m_arcache,
    output [2:0]           m_arprot,
    output                 m_arvalid,
    input                  m_arready,
    input  [3:0]           m_rid,
    input  [DW-1:0]        m_rdata,
    input  [1:0]           m_rresp,
    input                  m_rlast,
    input                  m_rvalid,
    output                 m_rready
);
    localparam [3:0] ID_HDMI = 4'd0, ID_BB = 4'd1;

    // ------------------------------------------------------------ AR arbiter (S0 > S1 > S2)
    reg        ar_lock;
    reg  [1:0] ar_sel_r;
    wire [1:0] ar_pick = s0_arvalid ? 2'd0 : s1_arvalid ? 2'd1 : 2'd2;
    wire [1:0] ar_sel  = ar_lock ? ar_sel_r : ar_pick;

    always @(posedge clk or negedge rstn)
        if (!rstn) begin ar_lock <= 1'b0; ar_sel_r <= 2'd0; end
        else if (!ar_lock) begin
            if (m_arvalid && !m_arready) begin ar_lock <= 1'b1; ar_sel_r <= ar_sel; end
        end else if (m_arready) ar_lock <= 1'b0;

    assign m_arvalid  = (ar_sel == 2'd0) ? s0_arvalid : (ar_sel == 2'd1) ? s1_arvalid : s2_arvalid;
    assign m_arid     = (ar_sel == 2'd0) ? ID_HDMI : (ar_sel == 2'd1) ? {1'b1, s1_arid[2:0]} : ID_BB;
    assign m_araddr   = (ar_sel == 2'd0) ? s0_araddr  : (ar_sel == 2'd1) ? s1_araddr  : s2_araddr;
    assign m_arlen    = (ar_sel == 2'd0) ? s0_arlen   : (ar_sel == 2'd1) ? s1_arlen   : s2_arlen;
    assign m_arsize   = (ar_sel == 2'd0) ? s0_arsize  : (ar_sel == 2'd1) ? s1_arsize  : s2_arsize;
    assign m_arburst  = (ar_sel == 2'd0) ? s0_arburst : (ar_sel == 2'd1) ? s1_arburst : s2_arburst;
    assign m_arlock   = (ar_sel == 2'd1) && s1_arlock;
    assign m_arcache  = (ar_sel == 2'd1) ? s1_arcache : 4'd0;
    assign m_arprot   = (ar_sel == 2'd1) ? s1_arprot  : 3'd0;
    assign s0_arready = (ar_sel == 2'd0) && m_arready;
    assign s1_arready = (ar_sel == 2'd1) && m_arready;
    assign s2_arready = (ar_sel == 2'd2) && m_arready;

    // ------------------------------------------------------------ R routing by ID
    wire r_cpu = m_rid[3], r_bb = (m_rid == ID_BB);
    assign s0_rdata = m_rdata;  assign s0_rresp = m_rresp;  assign s0_rlast = m_rlast;
    assign s1_rdata = m_rdata;  assign s1_rresp = m_rresp;  assign s1_rlast = m_rlast;
    assign s2_rdata = m_rdata;  assign s2_rresp = m_rresp;  assign s2_rlast = m_rlast;
    assign s1_rid    = {{(S1_ID_W-3){1'b0}}, m_rid[2:0]};
    assign s0_rvalid = m_rvalid && !r_cpu && !r_bb;
    assign s1_rvalid = m_rvalid && r_cpu;
    assign s2_rvalid = m_rvalid && r_bb;
    assign m_rready  = r_cpu ? s1_rready : r_bb ? s2_rready : s0_rready;

    // ------------------------------------------------------------ AW arbiter (S1 > S2) and W owner FIFO
    reg        aw_lock, aw_sel_r;                // 0 = S1, 1 = S2
    reg  [3:0] own_q;                            // owner of accepted AWs, oldest at own_q[rp]
    reg  [2:0] own_wp, own_rp;
    wire       own_full  = (own_wp - own_rp) == 3'd4;
    wire       own_empty = (own_wp == own_rp);
    wire       own_head  = own_q[own_rp[1:0]];
    wire       aw_sel    = aw_lock ? aw_sel_r : !s1_awvalid;
    wire       aw_v      = aw_sel ? s2_awvalid : s1_awvalid;
    wire       aw_hs     = m_awvalid && m_awready;
    wire       w_hs      = m_wvalid && m_wready;

    assign m_awvalid  = aw_v && !own_full;
    assign m_awid     = aw_sel ? ID_BB : {1'b1, s1_awid[2:0]};
    assign m_awaddr   = aw_sel ? s2_awaddr  : s1_awaddr;
    assign m_awlen    = aw_sel ? s2_awlen   : s1_awlen;
    assign m_awsize   = aw_sel ? s2_awsize  : s1_awsize;
    assign m_awburst  = aw_sel ? s2_awburst : s1_awburst;
    assign m_awlock   = !aw_sel && s1_awlock;
    assign m_awcache  = aw_sel ? 4'd0 : s1_awcache;
    assign m_awprot   = aw_sel ? 3'd0 : s1_awprot;
    assign s1_awready = !aw_sel && m_awready && !own_full;
    assign s2_awready =  aw_sel && m_awready && !own_full;

    always @(posedge clk or negedge rstn)
        if (!rstn) begin
            aw_lock <= 1'b0; aw_sel_r <= 1'b0; own_q <= 4'd0; own_wp <= 3'd0; own_rp <= 3'd0;
        end else begin
            if (!aw_lock) begin
                if (m_awvalid && !m_awready) begin aw_lock <= 1'b1; aw_sel_r <= aw_sel; end
            end else if (m_awready) aw_lock <= 1'b0;
            if (aw_hs) begin own_q[own_wp[1:0]] <= aw_sel; own_wp <= own_wp + 3'd1; end
            if (w_hs && m_wlast) own_rp <= own_rp + 3'd1;
        end

    assign m_wvalid  = !own_empty && (own_head ? s2_wvalid : s1_wvalid);
    assign m_wdata   = own_head ? s2_wdata : s1_wdata;
    assign m_wstrb   = own_head ? s2_wstrb : s1_wstrb;
    assign m_wlast   = own_head ? s2_wlast : s1_wlast;
    assign s1_wready = !own_empty && !own_head && m_wready;
    assign s2_wready = !own_empty &&  own_head && m_wready;

    // ------------------------------------------------------------ B routing by ID
    wire b_cpu = m_bid[3];
    assign s1_bid    = {{(S1_ID_W-3){1'b0}}, m_bid[2:0]};
    assign s1_bresp  = m_bresp;
    assign s2_bresp  = m_bresp;
    assign s1_bvalid = m_bvalid && b_cpu;
    assign s2_bvalid = m_bvalid && !b_cpu;
    assign m_bready  = b_cpu ? s1_bready : s2_bready;
endmodule
