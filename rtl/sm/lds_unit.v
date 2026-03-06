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
    // Each bank: sram_2p (1RW + 1R), but we need multi-port for lane conflicts
    // Simplification: Each cycle, each bank can accept 1 request (others stall)
    reg  [DATA_W-1:0] bank_mem [0:NUM_BANKS-1][0:BANK_SIZE/4-1];  // Use array for flexibility
    
    // Conflict Detection Logic
    // Check if multiple lanes access same bank in same cycle
    reg [NUM_LANES-1:0] lane_granted;
    reg [4:0] bank_conflict_map [0:NUM_BANKS-1];  // Which lane wins per bank (max 32)
    reg bank_has_req [0:NUM_BANKS-1];
    
    integer b, l;
    
    always @(*) begin
        // Initialize
        for (b = 0; b < NUM_BANKS; b = b + 1) begin
            bank_has_req[b] = 1'b0;
            bank_conflict_map[b] = 5'd0;
        end
        for (l = 0; l < NUM_LANES; l = l + 1) begin
            lane_granted[l] = 1'b0;
            lds_ready[l] = 1'b0;
        end
        conflict_count = 5'd0;
        
        // First-pass: mark requests per bank, first-come-first-serve (lower lane ID wins)
        for (l = 0; l < NUM_LANES; l = l + 1) begin
            if (lds_valid[l]) begin
                b = lane_bank_sel[l];
                if (!bank_has_req[b]) begin
                    // First request to this bank
                    bank_has_req[b] = 1'b1;
                    bank_conflict_map[b] = l[4:0];
                    lane_granted[l] = 1'b1;
                    lds_ready[l] = 1'b1;
                end else begin
                    // Conflict: stall this lane
                    conflict_count = conflict_count + 1'b1;
                end
            end
        end
    end
    
    assign conflict_detected = (conflict_count != 0);
    
    // Execution Phase (Sequential)
    integer lane_idx;
    reg [DATA_W-1:0] read_data [0:NUM_LANES-1];
    reg [DATA_W-1:0] atomic_result;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            lds_rdata_valid <= 1'b0;
            for (l = 0; l < NUM_LANES; l = l + 1) begin
                lds_rdata[l] <= {DATA_W{1'b0}};
            end
        end else begin
            lds_rdata_valid <= |lds_valid;  // Valid if any lane requested
            
            for (l = 0; l < NUM_LANES; l = l + 1) begin
                if (lane_granted[l]) begin
                    b = lane_bank_sel[l];
                    if (lds_wr_en[l] || atomic_en) begin
                        // Read-Modify-Write for Atomic or normal Write
                        if (atomic_en) begin
                            // Atomic operation
                            case (atomic_op)
                                3'd1: atomic_result = bank_mem[b][lane_bank_addr[l]] + lds_wdata[l]; // ADD
                                3'd2: atomic_result = (bank_mem[b][lane_bank_addr[l]] < lds_wdata[l]) ? 
                                                      bank_mem[b][lane_bank_addr[l]] : lds_wdata[l]; // MIN
                                3'd3: atomic_result = (bank_mem[b][lane_bank_addr[l]] > lds_wdata[l]) ? 
                                                      bank_mem[b][lane_bank_addr[l]] : lds_wdata[l]; // MAX
                                3'd4: atomic_result = bank_mem[b][lane_bank_addr[l]] & lds_wdata[l]; // AND
                                3'd5: atomic_result = bank_mem[b][lane_bank_addr[l]] | lds_wdata[l]; // OR
                                3'd6: atomic_result = bank_mem[b][lane_bank_addr[l]] ^ lds_wdata[l]; // XOR
                                default: atomic_result = lds_wdata[l];
                            endcase
                            bank_mem[b][lane_bank_addr[l]] <= atomic_result;
                            lds_rdata[l] <= bank_mem[b][lane_bank_addr[l]];  // Return old value
                        end else begin
                            // Normal write
                            bank_mem[b][lane_bank_addr[l]] <= lds_wdata[l];
                        end
                    end else begin
                        // Read
                        lds_rdata[l] <= bank_mem[b][lane_bank_addr[l]];
                    end
                end
            end
        end
    end
    
    // Initialize memory (Verilator)
    integer init_b, init_a;
    initial begin
        for (init_b = 0; init_b < NUM_BANKS; init_b = init_b + 1) begin
            for (init_a = 0; init_a < BANK_SIZE/4; init_a = init_a + 1) begin
                bank_mem[init_b][init_a] = {DATA_W{1'b0}};
            end
        end
    end

endmodule