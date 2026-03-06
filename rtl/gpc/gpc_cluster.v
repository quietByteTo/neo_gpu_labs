// ============================================================================
// Module: gpc_cluster
// Description: GPC Top Level, integrates SM Array and Router, handles CTA dispatch and barriers
// ============================================================================
module gpc_cluster #(
    parameter NUM_SM      = 4,
    parameter SM_ID_W     = $clog2(NUM_SM),
    parameter NUM_WARPS   = 32,
    parameter WARP_ID_W   = 5,
    parameter DATA_W      = 128,
    parameter ADDR_W      = 64
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // From Hub Block (Grid Level Dispatch)
    input  wire [63:0]               grid_entry_pc,
    input  wire [15:0]               grid_dim_x,      // Total blocks in grid
    input  wire [15:0]               grid_dim_y,
    input  wire [15:0]               grid_dim_z,
    input  wire [31:0]               kernel_args,     // Kernel argument pointer
    input  wire                      dispatch_valid,
    output wire                      dispatch_ready,
    
    // Completion to Hub
    output wire                      gpc_done_valid,
    input  wire                      gpc_done_ready,
    
    // To Hub L2 Interface
    output wire [ADDR_W-1:0]         l2_req_addr,
    output wire [DATA_W-1:0]         l2_req_data,
    output wire                      l2_req_wr_en,
    output wire [15:0]               l2_req_be,
    output wire                      l2_req_valid,
    input  wire                      l2_req_ready,
    input  wire [DATA_W-1:0]         l2_resp_data,
    input  wire                      l2_resp_valid,
    output wire                      l2_resp_ready,
    
    // Status
    output wire [NUM_SM-1:0]         sm_idle_status,
    output wire [15:0]               blocks_remaining  // How many blocks not yet dispatched
);

    // Internal Signals
    wire [63:0]               sm_array_entry_pc;
    wire [WARP_ID_W-1:0]      sm_array_warp_id;
    wire [4:0]                sm_array_num_warps;
    wire [SM_ID_W-1:0]        sm_array_target_id;
    wire                      sm_array_task_valid;
    wire                      sm_array_task_ready;
    
    wire [ADDR_W-1:0]         router_l1_addr;
    wire [DATA_W-1:0]         router_l1_data;
    wire                      router_l1_wr_en;
    wire [15:0]               router_l1_be;
    wire                      router_l1_valid;
    wire                      router_l1_ready;
    wire [DATA_W-1:0]         router_l1_rdata;
    wire                      router_l1_rvalid;
    wire [SM_ID_W-1:0]        router_l1_rsmid;
    
    // CTA (Block) Dispatch Logic
    // State Machine: Dispatch blocks to SMs in Round-Robin fashion
    
    localparam IDLE      = 2'b00;
    localparam DISPATCH  = 2'b01;
    localparam WAIT_DONE = 2'b10;
    localparam SIGNAL_DONE = 2'b11;
    
    reg [1:0] state;
    reg [15:0] block_counter_x;
    reg [15:0] block_counter_y;
    reg [15:0] block_counter_z;
    reg [SM_ID_W-1:0] next_sm_id;      // Round-robin SM selector
    reg [15:0] active_blocks;          // Count of blocks currently running
    
    wire [15:0] total_blocks = grid_dim_x * grid_dim_y * grid_dim_z;
    wire all_blocks_dispatched = (block_counter_x == grid_dim_x) && 
                                 (block_counter_y == grid_dim_y) && 
                                 (block_counter_z == grid_dim_z);
    wire all_blocks_done = (active_blocks == 0) && all_blocks_dispatched;
    
    // Block ID to Warp ID mapping (simplified: 1 block = 1 warp for now)
    // Full implementation: 1 block = multiple warps distributed across SMs
    
    assign dispatch_ready = (state == IDLE);
    assign blocks_remaining = total_blocks - (block_counter_z * grid_dim_y * grid_dim_x + 
                                               block_counter_y * grid_dim_x + 
                                               block_counter_x);
    
    // Task generation
    assign sm_array_entry_pc = grid_entry_pc;
    assign sm_array_warp_id = {next_sm_id, 3'b000};  // Warp base ID per SM
    assign sm_array_num_warps = 5'd1;  // 1 block per dispatch (simplified)
    assign sm_array_target_id = next_sm_id;
    assign sm_array_task_valid = (state == DISPATCH) && !all_blocks_dispatched;
    
    // CTA Dispatch State Machine
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            block_counter_x <= 16'd0;
            block_counter_y <= 16'd0;
            block_counter_z <= 16'd0;
            next_sm_id <= {SM_ID_W{1'b0}};
            active_blocks <= 16'd0;
        end else begin
            case (state)
                IDLE: begin
                    if (dispatch_valid) begin
                        state <= DISPATCH;
                        block_counter_x <= 16'd0;
                        block_counter_y <= 16'd0;
                        block_counter_z <= 16'd0;
                        active_blocks <= 16'd0;
                    end
                end
                
                DISPATCH: begin
                    if (sm_array_task_valid && sm_array_task_ready) begin
                        // Advance to next block
                        if (block_counter_x < grid_dim_x - 1) begin
                            block_counter_x <= block_counter_x + 1'b1;
                        end else begin
                            block_counter_x <= 16'd0;
                            if (block_counter_y < grid_dim_y - 1) begin
                                block_counter_y <= block_counter_y + 1'b1;
                            end else begin
                                block_counter_y <= 16'd0;
                                if (block_counter_z < grid_dim_z - 1) begin
                                    block_counter_z <= block_counter_z + 1'b1;
                                end
                            end
                        end
                        
                        // Round-robin SM selection
                        next_sm_id <= (next_sm_id == NUM_SM-1) ? {SM_ID_W{1'b0}} : next_sm_id + 1'b1;
                        active_blocks <= active_blocks + 1'b1;
                        
                        if (all_blocks_dispatched) begin
                            state <= WAIT_DONE;
                        end
                    end
                end
                
                WAIT_DONE: begin
                    // Monitor SM completion (decrement active_blocks when SM reports done)
                    // Simplified: use sm_idle_status to detect completion
                    // In reality, need explicit block completion signals
                    
                    if (all_blocks_done) begin
                        state <= SIGNAL_DONE;
                    end
                end
                
                SIGNAL_DONE: begin
                    if (gpc_done_ready) begin
                        state <= IDLE;
                    end
                end
                
                default: state <= IDLE;
            endcase
        end
    end
    
    assign gpc_done_valid = (state == SIGNAL_DONE);
    
    // Instantiate SM Array
    sm_array #(
        .NUM_SM(NUM_SM),
        .NUM_WARPS(NUM_WARPS)
    ) u_sm_array (
        .clk(clk),
        .rst_n(rst_n),
        .task_entry_pc(sm_array_entry_pc),
        .task_warp_id(sm_array_warp_id),
        .task_num_warps(sm_array_num_warps),
        .task_sm_id(sm_array_target_id),
        .task_valid(sm_array_task_valid),
        .task_ready(sm_array_task_ready),
        .l1_req_addr(router_l1_addr),
        .l1_req_data(router_l1_data),
        .l1_req_wr_en(router_l1_wr_en),
        .l1_req_be(router_l1_be),
        .l1_req_valid(router_l1_valid),
        .l1_req_ready(router_l1_ready),
        .l1_resp_data(router_l1_rdata),
        .l1_resp_valid(router_l1_rvalid),
        .l1_resp_ready(l2_resp_ready),  // Direct connect for now
        .l1_resp_sm_id(router_l1_rsmid),
        .sm_idle(sm_idle_status),
        .sm_done()  // Not used in simplified version
    );
    
    // Instantiate Router
    gpc_router #(
        .NUM_SM(NUM_SM),
        .MAX_OT(8)
    ) u_router (
        .clk(clk),
        .rst_n(rst_n),
        .sm_req_addr(router_l1_addr),
        .sm_req_data(router_l1_data),
        .sm_req_wr_en(router_l1_wr_en),
        .sm_req_be(router_l1_be),
        .sm_req_valid(router_l1_valid),
        .sm_req_ready(router_l1_ready),
        .sm_req_sm_id(router_l1_rsmid),  // SM ID from array (for OT tracking)
        .sm_resp_data(router_l1_rdata),
        .sm_resp_valid(router_l1_rvalid),
        .sm_resp_ready(l2_resp_ready),
        .sm_resp_sm_id(router_l1_rsmid),
        .l2_req_addr(l2_req_addr),
        .l2_req_data(l2_req_data),
        .l2_req_wr_en(l2_req_wr_en),
        .l2_req_be(l2_req_be),
        .l2_req_valid(l2_req_valid),
        .l2_req_ready(l2_req_ready),
        .l2_req_id(),  // Internal tracking
        .l2_resp_data(l2_resp_data),
        .l2_resp_valid(l2_resp_valid),
        .l2_resp_ready(l2_resp_ready),
        .l2_resp_id({OT_ID_W{1'b0}})  // Would come from L2
    );
    
    // CTA-level Barrier Implementation (Simplified)
    // In full GPU, barriers synchronize all warps within a block
    // Here we provide the wiring, actual barrier logic would be in SM or shared here
    
    // Performance/Debug
    reg [31:0] total_dispatches;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            total_dispatches <= 32'd0;
        end else if (sm_array_task_valid && sm_array_task_ready) begin
            total_dispatches <= total_dispatches + 1'b1;
        end
    end

endmodule