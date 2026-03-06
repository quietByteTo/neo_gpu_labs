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

    reg [W-1:0] pointer;                // Last granted position
    reg [W-1:0] next_pointer;  // 组合逻辑计算下一指针
    // Combinational logic for Round-Robin
    integer i, idx;  // 移到模块级或 always 外部
    reg found;
    always @(*) begin
        grant = {N{1'b0}};
        next_pointer = pointer;  // 默认保持
        found = 1'b0;
        if (|req) begin
            for (i = 1; i <= N && !found; i = i + 1) begin
                if (req[(pointer + i[W-1:0]) % N]) begin
                    grant[(pointer + i[W-1:0]) % N] = 1'b1;
                    next_pointer = ((pointer + i[W-1:0]) % N);
                    found = 1'b1;
                end
            end
        end
    end
    
    // Pointer Update
    // 时序逻辑：只需打一拍
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pointer <= {W{1'b0}};
        end else if (advance) begin
            pointer <= next_pointer;
        end
    end

endmodule