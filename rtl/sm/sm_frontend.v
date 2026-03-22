// ============================================================================
// Module: sm_frontend
// Description: SM Frontend with PC Management, I-Cache, Scheduler, Scoreboard
// ============================================================================
module sm_frontend #(
    parameter NUM_WARPS   = 32,
    parameter WARP_ID_W   = 5,
    parameter PC_W        = 64,
    parameter INST_W      = 32,
    parameter NUM_REGS    = 64,
    parameter REG_ADDR_W  = 6
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Task Launch (from GPC)
    input  wire [PC_W-1:0]           entry_pc,
    input  wire [WARP_ID_W-1:0]      start_warp_id,
    input  wire                      task_valid,
    output wire                      task_ready,
    
    // Instruction Memory (I-Cache or I-Buffer interface)
    output reg  [PC_W-1:0]           ifetch_addr,
    output reg                       ifetch_valid,
    input  wire [INST_W-1:0]         ifetch_data,
    input  wire                      ifetch_ready,
    
    // To Register File (Read Request)
    output wire [WARP_ID_W-1:0]      rf_rd_warp_id [0:3],
    output wire [REG_ADDR_W-1:0]     rf_rd_addr    [0:3],
    output wire                      rf_rd_en      [0:3],
    input  wire [1023:0]             rf_rd_data    [0:3],  // 32 lanes × 32bit
    
    // To Backend (Issue)
    output reg  [INST_W-1:0]         decoded_inst,
    output reg  [WARP_ID_W-1:0]      issue_warp_id,
    output reg  [1023:0]             issue_rs0_data,
    output reg  [1023:0]             issue_rs1_data,
    output reg                       issue_valid,
    input  wire                      issue_ready,
    
    // From Backend (Writeback/Control)
    input  wire [WARP_ID_W-1:0]      wb_warp_id,
    input  wire [REG_ADDR_W-1:0]     wb_rd_addr,
    input  wire                      wb_branch_taken,
    input  wire [PC_W-1:0]           wb_branch_target,
    input  wire                      wb_valid,
    
    // Scoreboard Interface（stall 由片内 scoreboard 驱动，供观测/上层可选连接）
    output wire [WARP_ID_W-1:0]      sc_warp_id,
    output wire [REG_ADDR_W-1:0]     sc_rs0, sc_rs1, sc_rs2, sc_rd,
    output wire                      sc_rd_wr_en,
    output wire                      sc_stall_raw,
    output wire                      sc_stall_waw,
    
    // Status
    output wire [NUM_WARPS-1:0]      warp_active  // Which warps are active
);

    // PC Management (per warp)
    reg [PC_W-1:0] pc_array [0:NUM_WARPS-1];
    reg [NUM_WARPS-1:0] active_mask;
    // 简化：所有 warp 视为可调度（未接 barrier/长延迟等待模型）
    wire [NUM_WARPS-1:0] warp_ready;
    assign warp_ready = {NUM_WARPS{1'b1}};
    
    // Scheduler
    wire [WARP_ID_W-1:0] selected_warp;
    wire                 schedule_valid;
    
    warp_scheduler #(
        .NUM_WARPS(NUM_WARPS)
    ) u_scheduler (
        .clk(clk),
        .rst_n(rst_n),
        .warp_valid(active_mask),
        .warp_ready(warp_ready),
        .stall(~issue_ready || sc_stall_raw || sc_stall_waw),  // Stall if backend busy or hazard
        .warp_id(selected_warp),
        .schedule_valid(schedule_valid)
    );
    
    // Scoreboard Instance
    scoreboard #(
        .NUM_WARPS(NUM_WARPS),
        .NUM_REGS(NUM_REGS)
    ) u_scoreboard (
        .clk(clk),
        .rst_n(rst_n),
        .warp_id(sc_warp_id),
        .rs0_addr(sc_rs0),
        .rs1_addr(sc_rs1),
        .rs2_addr(sc_rs2),
        .rd_addr(sc_rd),
        .rd_wr_en(sc_rd_wr_en),
        .stall_raw(sc_stall_raw),
        .stall_waw(sc_stall_waw),
        .issue_valid(issue_valid),
        .wb_warp_id(wb_warp_id),
        .wb_rd_addr(wb_rd_addr),
        .wb_valid(wb_valid)
    );
    
    // Instruction Fetch and Decode Pipeline
    reg [PC_W-1:0] fetch_pc;
    reg [WARP_ID_W-1:0] fetch_warp;
    reg fetch_valid;
    
    // Instruction Decode (Combinational)
    wire [REG_ADDR_W-1:0] dec_rs0 = ifetch_data[19:14];
    wire [REG_ADDR_W-1:0] dec_rs1 = ifetch_data[13:8];
    wire [REG_ADDR_W-1:0] dec_rd  = ifetch_data[7:2];
    wire [5:0] dec_opcode = ifetch_data[31:26];
    
    // Connect Scoreboard check
    assign sc_warp_id = selected_warp;
    assign sc_rs0 = dec_rs0;
    assign sc_rs1 = dec_rs1;
    assign sc_rs2 = 6'd0;  // Not used in this simplified ISA
    assign sc_rd  = dec_rd;
    assign sc_rd_wr_en = 1'b1;  // Assume all instructions write (simplified)
    
    // Register File Read Request (Tied to selected warp)
    assign rf_rd_warp_id[0] = selected_warp;
    assign rf_rd_warp_id[1] = selected_warp;
    assign rf_rd_addr[0] = dec_rs0;
    assign rf_rd_addr[1] = dec_rs1;
    assign rf_rd_en[0] = schedule_valid;
    assign rf_rd_en[1] = schedule_valid;
    // Ports 2,3 unused in this simplified model
    
    // Main Sequential Logic
    assign task_ready = 1'b1;  // Always ready to accept new task (simplified)
    
    integer w;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            active_mask <= {NUM_WARPS{1'b0}};
            ifetch_valid <= 1'b0;
            issue_valid <= 1'b0;
            for (w = 0; w < NUM_WARPS; w = w + 1) begin
                pc_array[w] <= {PC_W{1'b0}};
            end
        end else begin
            // Task Launch
            if (task_valid) begin
                pc_array[start_warp_id] <= entry_pc;
                active_mask[start_warp_id] <= 1'b1;
            end
            
            // Fetch Stage
            if (schedule_valid && ifetch_ready) begin
                ifetch_addr <= pc_array[selected_warp];
                ifetch_valid <= 1'b1;
                fetch_warp <= selected_warp;
            end else begin
                ifetch_valid <= 1'b0;
            end
            
            // Decode and Issue Stage
            if (ifetch_valid && !sc_stall_raw && !sc_stall_waw && issue_ready) begin
                decoded_inst <= ifetch_data;
                issue_warp_id <= fetch_warp;
                issue_rs0_data <= rf_rd_data[0];
                issue_rs1_data <= rf_rd_data[1];
                issue_valid <= 1'b1;
                
                // Advance PC
                pc_array[fetch_warp] <= pc_array[fetch_warp] + 4;
                
                // Branch handling (from writeback)
                if (wb_valid && wb_branch_taken) begin
                    pc_array[wb_warp_id] <= wb_branch_target;
                end
            end else begin
                issue_valid <= 1'b0;
            end
        end
    end
    
    assign warp_active = active_mask;

endmodule

