// ============================================================================
// Module: priority_encoder
// Description: Fixed priority encoder, finds lowest bit set (LSB priority)
// ============================================================================
module priority_encoder #(
    parameter N = 32,
    parameter W = $clog2(N)
) (
    input  wire [N-1:0]  in,
    output reg  [W-1:0]  out,
    output wire          valid
);

    assign valid = |in;
    
    integer i;
    reg found;  // 标志位替代 disable（Verilator 更友好）
    
    always @(*) begin
        out = {W{1'b0}};
        found = 1'b0;
        
        for (i = 0; i < N && !found; i = i + 1) begin
            if (in[i]) begin
                out = i[W-1:0];
                found = 1'b1;  // 设置标志，下次循环退出
            end
        end
    end

endmodule
