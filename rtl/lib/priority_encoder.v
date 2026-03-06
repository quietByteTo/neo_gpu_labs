// ============================================================================
// Module: priority_encoder
// Description: Fixed priority encoder, finds highest bit set
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
    
    // 给 always 块命名，然后用 disable 终止
    always @(*) begin : encode_loop
        out = {W{1'b0}};
        for (i = 0; i < N; i = i + 1) begin
            if (in[i]) begin
                out = i[W-1:0];
                disable encode_loop;    // ✅ 终止命名块，跳出循环
            end
        end
    end

endmodule