// ============================================================================
// True LRU Cache - 完整可综合版本
// ============================================================================
module l1_data_cache #(
    parameter CACHE_SIZE_KB = 64,
    parameter LINE_SIZE     = 128,
    parameter NUM_WAYS      = 4,
    parameter NUM_SETS      = (CACHE_SIZE_KB * 1024) / (LINE_SIZE * NUM_WAYS),
    parameter ADDR_W        = 64,
    parameter DATA_W        = 128,
    parameter SET_IDX_W     = $clog2(NUM_SETS),
    parameter LINE_OFFSET_W = $clog2(LINE_SIZE),
    parameter TAG_W         = ADDR_W - SET_IDX_W - LINE_OFFSET_W,
    parameter WAY_W         = $clog2(NUM_WAYS)
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    input  wire [ADDR_W-1:0]     req_addr,
    input  wire [DATA_W-1:0]     req_wdata,
    input  wire                  req_wr_en,
    input  wire [DATA_W/8-1:0]   req_be,
    input  wire                  req_valid,
    output wire                  req_ready,
    output reg  [DATA_W-1:0]     resp_rdata,
    output reg                   resp_valid,
    
    output reg  [ADDR_W-1:0]     mem_req_addr,
    output reg                   mem_req_valid,
    input  wire                  mem_req_ready,
    input  wire [511:0]          mem_fill_data,
    input  wire                  mem_fill_valid,
    
    input  wire                  cache_en,
    input  wire                  flush_req,
    output reg                   flush_done
);

    // 地址分解
    wire [TAG_W-1:0]     req_tag    = req_addr[ADDR_W-1 : ADDR_W-TAG_W];
    wire [SET_IDX_W-1:0] req_set    = req_addr[SET_IDX_W+LINE_OFFSET_W-1 : LINE_OFFSET_W];
    wire [LINE_OFFSET_W-1:0] req_offset = req_addr[LINE_OFFSET_W-1:0];
    wire [2:0]           req_sub_block = req_offset[6:4];
    
    // 状态机
    localparam IDLE      = 3'd0;
    localparam LOOKUP    = 3'd1;
    localparam MISS      = 3'd2;
    localparam FILL_REQ  = 3'd3;
    localparam FILL_WAIT = 3'd4;
    localparam WRITEBACK = 3'd5;
    localparam FLUSH     = 3'd6;
    
    reg [2:0] state;
    reg [SET_IDX_W-1:0] flush_counter;
    reg flushing;
    
    // Tag比较
    reg [NUM_WAYS-1:0] way_hit;
    reg [WAY_W-1:0] hit_way;
    reg [WAY_W-1:0] victim_way_reg;
    
    // =========================================================================
    // True LRU: 每路一个2位计数器 (0=LRU, 3=MRU)
    // =========================================================================
    reg [1:0] lru_cnt [0:NUM_SETS-1][0:NUM_WAYS-1];
    
    // Cache数组
    reg valid_bits [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg dirty_bits [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg [TAG_W-1:0] tag_array [0:NUM_SETS-1][0:NUM_WAYS-1];
    reg [127:0] data_array [0:NUM_SETS-1][0:NUM_WAYS-1][0:7];
    
    // Fill控制
    reg [511:0] fill_buffer;
    reg         fill_beat;
    
    integer set_idx, way_idx, sub_idx;
    
    // =========================================================================
    // Tag查找
    // =========================================================================
    always @(*) begin
        way_hit = {NUM_WAYS{1'b0}};
        hit_way = {WAY_W{1'b0}};
        for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
            if (valid_bits[req_set][way_idx] && 
                tag_array[req_set][way_idx] == req_tag) begin
                way_hit[way_idx] = 1'b1;
                hit_way = way_idx[WAY_W-1:0];
            end
        end
    end
    
    wire cache_hit = |way_hit;
    
    // =========================================================================
    // True LRU: 找计数器最小的way
    // =========================================================================
    reg [WAY_W-1:0] victim_way;
    reg [1:0] min_cnt;
    
    always @(*) begin
        victim_way = 2'd0;
        min_cnt = lru_cnt[req_set][0];
        
        for (way_idx = 1; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
            if (lru_cnt[req_set][way_idx] < min_cnt) begin
                min_cnt = lru_cnt[req_set][way_idx];
                victim_way = way_idx[WAY_W-1:0];
            end
        end
    end
    
    // =========================================================================
    // 输出
    // =========================================================================
    assign req_ready = (state == IDLE) || (state == LOOKUP && cache_hit);
    
    // =========================================================================
    // 状态机
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            resp_valid <= 1'b0;
            mem_req_valid <= 1'b0;
            flush_done <= 1'b0;
            flushing <= 1'b0;
            flush_counter <= {SET_IDX_W{1'b0}};
            victim_way_reg <= {WAY_W{1'b0}};
            fill_beat <= 1'b0;
            
            for (set_idx = 0; set_idx < NUM_SETS; set_idx = set_idx + 1) begin
                for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
                    lru_cnt[set_idx][way_idx] <= 2'd0;
                    valid_bits[set_idx][way_idx] <= 1'b0;
                    dirty_bits[set_idx][way_idx] <= 1'b0;
                    tag_array[set_idx][way_idx] <= {TAG_W{1'b0}};
                    for (sub_idx = 0; sub_idx < 8; sub_idx = sub_idx + 1) begin
                        data_array[set_idx][way_idx][sub_idx] <= 128'd0;
                    end
                end
            end
        end else begin
            resp_valid <= 1'b0;
            flush_done <= 1'b0;
            
            case (state)
                IDLE: begin
                    mem_req_valid <= 1'b0;
                    fill_beat <= 1'b0;
                    
                    if (flush_req && !flushing) begin
                        state <= FLUSH;
                        flush_counter <= {SET_IDX_W{1'b0}};
                        flushing <= 1'b1;
                    end else if (req_valid && cache_en) begin
                        state <= LOOKUP;
                    end else if (req_valid && !cache_en) begin
                        victim_way_reg <= 2'd0;
                        state <= MISS;
                    end else begin
                        flushing <= 1'b0;
                    end
                end
                
                LOOKUP: begin
                    if (cache_hit) begin
                        // 命中处理
                        if (req_wr_en) begin
                            dirty_bits[req_set][hit_way] <= 1'b1;
                            data_array[req_set][hit_way][req_sub_block] <= req_wdata;
                        end else begin
                            resp_rdata <= data_array[req_set][hit_way][req_sub_block];
                            resp_valid <= 1'b1;
                        end
                        
                        // =========================================================================
                        // True LRU更新: 命中way变MRU，比它大的减1
                        // =========================================================================
                        for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
                            if (way_idx[WAY_W-1:0] == hit_way)
                                lru_cnt[req_set][way_idx] <= 2'd3;
                            else if (lru_cnt[req_set][way_idx] > lru_cnt[req_set][hit_way])
                                lru_cnt[req_set][way_idx] <= lru_cnt[req_set][way_idx] - 2'd1;
                        end
                        
                        state <= IDLE;
                    end else begin
                        victim_way_reg <= victim_way;
                        state <= MISS;
                    end
                end
                
                MISS: begin
                    if (dirty_bits[req_set][victim_way_reg]) begin
                        state <= WRITEBACK;
                        mem_req_addr <= {tag_array[req_set][victim_way_reg], req_set, {LINE_OFFSET_W{1'b0}}};
                        mem_req_valid <= 1'b1;
                    end else begin
                        state <= FILL_REQ;
                        mem_req_addr <= {req_tag, req_set, {LINE_OFFSET_W{1'b0}}};
                        mem_req_valid <= 1'b1;
                    end
                end
                
                WRITEBACK: begin
                    if (mem_req_ready) begin
                        dirty_bits[req_set][victim_way_reg] <= 1'b0;
                        state <= FILL_REQ;
                        mem_req_addr <= {req_tag, req_set, {LINE_OFFSET_W{1'b0}}};
                        mem_req_valid <= 1'b1;
                    end
                end
                
                FILL_REQ: begin
                    if (mem_req_ready) begin
                        mem_req_valid <= 1'b0;
                        state <= FILL_WAIT;
                        fill_beat <= 1'b0;
                    end
                end
                
                FILL_WAIT: begin
                    if (mem_fill_valid) begin
                        if (!fill_beat) begin
                            fill_buffer <= mem_fill_data;
                            fill_beat <= 1'b1;
                        end else begin
                            // 写入数据
                            {data_array[req_set][victim_way_reg][7],
                             data_array[req_set][victim_way_reg][6],
                             data_array[req_set][victim_way_reg][5],
                             data_array[req_set][victim_way_reg][4]} <= mem_fill_data;
                             
                            {data_array[req_set][victim_way_reg][3],
                             data_array[req_set][victim_way_reg][2],
                             data_array[req_set][victim_way_reg][1],
                             data_array[req_set][victim_way_reg][0]} <= fill_buffer;
                            
                            valid_bits[req_set][victim_way_reg] <= 1'b1;
                            dirty_bits[req_set][victim_way_reg] <= 1'b0;
                            tag_array[req_set][victim_way_reg] <= req_tag;
                            
                            // =========================================================================
                            // True LRU更新: 新填入的way变MRU
                            // =========================================================================
                            for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
                                if (way_idx[WAY_W-1:0] == victim_way_reg)
                                    lru_cnt[req_set][way_idx] <= 2'd3;
                                else if (lru_cnt[req_set][way_idx] > 0)
                                    lru_cnt[req_set][way_idx] <= lru_cnt[req_set][way_idx] - 2'd1;
                            end
                            
                            state <= LOOKUP;
                        end
                    end
                end
                
                FLUSH: begin
                    for (way_idx = 0; way_idx < NUM_WAYS; way_idx = way_idx + 1) begin
                        valid_bits[flush_counter][way_idx] <= 1'b0;
                        dirty_bits[flush_counter][way_idx] <= 1'b0;
                        lru_cnt[flush_counter][way_idx] <= 2'd0;
                    end
                    
                    if (&flush_counter) begin  // 全1检测
                        flush_done <= 1'b1;
                        flushing <= 1'b0;
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
