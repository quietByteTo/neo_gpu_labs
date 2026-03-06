// ============================================================================
// Module: tensor_core
// Description: Systolic Array based Tensor Core, 4×4 MAC array
// FP16 input, FP32 accumulation, Weight Stationary
// ============================================================================
module tensor_core #(
    parameter ARRAY_SIZE = 4,           // 4×4 systolic array
    parameter FP16_W     = 16,
    parameter FP32_W     = 32,
    parameter LATENCY    = 4            // Fixed pipeline latency
) (
    input  wire                    clk,
    input  wire                    rst_n,
    
    // Configuration
    input  wire [1:0]              precision,  // 00:FP16, 01:TF32, 10:INT8
    input  wire [2:0]              layout,     // Matrix layout (row/col major)
    
    // Input Matrices (4×4 blocks)
    input  wire [ARRAY_SIZE*ARRAY_SIZE*FP16_W-1:0] mat_a,  // 16×16bit = 256bit
    input  wire [ARRAY_SIZE*ARRAY_SIZE*FP16_W-1:0] mat_b,
    input  wire [ARRAY_SIZE*ARRAY_SIZE*FP32_W-1:0] mat_c,  // Accumulator
    
    input  wire                    valid_in,
    output wire                    ready,
    
    // Output
    output reg  [ARRAY_SIZE*ARRAY_SIZE*FP32_W-1:0] mat_d,
    output reg                     valid_out
);

    // State Machine
    localparam IDLE  = 2'b00;
    localparam LOAD  = 2'b01;  // Load weights into array
    localparam COMP  = 2'b10;  // Compute (systolic flow)
    localparam DONE  = 2'b11;
    
    reg [1:0] state;
    reg [2:0] cycle_cnt;
    
    // Systolic Array Structures
    // Weight Stationary: Weights stay in MAC, activations flow through
    reg [FP16_W-1:0] weights [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];      // Stationary
    reg [FP32_W-1:0] accumulators [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1]; // Accumulation
    
    // Input staging
    reg [FP16_W-1:0] act_row [0:ARRAY_SIZE-1];  // Activation row input
    
    // Unpack input matrices
    wire [FP16_W-1:0] a_elem [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    wire [FP16_W-1:0] b_elem [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    wire [FP32_W-1:0] c_elem [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    
    genvar i, j;
    generate
        for (i = 0; i < ARRAY_SIZE; i = i + 1) begin : unpack_a
            for (j = 0; j < ARRAY_SIZE; j = j + 1) begin : unpack_a_inner
                assign a_elem[i][j] = mat_a[(i*ARRAY_SIZE + j)*FP16_W +: FP16_W];
            end
        end
        for (i = 0; i < ARRAY_SIZE; i = i + 1) begin : unpack_b
            for (j = 0; j < ARRAY_SIZE; j = j + 1) begin : unpack_b_inner
                assign b_elem[i][j] = mat_b[(i*ARRAY_SIZE + j)*FP16_W +: FP16_W];
            end
        end
        for (i = 0; i < ARRAY_SIZE; i = i + 1) begin : unpack_c
            for (j = 0; j < ARRAY_SIZE; j = j + 1) begin : unpack_c_inner
                assign c_elem[i][j] = mat_c[(i*ARRAY_SIZE + j)*FP32_W +: FP32_W];
            end
        end
    endgenerate
    
    // Control Logic
    assign ready = (state == IDLE);
    
    integer row, col, k;
    reg [FP32_W-1:0] mult_result [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            valid_out <= 1'b0;
            cycle_cnt <= 3'd0;
        end else begin
            valid_out <= 1'b0;
            
            case (state)
                IDLE: begin
                    if (valid_in) begin
                        state <= LOAD;
                        cycle_cnt <= 3'd0;
                        
                        // Load accumulators with C matrix
                        for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                            for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                                accumulators[row][col] <= c_elem[row][col];
                            end
                        end
                    end
                end
                
                LOAD: begin
                    // Load weights (B matrix) into systolic array
                    // In weight stationary, we load B as weights
                    for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                        for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                            weights[row][col] <= b_elem[row][col];
                        end
                    end
                    state <= COMP;
                    cycle_cnt <= 3'd0;
                end
                
                COMP: begin
                    // Systolic computation: A flows through, multiplies with B (weights), accumulates
                    // Simplified: treat as single cycle MAC for now (actual systolic has wavefront)
                    
                    for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                        for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                            // MAC: Accum += A[row][k] * B[k][col]
                            // Simplified to single inner product for demo
                            mult_result[row][col] <= a_elem[row][col] * weights[col][row];  // Simplified
                            accumulators[row][col] <= accumulators[row][col] + 
                                                     (a_elem[row][col] * weights[col][row]);
                        end
                    end
                    
                    if (cycle_cnt >= LATENCY-1) begin
                        state <= DONE;
                    end else begin
                        cycle_cnt <= cycle_cnt + 1'b1;
                    end
                end
                
                DONE: begin
                    // Pack output
                    for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                        for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                            mat_d[(row*ARRAY_SIZE + col)*FP32_W +: FP32_W] <= accumulators[row][col];
                        end
                    end
                    valid_out <= 1'b1;
                    state <= IDLE;
                end
                
                default: state <= IDLE;
            endcase
        end
    end

endmodule