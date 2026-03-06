// ============================================================================
// Module: lsu
// Description: Load/Store Unit with Coalescing Logic
// Generates L1/LDS requests, handles address translation (simplified)
// ============================================================================
module lsu #(
    parameter NUM_LANES   = 32,
    parameter ADDR_W      = 64,
    parameter DATA_W      = 32,      // Per-lane data width
    parameter WARP_ID_W   = 5
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // From Execution Unit (Frontend)
    input  wire [WARP_ID_W-1:0]      warp_id,
    input  wire [ADDR_W-1:0]         base_addr,      // From SGPR (uniform)
    input  wire [NUM_LANES-1:0]      offset_valid,   // Which lanes active (from Exec Mask)
    input  wire [NUM_LANES*32-1:0]   offset,         // Per-lane offset (from VGPR)
    input  wire [NUM_LANES*DATA_W-1:0] store_data,
    input  wire                      is_load,
    input  wire                      is_store,
    input  wire                      is_lds,         // 1=LDS, 0=Global (L1)
    input  wire                      valid,
    output wire                      ready,
    
    // To L1 Cache (Global Memory)
    output reg  [ADDR_W-1:0]         l1_addr,
    output reg  [127:0]              l1_wdata,       // 128bit aligned
    output reg                       l1_wr_en,
    output reg  [15:0]               l1_be,          // Byte enable
    output reg                       l1_valid,
    input  wire                      l1_ready,
    input  wire [127:0]              l1_rdata,
    input  wire                      l1_rdata_valid,
    
    // To LDS Unit
    output reg  [15:0]               lds_addr   [0:NUM_LANES-1],
    output reg  [DATA_W-1:0]         lds_wdata  [0:NUM_LANES-1],
    output reg                       lds_wr_en  [0:NUM_LANES-1],
    output reg                       lds_valid  [0:NUM_LANES-1],
    input  wire [NUM_LANES-1:0]      lds_ready,
    input  wire [DATA_W-1:0]         lds_rdata  [0:NUM_LANES-1],
    input  wire                      lds_rdata_valid,
    
    // Response to Backend
    output reg  [NUM_LANES*DATA_W-1:0] load_result,
    output reg                       load_valid,
    output reg  [WARP_ID_W-1:0]      resp_warp_id
);

    // Address Calculation (Combinational)
    wire [ADDR_W-1:0] lane_addr [0:NUM_LANES-1];
    wire [ADDR_W-1:0] effective_addr [0:NUM_LANES-1];
    
    genvar i;
    generate
        for (i = 0; i < NUM_LANES; i = i + 1) begin : addr_calc
            assign lane_addr[i] = base_addr + offset[i*32 +: 32];
            // For LDS, use lower 16 bits only
            assign effective_addr[i] = is_lds ? {48'd0, lane_addr[i][15:0]} : lane_addr[i];
        end
    endgenerate
    
    // Coalescing Detection (simplified)
    // Check if all active lanes access same 128B cache line (aligned)
    wire [ADDR_W-7:0] line_addr [0:NUM_LANES-1];  // 128B aligned address (drop lower 7 bits)
    reg  all_same_line;
    reg [ADDR_W-1:0] coalesced_addr;
    
    integer l;
    always @(*) begin
        all_same_line = 1'b1;
        coalesced_addr = effective_addr[0];
        for (l = 1; l < NUM_LANES; l = l + 1) begin
            if (offset_valid[l] && (effective_addr[l][ADDR_W-1:7] != effective_addr[0][ADDR_W-1:7])) begin
                all_same_line = 1'b0;
            end
        end
        // Use first active lane's address as representative
        coalesced_addr = effective_addr[0];
    end
    
    // State Machine
    localparam IDLE    = 2'b00;
    localparam L1_REQ  = 2'b01;  // Request to L1
    localparam LDS_REQ = 2'b10;  // Request to LDS
    localparam WAIT    = 2'b11;  // Wait for response
    
    reg [1:0] state;
    reg [NUM_LANES-1:0] active_mask;  // Save which lanes were active
    
    assign ready = (state == IDLE);
    
    integer lane_idx;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            l1_valid <= 1'b0;
            lds_valid <= {NUM_LANES{1'b0}};
            load_valid <= 1'b0;
        end else begin
            l1_valid <= 1'b0;
            lds_valid <= {NUM_LANES{1'b0}};
            load_valid <= 1'b0;
            
            case (state)
                IDLE: begin
                    if (valid) begin
                        active_mask <= offset_valid;
                        resp_warp_id <= warp_id;
                        
                        if (is_lds) begin
                            state <= LDS_REQ;
                        end else begin
                            state <= L1_REQ;
                        end
                    end
                end
                
                L1_REQ: begin
                    // Coalesced request to L1
                    // Simplified: assume all accesses coalesced to one 128B request
                    // In reality: multiple requests for scattered accesses
                    l1_addr <= coalesced_addr;
                    l1_wr_en <= is_store;
                    l1_valid <= 1'b1;
                    
                    // Pack store data (simplified: assume aligned and contiguous)
                    l1_wdata <= store_data[127:0];  // First 4 lanes
                    l1_be <= is_store ? 16'hFFFF : 16'd0;  // Full line for now
                    
                    if (l1_ready) begin
                        if (is_load) begin
                            state <= WAIT;
                        end else begin
                            state <= IDLE;  // Store done
                        end
                    end
                end
                
                LDS_REQ: begin
                    // Scatter/Gather to LDS (each lane independent)
                    for (lane_idx = 0; lane_idx < NUM_LANES; lane_idx = lane_idx + 1) begin
                        if (offset_valid[lane_idx]) begin
                            lds_addr[lane_idx] <= effective_addr[lane_idx][15:0];
                            lds_wdata[lane_idx] <= store_data[lane_idx*DATA_W +: DATA_W];
                            lds_wr_en[lane_idx] <= is_store;
                            lds_valid[lane_idx] <= 1'b1;
                        end
                    end
                    
                    if (&(lds_ready | ~offset_valid)) begin
                        // All active lanes accepted (or inactive)
                        if (is_load) begin
                            state <= WAIT;
                        end else begin
                            state <= IDLE;
                        end
                    end
                    // If not all ready, LDS unit will stall conflicting lanes
                end
                
                WAIT: begin
                    if (is_lds) begin
                        if (lds_rdata_valid) begin
                            for (lane_idx = 0; lane_idx < NUM_LANES; lane_idx = lane_idx + 1) begin
                                load_result[lane_idx*DATA_W +: DATA_W] <= lds_rdata[lane_idx];
                            end
                            load_valid <= 1'b1;
                            state <= IDLE;
                        end
                    end else begin
                        if (l1_rdata_valid) begin
                            // Broadcast L1 data to all lanes (simplified coalescing)
                            for (lane_idx = 0; lane_idx < NUM_LANES; lane_idx = lane_idx + 1) begin
                                load_result[lane_idx*DATA_W +: DATA_W] <= l1_rdata[31:0];  // Simplified
                            end
                            load_valid <= 1'b1;
                            state <= IDLE;
                        end
                    end
                end
                
                default: state <= IDLE;
            endcase
        end
    end

endmodule