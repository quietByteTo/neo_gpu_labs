// ============================================================================
// Module: l1_data_cache
// Description: Configurable L1 Data Cache / Shared Memory
// 4-Way Set Associative, 128B Line, Write-Back, Write-Allocate
// ============================================================================
module l1_data_cache #(
    parameter CACHE_SIZE_KB = 64,       // Total cache size
    parameter LINE_SIZE     = 128,      // Bytes per line
    parameter NUM_WAYS      = 4,        // Associativity
    parameter NUM_SETS      = (CACHE_SIZE_KB * 1024) / (LINE_SIZE * NUM_WAYS),
    parameter ADDR_W        = 64,
    parameter DATA_W        = 128,      // Interface data width (128bit)
    parameter SET_IDX_W     = $clog2(NUM_SETS),
    parameter LINE_OFFSET_W = $clog2(LINE_SIZE),
    parameter TAG_W         = ADDR_W - SET_IDX_W - LINE_OFFSET_W,
    parameter WAY_W         = $clog2(NUM_WAYS)  // 2 bits for 4 ways
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    // LSU Interface
    input  wire [ADDR_W-1:0]     req_addr,
    input  wire [DATA_W-1:0]     req_wdata,
    input  wire                  req_wr_en,
    input  wire [DATA_W/8-1:0]   req_be,         // Byte enable (16 bits for 128bit)
    input  wire                  req_valid,
    output wire                  req_ready,
    output reg  [DATA_W-1:0]     resp_rdata,
    output reg                   resp_valid,
    
    // L2 Interface (Miss Fill)
    output reg  [ADDR_W-1:0]     mem_req_addr,
    output reg                   mem_req_valid,
    input  wire                  mem_req_ready,
    input  wire [511:0]          mem_fill_data,  // 512bit (4×128bit) for 128B line
    input  wire                  mem_fill_valid,
    
    // Configuration
    input  wire                  cache_en,       // 0=Bypass (Shared Mem mode), 1=Cache mode
    input  wire                  flush_req,      // Flush all lines
    output reg                   flush_done
);

    // Address decomposition
    wire [TAG_W-1:0]     req_tag    = req_addr[ADDR_W-1 : ADDR_W-TAG_W];
    wire [SET_IDX_W-1:0] req_set    = req_addr[SET_IDX_W+LINE_OFFSET_W-1 : LINE_OFFSET_W];
    wire [LINE_OFFSET_W-1:0] req_offset = req_addr[LINE_OFFSET_W-1:0];
    
    // State Machine
    localparam IDLE      = 3'b000;
    localparam LOOKUP    = 3'b001;  // Tag lookup stage
    localparam MISS      = 3'b010;  // Handle miss
    localparam FILL      = 3'b011;  // Fill from L2
    localparam WRITEBACK = 3'b100;  // Writeback dirty line
    localparam FLUSH     = 3'b101;  // Flush all
    
    reg [2:0] state;
    reg [SET_IDX_W-1:0] flush_counter;
    
    // Tag comparison (combinational)
    reg [NUM_WAYS-1:0] way_hit;
    reg [WAY_W-1:0] hit_way;        // ✅ 参数化位宽
    reg [WAY_W-1:0] victim_way;     // ✅ 移到模块开头，参数化位宽
    
    // LRU Replacement (per set)
    reg [2:0] lru_bits [0:NUM_SETS-1];  // Simple 3-bit LRU for 4 ways
    
    // Way Data (simplified: using arrays for Verilator compatibility)
    reg valid_bits [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg dirty_bits [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg [TAG_W-1:0] tag_array [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg [127:0] data_array [0:NUM_SETS-1][0:NUM_WAYS-1][0:7];  // 8 sub-blocks per line
    
    integer set_idx, way_idx, sub_idx;
    
    // Tag Lookup (Combinational) - ✅ 修复 LATCH
    always @(*) begin
        way_hit = {NUM_WAYS{1'b0}};
        hit_way = {WAY_W{1'b0}};        // ✅ 默认值，避免 latch
        
        for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
            if (valid_bits[req_set][way_idx] && 
                tag_array[req_set][way_idx] == req_tag) begin
                way_hit[way_idx] = 1'b1;
                hit_way = way_idx[WAY_W-1:0];
            end
        end
    end
    
    wire cache_hit = |way_hit;
    
    // Main State Machine
    assign req_ready = (state == IDLE) || (state == LOOKUP && cache_hit);
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            resp_valid <= 1'b0;
            mem_req_valid <= 1'b0;
            flush_done <= 1'b0;
            
            // Initialize arrays
            for (set_idx = 0; set_idx < NUM_SETS; set_idx = set_idx + 1) begin
                lru_bits[set_idx] <= 3'd0;
                for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
                    valid_bits[set_idx][way_idx] <= 1'b0;
                    dirty_bits[set_idx][way_idx] <= 1'b0;
                    tag_array[set_idx][way_idx] <= {TAG_W{1'b0}};
                    for (sub_idx = 0; sub_idx < 8; sub_idx = sub_idx + 1) begin
                        data_array[set_idx][way_idx][sub_idx] <= 128'd0;
                    end
                end
            end
        end else begin
            resp_valid <= 1'b0;  // Default pulse
            flush_done <= 1'b0;
            
            case (state)
                IDLE: begin
                    if (flush_req) begin
                        state <= FLUSH;
                        flush_counter <= {SET_IDX_W{1'b0}};
                    end else if (req_valid && cache_en) begin
                        state <= LOOKUP;
                    end else if (req_valid && !cache_en) begin
                        // Bypass mode: direct to memory (simplified)
                        state <= MISS;
                    end
                end
                
                LOOKUP: begin
                    if (cache_hit) begin
                        // Handle Hit
                        if (req_wr_en) begin
                            // Write hit: update data and set dirty
                            dirty_bits[req_set][hit_way] <= 1'b1;
                            // Merge write data with byte enable
                            // Simplified: assume aligned 128bit write for now
                            data_array[req_set][hit_way][req_offset[6:4]] <= req_wdata;
                        end else begin
                            // Read hit: return data
                            resp_rdata <= data_array[req_set][hit_way][req_offset[6:4]];
                            resp_valid <= 1'b1;
                        end
                        
                        // Update LRU
                        // Simple algorithm: if hit_way==0, LRU=0; else increment
                        if (hit_way != {WAY_W{1'b0}}) 
                            lru_bits[req_set] <= lru_bits[req_set] + 1'b1;
                        
                        state <= IDLE;
                    end else begin
                        // Miss
                        state <= MISS;
                    end
                end
                
                MISS: begin
                    // Select victim way (simplified: use LRU counter mod 4)
                    victim_way <= lru_bits[req_set][WAY_W-1:0];
                    
                    if (dirty_bits[req_set][victim_way]) begin
                        state <= WRITEBACK;
                    end else begin
                        state <= FILL;
                        mem_req_valid <= 1'b1;
                        mem_req_addr <= {req_tag, req_set, {LINE_OFFSET_W{1'b0}}};
                    end
                end
                
                WRITEBACK: begin
                    // Writeback dirty line to L2 (simplified: assume always ready)
                    dirty_bits[req_set][victim_way] <= 1'b0;
                    state <= FILL;
                    mem_req_valid <= 1'b1;
                    mem_req_addr <= {tag_array[req_set][victim_way], req_set, {LINE_OFFSET_W{1'b0}}};
                end
                
                FILL: begin
                    if (mem_fill_valid) begin
                        // Receive 512bit (4 cycles for 128B line)
                        // Simplified: assume single cycle fill for now
                        valid_bits[req_set][victim_way] <= 1'b1;
                        dirty_bits[req_set][victim_way] <= 1'b0;
                        tag_array[req_set][victim_way] <= req_tag;
                        
                        // Store fill data (simplified)
                        // data_array[req_set][victim_way] <= mem_fill_data;
                        
                        state <= LOOKUP;  // Re-lookup to get data
                    end
                end
                
                FLUSH: begin
                    // Invalidate all lines
                    for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
                        valid_bits[flush_counter][way_idx] <= 1'b0;
                        dirty_bits[flush_counter][way_idx] <= 1'b0;
                    end
                    
                    // ✅ 修复 WIDTHEXPAND：显式指定位宽
                    if (flush_counter == (NUM_SETS[SET_IDX_W-1:0] - {{SET_IDX_W-1{1'b0}}, 1'b1})) begin
                        flush_done <= 1'b1;
                        state <= IDLE;
                    end else begin
                        flush_counter <= flush_counter + {{SET_IDX_W-1{1'b0}}, 1'b1};
                    end
                end
                
                default: state <= IDLE;
            endcase
        end
    end

endmodule