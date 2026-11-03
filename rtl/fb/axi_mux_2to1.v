// =============================================================================
// axi_mux_2to1.v - 2 masters -> 1 DDR controller AXI4 port (single clock).
// Structure follows par/ddr_demo_ti60/src/Axi_Mux.v (Axi_Mux_Rd_Ctrl / Axi_Mux_Wr_Ctrl,
// ID_MODE_SEL = "Extend": the controller-side ID carries the channel number).
//
//   S0 = HDMI read master (AR/R only)   S1 = SoC io_ddrA (AW/W/B + AR/R)
//
// Read path : fixed priority S0 > S1. When idle the channel is chosen combinationally (no
//             added latency, one AR per cycle possible); if the AR is not accepted in that
//             cycle the channel is locked until its handshake (address never changes under
//             ARVALID: the locked master holds VALID and address, AXI rule).
// Write path: only S1 writes; AW, W, B pass through (bursts are never split or switched).
// Read and write paths are independent: AW and AR may reach the controller in the same
// cycle; there is no cross-direction mutual exclusion.
// ID mapping: controller ID (M_ID_W = 4) = {channel, master_id[2:0]}; R and B are routed
// beat by beat on the ID MSB and the master ID is restored (upper bits zero). This works
// for any return order/interleaving of the controller - none is assumed.
// The SoC ID must fit in 3 bits (the CPU uses ID 0; checked in simulation).
// Adaptations vs. Axi_Mux.v: no WID (controller has none), LOCK is 1 bit, RRESP/BRESP
// are passed through unchanged.
// =============================================================================
module axi_mux_2to1 #(
    parameter S0_ID_W = 4,
    parameter S1_ID_W = 8,
    parameter M_ID_W  = 4,
    parameter AW      = 32,
    parameter DW      = 128
)(
    input                  clk,
    input                  rstn,

    // S0: HDMI, read only
    input  [S0_ID_W-1:0]   s0_arid,
    input  [AW-1:0]        s0_araddr,
    input  [7:0]           s0_arlen,
    input  [2:0]           s0_arsize,
    input  [1:0]           s0_arburst,
    input                  s0_arvalid,
    output                 s0_arready,
    output [S0_ID_W-1:0]   s0_rid,
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

    // M: DDR controller
    output [M_ID_W-1:0]    m_awid,
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
    input  [M_ID_W-1:0]    m_bid,
    input  [1:0]           m_bresp,
    input                  m_bvalid,
    output                 m_bready,
    output [M_ID_W-1:0]    m_arid,
    output [AW-1:0]        m_araddr,
    output [7:0]           m_arlen,
    output [2:0]           m_arsize,
    output [1:0]           m_arburst,
    output                 m_arlock,
    output [3:0]           m_arcache,
    output [2:0]           m_arprot,
    output                 m_arvalid,
    input                  m_arready,
    input  [M_ID_W-1:0]    m_rid,
    input  [DW-1:0]        m_rdata,
    input  [1:0]           m_rresp,
    input                  m_rlast,
    input                  m_rvalid,
    output                 m_rready
);
    localparam LID = M_ID_W - 1;          // bits of master ID carried to the controller

    // ------------------------------------------------------------ write path (S1 only)
    assign m_awid     = {1'b1, s1_awid[LID-1:0]};
    assign m_awaddr   = s1_awaddr;
    assign m_awlen    = s1_awlen;
    assign m_awsize   = s1_awsize;
    assign m_awburst  = s1_awburst;
    assign m_awlock   = s1_awlock;
    assign m_awcache  = s1_awcache;
    assign m_awprot   = s1_awprot;
    assign m_awvalid  = s1_awvalid;
    assign s1_awready = m_awready;
    assign m_wdata    = s1_wdata;
    assign m_wstrb    = s1_wstrb;
    assign m_wlast    = s1_wlast;
    assign m_wvalid   = s1_wvalid;
    assign s1_wready  = m_wready;
    assign s1_bid     = {{(S1_ID_W-LID){1'b0}}, m_bid[LID-1:0]};//去掉用于标识主机的最高位，再补零恢复成 CPU 使用的 8-bit ID。
    assign s1_bresp   = m_bresp;
    assign s1_bvalid  = m_bvalid && m_bid[LID];
    assign m_bready   = m_bid[LID] ? s1_bready : 1'b1;   // ID MSB 0 never occurs on B

    // ------------------------------------------------------------ AR arbiter (S0 > S1)
    reg  ar_lock, ar_sel_r;
    wire ar_sel = ar_lock ? ar_sel_r : !s0_arvalid;   // 0 = S0, 1 = S1; idle: S0 first

        //if (ar_lock)
        //    ar_sel = ar_sel_r;
        //else
        //    ar_sel = !s0_arvalid;    
    
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin ar_lock <= 1'b0; ar_sel_r <= 1'b0; end
        else if (!ar_lock) begin
            if (m_arvalid && !m_arready) begin ar_lock <= 1'b1; ar_sel_r <= ar_sel; end  // not accepted: lock
        end else if (m_arready) ar_lock <= 1'b0;//解除锁定（锁定期间 m_arvalid 一直为 1）
    end

    assign m_arvalid  = ar_sel ? s1_arvalid : s0_arvalid;
    assign m_arid     = ar_sel ? {1'b1, s1_arid[LID-1:0]} : {1'b0, s0_arid[LID-1:0]};
    assign m_araddr   = ar_sel ? s1_araddr  : s0_araddr;
    assign m_arlen    = ar_sel ? s1_arlen   : s0_arlen;
    assign m_arsize   = ar_sel ? s1_arsize  : s0_arsize;
    assign m_arburst  = ar_sel ? s1_arburst : s0_arburst;
    assign m_arlock   = ar_sel ? s1_arlock  : 1'b0;
    assign m_arcache  = ar_sel ? s1_arcache : 4'd0;
    assign m_arprot   = ar_sel ? s1_arprot  : 3'd0;
    assign s0_arready = !ar_sel && m_arready;
    assign s1_arready =  ar_sel && m_arready;

    // ------------------------------------------------------------ R routing by ID MSB
    assign s0_rid    = {{(S0_ID_W-LID){1'b0}}, m_rid[LID-1:0]};
    assign s1_rid    = {{(S1_ID_W-LID){1'b0}}, m_rid[LID-1:0]};
    assign s0_rdata  = m_rdata;
    assign s1_rdata  = m_rdata;
    assign s0_rresp  = m_rresp;
    assign s1_rresp  = m_rresp;
    assign s0_rlast  = m_rlast;
    assign s1_rlast  = m_rlast;
    assign s0_rvalid = m_rvalid && !m_rid[LID];
    assign s1_rvalid = m_rvalid &&  m_rid[LID];
    assign m_rready  = m_rid[LID] ? s1_rready : s0_rready;
endmodule
