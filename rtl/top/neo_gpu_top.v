// ============================================================================
// Module: neo_gpu_top
// Description: Top Level Integration of NeoGPU SoC
//              Integrates PCIe, Hub, GPC Clusters, Copy Engine, and HBM
//              Handles clock domain crossing and reset synchronization
// ============================================================================
// Add to neo_gpu_top.v
`ifdef VERILATOR

// DPI-C exports for co-simulation
export "DPI-C" task dpi_pcie_tx_tlp;
export "DPI-C" task dpi_pcie_rx_poll;

task dpi_pcie_tx_tlp;
    input [31:0] data;
    input        valid;
    input        is_irq;
    begin
        // Connect to actual pcie_tx signals
        pcie_tx_tlp_data = data;
        pcie_tx_tlp_valid = valid;
        if (is_irq) pcie_irq_req = 1'b1;
    end
endtask

task dpi_pcie_rx_poll;
    output        valid;
    output [63:0] addr;
    output [31:0] data;
    output        is_write;
    output [3:0]  byte_en;
    begin
        valid = pcie_rx_tlp_valid;
        addr = pcie_rx_tlp_addr;
        data = pcie_rx_tlp_data;
        is_write = pcie_rx_tlp_is_write;
        byte_en = pcie_rx_tlp_be;
        pcie_rx_tlp_ready = 1'b1;  // Accept in this cycle
    end
endtask

`endif

module neo_gpu_top #(
    // System Configuration
    parameter NUM_GPC           = 4,        // Number of Graphics Processing Clusters
    parameter NUM_SM_PER_GPC    = 4,        // SMs per GPC
    parameter NUM_WARPS_PER_SM  = 32,       // Warps per SM
    parameter NUM_LANES_PER_SM  = 32,       // SIMD lanes per SM
    
    // Memory Configuration
    parameter HBM_SIZE_MB       = 4096,     // 4GB HBM
    parameter HBM_CHANNELS      = 4,        // 4 HBM pseudo-channels
    parameter L2_CACHE_SIZE_KB  = 512,      // 512KB L2 Cache
    
    // PCIe Configuration
    parameter PCIE_DATA_W       = 32,       // PCIe TLP data width
    parameter PCIE_ADDR_W       = 64,       // PCIe address width
    parameter NUM_MSI_VECTORS   = 8         // Number of MSI interrupt vectors
) (
    // System Clocks
    input  wire                 clk_sys,            // GPU Core Clock (1GHz)
    input  wire                 clk_pcie,           // PCIe Clock (250MHz/500MHz)
    
    // Global Reset (Active Low, Async)
    input  wire                 rst_n,
    
    // PCIe PHY Interface (External Pins)
    input  wire [PCIE_ADDR_W-1:0] pcie_rx_tlp_addr,
    input  wire [PCIE_DATA_W-1:0] pcie_rx_tlp_data,
    input  wire [3:0]           pcie_rx_tlp_be,
    input  wire                 pcie_rx_tlp_valid,
    input  wire                 pcie_rx_tlp_is_write,
    output wire                 pcie_rx_tlp_ready,
    
    output wire [PCIE_DATA_W-1:0] pcie_tx_tlp_data,
    output wire                 pcie_tx_tlp_valid,
    input  wire                 pcie_tx_tlp_ready,
    
    output wire                 pcie_irq_req,       // MSI Interrupt request
    
    // Debug Interface (Optional JTAG/UART)
    input  wire                 dbg_en,
    input  wire [7:0]           dbg_addr,
    output wire [31:0]          dbg_data,
    input  wire                 dbg_wr,
    input  wire [31:0]          dbg_wdata,
    
    // Power Management (Optional)
    input  wire                 pwr_sleep_req,      // Enter sleep mode
    output wire                 pwr_sleep_ack,      // Sleep acknowledge
    
    // Status Outputs (for monitoring)
    output wire [31:0]          status_gpu_busy,    // High when GPU working
    output wire [31:0]          status_interrupts   // Pending interrupts
);

    // =========================================================================
    // Clock Domain Crossing (CDC) Infrastructure
    // =========================================================================
    
    // Reset Synchronization
    // Generate synchronized resets for each clock domain
    reg [2:0] rst_sys_sync, rst_pcie_sync;
    wire rst_sys_n, rst_pcie_n;
    
    always @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            rst_sys_sync <= 3'b000;
        else
            rst_sys_sync <= {rst_sys_sync[1:0], 1'b1};
    end
    assign rst_sys_n = rst_sys_sync[2];
    
    always @(posedge clk_pcie or negedge rst_n) begin
        if (!rst_n)
            rst_pcie_sync <= 3'b000;
        else
            rst_pcie_sync <= {rst_pcie_sync[1:0], 1'b1};
    end
    assign rst_pcie_n = rst_pcie_sync[2];
    
    // =========================================================================
    // Async FIFO for PCIe -> System Clock Domain Crossing
    // =========================================================================
    
    // Doorbell CDC (PCIe -> SYS)
    wire [63:0] doorbell_addr_sys;
    wire [31:0] doorbell_data_sys;
    wire        doorbell_valid_sys;
    wire        doorbell_ready_pcie;
    
    // Simple 2-flop synchronizer for control signals (pulse stretching required)
    // For data, use dual-clock FIFO (simplified here as direct connection with valid/ready)
    // In real design, use proper dual-clock FIFO
    
    // Simplified CDC: Assume clock ratio is integer and use handshaking
    reg         db_valid_pcie_clk;
    reg [63:0]  db_addr_pcie_clk;
    reg [31:0]  db_data_pcie_clk;
    reg         db_ack_sys_clk;
    
    always @(posedge clk_pcie or negedge rst_pcie_n) begin
        if (!rst_pcie_n) begin
            db_valid_pcie_clk <= 1'b0;
        end else begin
            if (pcie_rx_tlp_valid && pcie_rx_tlp_is_write && 
                pcie_rx_tlp_addr[15:0] == 16'h1000) begin // Doorbell address
                db_valid_pcie_clk <= 1'b1;
                db_addr_pcie_clk <= pcie_rx_tlp_addr;
                db_data_pcie_clk <= pcie_rx_tlp_data;
            end else if (doorbell_ready_pcie) begin
                db_valid_pcie_clk <= 1'b0;
            end
        end
    end
    
    // 2-flop sync to sys clock
    reg [2:0] db_valid_sync;
    always @(posedge clk_sys or negedge rst_sys_n) begin
        if (!rst_sys_n)
            db_valid_sync <= 3'b000;
        else
            db_valid_sync <= {db_valid_sync[1:0], db_valid_pcie_clk};
    end
    
    assign doorbell_valid_sys = db_valid_sync[1] && !db_valid_sync[2]; // Rising edge detect
    assign doorbell_addr_sys = db_addr_pcie_clk;
    assign doorbell_data_sys = db_data_pcie_clk;
    assign doorbell_ready_pcie = !db_valid_pcie_clk; // Simple handshake
    
    // =========================================================================
    // Module Instantiations
    // =========================================================================
    
    // PCIe Bridge (in PCIe clock domain mostly)
    wire [31:0]  pcie_cfg_addr;
    wire [63:0]  pcie_cfg_wdata;
    wire         pcie_cfg_wr;
    wire         pcie_cfg_rd;
    wire [63:0]  pcie_cfg_rdata;
    wire         pcie_cfg_ready;
    
    wire [63:0]  pcie_bar0_addr;
    wire [63:0]  pcie_bar0_wdata;
    wire         pcie_bar0_wr;
    wire         pcie_bar0_rd;
    wire [63:0]  pcie_bar0_rdata;
    wire         pcie_bar0_ready;
    
    pciex5_bridge #(
        .ADDR_W(PCIE_ADDR_W),
        .DATA_W(PCIE_DATA_W),
        .NUM_IRQ(NUM_MSI_VECTORS)
    ) u_pcie_bridge (
        .clk_pcie(clk_pcie),
        .clk_sys(clk_sys),
        .rst_n(rst_pcie_n),
        .rx_tlp_addr(pcie_rx_tlp_addr),
        .rx_tlp_data(pcie_rx_tlp_data),
        .rx_tlp_be(pcie_rx_tlp_be),
        .rx_tlp_valid(pcie_rx_tlp_valid),
        .rx_tlp_is_write(pcie_rx_tlp_is_write),
        .rx_tlp_ready(pcie_rx_tlp_ready),
        .tx_tlp_data(pcie_tx_tlp_data),
        .tx_tlp_valid(pcie_tx_tlp_valid),
        .tx_tlp_ready(pcie_tx_tlp_ready),
        .cfg_vendor_id(),
        .cfg_device_id(),
        .cfg_bar0_addr(),
        .cfg_bar1_addr(),
        .bar0_reg_addr(pcie_cfg_addr),
        .bar0_reg_wdata(pcie_cfg_wdata),
        .bar0_reg_wr(pcie_cfg_wr),
        .bar0_reg_rd(pcie_cfg_rd),
        .bar0_reg_rdata(pcie_cfg_rdata),
        .bar0_reg_ready(pcie_cfg_ready),
        .bar1_mem_addr(pcie_bar0_addr),
        .bar1_mem_wdata(pcie_bar0_wdata),
        .bar1_mem_wr(pcie_bar0_wr),
        .bar1_mem_rd(pcie_bar0_rd),
        .bar1_mem_rdata(pcie_bar0_rdata),
        .bar1_mem_ready(pcie_bar0_ready),
        .doorbell_addr(db_addr_pcie_clk),
        .doorbell_data(db_data_pcie_clk),
        .doorbell_valid(db_valid_pcie_clk),
        .doorbell_ready(doorbell_ready_pcie),
        .msi_irq(),
        .msi_valid(),
        .msi_ready(1'b1),
        .link_up(),
        .rx_counter(),
        .tx_counter()
    );
    
    // Hub Block (System Clock Domain)
    wire [NUM_GPC-1:0] gpc_done_valid_hub;
    wire [NUM_GPC-1:0] gpc_done_ready_hub;
    wire [63:0]        gpc_grid_base_addr_hub;
    wire [15:0]        gpc_grid_dim_x_hub, gpc_grid_dim_y_hub, gpc_grid_dim_z_hub;
    wire [31:0]        gpc_kernel_args_hub;
    wire [$clog2(NUM_GPC)-1:0] gpc_dispatch_id_hub;
    wire               gpc_dispatch_valid_hub;
    wire [NUM_GPC-1:0] gpc_dispatch_ready_hub;
    
    // Hub <-> GPC L2 Interface (Aggregated)
    wire [63:0]        gpc_l2_addr     [0:NUM_GPC-1];
    wire [127:0]       gpc_l2_wdata    [0:NUM_GPC-1];
    wire               gpc_l2_wr_en    [0:NUM_GPC-1];
    wire [15:0]        gpc_l2_be       [0:NUM_GPC-1];
    wire               gpc_l2_valid    [0:NUM_GPC-1];
    wire               gpc_l2_ready    [0:NUM_GPC-1];
    wire [127:0]       gpc_l2_rdata    [0:NUM_GPC-1];
    wire               gpc_l2_rvalid   [0:NUM_GPC-1];
    
    // Hub <-> HBM
    wire [63:0]        hbm_req_addr_hub;
    wire [511:0]       hbm_req_data_hub;
    wire               hbm_req_wr_en_hub;
    wire               hbm_req_valid_hub;
    wire               hbm_req_ready_hub;
    wire [511:0]       hbm_resp_data_hub;
    wire               hbm_resp_valid_hub;
    wire               hbm_resp_ready_hub;
    
    // Hub <-> Copy Engine
    wire [63:0]        ce_l2_addr_hub;
    wire [511:0]       ce_l2_wdata_hub;
    wire               ce_l2_wr_en_hub;
    wire               ce_l2_valid_hub;
    wire               ce_l2_ready_hub;
    wire [511:0]       ce_l2_rdata_hub;
    wire               ce_l2_rdata_valid_hub;
    
    hub_block #(
        .NUM_GPC(NUM_GPC),
        .NUM_CHANNELS(HBM_CHANNELS)
    ) u_hub (
        .clk(clk_sys),
        .rst_n(rst_sys_n),
        .cfg_addr(pcie_cfg_addr),
        .cfg_wdata(pcie_cfg_wdata),
        .cfg_wr(pcie_cfg_wr),
        .cfg_rd(pcie_cfg_rd),
        .cfg_rdata(pcie_cfg_rdata),
        .cfg_ready(pcie_cfg_ready),
        .doorbell_addr(doorbell_addr_sys),
        .doorbell_data(doorbell_data_sys),
        .doorbell_valid(doorbell_valid_sys),
        .doorbell_ready(1'b1), // Always ready in hub
        .gpc_grid_base_addr(gpc_grid_base_addr_hub),
        .gpc_grid_dim_x(gpc_grid_dim_x_hub),
        .gpc_grid_dim_y(gpc_grid_dim_y_hub),
        .gpc_grid_dim_z(gpc_grid_dim_z_hub),
        .gpc_kernel_args(gpc_kernel_args_hub),
        .gpc_dispatch_id(gpc_dispatch_id_hub),
        .gpc_dispatch_valid(gpc_dispatch_valid_hub),
        .gpc_dispatch_ready(gpc_dispatch_ready_hub),
        .gpc_done_valid(gpc_done_valid_hub),
        .gpc_done_ready(gpc_done_ready_hub),
        .gpc_req_addr(/* aggregated */),
        .gpc_req_data(),
        .gpc_req_wr_en(),
        .gpc_req_be(),
        .gpc_req_valid(),
        .gpc_req_ready(1'b1),
        .gpc_resp_data(),
        .gpc_resp_valid(),
        .gpc_resp_ready(1'b1),
        .hbm_req_addr(hbm_req_addr_hub),
        .hbm_req_data(hbm_req_data_hub),
        .hbm_req_wr_en(hbm_req_wr_en_hub),
        .hbm_req_valid(hbm_req_valid_hub),
        .hbm_req_ready(hbm_req_ready_hub),
        .hbm_resp_data(hbm_resp_data_hub),
        .hbm_resp_valid(hbm_resp_valid_hub),
        .hbm_resp_ready(hbm_resp_ready_hub),
        .ce_l2_addr(ce_l2_addr_hub),
        .ce_l2_wdata(ce_l2_wdata_hub),
        .ce_l2_wr_en(ce_l2_wr_en_hub),
        .ce_l2_valid(ce_l2_valid_hub),
        .ce_l2_ready(ce_l2_ready_hub),
        .ce_l2_rdata(ce_l2_rdata_hub),
        .ce_l2_rdata_valid(ce_l2_rdata_valid_hub),
        .hub_irq(pcie_irq_req),
        .irq_vector(),
        .hub_status(status_gpu_busy),
        .perf_l2_hits(),
        .perf_l2_misses()
    );
    
    // GPC Cluster Instantiations
    genvar gpc_i;
    generate
        for (gpc_i = 0; gpc_i < NUM_GPC; gpc_i = gpc_i + 1) begin : gpc_inst
            wire [63:0] gpc_entry_pc;
            wire [15:0] gpc_block_x, gpc_block_y, gpc_block_z;
            wire [31:0] gpc_kernel_args;
            wire        gpc_dispatch_valid;
            wire        gpc_dispatch_ready;
            wire        gpc_done_valid;
            
            // Connect to Hub (demux based on gpc_dispatch_id)
            assign gpc_entry_pc = (gpc_dispatch_id_hub == gpc_i) ? gpc_grid_base_addr_hub : 64'd0;
            assign gpc_block_x  = (gpc_dispatch_id_hub == gpc_i) ? gpc_grid_dim_x_hub : 16'd0;
            assign gpc_block_y  = (gpc_dispatch_id_hub == gpc_i) ? gpc_grid_dim_y_hub : 16'd0;
            assign gpc_block_z  = (gpc_dispatch_id_hub == gpc_i) ? gpc_grid_dim_z_hub : 16'd0;
            assign gpc_kernel_args = (gpc_dispatch_id_hub == gpc_i) ? gpc_kernel_args_hub : 32'd0;
            assign gpc_dispatch_valid = (gpc_dispatch_id_hub == gpc_i) ? gpc_dispatch_valid_hub : 1'b0;
            assign gpc_dispatch_ready_hub[gpc_i] = gpc_dispatch_ready;
            assign gpc_done_valid_hub[gpc_i] = gpc_done_valid;
            
            gpc_cluster #(
                .NUM_SM(NUM_SM_PER_GPC),
                .NUM_WARPS(NUM_WARPS_PER_SM)
            ) u_gpc (
                .clk(clk_sys),
                .rst_n(rst_sys_n),
                .grid_entry_pc(gpc_entry_pc),
                .grid_dim_x(gpc_block_x),
                .grid_dim_y(gpc_block_y),
                .grid_dim_z(gpc_block_z),
                .kernel_args(gpc_kernel_args),
                .dispatch_valid(gpc_dispatch_valid),
                .dispatch_ready(gpc_dispatch_ready),
                .gpc_done_valid(gpc_done_valid),
                .gpc_done_ready(gpc_done_ready_hub[gpc_i]),
                .l2_req_addr(gpc_l2_addr[gpc_i]),
                .l2_req_data(gpc_l2_wdata[gpc_i]),
                .l2_req_wr_en(gpc_l2_wr_en[gpc_i]),
                .l2_req_be(gpc_l2_be[gpc_i]),
                .l2_req_valid(gpc_l2_valid[gpc_i]),
                .l2_req_ready(gpc_l2_ready[gpc_i]),
                .l2_resp_data(gpc_l2_rdata[gpc_i]),
                .l2_resp_valid(gpc_l2_rvalid[gpc_i]),
                .l2_resp_ready(1'b1),
                .sm_idle_status(),
                .blocks_remaining()
            );
        end
    endgenerate
    
    // Copy Engine
    copy_engine #(
        .ADDR_W(64),
        .DATA_W(512)
    ) u_copy_engine (
        .clk(clk_sys),
        .rst_n(rst_sys_n),
        .cfg_addr(pcie_cfg_addr),
        .cfg_wdata(pcie_cfg_wdata),
        .cfg_wr(pcie_cfg_wr),
        .cfg_rd(pcie_cfg_rd),
        .cfg_rdata(),
        .cfg_ready(),
        .pcie_rd_addr(),
        .pcie_rd_len(),
        .pcie_rd_valid(),
        .pcie_rd_ready(1'b1),
        .pcie_rd_data(512'd0),
        .pcie_rd_data_valid(1'b0),
        .pcie_rd_data_ready(),
        .pcie_wr_addr(),
        .pcie_wr_data(),
        .pcie_wr_len(),
        .pcie_wr_valid(),
        .pcie_wr_ready(1'b1),
        .l2_addr(ce_l2_addr_hub),
        .l2_wdata(ce_l2_wdata_hub),
        .l2_wr_en(ce_l2_wr_en_hub),
        .l2_valid(ce_l2_valid_hub),
        .l2_ready(ce_l2_ready_hub),
        .l2_rdata(ce_l2_rdata_hub),
        .l2_rdata_valid(ce_l2_rdata_valid_hub),
        .snoop_addr(),
        .snoop_valid(),
        .snoop_hit(1'b0),
        .sema_in(16'd0),
        .sema_out(),
        .sema_valid(),
        .ce_busy(),
        .bytes_copied()
    );
    
    // HBM Controller
    hbm2_controller #(
        .NUM_CHANNELS(HBM_CHANNELS),
        .MEM_SIZE_MB(HBM_SIZE_MB)
    ) u_hbm (
        .clk(clk_sys),
        .rst_n(rst_sys_n),
        .req_addr(hbm_req_addr_hub),
        .req_wdata(hbm_req_data_hub),
        .req_wr_en(hbm_req_wr_en_hub),
        .req_be({64{1'b1}}), // Full line
        .req_valid(hbm_req_valid_hub),
        .req_ready(hbm_req_ready_hub),
        .req_channel(2'd0), // Single channel for now
        .resp_rdata(hbm_resp_data_hub),
        .resp_valid(hbm_resp_valid_hub),
        .resp_ready(hbm_resp_ready_hub),
        .resp_channel(),
        .ecc_error(),
        .ecc_error_count(),
        .refresh_pending()
    );
    
    // =========================================================================
    // Debug Interface (Simple Register Access)
    // =========================================================================
    reg [31:0] debug_regs [0:3];
    
    always @(posedge clk_sys or negedge rst_sys_n) begin
        if (!rst_sys_n) begin
            debug_regs[0] <= 32'hDEAD_BEEF; // Magic number
            debug_regs[1] <= 32'd0;
            debug_regs[2] <= 32'd0;
            debug_regs[3] <= 32'd0;
        end else if (dbg_en && dbg_wr) begin
            debug_regs[dbg_addr[1:0]] <= dbg_wdata;
        end
    end
    
    assign dbg_data = dbg_en ? debug_regs[dbg_addr[1:0]] : 32'd0;
    
    // =========================================================================
    // Power Management (Placeholder)
    // =========================================================================
    assign pwr_sleep_ack = pwr_sleep_req;
    
    // =========================================================================
    // Status Aggregation
    // =========================================================================
    assign status_interrupts = {24'd0, gpc_done_valid_hub};

endmodule