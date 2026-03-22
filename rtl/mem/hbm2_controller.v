// ============================================================================
// Module: hbm2_controller
// Description: Simplified HBM2 Controller with single-cycle latency
// Models 4-8GB HBM as SRAM array with fixed latency pipeline
// ============================================================================
module hbm2_controller #(
    parameter NUM_CHANNELS  = 4,        // HBM pseudo-channels
    parameter CH_ID_W       = $clog2(NUM_CHANNELS),
    parameter MEM_SIZE_MB   = 4096,     // 4GB total
    parameter DATA_W        = 512,      // 512-bit wide HBM interface
    parameter ADDR_W        = 32,       // 32-bit address (4GB)
    parameter LATENCY       = 2         // Fixed read latency cycles (HBM typically ~10ns)
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // From L2 Cache / Hub (Request)
    input  wire [ADDR_W-1:0]         req_addr,
    input  wire [DATA_W-1:0]         req_wdata,
    input  wire                      req_wr_en,
    input  wire [DATA_W/8-1:0]       req_be,            // 64-bit byte enable
    input  wire                      req_valid,
    output wire                      req_ready,
    input  wire [CH_ID_W-1:0]        req_channel,       // Target HBM channel
    
    // To L2 Cache (Response)
    output reg  [DATA_W-1:0]         resp_rdata,
    output reg                       resp_valid,
    input  wire                      resp_ready,
    output wire [CH_ID_W-1:0]        resp_channel,
    
    // ECC Status (Debug)
    output reg                       ecc_error,         // ECC error detected (simulated)
    output reg  [31:0]               ecc_error_count,
    
    // Refresh (optional, for realism)
    output reg                       refresh_pending    // Indicates refresh happening
);

    // Simplified Memory Model: Large SRAM array per channel
    // In reality HBM has complex row/column/bank timing, here we use flat addressing
    localparam MEM_DEPTH = (MEM_SIZE_MB * 1024 * 1024) / (DATA_W/8) / NUM_CHANNELS;
    localparam MEM_ADDR_W = $clog2(MEM_DEPTH);
    
    // Memory Arrays (one per channel)
    reg [DATA_W-1:0] hbm_mem [0:NUM_CHANNELS-1][0:MEM_DEPTH-1];
    
    // Address Mapping: Linear mapping with channel selection
    // req_addr[ADDR_W-1:4] selects word address, lower bits select channel
    wire [MEM_ADDR_W-1:0] mem_addr = req_addr[MEM_ADDR_W+3:4];
    wire [CH_ID_W-1:0]    addr_channel = req_channel;  // Direct channel assignment
    
    // ECC Simulation: Add 1 cycle delay for "ECC check" on reads
    reg [DATA_W-1:0]    ecc_pipeline [0:LATENCY-1];
    reg                 valid_pipeline [0:LATENCY-1];
    reg [CH_ID_W-1:0]   channel_pipeline [0:LATENCY-1];
    
    integer i;
    
    // Memory Access (Combinational for write, Registered for read)
    assign req_ready = !refresh_pending;  // Stall if refreshing (rare)
    
    always @(posedge clk) begin
        // Default
        resp_valid <= 1'b0;
        refresh_pending <= 1'b0;  // Simplified: no real refresh stall
        
        // Handle Request
        if (req_valid && req_ready) begin
            if (req_wr_en) begin
                // Write with byte enable
                // Simplified: full line write for now (BE ignored in basic version)
                hbm_mem[addr_channel][mem_addr] <= req_wdata;
            end else begin
                // Read: push to ECC pipeline
                ecc_pipeline[0] <= hbm_mem[addr_channel][mem_addr];
                valid_pipeline[0] <= 1'b1;
                channel_pipeline[0] <= addr_channel;
            end
        end else begin
            valid_pipeline[0] <= 1'b0;
        end
        
        // ECC Pipeline (Shift register for latency simulation)
        for (i = 1; i < LATENCY; i = i + 1) begin
            ecc_pipeline[i] <= ecc_pipeline[i-1];
            valid_pipeline[i] <= valid_pipeline[i-1];
            channel_pipeline[i] <= channel_pipeline[i-1];
        end
        
        // Output stage
        if (valid_pipeline[LATENCY-1]) begin
            resp_rdata <= ecc_pipeline[LATENCY-1];
            resp_valid <= 1'b1;
            
            // Simulate random ECC error (1 in 1000 chance)
            if (($random % 1000) == 0) begin
                ecc_error <= 1'b1;
                ecc_error_count <= ecc_error_count + 1'b1;
                // Corrupt one bit to simulate error
                resp_rdata[0] <= ~ecc_pipeline[LATENCY-1][0];
            end else begin
                ecc_error <= 1'b0;
            end
        end else begin
            ecc_error <= 1'b0;
        end
    end
    integer ch, addr;
    initial begin
        ecc_error_count = 32'd0;
        // Initialize memory to zero
        
        for (ch = 0; ch < NUM_CHANNELS; ch = ch + 1) begin
            for (addr = 0; addr < MEM_DEPTH; addr = addr + 1) begin
                hbm_mem[ch][addr] = {DATA_W{1'b0}};
            end
        end
    end
    
    assign resp_channel = channel_pipeline[LATENCY-1];

endmodule
