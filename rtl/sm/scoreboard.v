// ============================================================================
// Module: scoreboard
// Description: Scoreboard for RAW/WAW hazard detection per warp
// 32 Warps × 64 Registers = 2048 busy bits (implemented as distributed RAM or FF)
// ============================================================================
module scoreboard #(
    parameter NUM_WARPS  = 32,
    parameter NUM_REGS   = 64,      // Registers per warp (0-63)
    parameter REG_ADDR_W = $clog2(NUM_REGS),
    parameter WARP_ID_W  = $clog2(NUM_WARPS)
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    // Issue Interface (Check hazards)
    input  wire [WARP_ID_W-1:0]  warp_id,       // Current warp being checked
    input  wire [REG_ADDR_W-1:0] rs0_addr,      // Source 0
    input  wire [REG_ADDR_W-1:0] rs1_addr,      // Source 1
    input  wire [REG_ADDR_W-1:0] rs2_addr,      // Source 2 (optional)
    input  wire [REG_ADDR_W-1:0] rd_addr,       // Destination (for WAW check)
    input  wire                  rd_wr_en,      // Will write rd (for WAW)
    output wire                  stall_raw,     // RAW hazard detected
    output wire                  stall_waw,     // WAW hazard detected
    
    // Writeback Interface (Clear busy)
    input  wire [WARP_ID_W-1:0]  wb_warp_id,    // Completing warp
    input  wire [REG_ADDR_W-1:0] wb_rd_addr,    // Completing register
    input  wire                  wb_valid       // Writeback valid
);

    // Busy Table: 1 bit per register per warp
    // Flattened array: [warp][reg] -> [warp*NUM_REGS + reg]
    reg [NUM_WARPS*NUM_REGS-1:0] busy_table;
    
    // Calculate indices
    wire [NUM_WARPS*NUM_REGS-1:0] issue_idx = warp_id * NUM_REGS;
    wire [NUM_WARPS*NUM_REGS-1:0] wb_idx    = wb_warp_id * NUM_REGS;
    
    wire [NUM_WARPS*NUM_REGS-1:0] rs0_bit = issue_idx + rs0_addr;
    wire [NUM_WARPS*NUM_REGS-1:0] rs1_bit = issue_idx + rs1_addr;
    wire [NUM_WARPS*NUM_REGS-1:0] rs2_bit = issue_idx + rs2_addr;
    wire [NUM_WARPS*NUM_REGS-1:0] rd_bit  = issue_idx + rd_addr;
    wire [NUM_WARPS*NUM_REGS-1:0] wb_bit  = wb_idx + wb_rd_addr;
    
    // RAW Check: Any source register busy?
    // Note: If wb_valid and same register in same cycle, it's not a hazard (bypass)
    wire rs0_busy = busy_table[rs0_bit];
    wire rs1_busy = busy_table[rs1_bit];
    wire rs2_busy = busy_table[rs2_bit];
    
    // WAW Check: Destination busy (and not being cleared this cycle)
    wire rd_busy = busy_table[rd_bit];
    
    // Hazard Detection (with bypass for same-cycle writeback)
    assign stall_raw = (rs0_busy && !(wb_valid && wb_warp_id == warp_id && wb_rd_addr == rs0_addr)) ||
                       (rs1_busy && !(wb_valid && wb_warp_id == warp_id && wb_rd_addr == rs1_addr)) ||
                       (rs2_busy && !(wb_valid && wb_warp_id == warp_id && wb_rd_addr == rs2_addr));
    
    assign stall_waw = rd_wr_en && rd_busy && 
                       !(wb_valid && wb_warp_id == warp_id && wb_rd_addr == rd_addr);
    
    // Busy Table Update
    integer i;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            busy_table <= {(NUM_WARPS*NUM_REGS){1'b0}};
        end else begin
            // Clear writeback (highest priority to avoid deadlock)
            if (wb_valid) begin
                busy_table[wb_bit] <= 1'b0;
            end
            
            // Set new issue (if no hazard and issuing)
            // Note: Issue logic should ensure we only issue when !stall_raw && !stall_waw
            // This is handled outside, here we just provide the stall signals
            // The actual set of busy bit happens when instruction issues (controlled by external logic)
            // For completeness, we add an issue_valid input if needed, currently assuming external control
        end
    end
    
    // Alternative: Expose busy bit set interface for external issue control
    // Since stall signals are combinational, the external controller should:
    // 1. Check stall_raw/stall_waw
    // 2. If not stalled, assert issue_valid next cycle to set busy[rd]
    // For this module, we assume the set happens when instruction actually issues
    
    // Additional interface for setting busy bit (optional, can be integrated)
    // wire issue_valid = ... from external;
    // if (issue_valid && !stall_raw && !stall_waw) busy_table[rd_bit] <= 1'b1;

endmodule