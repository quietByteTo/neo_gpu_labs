// ============================================================================
// Module: sync_fifo
// Description: Synchronous FIFO with binary pointers, registered read output
// ============================================================================
module sync_fifo #(
    parameter DATA_W   = 32,
    parameter DEPTH    = 16,            // Must be power of 2
    parameter PTR_W    = $clog2(DEPTH)
) (
    input  wire              clk,
    input  wire              rst_n,
    
    // Write Interface
    input  wire              wr_en,
    input  wire [DATA_W-1:0] wdata,
    output wire              full,
    
    // Read Interface
    input  wire              rd_en,
    output reg  [DATA_W-1:0] rdata,
    output wire              empty,
    
    // Status
    output reg  [PTR_W:0]    count      // Current data count
);

    reg [DATA_W-1:0] mem [0:DEPTH-1];
    reg [PTR_W:0]    wr_ptr;
    reg [PTR_W:0]    rd_ptr;
    
    wire [PTR_W-1:0] wr_addr = wr_ptr[PTR_W-1:0];
    wire [PTR_W-1:0] rd_addr = rd_ptr[PTR_W-1:0];
    
    // Memory Write
    always @(posedge clk) begin
        if (wr_en && !full) begin
            mem[wr_addr] <= wdata;
        end
    end
    
    // Memory Read (registered output)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rdata <= {DATA_W{1'b0}};
        end else if (rd_en && !empty) begin
            rdata <= mem[rd_addr];
        end
    end
    
    // Pointer Update
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ptr <= {(PTR_W+1){1'b0}};
            rd_ptr <= {(PTR_W+1){1'b0}};
            count  <= {(PTR_W+1){1'b0}};
        end else begin
            case ({wr_en && !full, rd_en && !empty})
                2'b01: begin // Read only
                    rd_ptr <= rd_ptr + 1'b1;
                    count  <= count - 1'b1;
                end
                2'b10: begin // Write only
                    wr_ptr <= wr_ptr + 1'b1;
                    count  <= count + 1'b1;
                end
                2'b11: begin // Simultaneous
                    wr_ptr <= wr_ptr + 1'b1;
                    rd_ptr <= rd_ptr + 1'b1;
                    // count unchanged
                end
                default: begin // Idle
                end
            endcase
        end
    end
    
    // Status Flags
    assign empty = (wr_ptr == rd_ptr);
    assign full  = (wr_ptr[PTR_W] != rd_ptr[PTR_W]) && 
                   (wr_ptr[PTR_W-1:0] == rd_ptr[PTR_W-1:0]);

endmodule

