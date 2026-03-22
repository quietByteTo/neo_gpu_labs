// ============================================================================
// Module: lds_unit
// Description: Local Data Share (Shared Memory), 32-Bank, 64KB total
// 2KB per bank, supports atomic operations, conflict detection & replay
// ============================================================================
module lds_unit #(
    parameter NUM_LANES   = 32,
    parameter NUM_BANKS   = 32,         // 32 banks for conflict-free stride-32 access
    parameter BANK_SIZE   = 2048,       // 2KB per bank
    parameter ADDR_W      = 16,         // 64KB address space
    parameter DATA_W      = 32,
    parameter BANK_ADDR_W = $clog2(BANK_SIZE/4)  // Word address within bank (512 words)
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    // Lane Requests (32 lanes)
    input  wire [ADDR_W-1:0]     lds_addr   [0:NUM_LANES-1],  // Per-lane address
    input  wire [DATA_W-1:0]     lds_wdata  [0:NUM_LANES-1],
    input  wire                  lds_wr_en  [0:NUM_LANES-1],
    input  wire                  lds_valid  [0:NUM_LANES-1],  // Which lanes active
    output reg                   lds_ready  [0:NUM_LANES-1],  // Which lanes accepted
    
    // Response
    output reg  [DATA_W-1:0]     lds_rdata  [0:NUM_LANES-1],
    output reg                   lds_rdata_valid,
    
    // Atomic Operation Type
    input  wire [2:0]            atomic_op,   // 0:NOP, 1:ADD, 2:MIN, 3:MAX, 4:AND, 5:OR, 6:XOR
    input  wire                  atomic_en,
    
    // Status
    output wire                  conflict_detected,
    output reg  [4:0]            conflict_count  // Number of conflicting lanes
);

    // Bank Selection: Address[6:2] (5 bits) selects bank (32 banks)
    // This maps consecutive addresses to different banks (stride-1 conflict-free)
    wire [4:0]  lane_bank_sel [0:NUM_LANES-1];
    wire [BANK_ADDR_W-1:0] lane_bank_addr [0:NUM_LANES-1];
    
    genvar i;
    generate
        for (i = 0; i < NUM_LANES; i = i + 1) begin : addr_decode
            assign lane_bank_sel[i]  = lds_addr[i][6:2];   // Bits [6:2] for 32 banks
            assign lane_bank_addr[i] = lds_addr[i][ADDR_W-1:7];  // Upper bits for bank addr
        end
    endgenerate
    
    // Bank Instances (32 banks)
    reg  [DATA_W-1:0] bank_mem [0:NUM_BANKS-1][0:BANK_SIZE/4-1];
    
    // Conflict Detection Logic
    reg [NUM_LANES-1:0] lane_granted;
    reg [4:0] bank_conflict_map [0:NUM_BANKS-1];
    reg bank_has_req [0:NUM_BANKS-1];
    
    integer l;
    integer bi;
    reg [4:0] cur_bank0;
    
    always @(*) begin
        for (bi = 0; bi < NUM_BANKS; bi = bi + 1) begin
            bank_has_req[bi] = 1'b0;
            bank_conflict_map[bi] = 5'd0;
        end
        for (l = 0; l < NUM_LANES; l = l + 1) begin
            lane_granted[l] = 1'b0;
            lds_ready[l] = 1'b0;
        end
        conflict_count = 5'd0;
        
        for (l = 0; l < NUM_LANES; l = l + 1) begin
            cur_bank0 = 5'd0;
            if (lds_valid[l]) begin
                cur_bank0 = lane_bank_sel[l];
                if (!bank_has_req[cur_bank0]) begin
                    bank_has_req[cur_bank0] = 1'b1;
                    bank_conflict_map[cur_bank0] = l[4:0];
                    lane_granted[l] = 1'b1;
                    lds_ready[l] = 1'b1;
                end else begin
                    conflict_count = conflict_count + 1'b1;
                end
            end
        end
    end
    
    assign conflict_detected = (conflict_count != 0);
    
    // 组合逻辑计算 any_valid
    reg any_valid;
    integer v;
    always @(*) begin
        any_valid = 1'b0;
        for (v = 0; v < NUM_LANES; v = v + 1) begin
            any_valid = any_valid | lds_valid[v];
        end
    end
    
    // 组合逻辑计算 atomic_result
    reg [DATA_W-1:0] atomic_result [0:NUM_LANES-1];
    reg [DATA_W-1:0] old_value [0:NUM_LANES-1];
    reg [4:0]        cur_bank1;
    
    always @(*) begin
        for (l = 0; l < NUM_LANES; l = l + 1) begin
            cur_bank1 = 5'd0;
            atomic_result[l] = {DATA_W{1'b0}};
            old_value[l] = {DATA_W{1'b0}};
            if (lane_granted[l]) begin
                cur_bank1 = lane_bank_sel[l];
                old_value[l] = bank_mem[cur_bank1][lane_bank_addr[l]];
                if (atomic_en) begin
                    case (atomic_op)
                        3'd1: atomic_result[l] = old_value[l] + lds_wdata[l];
                        3'd2: atomic_result[l] = (old_value[l] < lds_wdata[l]) ? old_value[l] : lds_wdata[l];
                        3'd3: atomic_result[l] = (old_value[l] > lds_wdata[l]) ? old_value[l] : lds_wdata[l];
                        3'd4: atomic_result[l] = old_value[l] & lds_wdata[l];
                        3'd5: atomic_result[l] = old_value[l] | lds_wdata[l];
                        3'd6: atomic_result[l] = old_value[l] ^ lds_wdata[l];
                        default: atomic_result[l] = lds_wdata[l];
                    endcase
                end
            end
        end
    end
    
    // Execution Phase (Sequential)：索引用 lane_bank_sel[l]，避免时序块内阻塞赋值（BLKSEQ）
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            lds_rdata_valid <= 1'b0;
            for (l = 0; l < NUM_LANES; l = l + 1) begin
                lds_rdata[l] <= {DATA_W{1'b0}};
            end
        end else begin
            lds_rdata_valid <= any_valid;
            
            for (l = 0; l < NUM_LANES; l = l + 1) begin
                if (lane_granted[l]) begin
                    if (lds_wr_en[l] || atomic_en) begin
                        if (atomic_en) begin
                            bank_mem[lane_bank_sel[l]][lane_bank_addr[l]] <= atomic_result[l];
                            lds_rdata[l] <= old_value[l];
                        end else begin
                            bank_mem[lane_bank_sel[l]][lane_bank_addr[l]] <= lds_wdata[l];
                        end
                    end else begin
                        lds_rdata[l] <= bank_mem[lane_bank_sel[l]][lane_bank_addr[l]];
                    end
                end
            end
        end
    end
    
    // Initialize memory
    integer init_b, init_a;
    initial begin
        for (init_b = 0; init_b < NUM_BANKS; init_b = init_b + 1) begin
            for (init_a = 0; init_a < BANK_SIZE/4; init_a = init_a + 1) begin
                bank_mem[init_b][init_a] = {DATA_W{1'b0}};
            end
        end
    end

endmodule

