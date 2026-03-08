// scoreboard.v - 修复乘法常数位宽
module scoreboard #(
    parameter NUM_WARPS  = 32,
    parameter NUM_REGS   = 64,
    parameter REG_ADDR_W = $clog2(NUM_REGS),
    parameter WARP_ID_W  = $clog2(NUM_WARPS)
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    input  wire [WARP_ID_W-1:0]  warp_id,
    input  wire [REG_ADDR_W-1:0] rs0_addr,
    input  wire [REG_ADDR_W-1:0] rs1_addr,
    input  wire [REG_ADDR_W-1:0] rs2_addr,
    input  wire [REG_ADDR_W-1:0] rd_addr,
    input  wire                  rd_wr_en,
    output wire                  stall_raw,
    output wire                  stall_waw,
    
    input  wire                  issue_valid,
    
    input  wire [WARP_ID_W-1:0]  wb_warp_id,
    input  wire [REG_ADDR_W-1:0] wb_rd_addr,
    input  wire                  wb_valid
);

    reg [NUM_WARPS*NUM_REGS-1:0] busy_table;
    
    // 修复：使用正确的常数位宽
    wire [10:0] issue_idx = warp_id * 7'd64;  // 64 需要 7 位
    wire [10:0] wb_idx    = wb_warp_id * 7'd64;
    
    wire [10:0] rs0_idx = issue_idx + {5'b0, rs0_addr};
    wire [10:0] rs1_idx = issue_idx + {5'b0, rs1_addr};
    wire [10:0] rs2_idx = issue_idx + {5'b0, rs2_addr};
    wire [10:0] rd_idx  = issue_idx + {5'b0, rd_addr};
    wire [10:0] wb_idx2 = wb_idx + {5'b0, wb_rd_addr};
    
    wire rs0_busy = busy_table[rs0_idx];
    wire rs1_busy = busy_table[rs1_idx];
    wire rs2_busy = busy_table[rs2_idx];
    wire rd_busy = busy_table[rd_idx];
    
    wire rs0_bypass = wb_valid && (wb_warp_id == warp_id) && (wb_rd_addr == rs0_addr);
    wire rs1_bypass = wb_valid && (wb_warp_id == warp_id) && (wb_rd_addr == rs1_addr);
    wire rs2_bypass = wb_valid && (wb_warp_id == warp_id) && (wb_rd_addr == rs2_addr);
    wire rd_bypass  = wb_valid && (wb_warp_id == warp_id) && (wb_rd_addr == rd_addr);
    
    assign stall_raw = (rs0_busy && !rs0_bypass) || 
                       (rs1_busy && !rs1_bypass) || 
                       (rs2_busy && !rs2_bypass);
    
    assign stall_waw = rd_wr_en && rd_busy && !rd_bypass;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            busy_table <= {(NUM_WARPS*NUM_REGS){1'b0}};
        end else begin
            if (wb_valid) begin
                busy_table[wb_idx2] <= 1'b0;
            end
            
            if (issue_valid && !stall_raw && !stall_waw && rd_wr_en) begin
                busy_table[rd_idx] <= 1'b1;
            end
        end
    end

endmodule

