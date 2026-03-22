// ============================================================================
// Module: gpc_router
// Description: Routes SM requests to L2 Cache or P2P, manages outstanding transactions
// ============================================================================
module gpc_router #(
    parameter NUM_SM      = 4,
    parameter SM_ID_W     = $clog2(NUM_SM),
    parameter ADDR_W      = 64,
    parameter DATA_W      = 128,
    parameter MAX_OT      = 8,          // Max outstanding transactions per SM
    parameter OT_ID_W     = $clog2(MAX_OT * NUM_SM)  // Transaction ID width
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // From SM Array (Upstream)
    input  wire [ADDR_W-1:0]         sm_req_addr,
    input  wire [DATA_W-1:0]         sm_req_data,
    input  wire                      sm_req_wr_en,
    input  wire [15:0]               sm_req_be,
    input  wire                      sm_req_valid,
    output wire                      sm_req_ready,
    input  wire [SM_ID_W-1:0]        sm_req_sm_id,    // Source SM ID
    
    // To SM Array (Responses)
    output reg  [DATA_W-1:0]         sm_resp_data,
    output reg                       sm_resp_valid,
    input  wire                      sm_resp_ready,
    output reg  [SM_ID_W-1:0]        sm_resp_sm_id,
    
    // To L2 Cache (Downstream)
    output reg  [ADDR_W-1:0]         l2_req_addr,
    output reg  [DATA_W-1:0]         l2_req_data,
    output reg                       l2_req_wr_en,
    output reg  [15:0]               l2_req_be,
    output reg                       l2_req_valid,
    input  wire                      l2_req_ready,
    output reg  [OT_ID_W-1:0]        l2_req_id,       // Transaction ID for tracking
    
    // From L2 Cache (Responses)
    input  wire [DATA_W-1:0]         l2_resp_data,
    input  wire                      l2_resp_valid,
    output wire                      l2_resp_ready,
    input  wire [OT_ID_W-1:0]        l2_resp_id       // Response transaction ID
);

    // Address Routing Logic
    // Simplified: Assume all addresses go to L2 (no P2P in initial version)
    // Address map: 
    //   [63:48] == 16'h0000 -> L2 Cache (Local VRAM)
    //   [63:48] != 16'h0000 -> Reserved for P2P or System Memory
    
    wire is_l2_access = (sm_req_addr[63:48] == 16'h0000);
    wire is_p2p = !is_l2_access;  // Future expansion
    
    // Outstanding Transaction (OT) Management
    // Need to track: SM_ID for each outstanding request to route response back
    // OT Buffer: Stores SM_ID indexed by transaction ID
    
    reg [SM_ID_W-1:0] ot_sm_table [0:MAX_OT*NUM_SM-1];
    reg [OT_ID_W:0]   ot_count;      // Current outstanding count (extra bit for full detection)
    reg [OT_ID_W-1:0] ot_head;       // Next ID to allocate
    reg [OT_ID_W-1:0] ot_tail;       // Next ID to retire
    
    wire ot_full = (ot_count >= MAX_OT * NUM_SM);
    wire ot_empty = (ot_count == 0);
    
    // Request Forwarding
    assign sm_req_ready = l2_req_ready && !ot_full;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ot_count <= {(OT_ID_W+1){1'b0}};
            ot_head <= {OT_ID_W{1'b0}};
            ot_tail <= {OT_ID_W{1'b0}};
            l2_req_valid <= 1'b0;
        end else begin
            l2_req_valid <= 1'b0;  // Default
            
            // Allocate OT entry and forward request
            if (sm_req_valid && sm_req_ready) begin
                // Store SM ID for response routing
                ot_sm_table[ot_head] <= sm_req_sm_id;
                
                // Send to L2
                l2_req_addr <= sm_req_addr;
                l2_req_data <= sm_req_data;
                l2_req_wr_en <= sm_req_wr_en;
                l2_req_be <= sm_req_be;
                l2_req_id <= ot_head;
                l2_req_valid <= 1'b1;
                
                // Update OT pointer
                ot_head <= ot_head + 1'b1;
                ot_count <= ot_count + 1'b1;
            end
            
            // Retire OT entry on response
            if (l2_resp_valid && l2_resp_ready) begin
                ot_tail <= ot_tail + 1'b1;
                ot_count <= ot_count - 1'b1;
            end
        end
    end
    
    // Response Routing
    // Look up SM_ID from OT table using response ID
    always @(*) begin
        if (l2_resp_valid) begin
            sm_resp_data = l2_resp_data;
            sm_resp_valid = 1'b1;
            sm_resp_sm_id = ot_sm_table[l2_resp_id];
        end else begin
            sm_resp_data = {DATA_W{1'b0}};
            sm_resp_valid = 1'b0;
            sm_resp_sm_id = {SM_ID_W{1'b0}};
        end
    end
    
    assign l2_resp_ready = sm_resp_ready;
    
    // P2P Routing (Future expansion)
    // If is_p2p, route to P2P interface instead of L2
    // For now, assert error or stall

endmodule

