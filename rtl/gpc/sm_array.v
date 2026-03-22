// ============================================================================
// Module: sm_array
// Description: Array of SM cores with per-SM task queues and L1 request arbitration
// ============================================================================
module sm_array #(
    parameter NUM_SM      = 4,          // Number of SMs in this array
    parameter SM_ID_W     = $clog2(NUM_SM),
    parameter NUM_WARPS   = 32,
    parameter WARP_ID_W   = 5,
    parameter L1_DATA_W   = 128,
    parameter ADDR_W      = 64
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Task Distribution Interface (from GPC Cluster)
    input  wire [63:0]               task_entry_pc,
    input  wire [WARP_ID_W-1:0]      task_warp_id,
    input  wire [4:0]                task_num_warps,    // Number of warps for this task
    input  wire [SM_ID_W-1:0]        task_sm_id,        // Target SM ID
    input  wire                      task_valid,
    output wire                      task_ready,
    
    // Aggregated L1 Interface (to GPC Router)
    // Requests from all SMs arbitrated to Router
    output wire [ADDR_W-1:0]         l1_req_addr,
    output wire [L1_DATA_W-1:0]      l1_req_data,
    output wire                      l1_req_wr_en,
    output wire [15:0]               l1_req_be,         // Byte enable
    output wire                      l1_req_valid,
    input  wire                      l1_req_ready,
    
    // Responses from Router back to SMs
    input  wire [L1_DATA_W-1:0]      l1_resp_data,
    input  wire                      l1_resp_valid,
    output wire                      l1_resp_ready,
    input  wire [SM_ID_W-1:0]        l1_resp_sm_id,     // Which SM this response is for
    
    // Status
    output wire [NUM_SM-1:0]         sm_idle,           // Per-SM idle status
    output wire [NUM_SM-1:0]         sm_done            // Per-SM task completion
);

    // Internal Signals
    wire [63:0]               sm_entry_pc    [0:NUM_SM-1];
    wire [WARP_ID_W-1:0]      sm_warp_id     [0:NUM_SM-1];
    wire [4:0]                sm_num_warps   [0:NUM_SM-1];
    wire                      sm_task_valid  [0:NUM_SM-1];
    wire                      sm_task_ready  [0:NUM_SM-1];
    
    wire [ADDR_W-1:0]         sm_l1_addr     [0:NUM_SM-1];
    wire [L1_DATA_W-1:0]      sm_l1_wdata    [0:NUM_SM-1];
    wire                      sm_l1_wr_en    [0:NUM_SM-1];
    wire                      sm_l1_valid    [0:NUM_SM-1];
    wire                      sm_l1_ready    [0:NUM_SM-1];
    wire [L1_DATA_W-1:0]      sm_l1_rdata    [0:NUM_SM-1];
    wire                      sm_l1_rvalid   [0:NUM_SM-1];

    // 每路任务 FIFO 的 full（与 sm_core 的 task_ready 分离，避免与 .full() 多驱动同一 net）
    wire [NUM_SM-1:0]         fifo_full;
    
    // Task Distribution Demux (based on task_sm_id)：GPC 仅当目标 SM 的 FIFO 未满时可写
    assign task_ready = !fifo_full[task_sm_id];

    // sm_core_top 无响应反压口：阵列始终可接收 Router 返回的读数据拍
    assign l1_resp_ready = 1'b1;
    
    genvar i;
    generate
        for (i = 0; i < NUM_SM; i = i + 1) begin : sm_inst
            // Task FIFO per SM (decouples GPC from SM timing)
            wire [63:0]               fifo_pc;
            wire [WARP_ID_W-1:0]      fifo_warp;
            wire [4:0]                fifo_num_warps;
            wire                      fifo_empty;
            wire                      fifo_ready_out;
            // DEPTH=4 -> PTR_W=$clog2(4)=2，count 宽 PTR_W+1=3
            wire [2:0]                fifo_count_unused;
            
            sync_fifo #(
                .DATA_W(64 + WARP_ID_W + 5),  // PC + warp_id + num_warps
                .DEPTH(4)                     // Small FIFO, 4 entries enough
            ) u_task_fifo (
                .clk(clk),
                .rst_n(rst_n),
                .wr_en(task_valid && (task_sm_id == i)),
                .wdata({task_entry_pc, task_warp_id, task_num_warps}),
                .full(fifo_full[i]),
                .rd_en(fifo_ready_out),
                .rdata({fifo_pc, fifo_warp, fifo_num_warps}),
                .empty(fifo_empty),
                .count(fifo_count_unused)
            );
            
            // 有数据可读：!empty（勿用 .empty(!wire) 再 assign wire，会与隐式驱动冲突）
            assign sm_task_valid[i] = !fifo_empty;
            assign fifo_ready_out = sm_task_ready[i];  // SM accepts when ready

            wire [31:0]               perf_inst_unused;
            wire [NUM_WARPS-1:0]      warp_status_unused;
            
            // SM Core Instance
            sm_core_top #(
                .NUM_WARPS(NUM_WARPS),
                .NUM_LANES(32)
            ) u_sm_core (
                .clk(clk),
                .rst_n(rst_n),
                .entry_pc(fifo_pc),
                .start_warp_id(fifo_warp),
                .num_warps(fifo_num_warps),
                .task_valid(sm_task_valid[i]),
                .task_ready(sm_task_ready[i]),
                .task_done(sm_done[i]),
                .l1_req_addr(sm_l1_addr[i]),
                .l1_req_data(sm_l1_wdata[i]),
                .l1_req_wr_en(sm_l1_wr_en[i]),
                .l1_req_valid(sm_l1_valid[i]),
                .l1_req_ready(sm_l1_ready[i]),
                .l1_resp_data(sm_l1_rdata[i]),
                .l1_resp_valid(sm_l1_rvalid[i]),
                .perf_inst_count(perf_inst_unused),
                .warp_status(warp_status_unused)
            );
            
            assign sm_idle[i] = !sm_task_valid[i];  // Simplified idle detection
        end
    endgenerate
    
    // L1 Request Arbitration (Round Robin among NUM_SM SMs)
    // Need to arbitrate both requests and route responses
    
    wire [NUM_SM-1:0] sm_req_vec;
    wire [NUM_SM-1:0] sm_grant_vec;
    
    generate
        for (i = 0; i < NUM_SM; i = i + 1) begin : req_vec_gen
            assign sm_req_vec[i] = sm_l1_valid[i];
        end
    endgenerate
    
    rr_arbiter #(
        .N(NUM_SM)
    ) u_req_arbiter (
        .clk(clk),
        .rst_n(rst_n),
        .req(sm_req_vec),
        .grant(sm_grant_vec),
        .advance(l1_req_valid && l1_req_ready)  // Advance when transaction accepted
    );
    
    // Mux selected SM's request to output
    reg [ADDR_W-1:0]    muxed_addr;
    reg [L1_DATA_W-1:0] muxed_data;
    reg                 muxed_wr_en;
    reg                 muxed_valid;
    reg [SM_ID_W-1:0]   selected_sm;
    reg [15:0]          muxed_be;  // Byte enable, default all enabled
    
    integer sel;
    always @(*) begin
        muxed_addr = {ADDR_W{1'b0}};
        muxed_data = {L1_DATA_W{1'b0}};
        muxed_wr_en = 1'b0;
        muxed_valid = 1'b0;
        selected_sm = {SM_ID_W{1'b0}};
        muxed_be = 16'hFFFF;  // Full line enable by default
        
        for (sel = 0; sel < NUM_SM; sel = sel + 1) begin
            if (sm_grant_vec[sel]) begin
                muxed_addr = sm_l1_addr[sel];
                muxed_data = sm_l1_wdata[sel];
                muxed_wr_en = sm_l1_wr_en[sel];
                muxed_valid = sm_l1_valid[sel];
                selected_sm = sel[SM_ID_W-1:0];
            end
        end
    end
    
    assign l1_req_addr = muxed_addr;
    assign l1_req_data = muxed_data;
    assign l1_req_wr_en = muxed_wr_en;
    assign l1_req_be = muxed_be;
    assign l1_req_valid = muxed_valid;
    
    // Response Routing (Demux from Router to correct SM)
    // Responses come back with l1_resp_sm_id indicating target SM
    generate
        for (i = 0; i < NUM_SM; i = i + 1) begin : resp_route
            assign sm_l1_rdata[i] = l1_resp_data;
            assign sm_l1_rvalid[i] = l1_resp_valid && (l1_resp_sm_id == i);
            assign sm_l1_ready[i] = (l1_resp_sm_id == i) ? l1_resp_ready : 1'b0;
        end
    endgenerate
    
    // Track outstanding transactions for flow control (simplified)
    // In full implementation, need credit-based flow control per SM

endmodule

