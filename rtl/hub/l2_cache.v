// ============================================================================
// Module: l2_cache
// Description: Large L2 Cache (8-Way Set Associative, Write-Back)
// Interfaces with multiple HBM channels and Copy Engine
// ============================================================================
module l2_cache #(
    parameter NUM_PORTS    = 4,         // Number of HBM channels (or GPC ports)
    parameter PORT_ID_W    = $clog2(NUM_PORTS),
    parameter CACHE_SIZE_KB= 512,       // 512KB L2
    parameter LINE_SIZE    = 128,       // 128B cache line
    parameter NUM_WAYS     = 8,         // 8-Way Set Associative
    parameter NUM_SETS     = (CACHE_SIZE_KB * 1024) / (LINE_SIZE * NUM_WAYS),
    parameter SET_IDX_W    = $clog2(NUMSETS),
    parameter TAG_W        = 40,        // Assuming 48-bit physical address, upper bits
    parameter HBM_CHANNELS = 4,
    parameter HBM_ID_W     = $clog2(HBM_CHANNELS)
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // From Crossbar (GPC Requests)
    input  wire [63:0]               gpc_req_addr,
    input  wire [127:0]              gpc_req_data,      // 128bit interface
    input  wire                      gpc_req_wr_en,
    input  wire [15:0]               gpc_req_be,
    input  wire                      gpc_req_valid,
    output wire                      gpc_req_ready,
    output wire [127:0]              gpc_resp_data,
    output wire                      gpc_resp_valid,
    input  wire                      gpc_resp_ready,
    
    // To HBM Controller (Memory Interface)
    output reg  [63:0]               hbm_req_addr,
    output reg  [127:0]              hbm_req_data,
    output reg                       hbm_req_wr_en,
    output reg  [15:0]               hbm_req_be,
    output reg                       hbm_req_valid,
    input  wire                      hbm_req_ready,
    input  wire [HBM_ID_W-1:0]       hbm_req_id,        // Channel ID
    
    input  wire [127:0]              hbm_resp_data,
    input  wire                      hbm_resp_valid,
    output reg                       hbm_resp_ready,
    input  wire [HBM_ID_W-1:0]       hbm_resp_id,
    
    // Copy Engine Interface (Snooping/Coherence)
    input  wire [63:0]               ce_snoop_addr,
    input  wire                      ce_snoop_valid,
    output wire                      ce_snoop_ready,
    output wire                      ce_snoop_hit,      // Is address in L2?
    
    // Cache Maintenance
    input  wire                      flush_req,         // Flush all dirty lines
    output reg                       flush_done,
    input  wire                      invalidate_req,    // Invalidate without writeback
    output reg                       invalidate_done
);

    // Cache Structures (modeled as arrays for Verilator)
    // Tag Array: Valid + Dirty + Tag + LRU bits
    reg valid_bits [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg dirty_bits [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg [TAG_W-1:0] tag_array [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg [2:0] lru_counter [0:NUM_SETS-1][0:NUM_WAYS-1];  // 3 bits per way for 8-way
    
    // Data Array: 128B per line = 1024 bits
    // Split into 8 banks of 128 bits for byte-accessibility
    reg [127:0] data_array [0:NUM_SETS-1][0:NUM_WAYS-1][0:7];
    
    // Address Decomposition
    wire [TAG_W-1:0]     req_tag    = gpc_req_addr[63:64-TAG_W];
    wire [SET_IDX_W-1:0] req_set    = gpc_req_addr[SET_IDX_W+6:7];  // Assuming 128B line (7 bits offset)
    wire [6:0]           req_offset = gpc_req_addr[6:0];
    wire [2:0]           req_bank   = req_offset[6:4];  // 8 banks, 16 bytes each
    
    // Hit Detection Logic (Combinational)
    reg [NUM_WAYS-1:0] way_hit_vec;
    reg [2:0] hit_way;
    reg cache_hit;
    integer way_i;
    
    always @(*) begin
        way_hit_vec = {NUM_WAYS{1'b0}};
        hit_way = 3'd0;
        cache_hit = 1'b0;
        
        for (way_i = 0; way_i < NUM_WAYS; way_i = way_i + 1) begin
            if (valid_bits[req_set][way_i] && 
                tag_array[req_set][way_i] == req_tag) begin
                way_hit_vec[way_i] = 1'b1;
                hit_way = way_i[2:0];
                cache_hit = 1'b1;
            end
        end
    end
    
    // Victim Selection (LRU)
    reg [2:0] victim_way;
    reg victim_dirty;
    integer lru_i;
    
    always @(*) begin
        // Find LRU way (counter == 0)
        victim_way = 3'd0;
        for (lru_i = 0; lru_i < NUM_WAYS; lru_i = lru_i + 1) begin
            if (lru_counter[req_set][lru_i] == 3'd0) begin
                victim_way = lru_i[2:0];
            end
        end
        victim_dirty = dirty_bits[req_set][victim_way];
    end
    
    // State Machine
    localparam IDLE      = 3'b000;
    localparam LOOKUP    = 3'b001;
    localparam HIT_RW    = 3'b010;  // Read/Write hit
    localparam MISS_ALLOC= 3'b011;  // Allocate line (evict if dirty)
    localparam WRITEBACK = 3'b100;  // Write dirty line to HBM
    localparam REFILL    = 3'b101;  // Read from HBM
    localparam FLUSH     = 3'b110;  // Flush all dirty lines
    
    reg [2:0] state;
    reg [SET_IDX_W-1:0] flush_counter;
    reg [2:0] flush_way_counter;
    
    // Transaction Tracking
    reg [2:0]  pending_way;     // Way allocated for refill
    reg [127:0] refill_buffer [0:7];  // 8x128bit = 1024bit line buffer
    reg [2:0] refill_count;
    
    // Main State Machine
    assign gpc_req_ready = (state == IDLE) || (state == LOOKUP && cache_hit);
    // Initialize arrays
    integer s, w, b;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            flush_done <= 1'b0;
            invalidate_done <= 1'b0;
            gpc_resp_valid <= 1'b0;
            hbm_req_valid <= 1'b0;
            
            
            for (s = 0; s < NUM_SETS; s = s + 1) begin
                for (w = 0; w < NUM_WAYS; w = w + 1) begin
                    valid_bits[s][w] <= 1'b0;
                    dirty_bits[s][w] <= 1'b0;
                    tag_array[s][w] <= {TAG_W{1'b0}};
                    lru_counter[s][w] <= w[2:0];  // Initialize staggered
                    for (b = 0; b < 8; b = b + 1) begin
                        data_array[s][w][b] <= 128'd0;
                    end
                end
            end
        end else begin
            flush_done <= 1'b0;
            invalidate_done <= 1'b0;
            gpc_resp_valid <= 1'b0;
            hbm_req_valid <= 1'b0;
            
            case (state)
                IDLE: begin
                    if (flush_req) begin
                        state <= FLUSH;
                        flush_counter <= {SET_IDX_W{1'b0}};
                        flush_way_counter <= 3'd0;
                    end else if (invalidate_req) begin
                        // Quick invalidate all
                        for (integer s = 0; s < NUM_SETS; s = s + 1) begin
                            for (integer w = 0; w < NUM_WAYS; w = w + 1) begin
                                valid_bits[s][w] <= 1'b0;
                            end
                        end
                        invalidate_done <= 1'b1;
                    end else if (gpc_req_valid) begin
                        state <= LOOKUP;
                    end
                end
                
                LOOKUP: begin
                    if (cache_hit) begin
                        state <= HIT_RW;
                    end else begin
                        state <= MISS_ALLOC;
                    end
                end
                
                HIT_RW: begin
                    // Handle hit
                    if (gpc_req_wr_en) begin
                        // Write hit: update data, set dirty
                        dirty_bits[req_set][hit_way] <= 1'b1;
                        // Merge with byte enable
                        // Simplified: full 128bit write for now
                        data_array[req_set][hit_way][req_bank] <= gpc_req_data;
                    end else begin
                        // Read hit: return data
                        gpc_resp_data <= data_array[req_set][hit_way][req_bank];
                        gpc_resp_valid <= 1'b1;
                    end
                    
                    // Update LRU (simple counter increment)
                    lru_counter[req_set][hit_way] <= 3'd7;  // Most recent
                    for (integer w = 0; w < NUM_WAYS; w = w + 1) begin
                        if (w != hit_way && lru_counter[req_set][w] > 0) begin
                            lru_counter[req_set][w] <= lru_counter[req_set][w] - 1'b1;
                        end
                    end
                    
                    state <= IDLE;
                end
                
                MISS_ALLOC: begin
                    pending_way <= victim_way;
                    if (victim_dirty) begin
                        state <= WRITEBACK;
                    end else begin
                        state <= REFILL;
                    end
                end
                
                WRITEBACK: begin
                    // Write dirty line back to HBM
                    hbm_req_addr <= {tag_array[req_set][victim_way], req_set, 7'b0};
                    hbm_req_data <= data_array[req_set][victim_way][refill_count];  // 128bit chunks
                    hbm_req_wr_en <= 1'b1;
                    hbm_req_valid <= 1'b1;
                    
                    if (hbm_req_ready) begin
                        if (refill_count == 7) begin
                            refill_count <= 3'd0;
                            state <= REFILL;
                            dirty_bits[req_set][victim_way] <= 1'b0;
                        end else begin
                            refill_count <= refill_count + 1'b1;
                        end
                    end
                end
                
                REFILL: begin
                    // Read line from HBM
                    hbm_req_addr <= {req_tag, req_set, 7'b0};
                    hbm_req_wr_en <= 1'b0;
                    hbm_req_valid <= 1'b1;
                    
                    if (hbm_resp_valid) begin
                        refill_buffer[refill_count] <= hbm_resp_data;
                        if (refill_count == 7) begin
                            // Refill complete
                            state <= HIT_RW;  // Re-execute original request
                            // Install line
                            valid_bits[req_set][pending_way] <= 1'b1;
                            dirty_bits[req_set][pending_way] <= 1'b0;
                            tag_array[req_set][pending_way] <= req_tag;
                            for (integer b = 0; b < 8; b = b + 1) begin
                                data_array[req_set][pending_way][b] <= refill_buffer[b];
                            end
                            refill_count <= 3'd0;
                        end else begin
                            refill_count <= refill_count + 1'b1;
                        end
                    end
                end
                
                FLUSH: begin
                    // Flush all dirty lines
                    if (dirty_bits[flush_counter][flush_way_counter]) begin
                        // Write back this line (simplified: assume instant)
                        dirty_bits[flush_counter][flush_way_counter] <= 1'b0;
                    end
                    
                    if (flush_way_counter == NUM_WAYS-1) begin
                        flush_way_counter <= 3'd0;
                        if (flush_counter == NUM_SETS-1) begin
                            flush_done <= 1'b1;
                            state <= IDLE;
                        end else begin
                            flush_counter <= flush_counter + 1'b1;
                        end
                    end else begin
                        flush_way_counter <= flush_way_counter + 1'b1;
                    end
                end
                
                default: state <= IDLE;
            endcase
        end
    end
    
    // Snoop Logic (for Copy Engine coherence)
    // Simple: just report if address is in cache (can be extended to invalidate)
    assign ce_snoop_ready = 1'b1;
    
    reg snoop_hit_internal;
    always @(*) begin
        snoop_hit_internal = 1'b0;
        for (integer w = 0; w < NUM_WAYS; w = w + 1) begin
            if (valid_bits[ce_snoop_addr[SET_IDX_W+6:7]][w] && 
                tag_array[ce_snoop_addr[SET_IDX_W+6:7]][w] == ce_snoop_addr[63:64-TAG_W]) begin
                snoop_hit_internal = 1'b1;
            end
        end
    end
    assign ce_snoop_hit = snoop_hit_internal;

endmodule