// ============================================================================
// Module: sm_backend
// Description: SM Backend with Execution Units and Writeback Arbitration
// ============================================================================
module sm_backend #(
    parameter NUM_LANES  = 32,
    parameter WARP_ID_W  = 5,
    parameter DATA_W     = 32
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // From Frontend (Issue)
    input  wire [31:0]               instr,
    input  wire [WARP_ID_W-1:0]      warp_id,
    input  wire [1023:0]             rs0_data,
    input  wire [1023:0]             rs1_data,
    input  wire                      valid,
    output wire                      ready,
    
    // To Frontend (Writeback/Control)
    output reg  [WARP_ID_W-1:0]      wb_warp_id,
    output reg  [5:0]                wb_rd_addr,
    output reg  [1023:0]             wb_data,
    output reg  [31:0]               wb_mask,
    output reg                       wb_en,
    output reg                       branch_taken,
    output reg  [63:0]               branch_target,
    
    // To Register File (Writeback)
    output wire [WARP_ID_W-1:0]      rf_wr_warp_id [0:1],
    output wire [5:0]                rf_wr_addr    [0:1],
    output wire [1023:0]             rf_wr_data    [0:1],
    output wire [31:0]               rf_wr_mask    [0:1],
    output wire                      rf_wr_en      [0:1],
    
    // Execution Units Interface
    // ALU
    output reg  [31:0]               alu_instr,
    output reg  [1023:0]             alu_src_a,
    output reg  [1023:0]             alu_src_b,
    output reg  [1023:0]             alu_src_c,
    output reg                       alu_valid,
    input  wire                      alu_ready,
    input  wire [1023:0]             alu_result,
    input  wire                      alu_valid_out,
    
    // SFU
    output reg  [2:0]                sfu_op,
    output reg  [1023:0]             sfu_src,
    output reg                       sfu_valid,
    input  wire                      sfu_ready,
    input  wire [1023:0]             sfu_result,
    input  wire                      sfu_valid_out,
    
    // LSU
    output reg  [63:0]               lsu_base_addr,
    output reg  [31:0]               lsu_offset,  // Simplified: single offset
    output reg                       lsu_is_load,
    output reg                       lsu_is_store,
    output reg                       lsu_valid,
    input  wire                      lsu_ready,
    input  wire [1023:0]             lsu_load_data,
    input  wire                      lsu_load_valid,
    
    // LDS
    output reg  [15:0]               lds_addr,
    output reg  [31:0]               lds_data,
    output reg                       lds_wr_en,
    output reg                       lds_valid,
    input  wire                      lds_ready,
    input  wire [31:0]               lds_rdata,
    input  wire                      lds_rdata_valid
);

    // Instruction Decode (simplified)
    wire [5:0] opcode = instr[31:26];
    wire [5:0] rd_addr = instr[7:2];
    
    // Unit Selection
    localparam UNIT_ALU  = 2'b00;
    localparam UNIT_SFU  = 2'b01;
    localparam UNIT_LSU  = 2'b10;
    localparam UNIT_LDS  = 2'b11;
    
    reg [1:0] unit_sel;
    reg [3:0] latency;  // Expected latency for tracking
    
    // Dispatch Logic
    assign ready = alu_ready && sfu_ready && lsu_ready && lds_ready;  // All ready (simplified)
    
    always @(*) begin
        // Default
        unit_sel = UNIT_ALU;
        alu_valid = 1'b0;
        sfu_valid = 1'b0;
        lsu_valid = 1'b0;
        lds_valid = 1'b0;
        
        if (valid) begin
            case (opcode[5:4])  // Use upper bits to select unit
                2'b00, 2'b01: begin  // ALU operations
                    unit_sel = UNIT_ALU;
                    alu_instr = instr;
                    alu_src_a = rs0_data;
                    alu_src_b = rs1_data;
                    alu_src_c = 1024'd0;  // For MAD
                    alu_valid = 1'b1;
                end
                
                2'b10: begin  // SFU
                    unit_sel = UNIT_SFU;
                    sfu_op = instr[2:0];
                    sfu_src = rs0_data;
                    sfu_valid = 1'b1;
                end
                
                2'b11: begin
                    if (instr[3]) begin  // LDS
                        unit_sel = UNIT_LDS;
                        lds_addr = rs0_data[15:0];
                        lds_data = rs1_data[31:0];
                        lds_wr_en = instr[0];  // Bit 0 indicates store
                        lds_valid = 1'b1;
                    end else begin  // LSU
                        unit_sel = UNIT_LSU;
                        lsu_base_addr = {32'd0, rs0_data[31:0]};
                        lsu_offset = rs1_data[31:0];
                        lsu_is_load = !instr[0];
                        lsu_is_store = instr[0];
                        lsu_valid = 1'b1;
                    end
                end
            endcase
        end
    end
    
    // Writeback Arbitration (Priority: LSU > ALU > SFU > LDS)
    // LSU has highest priority to prevent memory pipeline deadlock
    reg [1023:0] selected_result;
    reg [WARP_ID_W-1:0] selected_warp;
    reg [5:0] selected_rd;
    reg selected_valid;
    
    always @(*) begin
        selected_valid = 1'b0;
        selected_result = 1024'd0;
        selected_warp = {WARP_ID_W{1'b0}};
        selected_rd = 6'd0;
        
        if (lsu_load_valid) begin
            selected_valid = 1'b1;
            selected_result = lsu_load_data;
            selected_warp = warp_id;  // Should track per-unit warp_id in reality
            selected_rd = rd_addr;
        end else if (alu_valid_out) begin
            selected_valid = 1'b1;
            selected_result = alu_result;
            selected_warp = warp_id;
            selected_rd = rd_addr;
        end else if (sfu_valid_out) begin
            selected_valid = 1'b1;
            selected_result = sfu_result;
            selected_warp = warp_id;
            selected_rd = rd_addr;
        end else if (lds_rdata_valid) begin
            selected_valid = 1'b1;
            // Broadcast LDS data to all lanes (simplified)
            selected_result = {32{lds_rdata}};  // Replicate to 1024 bits
            selected_warp = warp_id;
            selected_rd = rd_addr;
        end
    end
    
    // Sequential Writeback to Register File
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wb_en <= 1'b0;
        end else begin
            wb_en <= selected_valid;
            wb_warp_id <= selected_warp;
            wb_rd_addr <= selected_rd;
            wb_data <= selected_result;
            wb_mask <= 32'hFFFFFFFF;  // Full mask (simplified)
            
            // Branch detection (simplified)
            branch_taken <= 1'b0;
            if (valid && opcode == 6'b111111) begin  // Branch opcode
                branch_taken <= 1'b1;
                branch_target <= rs0_data[63:0];  // Target from register
            end
        end
    end
    
    // Connect to RF write ports (only using port 0 for now)
    assign rf_wr_warp_id[0] = wb_warp_id;
    assign rf_wr_addr[0] = wb_rd_addr;
    assign rf_wr_data[0] = wb_data;
    assign rf_wr_mask[0] = wb_mask;
    assign rf_wr_en[0] = wb_en;
    
    // Port 1 unused in simplified model
    assign rf_wr_warp_id[1] = {WARP_ID_W{1'b0}};
    assign rf_wr_addr[1] = 6'd0;
    assign rf_wr_data[1] = 1024'd0;
    assign rf_wr_mask[1] = 32'd0;
    assign rf_wr_en[1] = 1'b0;

endmodule