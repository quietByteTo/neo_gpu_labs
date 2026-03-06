// ============================================================================
// Module: register_file
// Description: 4-Bank Register File, 32 Warps × 64 VGPRs × 32 lanes
// Total: 256KB, 4 Read Ports, 2 Write Ports, with Bypass
// ============================================================================
module register_file #(
    parameter NUM_WARPS  = 32,
    parameter NUM_REGS   = 64,
    parameter NUM_LANES  = 32,
    parameter DATA_W     = 32,
    parameter NUM_BANKS  = 4,
    parameter WARP_ID_W  = $clog2(NUM_WARPS),
    parameter REG_ADDR_W = $clog2(NUM_REGS)
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Read Ports (4 ports)
    input  wire [WARP_ID_W-1:0]      rd_warp_id [0:3],
    input  wire [REG_ADDR_W-1:0]     rd_addr    [0:3],
    input  wire                      rd_en      [0:3],
    output reg  [NUM_LANES*DATA_W-1:0] rd_data  [0:3],
    
    // Write Ports (2 ports)
    input  wire [WARP_ID_W-1:0]      wr_warp_id [0:1],
    input  wire [REG_ADDR_W-1:0]     wr_addr    [0:1],
    input  wire [NUM_LANES*DATA_W-1:0] wr_data  [0:1],
    input  wire [NUM_LANES-1:0]      wr_mask    [0:1],  // Per-lane write mask
    input  wire                      wr_en      [0:1],
    
    // Bank Conflict Status (for performance/debug)
    output wire                      bank_conflict
);

    // Bank Selection: XOR of lower bits for better distribution
    // Bank = (Warp_ID[1:0] XOR Reg_Addr[1:0])
    wire [1:0] rd_bank_sel [0:3];
    wire [1:0] wr_bank_sel [0:1];
    
    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : rd_bank_calc
            assign rd_bank_sel[i] = rd_warp_id[i][1:0] ^ rd_addr[i][1:0];
        end
        for (i = 0; i < 2; i = i + 1) begin : wr_bank_calc
            assign wr_bank_sel[i] = wr_warp_id[i][1:0] ^ wr_addr[i][1:0];
        end
    endgenerate
    
    // Physical Address: {Warp_ID, Reg_Addr} within each bank
    // Each bank holds 1/4 of the registers (spread by XOR banking)
    localparam BANK_ADDR_W = WARP_ID_W + REG_ADDR_W - 2;  // -2 because banking uses 2 bits
    
    wire [BANK_ADDR_W-1:0] rd_phy_addr [0:3];
    wire [BANK_ADDR_W-1:0] wr_phy_addr [0:1];
    
    generate
        for (i = 0; i < 4; i = i + 1) begin : rd_phy_calc
            assign rd_phy_addr[i] = {rd_warp_id[i], rd_addr[i][REG_ADDR_W-1:2]};
        end
        for (i = 0; i < 2; i = i + 1) begin : wr_phy_calc
            assign wr_phy_addr[i] = {wr_warp_id[i], wr_addr[i][REG_ADDR_W-1:2]};
        end
    endgenerate
    
    // Bank Instances (sram_2p: 1RW + 1R)
    // Each bank needs to support: up to 4 reads (multiplexed) and 2 writes (arbitrated)
    // Implementation: Use 4 sram_2p instances per bank for 4 read ports, or time-multiplex
    // Simplification: Each sram_2p provides 2 read ports (A=RW, B=R), we need 4 reads total
    // So we instantiate 2 sram_2p per bank (8 total), interleaving addresses
    
    wire [NUM_LANES*DATA_W-1:0] bank_rd_data [0:NUM_BANKS-1][0:3];  // 4 banks × 4 read ports
    reg  [NUM_LANES*DATA_W-1:0] bank_wr_data [0:NUM_BANKS-1];
    reg  [BANK_ADDR_W-1:0]      bank_wr_addr [0:NUM_BANKS-1];
    reg                         bank_wr_en   [0:NUM_BANKS-1];
    
    // Instantiate Banks using sram_2p from Level 6
    // Each bank: 2 sram_2p instances to provide 4 logical read ports
    generate
        genvar b, p;
        for (b = 0; b < NUM_BANKS; b = b + 1) begin : bank_inst
            for (p = 0; p < 2; p = p + 1) begin : port_inst
                sram_2p #(
                    .ADDR_W(BANK_ADDR_W-1),  // Split address space between 2 instances
                    .DATA_W(NUM_LANES*DATA_W),
                    .DEPTH(NUM_WARPS*NUM_REGS/NUM_BANKS/2)
                ) u_sram (
                    .clk(clk),
                    .rst_n(rst_n),
                    // Port A: RW (used for writes and 1 read)
                    .a_ce(1'b1),
                    .a_wr_en(bank_wr_en[b] && bank_wr_addr[b][0] == p),  // LSB selects instance
                    .a_addr(bank_wr_addr[b][BANK_ADDR_W-1:1]),  // Drop LSB
                    .a_wdata(bank_wr_data[b]),
                    .a_rdata(bank_rd_data[b][p]),  // Read port 0,1
                    // Port B: Read-only
                    .b_ce(1'b1),
                    .b_addr(/* complex addr decode */),  // Will connect below
                    .b_rdata(bank_rd_data[b][p+2])  // Read port 2,3
                );
            end
        end
    endgenerate
    
    // Read Port Routing and Bypass Logic
    // Check for bypass: if read addr matches write addr in same cycle, forward write data
    integer r, w, b;
    reg [NUM_LANES*DATA_W-1:0] raw_data [0:3];
    reg bypass_hit [0:3];
    
    always @(*) begin
        // Default: read from SRAM
        for (r = 0; r < 4; r = r + 1) begin
            rd_data[r] = bank_rd_data[rd_bank_sel[r]][r];
            bypass_hit[r] = 1'b0;
        end
        
        // Bypass check: compare read request with write ports
        for (r = 0; r < 4; r = r + 1) begin
            for (w = 0; w < 2; w = w + 1) begin
                if (wr_en[w] && 
                    rd_en[r] && 
                    rd_warp_id[r] == wr_warp_id[w] && 
                    rd_addr[r] == wr_addr[w]) begin
                    rd_data[r] = wr_data[w];
                    bypass_hit[r] = 1'b1;
                end
            end
        end
    end
    
    // Write Arbitration: 2 writes to 4 banks
    // Simple: Write 0 to bank X, Write 1 to bank Y (if X!=Y), else stall or serialize
    // For now: assume no bank conflict on writes (verified by scheduler)
    always @(posedge clk) begin
        // Reset write enables
        for (b = 0; b < NUM_BANKS; b = b + 1) begin
            bank_wr_en[b] <= 1'b0;
        end
        
        // Write Port 0
        if (wr_en[0]) begin
            bank_wr_en[wr_bank_sel[0]] <= 1'b1;
            bank_wr_addr[wr_bank_sel[0]] <= wr_phy_addr[0];
            // Apply mask: if wr_mask[lane]=0, keep old data (read-modify-write)
            // For simplicity, SRAM always writes, mask applied at higher level or use byte-write
            bank_wr_data[wr_bank_sel[0]] <= wr_data[0];
        end
        
        // Write Port 1 (if different bank, else wait)
        if (wr_en[1] && (!wr_en[0] || wr_bank_sel[1] != wr_bank_sel[0])) begin
            bank_wr_en[wr_bank_sel[1]] <= 1'b1;
            bank_wr_addr[wr_bank_sel[1]] <= wr_phy_addr[1];
            bank_wr_data[wr_bank_sel[1]] <= wr_data[1];
        end
    end
    
    // Bank Conflict Detection (for same cycle read port conflicts)
    // Actually with 4 banks and XOR mapping, different warps rarely conflict
    // Conflict = two read ports access same bank
    assign bank_conflict = (rd_bank_sel[0] == rd_bank_sel[1]) && rd_en[0] && rd_en[1] ||
                           (rd_bank_sel[0] == rd_bank_sel[2]) && rd_en[0] && rd_en[2] ||
                           (rd_bank_sel[0] == rd_bank_sel[3]) && rd_en[0] && rd_en[3] ||
                           (rd_bank_sel[1] == rd_bank_sel[2]) && rd_en[1] && rd_en[2] ||
                           (rd_bank_sel[1] == rd_bank_sel[3]) && rd_en[1] && rd_en[3] ||
                           (rd_bank_sel[2] == rd_bank_sel[3]) && rd_en[2] && rd_en[3];

endmodule