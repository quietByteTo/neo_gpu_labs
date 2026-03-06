// ============================================================================
// Module: hub_block
// Description: Hub Top Level - Integrates Scheduler, L2, Crossbar, and Config
// Central nervous system of the GPU
// ============================================================================
module hub_block #(
    parameter NUM_GPC       = 4,
    parameter GPC_ID_W      = $clog2(NUM_GPC),
    parameter NUM_CHANNELS  = 4,        // HBM channels
    parameter ADDR_W        = 64,
    parameter DATA_W        = 128       // GPC data width
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // PCIe Configuration Interface (from pciex5_bridge)
    input  wire [31:0]               cfg_addr,
    input  wire [63:0]               cfg_wdata,
    input  wire                      cfg_wr,
    input  wire                      cfg_rd,
    output wire [63:0]               cfg_rdata,
    output wire                      cfg_ready,
    
    // Doorbell from PCIe (Grid Launch)
    input  wire [63:0]               doorbell_addr,
    input  wire [31:0]               doorbell_data,
    input  wire                      doorbell_valid,
    output wire                      doorbell_ready,
    
    // GPC Interfaces (Array)
    output wire [63:0]               gpc_grid_base_addr,
    output wire [15:0]               gpc_grid_dim_x,
    output wire [15:0]               gpc_grid_dim_y,
    output wire [15:0]               gpc_grid_dim_z,
    output wire [31:0]               gpc_kernel_args,
    output wire [GPC_ID_W-1:0]       gpc_dispatch_id,
    output wire                      gpc_dispatch_valid,
    input  wire [NUM_GPC-1:0]        gpc_dispatch_ready,
    input  wire [NUM_GPC-1:0]        gpc_done_valid,
    output wire [NUM_GPC-1:0]        gpc_done_ready,
    
    // GPC Crossbar Interface (Bidirectional)
    // Requests from GPCs to L2
    input  wire [ADDR_W-1:0]         gpc_req_addr,
    input  wire [DATA_W-1:0]         gpc_req_data,
    input  wire                      gpc_req_wr_en,
    input  wire [15:0]               gpc_req_be,
    input  wire                      gpc_req_valid,
    output wire                      gpc_req_ready,
    // Responses to GPCs
    output wire [DATA_W-1:0]         gpc_resp_data,
    output wire                      gpc_resp_valid,
    input  wire                      gpc_resp_ready,
    
    // HBM Controller Interface
    output wire [ADDR_W-1:0]         hbm_req_addr,
    output wire [511:0]              hbm_req_data,      // 512-bit HBM width
    output wire                      hbm_req_wr_en,
    output wire                      hbm_req_valid,
    input  wire                      hbm_req_ready,
    input  wire [511:0]              hbm_resp_data,
    input  wire                      hbm_resp_valid,
    output wire                      hbm_resp_ready,
    
    // Copy Engine Interface
    output wire [ADDR_W-1:0]         ce_l2_addr,
    output wire [511:0]              ce_l2_wdata,
    output wire                      ce_l2_wr_en,
    output wire                      ce_l2_valid,
    input  wire                      ce_l2_ready,
    input  wire [511:0]              ce_l2_rdata,
    input  wire                      ce_l2_rdata_valid,
    
    // Interrupt Aggregation (to PCIe)
    output reg                       hub_irq,
    output reg  [7:0]                irq_vector,
    
    // Status
    output wire [31:0]               hub_status,
    output wire [31:0]               perf_l2_hits,
    output wire [31:0]               perf_l2_misses
);

    // Internal Integration Signals
    
    // GigaThread Scheduler Instance
    wire [63:0]               sched_entry_pc;
    wire [15:0]               sched_grid_x, sched_grid_y, sched_grid_z;
    wire [31:0]               sched_kernel_args;
    wire                      sched_launch_valid;
    wire                      sched_launch_ready;
    wire [GPC_ID_W-1:0]       sched_gpc_id;
    wire                      sched_dispatch_valid;
    wire [NUM_GPC-1:0]        sched_dispatch_ready;
    wire [NUM_GPC-1:0]        sched_done_valid;
    wire                      sched_completion_irq;
    
    // L2 Cache Instance
    wire [ADDR_W-1:0]         l2_req_addr;
    wire [127:0]              l2_req_data;      // 128-bit from crossbar
    wire                      l2_req_wr_en;
    wire [15:0]               l2_req_be;
    wire                      l2_req_valid;
    wire                      l2_req_ready;
    wire [127:0]              l2_resp_data;
    wire                      l2_resp_valid;
    wire                      l2_resp_ready;
    
    // Crossbar Instance
    wire [ADDR_W-1:0]         xbar_out_addr [0:3];
    wire [DATA_W-1:0]         xbar_out_data [0:3];
    wire                      xbar_out_wr_en[0:3];
    wire [15:0]               xbar_out_be   [0:3];
    wire                      xbar_out_valid[0:3];
    wire                      xbar_out_ready[0:3];
    
    // Configuration Registers
    reg [31:0]                hub_ctrl_reg;     // Control bits
    reg [31:0]                hub_int_mask;     // Interrupt mask
    reg [63:0]                total_cycles;     // Performance counter
    
    // Config Read MUX
    reg [63:0]                cfg_rdata_int;
    assign cfg_rdata = cfg_rdata_int;
    assign cfg_ready = 1'b1;  // Always ready
    
    always @(*) begin
        case (cfg_addr[7:0])
            8'h00: cfg_rdata_int = {32'd0, hub_ctrl_reg};
            8'h04: cfg_rdata_int = {32'd0, hub_int_mask};
            8'h10: cfg_rdata_int = total_cycles;
            8'h20: cfg_rdata_int = {32'd0, perf_l2_hits};
            8'h24: cfg_rdata_int = {32'd0, perf_l2_misses};
            default: cfg_rdata_int = 64'd0;
        endcase
    end
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            hub_ctrl_reg <= 32'd0;
            hub_int_mask <= 32'hFFFFFFFF;  // All masked by default
            total_cycles <= 64'd0;
        end else begin
            total_cycles <= total_cycles + 1'b1;
            
            if (cfg_wr) begin
                case (cfg_addr[7:0])
                    8'h00: hub_ctrl_reg <= cfg_wdata[31:0];
                    8'h04: hub_int_mask <= cfg_wdata[31:0];
                endcase
            end
        end
    end
    
    // Instantiate GigaThread Scheduler
    giga_thread_scheduler #(
        .NUM_GPC(NUM_GPC)
    ) u_scheduler (
        .clk(clk),
        .rst_n(rst_n),
        .host_grid_base_addr(doorbell_valid ? doorbell_addr : sched_entry_pc),
        .host_grid_dim_x(16'd8),      // From doorbell decode or config
        .host_grid_dim_y(16'd8),
        .host_grid_dim_z(16'd1),
        .host_kernel_args(doorbell_data),
        .host_launch_valid(doorbell_valid),
        .host_launch_ready(doorbell_ready),
        .gpc_entry_pc(gpc_grid_base_addr),
        .gpc_block_x(),
        .gpc_block_y(),
        .gpc_block_z(),
        .gpc_kernel_args(gpc_kernel_args),
        .gpc_target_id(gpc_dispatch_id),
        .gpc_dispatch_valid(gpc_dispatch_valid),
        .gpc_dispatch_ready(gpc_dispatch_ready),
        .gpc_done_valid(gpc_done_valid),
        .gpc_done_ready(gpc_done_ready),
        .completion_irq(sched_completion_irq),
        .completion_status(),
        .scheduler_state(),
        .ctas_remaining(),
        .ctas_dispatched(),
        .ctas_completed()
    );
    
    // Instantiate Crossbar (4x4: GPCs to L2 Banks)
    // Simplified: Direct mapping for single L2 instance
    crossbar_4x4 #(
        .NUM_PORTS(NUM_GPC)
    ) u_crossbar (
        .clk(clk),
        .rst_n(rst_n),
        .in_addr({NUM_GPC{gpc_req_addr}}),  // Simplified: broadcast
        .in_wdata({NUM_GPC{gpc_req_data}}),
        .in_wr_en({NUM_GPC{gpc_req_wr_en}}),
        .in_be({NUM_GPC{gpc_req_be}}),
        .in_valid({NUM_GPC{gpc_req_valid}}),
        .in_ready({gpc_req_ready}),  // Expand as needed
        .in_rdata(),
        .in_rvalid(),
        .in_rready({NUM_GPC{gpc_resp_ready}}),
        .out_addr({l2_req_addr, xbar_out_addr[2], xbar_out_addr[1], xbar_out_addr[0]}),
        .out_wdata({l2_req_data, xbar_out_data[2], xbar_out_data[1], xbar_out_data[0]}),
        .out_wr_en({l2_req_wr_en, xbar_out_wr_en[2], xbar_out_wr_en[1], xbar_out_wr_en[0]}),
        .out_be({l2_req_be, xbar_out_be[2], xbar_out_be[1], xbar_out_be[0]}),
        .out_valid({l2_req_valid, xbar_out_valid[2], xbar_out_valid[1], xbar_out_valid[0]}),
        .out_ready({l2_req_ready, xbar_out_ready[2], xbar_out_ready[1], xbar_out_ready[0]}),
        .out_rdata({l2_resp_data, 384'd0}),  // Other banks not connected in simplified
        .out_rvalid({l2_resp_valid, 3'b0}),
        .out_rready({l2_resp_ready, 3'b111})
    );
    
    // Instantiate L2 Cache
    l2_cache #(
        .NUM_PORTS(1),
        .CACHE_SIZE_KB(512)
    ) u_l2 (
        .clk(clk),
        .rst_n(rst_n),
        .gpc_req_addr(l2_req_addr),
        .gpc_req_data(l2_req_data),
        .gpc_req_wr_en(l2_req_wr_en),
        .gpc_req_be(l2_req_be),
        .gpc_req_valid(l2_req_valid),
        .gpc_req_ready(l2_req_ready),
        .gpc_resp_data(l2_resp_data),
        .gpc_resp_valid(l2_resp_valid),
        .gpc_resp_ready(l2_resp_ready),
        .hbm_req_addr(hbm_req_addr),
        .hbm_req_data(hbm_req_data),
        .hbm_req_wr_en(hbm_req_wr_en),
        .hbm_req_valid(hbm_req_valid),
        .hbm_req_ready(hbm_req_ready),
        .hbm_resp_data(hbm_resp_data),
        .hbm_resp_valid(hbm_resp_valid),
        .hbm_resp_ready(hbm_resp_ready),
        .ce_snoop_addr(ce_l2_addr),
        .ce_snoop_valid(ce_l2_valid),
        .ce_snoop_ready(ce_l2_ready),
        .ce_snoop_hit(),
        .flush_req(hub_ctrl_reg[0]),  // Bit 0 triggers flush
        .flush_done(),
        .invalidate_req(hub_ctrl_reg[1]),
        .invalidate_done()
    );
    
    // Interrupt Aggregation
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            hub_irq <= 1'b0;
            irq_vector <= 8'd0;
        end else begin
            // Combine all interrupt sources
            if (sched_completion_irq && !hub_int_mask[0]) begin
                hub_irq <= 1'b1;
                irq_vector <= 8'h01;
            end else if (|gpc_done_valid && !hub_int_mask[1]) begin
                hub_irq <= 1'b1;
                irq_vector <= 8'h02;
            end else begin
                hub_irq <= 1'b0;
            end
        end
    end
    
    assign hub_status = {hub_ctrl_reg[31:2], |gpc_done_valid, sched_completion_irq};

endmodule