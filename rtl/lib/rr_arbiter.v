// ============================================================================
// Module: rr_arbiter
// Description: Round-Robin Arbiter, N inputs, combinational grant, sequential update
// ============================================================================
module rr_arbiter #(
    parameter N = 4,                    // Number of requesters
    parameter W = $clog2(N)             // Width of pointer
) (
    input  wire          clk,
    input  wire          rst_n,
    
    input  wire [N-1:0]  req,           // Request vector
    output reg  [N-1:0]  grant,         // One-hot grant vector
    input  wire          advance        // Update priority pointer
);

    // 内部信号，添加 /*verilator public*/ 以便测试访问
    // Last granted position
    reg [W-1:0] pointer /*verilator public*/;  
    reg [W-1:0] next_pointer;           // 组合逻辑计算下一指针
    
    // 组合逻辑变量
    integer     i;
    reg [W:0]   idx;                    // W+1 bits，避免溢出
    reg         found;
    
    always @(*) begin
        grant = {N{1'b0}};
        next_pointer = pointer;         // 默认保持
        found = 1'b0;
        idx = {W+1{1'b0}};              // 默认值，避免 latch
        
        if (|req) begin
            for (i = 1; i <= N && !found; i = i + 1) begin
                // 使用 W+1 bits 计算，避免位宽警告
                idx = {1'b0, pointer} + i[W:0];
                
                // 手动实现 % N（N 必须是 2 的幂）
                if (idx >= N[W:0])
                    idx = idx - N[W:0];
                
                if (req[idx[W-1:0]]) begin
                    grant[idx[W-1:0]] = 1'b1;
                    next_pointer = idx[W-1:0];
                    found = 1'b1;
                end
            end
        end
    end
    
    // 时序逻辑：指针更新
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pointer <= {W{1'b0}};
        end else if (advance) begin
            pointer <= next_pointer;
        end
    end

endmodule
