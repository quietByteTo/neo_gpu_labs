// ============================================================================
// Module: sram_2p
// Description: Pseudo-dual-port SRAM (Port A: R/W, Port B: Read-only)
//              Supports write-to-read bypass on collision
// ============================================================================
module sram_2p #(
    parameter ADDR_W = 10,
    parameter DATA_W = 32,
    parameter DEPTH  = 1024
) (
    input  wire              clk,
    input  wire              rst_n,
    
    // Port A: Read/Write
    input  wire              a_ce,
    input  wire              a_wr_en,
    input  wire [ADDR_W-1:0] a_addr,
    input  wire [DATA_W-1:0] a_wdata,
    output reg  [DATA_W-1:0] a_rdata,
    
    // Port B: Read-only
    input  wire              b_ce,
    input  wire [ADDR_W-1:0] b_addr,
    output reg  [DATA_W-1:0] b_rdata
);

    reg [DATA_W-1:0] mem [0:DEPTH-1];
    integer i;
    
    // Port A Logic (R/W)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_rdata <= {DATA_W{1'b0}};
        end else if (a_ce) begin
            if (a_wr_en) begin
                mem[a_addr] <= a_wdata;
            end
            a_rdata <= mem[a_addr];
        end
    end
    
    // Port B Logic (Read-only) with Bypass
    // If A writes and B reads same address in same cycle, B gets new data (bypass)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            b_rdata <= {DATA_W{1'b0}};
        end else if (b_ce) begin
            if (a_wr_en && a_ce && (a_addr == b_addr)) begin
                b_rdata <= a_wdata;     // Bypass: forward write data to read
            end else begin
                b_rdata <= mem[b_addr]; // Normal read
            end
        end
    end
    
    initial begin
        for (i = 0; i < DEPTH; i = i + 1) begin
            mem[i] = {DATA_W{1'b0}};
        end
    end

endmodule