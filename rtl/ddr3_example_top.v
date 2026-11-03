//***************************************************************/
//
// Moudel Name    : ddr3_example_top.v
// Version        : 1.1
// Date Created   : 2023-02-23 10:37:59
// Last Modified  : 2023-02-24 15:27:41
// Abstract       : ---
//
//***************************************************************/
//Modification History
//1.initial
//***************************************************************/
`include "ddr3_controller/ddr3_parameter.vh"

`define Efinity_Debug
`timescale 1ps/1ps
// `define SOFT_JTAG

module ddr3_example_top #
(
parameter                       RANK_RATIO         = 1,       // # of unique CS outputs per rank
parameter                       CK_RATIO           = `CK_RATIO, 
parameter                       ASYN_AXI_CLK       = `ASYN_AXI_CLK, 
parameter                       RANKS              = `RANKS,
parameter                       CK_WIDTH           = `CK_WIDTH,       // # of CK/CK# outputs to memory   
parameter                       CKE_WIDTH          = `CKE_WIDTH,       // # of cke outputs
parameter                       CS_WIDTH           = `CS_WIDTH,       // # of unique CS outputs
parameter                       BANK_WIDTH         = `BANK_WIDTH,       // # of bank bits
parameter                       ROW_WIDTH          = `ROW_WIDTH,       // DRAM address bus width
parameter                       COL_WIDTH          = `COL_WIDTH,      // column address width
parameter                       DM_WIDTH           = `DM_WIDTH,       // # of DM (data mask)
parameter                       DQS_WIDTH          = `DQS_WIDTH,       // # of DQS (strobe)
parameter                       DQ_WIDTH           = `DQ_WIDTH,      // # of DQ (data)
parameter                       ODT_WIDTH          = `ODT_WIDTH,
parameter                       DQ_CNT_WIDTH       = `DQ_CNT_WIDTH,       // = ceil(log2(DQ_WIDTH))
parameter                       DQS_CNT_WIDTH      = `DQS_CNT_WIDTH,       // = ceil(log2(DQS_WIDTH))  
parameter                       DRAM_WIDTH         = `DRAM_WIDTH,       // # of DQ per DQS   
parameter                       DATA_WIDTH         = `DATA_WIDTH,
parameter                       ADDR_WIDTH         = `ADDR_WIDTH,    
parameter                       AXI_ID_WIDTH       = `AXI_ID_WIDTH,
parameter                       AXI_ADDR_WIDTH     = `AXI_ADDR_WIDTH,
parameter                       AXI_DATA_WIDTH     = `AXI_DATA_WIDTH,
parameter                       CNT_INIT           = 300000   // DDR reset release delay, in user_clk (core_clk 100 MHz) cycles.
                                                              // 300000 = 1.5 * 200 * 1000 from the original example (1.5 ms assumed at 200 MHz),
                                                              // i.e. 3 ms at 100 MHz. Simulation: override with #(.CNT_INIT(10)).
)
(

   // Clock and reset ports
   input                              axi_clk,      
   input                              core_clk,     // CORE CLK @ 100MHz
   input                              sdram_clk,    // SDRAM CK @ 400MHz
   input                              rx_cal_clk,   // SDRAM CK @ 400MHz
   input                              tx_cal_clk,   // SDRAM CK @ 400MHz
   input                              tx_cal_clk_90edge,   // SDRAM CK @ 400MHz
   input                              pll_locked,
   input                              user_pll_locked,
  //  input                              peripheralClk,
   //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&
   output  wire                       system_uart_0_io_txd,
   input                              system_uart_0_io_rxd,   
   input  [3:0]                       soc_gpio_IN,
   output [3:0]                       soc_gpio_OUT,
   output [3:0]                       soc_gpio_OE,
    // debug core ports
`ifdef  Efinity_Debug  //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&& 
`ifdef SOFT_JTAG
  input                               io_jtag_tms,
  input                               io_jtag_tdi,
  output  wire                        io_jtag_tdo,
  input                               io_jtag_tck,
`else
  input                               jtag_inst1_CAPTURE ,
  input                               jtag_inst1_DRCK    ,
  input                               jtag_inst1_RESET   ,
  input                               jtag_inst1_RUNTEST ,
  input                               jtag_inst1_SEL     ,
  input                               jtag_inst1_SHIFT   ,
  input                               jtag_inst1_TCK     ,
  input                               jtag_inst1_TDI     ,
  input                               jtag_inst1_TMS     ,
  input                               jtag_inst1_UPDATE  ,
  output                              jtag_inst1_TDO     ,
`endif 

`endif  //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&
   // PLL status flags  
   output [2:0]                       pll_shift,  
   output [4:0]                       pll_shift_sel,
   output                             pll_shift_ena,  
   // memory interface ports
   output                             ddr_ck_hi,
   output                             ddr_ck_lo,
   output                             ddr_reset_n,
   output [CKE_WIDTH-1:0]             ddr_cke,     
   output [ROW_WIDTH-1:0]             ddr_addr,
   output [BANK_WIDTH-1:0]            ddr_ba,
   output                             ddr_cas_n,

   output [CS_WIDTH*RANK_RATIO-1:0]   ddr_cs_n,
   output                             ddr_ras_n,
   output                             ddr_we_n,
   
   input  [DQS_WIDTH-1:0]             ddr_dqs_in_hi,
   input  [DQS_WIDTH-1:0]             ddr_dqs_in_lo,
   input  [DQ_WIDTH-1:0]              ddr_dq_in_hi,
   input  [DQ_WIDTH-1:0]              ddr_dq_in_lo,
   
   output [DQS_WIDTH-1:0]             ddr_dqs_oe,
   output [DQS_WIDTH-1:0]             ddr_dqs_oe_n,
   output [DQ_WIDTH-1:0]              ddr_dq_oe,  
   output [DQS_WIDTH-1:0]             ddr_dqs_out_hi,
   output [DQS_WIDTH-1:0]             ddr_dqs_out_lo,
   output [DQ_WIDTH-1:0]              ddr_dq_out_hi,
   output [DQ_WIDTH-1:0]              ddr_dq_out_lo,
   output [DM_WIDTH-1:0]              ddr_dm_hi,
   output [DM_WIDTH-1:0]              ddr_dm_lo,
   output [ODT_WIDTH-1:0]             ddr_odt,

   input                              system_spi_0_io_data_0_IN,
   input                              system_spi_0_io_data_1_IN,
   output                             system_spi_0_io_ss,
   output                             system_spi_0_io_sclk_write,
   output                             system_spi_0_io_data_0_OUT,
   output                             system_spi_0_io_data_0_OE,
   output                             system_spi_0_io_data_1_OUT,
   output                             system_spi_0_io_data_1_OE,
   // HDMI TX (720p60): PLL_BR0 from 27 MHz, LVDS TX half-rate 10:1 (same as Y04/hdmi_tx_720p60)
   input                              hdmi_tx_slow_clk,   // 74.25 MHz pixel clock
   input                              hdmi_tx_locked,
   output [9:0]                       tmds_clk_o,
   output [9:0]                       tmds_data0_o,
   output [9:0]                       tmds_data1_o,
   output [9:0]                       tmds_data2_o,
   output                             tmds_clk_TX_OE,
   output                             tmds_data0_TX_OE,
   output                             tmds_data1_TX_OE,
   output                             tmds_data2_TX_OE,
   output                             tmds_clk_TX_RST,
   output                             tmds_data0_TX_RST,
   output                             tmds_data1_TX_RST,
   output                             tmds_data2_TX_RST,
   output                             HPD_N
   );


      
  localparam   AIW               = AXI_ID_WIDTH      ;
  localparam   ADW               = AXI_DATA_WIDTH    ;
  localparam   ABN               = AXI_DATA_WIDTH/8   ;
  /////////////////////////////////////////////////////////
  // Wire declarations
  reg  [19:0]                       cnt;    
  wire                              clk;
  wire                              rst;
  wire                              mmcm_locked;
  reg                               aresetn;
  wire                              app_sr_active;
  wire                              app_ref_ack;
  wire                              app_zq_ack;

  wire                              cal_done;

  // Slave Interface Write Address Ports
  wire [AXI_ID_WIDTH-1:0]           s_axi_awid;
  wire [AXI_ADDR_WIDTH-1:0]         s_axi_awaddr;
  wire [7:0]                        s_axi_awlen;
  wire [2:0]                        s_axi_awsize;
  wire [1:0]                        s_axi_awburst;
  wire [0:0]                        s_axi_awlock;
  wire [3:0]                        s_axi_awcache;
  wire [2:0]                        s_axi_awprot;
  wire                              s_axi_awvalid;
  wire                              s_axi_awready;
   // Slave Interface Write Data Ports
  wire [AXI_DATA_WIDTH-1:0]         s_axi_wdata;
  wire [(AXI_DATA_WIDTH/8)-1:0]     s_axi_wstrb;
  wire                              s_axi_wlast;
  wire                              s_axi_wvalid;
  wire                              s_axi_wready;
   // Slave Interface Write Response Ports
  wire                              s_axi_bready;
  wire [AXI_ID_WIDTH-1:0]           s_axi_bid;
  wire [1:0]                        s_axi_bresp;
  wire                              s_axi_bvalid;
   // Slave Interface Read Address Ports
  wire [AXI_ID_WIDTH-1:0]           s_axi_arid;
  wire [AXI_ADDR_WIDTH-1:0]         s_axi_araddr;
  wire [7:0]                        s_axi_arlen;
  wire [2:0]                        s_axi_arsize;
  wire [1:0]                        s_axi_arburst;
  wire [0:0]                        s_axi_arlock;
  wire [3:0]                        s_axi_arcache;
  wire [2:0]                        s_axi_arprot;
  wire                              s_axi_arvalid;
  wire                              s_axi_arready;
   // Slave Interface Read Data Ports
  wire                              s_axi_rready;
  wire [AXI_ID_WIDTH-1:0]           s_axi_rid;
  wire [AXI_DATA_WIDTH-1:0]         s_axi_rdata;
  wire [1:0]                        s_axi_rresp;
  wire                              s_axi_rlast;
  wire                              s_axi_rvalid;


//***************************************************************************
  wire [2:0]                        vio_pll_shift;  
  wire [4:0]                        vio_pll_shift_sel;
  wire                              vio_pll_shift_ena; 
  wire                              pos_pll_shift_ena; 
  wire [2:0]                        phy_pll_shift;  
  wire [4:0]                        phy_pll_shift_sel;
  wire                              phy_pll_shift_ena; 
  wire                              ddr_reset; 
  wire                              sys_rst;
  wire  [2:0]                       phy_wr_pll_shift;
  wire                              idelay_ld       ;
  wire                              mpr_rdlvl_dly   ;
  wire  [7:0]                       wrlvl_dq_check  ;
  wire  [7:0]                       rd_level_dqs_check;  
  wire  [2:0]                       rdlvl_shift;  
  wire  [2:0]                       wrlvl_shift;  
  wire  [15:0]                      debug_fifo;  
  wire  [15:0]                      overflow_fifo;  
  wire  [6:0]                       init_cur_state;  
  wire  [35:0]                      ddr_debug_port;  
  wire                              app_rdy;  
  wire                              user_clk;  
  wire                              ddr_rstn;  
  wire                                phy_rddata_valid;
  wire [2*CK_RATIO*DQ_WIDTH-1:0]      phy_rd_data;
  wire [2*CK_RATIO*DATA_WIDTH-1:0]    app_rd_data_to_axi; 
  wire                                app_rd_data_end;
  wire                                app_rd_data_valid;
  wire [2*CK_RATIO*DQ_WIDTH-1:0]      rd_data;          
  wire                                rd_data_en;            
  wire                                rd_data_end;   
   
  
// Start of User Design top instance
//***************************************************************************
// The User design is instantiated below. The memory interface ports are
// connected to the top-level and the application interface ports are
// connected to the traffic generator module. This provides a reference
// for connecting the memory controller to system.
//***************************************************************************
always @(posedge user_clk or negedge sys_rst) begin
	if (!sys_rst)
		cnt <= 0;
    else if (cnt != CNT_INIT) 
        cnt <= cnt + 20'd1;
	else 
        cnt <= cnt;
end
assign ddr_rstn = (cnt == CNT_INIT);


generate
if (ASYN_AXI_CLK) begin
assign user_clk = axi_clk;

end else begin
assign user_clk = core_clk;
end
endgenerate
//***************************************************************************
// The traffic generation module instantiated below drives traffic (patterns)
// on the application interface of the memory controller
//***************************************************************************
//***************************************************************************

assign sys_rst       = pll_locked & user_pll_locked;

//***************************************************************************
ddr3_top                 u_ddr3_top
      (
      
    .axi_clk                (user_clk             ),
    .core_clk               (core_clk            ),
    .sdram_clk              (sdram_clk           ),  
    .rx_cal_clk             (rx_cal_clk          ),
    .tx_cal_clk             (tx_cal_clk          ),
    .tx_cal_clk_90edge      (tx_cal_clk_90edge   ),
    .rstn                   (ddr_rstn            ),      
    .pll_shift              (pll_shift           ),
    .pll_shift_sel          (pll_shift_sel       ),
    .pll_shift_ena          (pll_shift_ena       ), 
///////////////DDR BUS
    .ddr_ck_hi              (ddr_ck_hi           ),
    .ddr_ck_lo              (ddr_ck_lo           ),
    .ddr_cke                (ddr_cke             ),    
    .ddr_reset_n            (ddr_reset_n         ),
    .ddr_cs_n               (ddr_cs_n            ),
    .ddr_ras_n              (ddr_ras_n           ),
    .ddr_cas_n              (ddr_cas_n           ),
    .ddr_we_n               (ddr_we_n            ),     
    .ddr_addr               (ddr_addr            ),
    .ddr_ba                 (ddr_ba              ),

    .ddr_dqs_oe             (ddr_dqs_oe          ),
    .ddr_dqs_oe_n           (ddr_dqs_oe_n        ),
    .ddr_dq_oe              (ddr_dq_oe           ),
    .ddr_dqs_in_hi          (ddr_dqs_in_hi       ),
    .ddr_dqs_in_lo          (ddr_dqs_in_lo       ),
    .ddr_dq_in_hi           (ddr_dq_in_hi        ),
    .ddr_dq_in_lo           (ddr_dq_in_lo        ),

    .ddr_dqs_out_hi         (ddr_dqs_out_hi      ),
    .ddr_dqs_out_lo         (ddr_dqs_out_lo      ),
    .ddr_dq_out_hi          (ddr_dq_out_hi       ),
    .ddr_dq_out_lo          (ddr_dq_out_lo       ),
    
    .ddr_dm_hi              (ddr_dm_hi           ),
    .ddr_dm_lo              (ddr_dm_lo           ),
    .ddr_odt                (ddr_odt             ),

// Application interface ports
       .app_sr_req                     (1'b0),
       .app_ref_req                    (1'b0),
       .app_zq_req                     (1'b0),
       .app_sr_active                  (app_sr_active),
       .app_ref_ack                    (app_ref_ack),
       .app_zq_ack                     (app_zq_ack),

// Slave Interface Write Address Ports
       .s_axi_awid                     (s_axi_awid        ),
       .s_axi_awaddr                   (s_axi_awaddr      ),
       .s_axi_awlen                    (s_axi_awlen       ),
       .s_axi_awsize                   (s_axi_awsize      ),
       .s_axi_awburst                  (s_axi_awburst     ),
       .s_axi_awlock                   (s_axi_awlock      ),
       .s_axi_awcache                  (s_axi_awcache     ),
       .s_axi_awprot                   (s_axi_awprot      ),
       .s_axi_awqos                    (4'h0              ),
       .s_axi_awvalid                  (s_axi_awvalid     ),
       .s_axi_awready                  (s_axi_awready     ),
// Slave Interface Write Data Ports
       .s_axi_wdata                    (s_axi_wdata       ),
       .s_axi_wstrb                    (s_axi_wstrb       ),
       .s_axi_wlast                    (s_axi_wlast       ),
       .s_axi_wvalid                   (s_axi_wvalid      ),
       .s_axi_wready                   (s_axi_wready      ),
// Slave Interface Write Response Ports
       .s_axi_bid                      (s_axi_bid         ),
       .s_axi_bresp                    (s_axi_bresp       ),
       .s_axi_bvalid                   (s_axi_bvalid      ),
       .s_axi_bready                   (s_axi_bready      ),
// Slave Interface Read Address Ports
       .s_axi_arid                     (s_axi_arid        ),
       .s_axi_araddr                   (s_axi_araddr      ),
       .s_axi_arlen                    (s_axi_arlen       ),
       .s_axi_arsize                   (s_axi_arsize      ),
       .s_axi_arburst                  (s_axi_arburst     ),
       .s_axi_arlock                   (s_axi_arlock      ),
       .s_axi_arcache                  (s_axi_arcache     ),
       .s_axi_arprot                   (s_axi_arprot      ),
       .s_axi_arqos                    (4'h0              ),
       .s_axi_arvalid                  (s_axi_arvalid     ),
       .s_axi_arready                  (s_axi_arready     ),
// Slave Interface Read Data Ports
       .s_axi_rid                      (s_axi_rid         ),
       .s_axi_rdata                    (s_axi_rdata       ),
       .s_axi_rresp                    (s_axi_rresp       ),
       .s_axi_rlast                    (s_axi_rlast       ),
       .s_axi_rvalid                   (s_axi_rvalid      ),
       .s_axi_rready                   (s_axi_rready      ),
//DEBUG       
       .wrlvl_dq_check                 (wrlvl_dq_check    ) ,
       .rd_level_dqs_check             (rd_level_dqs_check) ,
       .rdlvl_shift                    (rdlvl_shift) ,
       .wrlvl_shift                    (wrlvl_shift) ,
       .init_cur_state                 (init_cur_state    ) ,
       .idelay_ld                      (idelay_ld         ) ,
       .mpr_rdlvl_dly                  (mpr_rdlvl_dly     ) ,
       .ddr_debug_port                 (ddr_debug_port    ) ,
       .cal_done                       (cal_done          ) 
       );
// End of User Design top instance

//***************************************************************************


//***************************************************************************
soc u_sapphire_soc(
    .io_asyncReset                      (!pll_locked                        ),
    .io_systemClk                       (user_clk                           ),

    //UART 0
    .system_uart_0_io_txd               (system_uart_0_io_txd               ),
    .system_uart_0_io_rxd               (system_uart_0_io_rxd               ),
    .io_memoryClk                       (user_clk                           ),
    .io_memoryReset                     (                                   ),
    .io_systemReset                     (                                   ),
    //External Memory AXI4 Interface
    .io_ddrA_aw_payload_prot             (s1_awprot                         ),
    .io_ddrA_aw_payload_qos              (                                  ),
    .io_ddrA_aw_payload_cache            (s1_awcache                        ),
    .io_ddrA_aw_payload_lock             (s1_awlock                         ),
    .io_ddrA_aw_payload_burst            (s1_awburst                        ),
    .io_ddrA_aw_payload_size             (s1_awsize                         ),
    .io_ddrA_aw_payload_len              (s1_awlen                          ),
    .io_ddrA_aw_payload_region           (                                  ),
    .io_ddrA_aw_payload_id               (s1_awid                           ),
    .io_ddrA_aw_payload_addr             (s1_awaddr                         ),
    .io_ddrA_aw_ready                    (s1_awready                        ),
    .io_ddrA_aw_valid                    (s1_awvalid                        ),
    .io_ddrA_w_payload_last              (s1_wlast                          ),
    .io_ddrA_w_ready                     (s1_wready                         ),
    .io_ddrA_w_valid                     (s1_wvalid                         ),
    .io_ddrA_w_payload_strb              (s1_wstrb                          ),
    .io_ddrA_w_payload_data              (s1_wdata                          ),
    .io_ddrA_b_payload_resp              (s1_bresp                          ),
    .io_ddrA_b_payload_id                (s1_bid                            ),
    .io_ddrA_b_ready                     (s1_bready                         ),
    .io_ddrA_b_valid                     (s1_bvalid                         ),
    .io_ddrA_ar_payload_prot             (s1_arprot                         ),
    .io_ddrA_ar_payload_qos              (                                  ),
    .io_ddrA_ar_payload_cache            (s1_arcache                        ),
    .io_ddrA_ar_payload_region           (                                  ),
    .io_ddrA_ar_payload_lock             (s1_arlock                         ),
    .io_ddrA_ar_payload_burst            (s1_arburst                        ),
    .io_ddrA_ar_payload_size             (s1_arsize                         ),
    .io_ddrA_ar_payload_len              (s1_arlen                          ),
    .io_ddrA_ar_payload_id               (s1_arid                           ),
    .io_ddrA_ar_payload_addr             (s1_araddr                         ),
    .io_ddrA_ar_ready                    (s1_arready                        ),
    .io_ddrA_ar_valid                    (s1_arvalid                        ),
    .io_ddrA_r_payload_last              (s1_rlast                          ),
    .io_ddrA_r_payload_resp              (s1_rresp                          ),
    .io_ddrA_r_payload_id                (s1_rid                            ),
    .io_ddrA_r_payload_data              (s1_rdata                          ),
    .io_ddrA_r_ready                     (s1_rready                         ),
    .io_ddrA_r_valid                     (s1_rvalid                         ),
     //SPI 0
    .system_spi_0_io_sclk_write         (system_spi_0_io_sclk_write         ),
    .system_spi_0_io_data_0_writeEnable (system_spi_0_io_data_0_OE ),
    .system_spi_0_io_data_0_read        (system_spi_0_io_data_0_IN        ),
    .system_spi_0_io_data_0_write       (system_spi_0_io_data_0_OUT       ),
    .system_spi_0_io_data_1_writeEnable (system_spi_0_io_data_1_OE ),
    .system_spi_0_io_data_1_read        (system_spi_0_io_data_1_IN        ),
    .system_spi_0_io_data_1_write       (system_spi_0_io_data_1_OUT       ),
    .system_spi_0_io_data_2_writeEnable (                                   ),
    .system_spi_0_io_data_2_read        (                                   ),
    .system_spi_0_io_data_2_write       (                                   ),
    .system_spi_0_io_data_3_writeEnable (                                   ),
    .system_spi_0_io_data_3_read        (                                   ),
    .system_spi_0_io_data_3_write       (                                   ),
    .system_spi_0_io_ss                 (system_spi_0_io_ss                 ),
    //AXI A (0xE1000000): framebuffer / HDMI control registers (fb_regs_axil)
    .axiA_awvalid                       (a_awvalid                           ),
    .axiA_awready                       (a_awready                           ),
    .axiA_awaddr                        (a_awaddr                            ),
    .axiA_awid                          (a_awid                              ),
    .axiA_awregion                      (                                    ),
    .axiA_awlen                         (a_awlen                             ),
    .axiA_awsize                        (                                    ),
    .axiA_awburst                       (                                    ),
    .axiA_awlock                        (                                    ),
    .axiA_awcache                       (                                    ),
    .axiA_awqos                         (                                    ),
    .axiA_awprot                        (                                    ),
    .axiA_wvalid                        (a_wvalid                            ),
    .axiA_wready                        (a_wready                            ),
    .axiA_wdata                         (a_wdata                             ),
    .axiA_wstrb                         (a_wstrb                             ),
    .axiA_wlast                         (a_wlast                             ),
    .axiA_bvalid                        (a_bvalid                            ),
    .axiA_bready                        (a_bready                            ),
    .axiA_bid                           (a_bid                               ),
    .axiA_bresp                         (a_bresp                             ),
    .axiA_arvalid                       (a_arvalid                           ),
    .axiA_arready                       (a_arready                           ),
    .axiA_araddr                        (a_araddr                            ),
    .axiA_arid                          (a_arid                              ),
    .axiA_arregion                      (                                    ),
    .axiA_arlen                         (a_arlen                             ),
    .axiA_arsize                        (                                    ),
    .axiA_arburst                       (                                    ),
    .axiA_arlock                        (                                    ),
    .axiA_arcache                       (                                    ),
    .axiA_arqos                         (                                    ),
    .axiA_arprot                        (                                    ),
    .axiA_rvalid                        (a_rvalid                            ),
    .axiA_rready                        (a_rready                            ),
    .axiA_rdata                         (a_rdata                             ),
    .axiA_rid                           (a_rid                               ),
    .axiA_rresp                         (a_rresp                             ),
    .axiA_rlast                         (a_rlast                             ),
    .axiAInterrupt                      (1'b0                               ),
    //User interrupt 0: reserved for the BitBlt done interrupt
    .userInterruptA                     (1'b0                               ),

    .system_gpio_0_io_write             ( soc_gpio_OUT                      ),//,
    .system_gpio_0_io_read              ( soc_gpio_IN                       ),//
    .system_gpio_0_io_writeEnable       ( soc_gpio_OE                       ),//, 


    //Hard Jtag Tap
    .jtagCtrl_tck                       (jtag_inst1_TCK                     ),
    .jtagCtrl_tdi                       (jtag_inst1_TDI                     ),
    .jtagCtrl_tdo                       (jtag_inst1_TDO                     ),
    .jtagCtrl_enable                    (jtag_inst1_SEL                     ),
    .jtagCtrl_capture                   (jtag_inst1_CAPTURE                 ),
    .jtagCtrl_shift                     (jtag_inst1_SHIFT                   ),
    .jtagCtrl_update                    (jtag_inst1_UPDATE                  ),
    .jtagCtrl_reset                     (jtag_inst1_RESET                   )

);


//***************************************************************************


//***************************************************************************


//***************************************************************************
// Framebuffer display: registers (axiA), triple-buffer page control, HDMI AXI read master,
// 8 KiB async FIFO, CPU/HDMI -> DDR AXI mux, 720p60 RGB565, FPS overlay, TMDS encoding.
//***************************************************************************
  // SoC io_ddrA (mux S1)
  wire [7:0]   s1_awid, s1_arid, s1_bid, s1_rid, s1_awlen, s1_arlen;
  wire [31:0]  s1_awaddr, s1_araddr;
  wire [2:0]   s1_awsize, s1_arsize, s1_awprot, s1_arprot;
  wire [1:0]   s1_awburst, s1_arburst, s1_bresp, s1_rresp;
  wire [0:0]   s1_awlock, s1_arlock;
  wire [3:0]   s1_awcache, s1_arcache;
  wire         s1_awvalid, s1_awready, s1_wlast, s1_wvalid, s1_wready, s1_bvalid, s1_bready;
  wire         s1_arvalid, s1_arready, s1_rlast, s1_rvalid, s1_rready;
  wire [127:0] s1_wdata, s1_rdata;
  wire [15:0]  s1_wstrb;
  // SoC axiA (registers)
  wire [7:0]   a_awid, a_arid, a_awlen, a_arlen, a_bid, a_rid;
  wire [31:0]  a_awaddr, a_araddr, a_wdata, a_rdata;
  wire [3:0]   a_wstrb;
  wire [1:0]   a_bresp, a_rresp;
  wire         a_awvalid, a_awready, a_wlast, a_wvalid, a_wready, a_bvalid, a_bready;
  wire         a_arvalid, a_arready, a_rlast, a_rvalid, a_rready;
  // controller side: 32-bit addresses from the mux, truncated to the controller width
  wire [31:0]  m_awaddr, m_araddr;
  assign s_axi_awaddr = m_awaddr[AXI_ADDR_WIDTH-1:0];
  assign s_axi_araddr = m_araddr[AXI_ADDR_WIDTH-1:0];

  // resets: asynchronous assertion, synchronous release; the pixel domain is held in reset
  // while the HDMI PLL is not locked (no FIFO consumption without a valid pixel clock)
  reg [2:0] sys_rst_sync;
  always @(posedge user_clk or negedge ddr_rstn)
    if (!ddr_rstn) sys_rst_sync <= 3'd0; else sys_rst_sync <= {sys_rst_sync[1:0], 1'b1};
  wire fb_sys_rstn = sys_rst_sync[2];
  wire pix_rstn;
  reset #(.IN_RST_ACTIVE("LOW"), .OUT_RST_ACTIVE("LOW"), .CYCLE(3)) u_pix_rst (
    .i_arst(hdmi_tx_locked & fb_sys_rstn), .i_clk(hdmi_tx_slow_clk), .o_srst(pix_rstn));

  wire [23:0] pix_rgb;
  wire        pix_hs, pix_vs, pix_de, pix_swap_done, pix_frame_start;
  wire [6:0]  render_fps;

  fb_hdmi_subsys #(.M_ID_W(AXI_ID_WIDTH), .S1_ID_W(8)) u_fb (
    .sys_clk(user_clk), .sys_rstn(fb_sys_rstn), .pix_clk(hdmi_tx_slow_clk), .pix_rstn(pix_rstn), .cal_done(cal_done),
    .a_awid(a_awid), .a_awaddr(a_awaddr), .a_awlen(a_awlen), .a_awvalid(a_awvalid), .a_awready(a_awready),
    .a_wdata(a_wdata), .a_wstrb(a_wstrb), .a_wlast(a_wlast), .a_wvalid(a_wvalid), .a_wready(a_wready),
    .a_bid(a_bid), .a_bresp(a_bresp), .a_bvalid(a_bvalid), .a_bready(a_bready),
    .a_arid(a_arid), .a_araddr(a_araddr), .a_arlen(a_arlen), .a_arvalid(a_arvalid), .a_arready(a_arready),
    .a_rid(a_rid), .a_rdata(a_rdata), .a_rresp(a_rresp), .a_rlast(a_rlast), .a_rvalid(a_rvalid), .a_rready(a_rready),
    .s1_awid(s1_awid), .s1_awaddr(s1_awaddr), .s1_awlen(s1_awlen), .s1_awsize(s1_awsize), .s1_awburst(s1_awburst),
    .s1_awlock(s1_awlock), .s1_awcache(s1_awcache), .s1_awprot(s1_awprot), .s1_awvalid(s1_awvalid), .s1_awready(s1_awready),
    .s1_wdata(s1_wdata), .s1_wstrb(s1_wstrb), .s1_wlast(s1_wlast), .s1_wvalid(s1_wvalid), .s1_wready(s1_wready),
    .s1_bid(s1_bid), .s1_bresp(s1_bresp), .s1_bvalid(s1_bvalid), .s1_bready(s1_bready),
    .s1_arid(s1_arid), .s1_araddr(s1_araddr), .s1_arlen(s1_arlen), .s1_arsize(s1_arsize), .s1_arburst(s1_arburst),
    .s1_arlock(s1_arlock), .s1_arcache(s1_arcache), .s1_arprot(s1_arprot), .s1_arvalid(s1_arvalid), .s1_arready(s1_arready),
    .s1_rid(s1_rid), .s1_rdata(s1_rdata), .s1_rresp(s1_rresp), .s1_rlast(s1_rlast), .s1_rvalid(s1_rvalid), .s1_rready(s1_rready),
    .m_awid(s_axi_awid), .m_awaddr(m_awaddr), .m_awlen(s_axi_awlen), .m_awsize(s_axi_awsize), .m_awburst(s_axi_awburst),
    .m_awlock(s_axi_awlock), .m_awcache(s_axi_awcache), .m_awprot(s_axi_awprot), .m_awvalid(s_axi_awvalid), .m_awready(s_axi_awready),
    .m_wdata(s_axi_wdata), .m_wstrb(s_axi_wstrb), .m_wlast(s_axi_wlast), .m_wvalid(s_axi_wvalid), .m_wready(s_axi_wready),
    .m_bid(s_axi_bid), .m_bresp(s_axi_bresp), .m_bvalid(s_axi_bvalid), .m_bready(s_axi_bready),
    .m_arid(s_axi_arid), .m_araddr(m_araddr), .m_arlen(s_axi_arlen), .m_arsize(s_axi_arsize), .m_arburst(s_axi_arburst),
    .m_arlock(s_axi_arlock), .m_arcache(s_axi_arcache), .m_arprot(s_axi_arprot), .m_arvalid(s_axi_arvalid), .m_arready(s_axi_arready),
    .m_rid(s_axi_rid), .m_rdata(s_axi_rdata), .m_rresp(s_axi_rresp), .m_rlast(s_axi_rlast), .m_rvalid(s_axi_rvalid), .m_rready(s_axi_rready),
    .pix_rgb(pix_rgb), .pix_hs(pix_hs), .pix_vs(pix_vs), .pix_de(pix_de),
    .pix_swap_done(pix_swap_done), .pix_frame_start(pix_frame_start));

  // ---------------------------------------------------------------- HDMI source test
  // Diagnostic build switch.  With TEST_STATIC_PIXELS = 1 the TMDS encoder is fed a
  // constant colour instead of the framebuffer.  Video timing, the DVI encoder, the
  // LVDS serialisers, the board wiring, the cable and the monitor are all identical -
  // only the pixel source changes.  So, comparing this build with the normal one on
  // the same monitor and cable splits the remaining suspects in two:
  //   still flickers -> the framebuffer read path is innocent, the fault is in the
  //                     link / monitor chain (cable, connector, scaler, backlight);
  //   rock steady    -> the fault is inside fb_read_path (FIFO / read master / DDR).
  // Default 0: normal operation, the logic below constant-folds away.
  localparam TEST_STATIC_PIXELS = 1'b0;
  wire [23:0] hdmi_rgb = TEST_STATIC_PIXELS ? 24'h808080 : pix_rgb;

  hdmi_out u_hdmi_out (
    .pix_clk(hdmi_tx_slow_clk), .pix_rstn(pix_rstn),
    .frame_start(pix_frame_start), .swap_done(pix_swap_done),
    .rgb(hdmi_rgb), .hs(pix_hs), .vs(pix_vs), .de(pix_de),
    .tmds_clk_o(tmds_clk_o), .tmds_data0_o(tmds_data0_o), .tmds_data1_o(tmds_data1_o), .tmds_data2_o(tmds_data2_o),
    .render_fps(render_fps));

  assign tmds_clk_TX_OE    = 1'b1;
  assign tmds_data0_TX_OE  = 1'b1;
  assign tmds_data1_TX_OE  = 1'b1;
  assign tmds_data2_TX_OE  = 1'b1;
  assign tmds_clk_TX_RST   = 1'b0;
  assign tmds_data0_TX_RST = 1'b0;
  assign tmds_data1_TX_RST = 1'b0;
  assign tmds_data2_TX_RST = 1'b0;
  // HPD_N (GPIOR_N_15) = HDMI_RX_HPD_N0: gate of Q3 (BSS138) in the hot-plug circuit of the HDMI *input*
  // connector (schematic "HPD Drive"); 0 = RX hot-plug asserted, 1 = de-asserted. The RX port is not used,
  // so it is de-asserted. The TX connector HPD is not wired to the FPGA.
  assign HPD_N             = 1'b1;

endmodule
