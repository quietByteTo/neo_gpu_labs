// ============================================================================
// Module: sram_1p
// Description: Single-port SRAM, Read-First behavior, 1-cycle read latency
// ============================================================================
module sram_1p #(
    parameter ADDR_W = 10,              // Address width
    parameter DATA_W = 32,              // Data width
    parameter DEPTH  = 1024             // Depth (default 2^ADDR_W)
) (
    input  wire              clk,
    input  wire              rst_n,     // Async reset, active low
    
    // Control
    input  wire              ce,        // Chip Enable
    input  wire              wr_en,     // Write Enable (1=write, 0=read)
    
    // Interface
    input  wire [ADDR_W-1:0] addr,
    input  wire [DATA_W-1:0] wdata,
    output reg  [DATA_W-1:0] rdata
);

    // Memory array
    reg [DATA_W-1:0] mem [0:DEPTH-1];
    
    integer i;
    
    // Sequential logic: Read-First (read old data, then write)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rdata <= {DATA_W{1'b0}};
        end else if (ce) begin
            // Read-first: 先读，后写
            rdata <= mem[addr];
            if (wr_en) begin
                mem[addr] <= wdata;
            end
        end
    end

endmodule
