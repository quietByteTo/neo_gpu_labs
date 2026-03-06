// ============================================================================
// Module: giga_thread_scheduler
// Description: Global Grid/CTA Scheduler, manages GPU workload distribution
// State Machine: IDLE -> DISTRIBUTE -> WAIT_COMPLETE -> INTERRUPT
// ============================================================================
module giga_thread_scheduler #(
    parameter NUM_GPC     = 4,
    parameter GPC_ID_W    = $clog2(NUM_GPC),
    parameter MAX_CTA_PER_GRID = 1024,  // Max CTAs in a grid
    parameter CTA_CNT_W   = $clog2(MAX_CTA_PER_GRID)
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Host Command Interface (from PCIe/Doorbell)
    input  wire [63:0]               host_grid_base_addr,   // Kernel code address
    input  wire [15:0]               host_grid_dim_x,       // Total blocks X
    input  wire [15:0]               host_grid_dim_y,       // Total blocks Y
    input  wire [15:0]               host_grid_dim_z,       // Total blocks Z
    input  wire [31:0]               host_kernel_args,      // Arguments pointer
    input  wire                      host_launch_valid,     // Doorbell trigger
    output wire                      host_launch_ready,     // Can accept new grid
    
    // To GPCs (Dispatch Interface)
    output reg  [63:0]               gpc_entry_pc,
    output reg  [15:0]               gpc_block_x,
    output reg  [15:0]               gpc_block_y,
    output reg  [15:0]               gpc_block_z,
    output reg  [31:0]               gpc_kernel_args,
    output reg  [GPC_ID_W-1:0]       gpc_target_id,         // Which GPC
    output reg                       gpc_dispatch_valid,
    input  wire [NUM_GPC-1:0]        gpc_dispatch_ready,    // Per-GPC ready
    
    // From GPCs (Completion Interface)
    input  wire [NUM_GPC-1:0]        gpc_done_valid,        // Per-GPC done pulse
    output wire [NUM_GPC-1:0]        gpc_done_ready,
    
    // To Host (Interrupt)
    output reg                       completion_irq,        // Grid complete interrupt
    output reg  [31:0]               completion_status,     // Status code
    
    // Status/Debug
    output wire [2:0]                scheduler_state,
    output wire [CTA_CNT_W-1:0]      ctas_remaining,
    output wire [CTA_CNT_W-1:0]      ctas_dispatched,
    output wire [CTA_CNT_W-1:0]      ctas_completed
);

    // State Machine
    localparam IDLE          = 3'b000;
    localparam SETUP         = 3'b001;
    localparam DISTRIBUTE    = 3'b010;
    localparam WAIT_COMPLETE = 3'b011;
    localparam WRITE_DOORBELL= 3'b100;  // Write completion to host memory
    localparam INTERRUPT     = 3'b101;
    localparam CLEANUP       = 3'b110;
    
    reg [2:0] state;
    reg [2:0] next_state;
    
    // Grid Configuration Registers (captured at launch)
    reg [63:0] grid_base_addr;
    reg [15:0] grid_dim_x, grid_dim_y, grid_dim_z;
    reg [31:0] kernel_args;
    reg [CTA_CNT_W-1:0] total_ctas;
    
    // CTA Tracking
    reg [15:0] cta_counter_x;
    reg [15:0] cta_counter_y;
    reg [15:0] cta_counter_z;
    reg [CTA_CNT_W-1:0] dispatched_count;
    reg [CTA_CNT_W-1:0] completed_count;
    reg [GPC_ID_W-1:0]  next_gpc_id;      // Round-robin selector
    
    // Outstanding CTA tracking per GPC (for flow control)
    reg [5:0] gpc_cta_count [0:NUM_GPC-1];  // How many CTAs each GPC has
    
    // Completion Doorbell Address (fixed offset from grid base or configurable)
    wire [63:0] doorbell_addr = grid_base_addr - 64'h100;  // Just below kernel code
    
    // State Machine Logic
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
        end else begin
            state <= next_state;
        end
    end
    
    always @(*) begin
        next_state = state;
        
        case (state)
            IDLE: begin
                if (host_launch_valid) begin
                    next_state = SETUP;
                end
            end
            
            SETUP: begin
                next_state = DISTRIBUTE;
            end
            
            DISTRIBUTE: begin
                // Check if all CTAs dispatched
                if (dispatched_count >= total_ctas) begin
                    next_state = WAIT_COMPLETE;
                end
            end
            
            WAIT_COMPLETE: begin
                if (completed_count >= total_ctas) begin
                    next_state = WRITE_DOORBELL;
                end
            end
            
            WRITE_DOORBELL: begin
                // Assume always succeeds in one cycle (simplified)
                next_state = INTERRUPT;
            end
            
            INTERRUPT: begin
                next_state = CLEANUP;
            end
            
            CLEANUP: begin
                next_state = IDLE;
            end
            
            default: next_state = IDLE;
        endcase
    end
    
    // Launch Capture
    always @(posedge clk) begin
        if (state == IDLE && host_launch_valid) begin
            grid_base_addr <= host_grid_base_addr;
            grid_dim_x <= host_grid_dim_x;
            grid_dim_y <= host_grid_dim_y;
            grid_dim_z <= host_grid_dim_z;
            kernel_args <= host_kernel_args;
            total_ctas <= host_grid_dim_x * host_grid_dim_y * host_grid_dim_z;
        end
    end
    
    // CTA Dispatch Logic (Round-Robin to GPCs)
    wire all_ctas_dispatched = (dispatched_count >= total_ctas);
    wire current_gpc_ready = gpc_dispatch_ready[next_gpc_id];
    wire can_dispatch = !all_ctas_dispatched && current_gpc_ready && (state == DISTRIBUTE);
    
    integer gpc_i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cta_counter_x <= 16'd0;
            cta_counter_y <= 16'd0;
            cta_counter_z <= 16'd0;
            dispatched_count <= {CTA_CNT_W{1'b0}};
            completed_count <= {CTA_CNT_W{1'b0}};
            next_gpc_id <= {GPC_ID_W{1'b0}};
            completion_irq <= 1'b0;
            
            for (gpc_i = 0; gpc_i < NUM_GPC; gpc_i = gpc_i + 1) begin
                gpc_cta_count[gpc_i] <= 6'd0;
            end
        end else begin
            completion_irq <= 1'b0;  // Default pulse
            
            // Dispatch CTA
            if (can_dispatch) begin
                // Increment 3D counter
                if (cta_counter_x < grid_dim_x - 1) begin
                    cta_counter_x <= cta_counter_x + 1'b1;
                end else begin
                    cta_counter_x <= 16'd0;
                    if (cta_counter_y < grid_dim_y - 1) begin
                        cta_counter_y <= cta_counter_y + 1'b1;
                    end else begin
                        cta_counter_y <= 16'd0;
                        if (cta_counter_z < grid_dim_z - 1) begin
                            cta_counter_z <= cta_counter_z + 1'b1;
                        end
                    end
                end
                
                // Round-robin GPC selection
                next_gpc_id <= (next_gpc_id == NUM_GPC-1) ? {GPC_ID_W{1'b0}} : next_gpc_id + 1'b1;
                dispatched_count <= dispatched_count + 1'b1;
                gpc_cta_count[next_gpc_id] <= gpc_cta_count[next_gpc_id] + 1'b1;
            end
            
            // Track completions
            for (gpc_i = 0; gpc_i < NUM_GPC; gpc_i = gpc_i + 1) begin
                if (gpc_done_valid[gpc_i]) begin
                    completed_count <= completed_count + 1'b1;
                    gpc_cta_count[gpc_i] <= gpc_cta_count[gpc_i] - 1'b1;
                end
            end
            
            // Generate interrupt
            if (state == INTERRUPT) begin
                completion_irq <= 1'b1;
                completion_status <= 32'h0000_0001;  // Success code
            end
        end
    end
    
    // Output Assignments
    assign host_launch_ready = (state == IDLE);
    
    always @(*) begin
        if (can_dispatch) begin
            gpc_entry_pc = grid_base_addr;
            gpc_block_x = cta_counter_x;
            gpc_block_y = cta_counter_y;
            gpc_block_z = cta_counter_z;
            gpc_kernel_args = kernel_args;
            gpc_target_id = next_gpc_id;
            gpc_dispatch_valid = 1'b1;
        end else begin
            gpc_entry_pc = 64'd0;
            gpc_block_x = 16'd0;
            gpc_block_y = 16'd0;
            gpc_block_z = 16'd0;
            gpc_kernel_args = 32'd0;
            gpc_target_id = {GPC_ID_W{1'b0}};
            gpc_dispatch_valid = 1'b0;
        end
    end
    
    assign gpc_done_ready = {NUM_GPC{1'b1}};  // Always ready to accept completions
    
    // Status Outputs
    assign scheduler_state = state;
    assign ctas_remaining = total_ctas - dispatched_count;
    assign ctas_dispatched = dispatched_count;
    assign ctas_completed = completed_count;

endmodule