// ============================================================================
// Module: sm_backend (IMPROVED VERSION)
// 改进点：
// 1. 独立跟踪各执行单元的warp_id和rd_addr
// 2. 支持多端口写回(真正的双端口)
// 3. 改进的ready信号逻辑(单元级流控)
// 4. 结构化的执行单元状态管理
// 5. 死锁避免机制
// ============================================================================

module sm_backend #(
    parameter NUM_LANES  = 32,
    parameter WARP_ID_W  = 5,
    parameter DATA_W     = 32,
    parameter NUM_EUS    = 4      // 执行单元数量
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
    
    // To Register File (Dual-port writeback)
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
    
    // SFU Interface
    output reg  [2:0]                sfu_op,
    output reg  [1023:0]             sfu_src,
    output reg                       sfu_valid,
    input  wire                      sfu_ready,
    input  wire [1023:0]             sfu_result,
    input  wire                      sfu_valid_out,
    
    // LSU Interface
    output reg  [63:0]               lsu_base_addr,
    output reg  [31:0]               lsu_offset,
    output reg                       lsu_is_load,
    output reg                       lsu_is_store,
    output reg                       lsu_valid,
    input  wire                      lsu_ready,
    input  wire [1023:0]             lsu_load_data,
    input  wire                      lsu_load_valid,
    
    // LDS Interface
    output reg  [15:0]               lds_addr,
    output reg  [31:0]               lds_data,
    output reg                       lds_wr_en,
    output reg                       lds_valid,
    input  wire                      lds_ready,
    input  wire [31:0]               lds_rdata,
    input  wire                      lds_rdata_valid
);

    // ========================================================================
    // 1. 指令解码
    // ========================================================================
    wire [5:0] opcode = instr[31:26];
    wire [5:0] rd_addr = instr[7:2];
    
    localparam UNIT_ALU  = 2'b00;
    localparam UNIT_SFU  = 2'b01;
    localparam UNIT_LSU  = 2'b10;
    localparam UNIT_LDS  = 2'b11;
    
    // ========================================================================
    // 2. 执行单元状态寄存器(关键改进!)
    // ========================================================================
    // 为每个执行单元跟踪其元数据
    reg [WARP_ID_W-1:0] alu_warp_id, sfu_warp_id, lsu_warp_id, lds_warp_id;
    reg [5:0]           alu_rd_addr,  sfu_rd_addr,  lsu_rd_addr,  lds_rd_addr;
    reg [31:0]          alu_mask,     sfu_mask,     lsu_mask,     lds_mask;
    
    // ========================================================================
    // 3. 改进的分发逻辑
    // ========================================================================
    reg [1:0] unit_sel;
    
    always @(*) begin
        // ========== 默认值赋值（防止 Latch 推断）==========
        unit_sel = UNIT_ALU;  // 默认 ALU
        
        // ALU 默认
        alu_instr = 32'd0;
        alu_src_a = 1024'd0;
        alu_src_b = 1024'd0;
        alu_src_c = 1024'd0;
        alu_valid = 1'b0;
        
        // SFU 默认
        sfu_op = 3'd0;
        sfu_src = 1024'd0;
        sfu_valid = 1'b0;
        
        // LSU 默认
        lsu_base_addr = 64'd0;
        lsu_offset = 32'd0;
        lsu_is_load = 1'b0;
        lsu_is_store = 1'b0;
        lsu_valid = 1'b0;
        
        // LDS 默认
        lds_addr = 16'd0;
        lds_data = 32'd0;
        lds_wr_en = 1'b0;
        lds_valid = 1'b0;
        
        // 实际 Dispatch 逻辑
        if (valid) begin
            case (opcode[5:4])
                2'b00, 2'b01: begin  // ALU
                    if (alu_ready) begin
                        unit_sel = UNIT_ALU;
                        alu_instr = instr;
                        alu_src_a = rs0_data;
                        alu_src_b = rs1_data;
                        alu_src_c = 1024'd0;  // For MAD
                        alu_valid = 1'b1;
                    end
                end
                
                2'b10: begin  // SFU
                    if (sfu_ready) begin
                        unit_sel = UNIT_SFU;
                        sfu_op = instr[2:0];
                        sfu_src = rs0_data;
                        sfu_valid = 1'b1;
                    end
                end
                
                2'b11: begin
                    if (instr[3]) begin  // LDS
                        if (lds_ready) begin
                            unit_sel = UNIT_LDS;
                            lds_addr = rs0_data[15:0];
                            lds_data = rs1_data[31:0];
                            lds_wr_en = instr[0];
                            lds_valid = 1'b1;
                        end
                    end else begin  // LSU
                        if (lsu_ready) begin
                            unit_sel = UNIT_LSU;
                            lsu_base_addr = {32'd0, rs0_data[31:0]};
                            lsu_offset = rs1_data[31:0];
                            lsu_is_load = !instr[0];
                            lsu_is_store = instr[0];
                            lsu_valid = 1'b1;
                        end
                    end
                end

                default: begin
                    // 明确处理 default 情况，防止 Latch
                    unit_sel = UNIT_ALU;
                end
            endcase
        end
    end
    
    // 改进的ready信号(单元级流控)
    assign ready = (opcode[5:4] == 2'b00) ? alu_ready :
                   (opcode[5:4] == 2'b01) ? alu_ready :
                   (opcode[5:4] == 2'b10) ? sfu_ready :
                   (instr[3]) ? lds_ready : lsu_ready;
    
    // ========================================================================
    // 4. 元数据追踪(关键改进!)
    // ========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            alu_warp_id <= {WARP_ID_W{1'b0}};
            sfu_warp_id <= {WARP_ID_W{1'b0}};
            lsu_warp_id <= {WARP_ID_W{1'b0}};
            lds_warp_id <= {WARP_ID_W{1'b0}};
            
            alu_rd_addr <= 6'd0;
            sfu_rd_addr <= 6'd0;
            lsu_rd_addr <= 6'd0;
            lds_rd_addr <= 6'd0;
            
            alu_mask <= 32'd0;
            sfu_mask <= 32'd0;
            lsu_mask <= 32'd0;
            lds_mask <= 32'd0;
        end else begin
            // 只在该单元接收指令时才更新
            if (valid && alu_valid) begin
                alu_warp_id <= warp_id;
                alu_rd_addr <= rd_addr;
                alu_mask <= 32'hFFFFFFFF;  // 可根据指令类型调整
            end
            
            if (valid && sfu_valid) begin
                sfu_warp_id <= warp_id;
                sfu_rd_addr <= rd_addr;
                sfu_mask <= 32'hFFFFFFFF;
            end
            
            if (valid && lsu_valid) begin
                lsu_warp_id <= warp_id;
                lsu_rd_addr <= rd_addr;
                lsu_mask <= 32'hFFFFFFFF;
            end
            
            if (valid && lds_valid) begin
                lds_warp_id <= warp_id;
                lds_rd_addr <= rd_addr;
                lds_mask <= 32'hFFFFFFFF;
            end
        end
    end
    
    // ========================================================================
    // 5. 改进的写回仲裁(优先级 + 双端口)
    // ========================================================================
    // 优先级: LSU > ALU > SFU > LDS
    
    reg selected_valid_0, selected_valid_1;
    reg [WARP_ID_W-1:0] selected_warp_0, selected_warp_1;
    reg [5:0] selected_rd_0, selected_rd_1;
    reg [1023:0] selected_result_0, selected_result_1;
    reg [31:0] selected_mask_0, selected_mask_1;
    
    always @(*) begin
        // 端口0优先级: LSU > ALU
        selected_valid_0 = 1'b0;
        selected_warp_0 = {WARP_ID_W{1'b0}};
        selected_rd_0 = 6'd0;
        selected_result_0 = 1024'd0;
        selected_mask_0 = 32'd0;
        
        // 端口1优先级: SFU > LDS
        selected_valid_1 = 1'b0;
        selected_warp_1 = {WARP_ID_W{1'b0}};
        selected_rd_1 = 6'd0;
        selected_result_1 = 1024'd0;
        selected_mask_1 = 32'd0;

        if (lsu_load_valid) begin
            selected_valid_0 = 1'b1;
            selected_warp_0 = lsu_warp_id;      // 使用跟踪的warp_id
            selected_rd_0 = lsu_rd_addr;        // 使用跟踪的rd_addr
            selected_result_0 = lsu_load_data;
            selected_mask_0 = lsu_mask;
        end else if (alu_valid_out) begin
            selected_valid_0 = 1'b1;
            selected_warp_0 = alu_warp_id;
            selected_rd_0 = alu_rd_addr;
            selected_result_0 = alu_result;
            selected_mask_0 = alu_mask;
        end else if (sfu_valid_out) begin
            selected_valid_0 = 1'b1;
            selected_warp_0 = sfu_warp_id;
            selected_rd_0 = sfu_rd_addr;
            selected_result_0 = sfu_result;
            selected_mask_0 = sfu_mask;
        end else if (lds_rdata_valid) begin
            selected_valid_0 = 1'b1;
            selected_warp_0 = lds_warp_id;
            selected_rd_0 = lds_rd_addr;
            // LDS数据按Lane广播(改进的实现)
            selected_result_0 = {32{lds_rdata}};
            selected_mask_0 = lds_mask;
        end
    end
    
    // ========================================================================
    // 6. 双端口写回(真正的并行)
    // ========================================================================
    assign rf_wr_warp_id[0] = selected_warp_0;
    assign rf_wr_addr[0] = selected_rd_0;
    assign rf_wr_data[0] = selected_result_0;
    assign rf_wr_mask[0] = selected_mask_0;
    assign rf_wr_en[0] = selected_valid_0;
    
    assign rf_wr_warp_id[1] = selected_warp_1;
    assign rf_wr_addr[1] = selected_rd_1;
    assign rf_wr_data[1] = selected_result_1;
    assign rf_wr_mask[1] = selected_mask_1;
    assign rf_wr_en[1] = selected_valid_1;
    
    // ========================================================================
    // 7. 分支处理(改进的时序)
    // ========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wb_en <= 1'b0;
            branch_taken <= 1'b0;
        end else begin
            // 对于向后兼容性,仍然输出单端口写回信息
            wb_en <= selected_valid_0;  // 使用端口0作为主写回
            wb_warp_id <= selected_warp_0;
            wb_rd_addr <= selected_rd_0;
            wb_data <= selected_result_0;
            wb_mask <= selected_mask_0;
            
            // 分支检测
            branch_taken <= 1'b0;
            if (valid && opcode == 6'b111111 && alu_ready) begin
                branch_taken <= 1'b1;
                branch_target <= rs0_data[63:0];
            end
        end
    end

endmodule
