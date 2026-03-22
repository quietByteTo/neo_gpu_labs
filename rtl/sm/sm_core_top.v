// ============================================================================
// Module: sm_core_top
// Description: SM Top Level, integrates all sub-modules
// ============================================================================
module sm_core_top #(
    parameter NUM_WARPS  = 32,
    parameter NUM_LANES  = 32,
    parameter WARP_ID_W  = 5
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Task Interface (from GPC)
    input  wire [63:0]               entry_pc,
    input  wire [WARP_ID_W-1:0]      start_warp_id,
    input  wire [4:0]                num_warps,      // How many warps to activate
    input  wire                      task_valid,
    output wire                      task_ready,
    output wire                      task_done,      // All warps completed
    
    // L1 Interface (to GPC Router)
    output wire [63:0]               l1_req_addr,
    output wire [127:0]              l1_req_data,
    output wire                      l1_req_wr_en,
    output wire                      l1_req_valid,
    input  wire                      l1_req_ready,
    input  wire [127:0]              l1_resp_data,
    input  wire                      l1_resp_valid,
    
    // Debug/Performance
    output wire [31:0]               perf_inst_count,
    output wire [NUM_WARPS-1:0]      warp_status     // Active/Idle per warp
);

    // Internal Signals
    
    // Register File connections
    wire [WARP_ID_W-1:0] rf_rd_wid [0:3];
    wire [5:0]           rf_rd_addr [0:3];
    wire                 rf_rd_en [0:3];
    wire [1023:0]        rf_rd_data [0:3];
    wire [WARP_ID_W-1:0] rf_wr_wid [0:1];
    wire [5:0]           rf_wr_addr [0:1];
    wire [1023:0]        rf_wr_data [0:1];
    wire [31:0]          rf_wr_mask [0:1];
    wire                 rf_wr_en [0:1];
    
    // Frontend ↔ Backend
    wire [31:0]          dec_instr;
    wire [WARP_ID_W-1:0] issue_wid;
    wire [1023:0]        issue_rs0, issue_rs1;
    wire                 issue_valid;
    wire                 issue_ready;
    wire [WARP_ID_W-1:0] wb_wid;
    wire [5:0]           wb_rd_addr;
    wire [1023:0]        wb_data;
    wire [31:0]          wb_mask;
    wire                 wb_en;
    wire                 wb_branch;
    wire [63:0]          wb_branch_target;
    wire                 wb_valid;
    
    assign wb_valid = wb_en;
    
    // Scoreboard
    wire [WARP_ID_W-1:0] sc_wid;
    wire [5:0]           sc_rs0, sc_rs1, sc_rs2, sc_rd;
    wire                 sc_rd_wr_en;
    wire                 sc_stall_raw, sc_stall_waw;
    
    // Execution Units
    wire [31:0]          alu_instr;
    wire [1023:0]        alu_src_a, alu_src_b, alu_src_c;
    wire                 alu_valid, alu_ready, alu_valid_out;
    wire [1023:0]        alu_result;
    
    wire [2:0]           sfu_op;
    wire [1023:0]        sfu_src;
    wire                 sfu_valid, sfu_ready, sfu_valid_out;
    wire [1023:0]        sfu_result;
    
    // LSU
    wire [63:0]          lsu_base;
    wire [31:0]          lsu_offset;
    wire                 lsu_is_load, lsu_is_store, lsu_valid, lsu_ready;
    wire [1023:0]        lsu_load_data;
    wire                 lsu_load_valid;
    
    // LDS
    wire [15:0]          lds_addr;
    wire [31:0]          lds_data;
    wire                 lds_wr_en, lds_valid, lds_ready;
    wire [31:0]          lds_rdata;
    wire                 lds_rdata_valid;
    
    wire [15:0]          lds_addr_lane   [0:NUM_LANES-1];
    wire [31:0]          lds_wdata_lane  [0:NUM_LANES-1];
    wire                 lds_wr_en_lane  [0:NUM_LANES-1];
    wire                 lds_valid_lane  [0:NUM_LANES-1];
    wire                 lds_ready_lane  [0:NUM_LANES-1];
    wire [31:0]          lds_rdata_lane  [0:NUM_LANES-1];
    wire                 unused_bank_conflict;
    wire [63:0]          ifetch_addr_dbg;
    wire                 ifetch_valid_dbg;
    wire [WARP_ID_W-1:0] lsu_resp_warp_id;
    wire                 lds_conflict_det;
    wire [4:0]           lds_conflict_cnt;
    
    genvar               gi_lds;
    generate
        for (gi_lds = 0; gi_lds < NUM_LANES; gi_lds = gi_lds + 1) begin : gen_lds_lane
            assign lds_addr_lane[gi_lds]  = lds_addr;
            assign lds_wdata_lane[gi_lds] = lds_data;
            assign lds_wr_en_lane[gi_lds] = lds_wr_en;
            assign lds_valid_lane[gi_lds] = lds_valid;
        end
    endgenerate
    
    assign lds_rdata    = lds_rdata_lane[0];
    // 不向 backend 反馈 lds_ready_lane，避免 LDS 组合逻辑与 sm_backend 形成环路（Verilator UNOPTFLAT）。
    // 集成时可改为寄存一拍或显式反压协议。
    assign lds_ready    = 1'b1;
    
    // Instantiate Register File
    register_file u_regfile (
        .clk(clk),
        .rst_n(rst_n),
        .rd_warp_id(rf_rd_wid),
        .rd_addr(rf_rd_addr),
        .rd_en(rf_rd_en),
        .rd_data(rf_rd_data),
        .wr_warp_id(rf_wr_wid),
        .wr_addr(rf_wr_addr),
        .wr_data(rf_wr_data),
        .wr_mask(rf_wr_mask),
        .wr_en(rf_wr_en),
        .bank_conflict(unused_bank_conflict)
    );
    
    // Instantiate Frontend
    sm_frontend u_frontend (
        .clk(clk),
        .rst_n(rst_n),
        .entry_pc(entry_pc),
        .start_warp_id(start_warp_id),
        .task_valid(task_valid),
        .task_ready(task_ready),
        .ifetch_addr(ifetch_addr_dbg),
        .ifetch_valid(ifetch_valid_dbg),
        .ifetch_data(32'd0),  // Placeholder
        .ifetch_ready(1'b1),
        .rf_rd_warp_id(rf_rd_wid),
        .rf_rd_addr(rf_rd_addr),
        .rf_rd_en(rf_rd_en),
        .rf_rd_data(rf_rd_data),
        .decoded_inst(dec_instr),
        .issue_warp_id(issue_wid),
        .issue_rs0_data(issue_rs0),
        .issue_rs1_data(issue_rs1),
        .issue_valid(issue_valid),
        .issue_ready(issue_ready),
        .wb_warp_id(wb_wid),
        .wb_rd_addr(wb_rd_addr),
        .wb_branch_taken(wb_branch),
        .wb_branch_target(wb_branch_target),
        .wb_valid(wb_valid),
        .sc_warp_id(sc_wid),
        .sc_rs0(sc_rs0),
        .sc_rs1(sc_rs1),
        .sc_rs2(sc_rs2),
        .sc_rd(sc_rd),
        .sc_rd_wr_en(sc_rd_wr_en),
        .sc_stall_raw(sc_stall_raw),
        .sc_stall_waw(sc_stall_waw),
        .warp_active(warp_status)
    );
    
    // Instantiate Backend
    sm_backend u_backend (
        .clk(clk),
        .rst_n(rst_n),
        .instr(dec_instr),
        .warp_id(issue_wid),
        .rs0_data(issue_rs0),
        .rs1_data(issue_rs1),
        .valid(issue_valid),
        .ready(issue_ready),
        .wb_warp_id(wb_wid),
        .wb_rd_addr(wb_rd_addr),
        .wb_data(wb_data),
        .wb_mask(wb_mask),
        .wb_en(wb_en),
        .branch_taken(wb_branch),
        .branch_target(wb_branch_target),
        .rf_wr_warp_id(rf_wr_wid),
        .rf_wr_addr(rf_wr_addr),
        .rf_wr_data(rf_wr_data),
        .rf_wr_mask(rf_wr_mask),
        .rf_wr_en(rf_wr_en),
        .alu_instr(alu_instr),
        .alu_src_a(alu_src_a),
        .alu_src_b(alu_src_b),
        .alu_src_c(alu_src_c),
        .alu_valid(alu_valid),
        .alu_ready(alu_ready),
        .alu_result(alu_result),
        .alu_valid_out(alu_valid_out),
        .sfu_op(sfu_op),
        .sfu_src(sfu_src),
        .sfu_valid(sfu_valid),
        .sfu_ready(sfu_ready),
        .sfu_result(sfu_result),
        .sfu_valid_out(sfu_valid_out),
        .lsu_base_addr(lsu_base),
        .lsu_offset(lsu_offset),
        .lsu_is_load(lsu_is_load),
        .lsu_is_store(lsu_is_store),
        .lsu_valid(lsu_valid),
        .lsu_ready(lsu_ready),
        .lsu_load_data(lsu_load_data),
        .lsu_load_valid(lsu_load_valid),
        .lds_addr(lds_addr),
        .lds_data(lds_data),
        .lds_wr_en(lds_wr_en),
        .lds_valid(lds_valid),
        .lds_ready(lds_ready),
        .lds_rdata(lds_rdata),
        .lds_rdata_valid(lds_rdata_valid)
    );
    
    // Instantiate Execution Units
    alu_pipeline u_alu (
        .clk(clk),
        .rst_n(rst_n),
        .valid_in(alu_valid),
        .ready_in(alu_ready),
        .instr(alu_instr),
        .src_a(alu_src_a),
        .src_b(alu_src_b),
        .src_c(alu_src_c),
        .result(alu_result),
        .valid_out(alu_valid_out)
    );
    
    sfu u_sfu (
        .clk(clk),
        .rst_n(rst_n),
        .sfu_op(sfu_op),
        .src(sfu_src),
        .valid_in(sfu_valid),
        .ready(sfu_ready),
        .result(sfu_result),
        .valid_out(sfu_valid_out)
    );
    
    // Instantiate LSU (simplified connections)
    lsu u_lsu (
        .clk(clk),
        .rst_n(rst_n),
        .warp_id(issue_wid),
        .base_addr(lsu_base),
        .offset_valid(32'hFFFFFFFF),  // All lanes active
        .offset({32{lsu_offset}}),    // Broadcast offset (simplified)
        .store_data({32{32'h0}}),
        .is_load(lsu_is_load),
        .is_store(lsu_is_store),
        .is_lds(1'b0),  // LSU handles global, LDS is separate
        .valid(lsu_valid),
        .ready(lsu_ready),
        .l1_addr(l1_req_addr),
        .l1_wdata(l1_req_data),
        .l1_wr_en(l1_req_wr_en),
        .l1_valid(l1_req_valid),
        .l1_ready(l1_req_ready),
        .l1_rdata(l1_resp_data),
        .l1_rdata_valid(l1_resp_valid),
        // LDS connections not used here
        .load_result(lsu_load_data),
        .load_valid(lsu_load_valid),
        .resp_warp_id(lsu_resp_warp_id)
    );
    
    // Instantiate LDS
    lds_unit u_lds (
        .clk(clk),
        .rst_n(rst_n),
        .lds_addr(lds_addr_lane),
        .lds_wdata(lds_wdata_lane),
        .lds_wr_en(lds_wr_en_lane),
        .lds_valid(lds_valid_lane),
        .lds_ready(lds_ready_lane),
        .lds_rdata(lds_rdata_lane),
        .lds_rdata_valid(lds_rdata_valid),
        .atomic_op(3'd0),
        .atomic_en(1'b0),
        .conflict_detected(lds_conflict_det),
        .conflict_count(lds_conflict_cnt)
    );
    
    // Performance Counter
    reg [31:0] inst_counter;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            inst_counter <= 32'd0;
        end else if (issue_valid && issue_ready) begin
            inst_counter <= inst_counter + 1'b1;
        end
    end
    assign perf_inst_count = inst_counter;
    
    // Task Done Detection (all warps inactive)
    assign task_done = ~|warp_status;

endmodule

