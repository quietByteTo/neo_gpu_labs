// ============================================================================
// Module: alu_pipeline
// Description: 3-stage pipelined ALU, 32 lanes, INT32/FP32 operations
// Stages: Decode(COMB) -> Execute(REG) -> Writeback(REG)
// ============================================================================
module alu_pipeline #(
    parameter NUM_LANES = 32,
    parameter DATA_W    = 32,
    parameter LANES_W   = $clog2(NUM_LANES)
) (
    input  wire                    clk,
    input  wire                    rst_n,
    
    // Pipeline Control
    input  wire                    valid_in,
    output wire                    ready_in,      // Always ready (no structural hazard)
    input  wire [31:0]             instr,         // Instruction encoding
    
    // Operands (32 lanes × 32bit)
    input  wire [NUM_LANES*DATA_W-1:0] src_a,
    input  wire [NUM_LANES*DATA_W-1:0] src_b,
    input  wire [NUM_LANES*DATA_W-1:0] src_c,     // For MAD (Multiply-Add)
    
    // Output
    output reg  [NUM_LANES*DATA_W-1:0] result,
    output reg                     valid_out
);

    // Instruction Format (simplified):
    // [31:28] - Major Opcode (ALU=4'h0)
    // [27:24] - ALU Op Type
    // [23:20] - Data Type (0=INT32, 1=FP32, 2=FP16)
    // [19:16] - Reserved
    
    localparam OP_ADD  = 4'b0000;
    localparam OP_SUB  = 4'b0001;
    localparam OP_MUL  = 4'b0010;
    localparam OP_MAD  = 4'b0011;  // src_a * src_b + src_c
    localparam OP_AND  = 4'b0100;
    localparam OP_OR   = 4'b0101;
    localparam OP_SHL  = 4'b0110;  // Shift Left
    localparam OP_SHR  = 4'b0111;  // Shift Right (Logical)
    localparam OP_MIN  = 4'b1000;
    localparam OP_MAX  = 4'b1001;
    
    localparam TYPE_INT32 = 4'b0000;
    localparam TYPE_FP32  = 4'b0001;
    
    // Stage 1: Decode (Registered Inputs)
    reg [3:0]  s1_op;
    reg [3:0]  s1_type;
    reg [NUM_LANES*DATA_W-1:0] s1_a, s1_b, s1_c;
    reg        s1_valid;
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s1_valid <= 1'b0;
            s1_op    <= 4'b0;
            s1_type  <= 4'b0;
            s1_a     <= {(NUM_LANES*DATA_W){1'b0}};
            s1_b     <= {(NUM_LANES*DATA_W){1'b0}};
            s1_c     <= {(NUM_LANES*DATA_W){1'b0}};
        end else begin
            s1_valid <= valid_in;
            if (valid_in) begin
                s1_op   <= instr[27:24];
                s1_type <= instr[23:20];
                s1_a    <= src_a;
                s1_b    <= src_b;
                s1_c    <= src_c;
            end
        end
    end
    
    // Stage 2: Execute (Parallel Lane Operations)
    reg [NUM_LANES*DATA_W-1:0] s2_result;
    reg                        s2_valid;
    reg [3:0]                  s2_op;  // Pass through for debug/monitoring
    
    // Combinational logic for each lane
    integer lane;
    wire [DATA_W-1:0] lane_a [0:NUM_LANES-1];
    wire [DATA_W-1:0] lane_b [0:NUM_LANES-1];
    wire [DATA_W-1:0] lane_c [0:NUM_LANES-1];
    reg  [DATA_W-1:0] lane_res [0:NUM_LANES-1];
    
    // Unpack vectors to arrays for processing
    generate
        genvar i;
        for (i = 0; i < NUM_LANES; i = i + 1) begin : unpack
            assign lane_a[i] = s1_a[i*DATA_W +: DATA_W];
            assign lane_b[i] = s1_b[i*DATA_W +: DATA_W];
            assign lane_c[i] = s1_c[i*DATA_W +: DATA_W];
        end
    endgenerate
    
    // Execute operations per lane (combinational)
    always @(*) begin
        for (lane = 0; lane < NUM_LANES; lane = lane + 1) begin
            case (s1_op)
                OP_ADD: lane_res[lane] = lane_a[lane] + lane_b[lane];
                OP_SUB: lane_res[lane] = lane_a[lane] - lane_b[lane];
                OP_MUL: lane_res[lane] = lane_a[lane] * lane_b[lane];  // Simple multiplication
                OP_MAD: lane_res[lane] = (lane_a[lane] * lane_b[lane]) + lane_c[lane];
                OP_AND: lane_res[lane] = lane_a[lane] & lane_b[lane];
                OP_OR:  lane_res[lane] = lane_a[lane] | lane_b[lane];
                OP_SHL: lane_res[lane] = lane_a[lane] << lane_b[lane][4:0];  // Use lower 5 bits for shift amount
                OP_SHR: lane_res[lane] = lane_a[lane] >> lane_b[lane][4:0];
                OP_MIN: lane_res[lane] = ($signed(lane_a[lane]) < $signed(lane_b[lane])) ? lane_a[lane] : lane_b[lane];
                OP_MAX: lane_res[lane] = ($signed(lane_a[lane]) > $signed(lane_b[lane])) ? lane_a[lane] : lane_b[lane];
                default: lane_res[lane] = {DATA_W{1'b0}};
            endcase
        end
    end
    
    // Pack results back to vector
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s2_valid <= 1'b0;
            s2_result <= {(NUM_LANES*DATA_W){1'b0}};
            s2_op <= 4'b0;
        end else begin
            s2_valid <= s1_valid;
            s2_op <= s1_op;
            if (s1_valid) begin
                for (lane = 0; lane < NUM_LANES; lane = lane + 1) begin
                    s2_result[lane*DATA_W +: DATA_W] <= lane_res[lane];
                end
            end
        end
    end
    
    // Stage 3: Writeback (Output Register)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_out <= 1'b0;
            result    <= {(NUM_LANES*DATA_W){1'b0}};
        end else begin
            valid_out <= s2_valid;
            if (s2_valid) begin
                result <= s2_result;
            end
        end
    end
    
    assign ready_in = 1'b1;  // Fully pipelined, always ready

endmodule