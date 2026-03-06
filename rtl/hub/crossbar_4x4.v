// ============================================================================
// Module: crossbar_4x4
// Description: 4x4 Full Crossbar Switch with input buffering and RR arbitration
// Connects 4 GPCs to 4 L2 Banks (or HBM channels)
// ============================================================================
module crossbar_4x4 #(
    parameter NUM_PORTS   = 4,          // 4x4 configuration
    parameter PORT_ID_W   = $clog2(NUM_PORTS),
    parameter ADDR_W      = 64,
    parameter DATA_W      = 128,
    parameter FIFO_DEPTH  = 4           // Per-input FIFO depth
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Input Ports (from GPCs or upper level)
    input  wire [ADDR_W-1:0]         in_addr    [0:NUM_PORTS-1],
    input  wire [DATA_W-1:0]         in_wdata   [0:NUM_PORTS-1],
    input  wire                      in_wr_en   [0:NUM_PORTS-1],
    input  wire [15:0]               in_be      [0:NUM_PORTS-1],
    input  wire                      in_valid   [0:NUM_PORTS-1],
    output wire                      in_ready   [0:NUM_PORTS-1],
    output wire [DATA_W-1:0]         in_rdata   [0:NUM_PORTS-1],
    output wire                      in_rvalid  [0:NUM_PORTS-1],
    input  wire                      in_rready  [0:NUM_PORTS-1],
    
    // Output Ports (to L2 Banks)
    output wire [ADDR_W-1:0]         out_addr   [0:NUM_PORTS-1],
    output wire [DATA_W-1:0]         out_wdata  [0:NUM_PORTS-1],
    output wire                      out_wr_en  [0:NUM_PORTS-1],
    output wire [15:0]               out_be     [0:NUM_PORTS-1],
    output wire                      out_valid  [0:NUM_PORTS-1],
    input  wire                      out_ready  [0:NUM_PORTS-1],
    input  wire [DATA_W-1:0]         out_rdata  [0:NUM_PORTS-1],
    input  wire                      out_rvalid [0:NUM_PORTS-1],
    output wire                      out_rready [0:NUM_PORTS-1]
);

    // Input FIFOs (prevent Head-of-Line blocking)
    // Each input has a FIFO to buffer requests when output is busy
    
    wire [ADDR_W-1:0]         fifo_addr  [0:NUM_PORTS-1];
    wire [DATA_W-1:0]         fifo_wdata [0:NUM_PORTS-1];
    wire                      fifo_wr_en [0:NUM_PORTS-1];
    wire [15:0]               fifo_be    [0:NUM_PORTS-1];
    wire                      fifo_valid [0:NUM_PORTS-1];
    wire                      fifo_ready [0:NUM_PORTS-1];
    
    genvar i;
    generate
        for (i = 0; i < NUM_PORTS; i = i + 1) begin : input_fifo
            sync_fifo #(
                .DATA_W(ADDR_W + DATA_W + 1 + 16),  // Addr + Data + Wr_en + BE
                .DEPTH(FIFO_DEPTH)
            ) u_fifo (
                .clk(clk),
                .rst_n(rst_n),
                .wr_en(in_valid[i]),
                .wdata({in_addr[i], in_wdata[i], in_wr_en[i], in_be[i]}),
                .full(!in_ready[i]),
                .rd_en(fifo_ready[i]),
                .rdata({fifo_addr[i], fifo_wdata[i], fifo_wr_en[i], fifo_be[i]}),
                .empty(!fifo_valid[i]),
                .count()
            );
        end
    endgenerate
    
    // Route Computation: Determine output port based on address
    // Simple scheme: Address[7:6] selects bank (for 4 banks)
    wire [PORT_ID_W-1:0] route_dest [0:NUM_PORTS-1];
    
    generate
        for (i = 0; i < NUM_PORTS; i = i + 1) begin : route_logic
            assign route_dest[i] = fifo_addr[i][7:6];  // Bank selection bits
        end
    endgenerate
    
    // Output Port Arbitration (per output port)
    // Each output port arbitrates among inputs that target it
    
    reg [NUM_PORTS-1:0] out_port_valid [0:NUM_PORTS-1];  // Which inputs want this output
    reg [NUM_PORTS-1:0] out_port_grant [0:NUM_PORTS-1];  // RR grant vector
    wire [NUM_PORTS-1:0] out_port_ready_local [0:NUM_PORTS-1];
    
    // Generate request vectors for each output
    integer inp, outp;
    always @(*) begin
        for (outp = 0; outp < NUM_PORTS; outp = outp + 1) begin
            out_port_valid[outp] = {NUM_PORTS{1'b0}};
            for (inp = 0; inp < NUM_PORTS; inp = inp + 1) begin
                if (fifo_valid[inp] && route_dest[inp] == outp[PORT_ID_W-1:0]) begin
                    out_port_valid[outp][inp] = 1'b1;
                end
            end
        end
    end
    
    // Round-Robin Arbiters for each output port
    generate
        for (i = 0; i < NUM_PORTS; i = i + 1) begin : output_arbiter
            rr_arbiter #(
                .N(NUM_PORTS)
            ) u_rr (
                .clk(clk),
                .rst_n(rst_n),
                .req(out_port_valid[i]),
                .grant(out_port_grant[i]),
                .advance(out_valid[i] && out_ready[i])  // Advance on successful transfer
            );
        end
    endgenerate
    
    // Output Muxing (combinational)
    // For each output port, select the granted input
    
    generate
        for (outp = 0; outp < NUM_PORTS; outp = outp + 1) begin : output_mux
            reg [ADDR_W-1:0]  mux_addr;
            reg [DATA_W-1:0]  mux_data;
            reg               mux_wr_en;
            reg [15:0]        mux_be;
            reg               mux_valid;
            reg [PORT_ID_W-1:0] selected_inp;
            
            integer sel;
            always @(*) begin
                mux_addr = {ADDR_W{1'b0}};
                mux_data = {DATA_W{1'b0}};
                mux_wr_en = 1'b0;
                mux_be = 16'd0;
                mux_valid = 1'b0;
                selected_inp = {PORT_ID_W{1'b0}};
                
                for (sel = 0; sel < NUM_PORTS; sel = sel + 1) begin
                    if (out_port_grant[outp][sel]) begin
                        mux_addr = fifo_addr[sel];
                        mux_data = fifo_wdata[sel];
                        mux_wr_en = fifo_wr_en[sel];
                        mux_be = fifo_be[sel];
                        mux_valid = fifo_valid[sel];
                        selected_inp = sel[PORT_ID_W-1:0];
                    end
                end
                
                out_addr[outp] = mux_addr;
                out_wdata[outp] = mux_data;
                out_wr_en[outp] = mux_wr_en;
                out_be[outp] = mux_be;
                out_valid[outp] = mux_valid;
            end
            
            // Backpressure to FIFO (only selected input can pop)
            assign out_port_ready_local[outp] = out_port_grant[outp] & {NUM_PORTS{out_ready[outp]}};
        end
    endgenerate
    
    // Consolidate backpressure to input FIFOs
    generate
        for (inp = 0; inp < NUM_PORTS; inp = inp + 1) begin : backpressure_consolidate
            assign fifo_ready[inp] = |{out_port_ready_local[0][inp], 
                                       out_port_ready_local[1][inp],
                                       out_port_ready_local[2][inp], 
                                       out_port_ready_local[3][inp]};
        end
    endgenerate
    
    // Response Path Routing (L2 -> GPC)
    // Responses must be routed back to the source GPC
    // Need to track which input (GPC) sent the request for each transaction
    
    // Simplified: Assume responses come back with ID indicating source port
    // In reality, need transaction ID tracking
    
    reg [PORT_ID_W-1:0] resp_route_table [0:NUM_PORTS-1];  // For each output port, track source
    
    // Response Routing (Demux based on assumed ID field)
    // For now, direct connect assuming L2 returns with proper routing info
    generate
        for (i = 0; i < NUM_PORTS; i = i + 1) begin : resp_path
            assign in_rdata[i] = out_rdata[i];  // Simplified: direct mapping
            assign in_rvalid[i] = out_rvalid[i];
            assign out_rready[i] = in_rready[i];
        end
    endgenerate

endmodule