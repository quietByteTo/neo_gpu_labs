// ============================================================================
// Module: tensor_core - 修复版本（正确的矩阵乘法）
// ============================================================================
module tensor_core #(
    parameter ARRAY_SIZE = 4,
    parameter FP16_W     = 16,
    parameter FP32_W     = 32,
    parameter LATENCY    = 4
) (
    input  wire                    clk,
    input  wire                    rst_n,
    
    input  wire [1:0]              precision,
    input  wire [2:0]              layout,
    
    input  wire [FP16_W-1:0]       mat_a [0:ARRAY_SIZE*ARRAY_SIZE-1],
    input  wire [FP16_W-1:0]       mat_b [0:ARRAY_SIZE*ARRAY_SIZE-1],
    input  wire [FP32_W-1:0]       mat_c [0:ARRAY_SIZE*ARRAY_SIZE-1],
    
    input  wire                    valid_in,
    output wire                    ready,
    
    output reg  [FP32_W-1:0]       mat_d [0:ARRAY_SIZE*ARRAY_SIZE-1],
    output reg                     valid_out
);

    localparam IDLE  = 2'b00;
    localparam LOAD  = 2'b01;
    localparam COMP  = 2'b10;
    localparam DONE  = 2'b11;
    
    reg [1:0] state;
    reg [2:0] cycle_cnt;
    
    // 输入缓冲（双缓冲支持连续发射）
    reg [FP16_W-1:0] a_buf [0:ARRAY_SIZE*ARRAY_SIZE-1];
    reg [FP16_W-1:0] b_buf [0:ARRAY_SIZE*ARRAY_SIZE-1];
    reg [FP32_W-1:0] c_buf [0:ARRAY_SIZE*ARRAY_SIZE-1];
    reg              buf_valid;
    
    reg [FP16_W-1:0] a_buf2 [0:ARRAY_SIZE*ARRAY_SIZE-1];
    reg [FP16_W-1:0] b_buf2 [0:ARRAY_SIZE*ARRAY_SIZE-1];
    reg [FP32_W-1:0] c_buf2 [0:ARRAY_SIZE*ARRAY_SIZE-1];
    reg              buf2_valid;
    
    // 工作寄存器
    reg [FP16_W-1:0] a_reg [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    reg [FP16_W-1:0] b_reg [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    reg [FP32_W-1:0] c_reg [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    reg [FP32_W-1:0] result_reg [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    
    reg              busy;
    assign ready = !buf2_valid;
    
    // 组合逻辑：计算矩阵乘法 A × B（纯组合，单周期完成）
    reg [FP32_W-1:0] mac_result [0:ARRAY_SIZE-1][0:ARRAY_SIZE-1];
    
    integer row, col, k, idx;
    
    always @(*) begin
        for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
            for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                mac_result[row][col] = 0;
                for (k = 0; k < ARRAY_SIZE; k = k + 1) begin
                    // 符号扩展 FP16 到 FP32，然后相乘累加
                    mac_result[row][col] = mac_result[row][col] + 
                                          ($signed({{16{a_reg[row][k][15]}}, a_reg[row][k]}) * 
                                           $signed({{16{b_reg[k][col][15]}}, b_reg[k][col]}));
                end
            end
        end
    end
    
    // 时序逻辑：状态机和数据流
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            valid_out <= 1'b0;
            cycle_cnt <= 3'd0;
            buf_valid <= 1'b0;
            buf2_valid <= 1'b0;
            busy <= 1'b0;
        end else begin
            valid_out <= 1'b0;
            
            // 接收新数据到二级缓冲
            if (valid_in && ready) begin
                for (idx = 0; idx < ARRAY_SIZE*ARRAY_SIZE; idx = idx + 1) begin
                    a_buf2[idx] <= mat_a[idx];
                    b_buf2[idx] <= mat_b[idx];
                    c_buf2[idx] <= mat_c[idx];
                end
                buf2_valid <= 1'b1;
            end
            
            // 缓冲晋升：二级 -> 一级
            if (!buf_valid && buf2_valid) begin
                for (idx = 0; idx < ARRAY_SIZE*ARRAY_SIZE; idx = idx + 1) begin
                    a_buf[idx] <= a_buf2[idx];
                    b_buf[idx] <= b_buf2[idx];
                    c_buf[idx] <= c_buf2[idx];
                end
                buf_valid <= 1'b1;
                buf2_valid <= 1'b0;
            end
            
            case (state)
                IDLE: begin
                    // 优先使用一级缓冲，其次直接输入
                    if (buf_valid || valid_in) begin
                        // 加载数据到工作寄存器
                        for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                            for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                                idx = row * ARRAY_SIZE + col;
                                if (buf_valid) begin
                                    a_reg[row][col] <= a_buf[idx];
                                    b_reg[row][col] <= b_buf[idx];
                                    c_reg[row][col] <= c_buf[idx];
                                end else begin
                                    a_reg[row][col] <= mat_a[idx];
                                    b_reg[row][col] <= mat_b[idx];
                                    c_reg[row][col] <= mat_c[idx];
                                end
                            end
                        end
                        buf_valid <= 1'b0;
                        busy <= 1'b1;
                        state <= LOAD;
                    end
                end
                
                LOAD: begin
                    // 关键修复：直接计算最终结果 C + A×B（只加一次！）
                    for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                        for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                            result_reg[row][col] <= c_reg[row][col] + mac_result[row][col];
                        end
                    end
                    state <= COMP;
                    cycle_cnt <= 3'd0;
                end
                
                COMP: begin
                    // 只是等待时序收敛，不再累加
                    if (cycle_cnt >= LATENCY-1)
                        state <= DONE;
                    else
                        cycle_cnt <= cycle_cnt + 1'b1;
                end
                
                DONE: begin
                    // 输出结果
                    for (row = 0; row < ARRAY_SIZE; row = row + 1) begin
                        for (col = 0; col < ARRAY_SIZE; col = col + 1) begin
                            idx = row * ARRAY_SIZE + col;
                            mat_d[idx] <= result_reg[row][col];
                        end
                    end
                    valid_out <= 1'b1;
                    busy <= 1'b0;
                    state <= IDLE;
                end
                
                default: state <= IDLE;
            endcase
        end
    end

endmodule

