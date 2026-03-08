/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off CASEINCOMPLETE */
/* verilator lint_off WIDTHEXPAND */
/* verilator lint_off WIDTHTRUNC */

module cordic_sincos #(
    parameter NUM_LANES = 32,
    parameter DATA_W    = 32
) (
    input  wire                    clk,
    input  wire                    rst_n,
    input  wire [NUM_LANES*DATA_W-1:0] angle,
    input  wire                    valid_in,
    input  wire                    cos_sel,
    output reg  [NUM_LANES*DATA_W-1:0] result,
    output reg                     valid_out,
    output wire                    ready
);

    // Q16.16 format
    localparam signed [31:0] CORDIC_K = 32'sd39796;  // 0.60725 * 65536
    
    localparam signed [31:0] ATAN [0:7] = '{
        32'sd51471,  // atan(1)    = 0.785398 * 65536
        32'sd30386,  // atan(0.5)  = 0.463648 * 65536
        32'sd16054,  // atan(0.25) = 0.244979 * 65536
        32'sd8144,   // atan(0.125)= 0.124355 * 65536
        32'sd4091,   // atan(0.0625)=0.062419 * 65536
        32'sd2047,   // atan(0.03125)=0.031240 * 65536
        32'sd1024,   // atan(0.015625)=0.015624 * 65536
        32'sd512     // atan(0.0078125)=0.007812 * 65536
    };

    localparam ST_IDLE  = 4'd0;
    localparam ST_INIT  = 4'd1;
    localparam ST_ITER0 = 4'd2;
    localparam ST_ITER1 = 4'd3;
    localparam ST_ITER2 = 4'd4;
    localparam ST_ITER3 = 4'd5;
    localparam ST_ITER4 = 4'd6;
    localparam ST_ITER5 = 4'd7;
    localparam ST_ITER6 = 4'd8;
    localparam ST_ITER7 = 4'd9;
    localparam ST_DONE  = 4'd10;

    reg [3:0] state;
    reg       cos_sel_reg;
    
    reg signed [31:0] x_reg [0:NUM_LANES-1];
    reg signed [31:0] y_reg [0:NUM_LANES-1];
    reg signed [31:0] z_reg [0:NUM_LANES-1];
    reg        [31:0] out_reg [0:NUM_LANES-1];

    assign ready = (state == ST_IDLE);

    genvar g;
    generate
        for (g = 0; g < NUM_LANES; g = g + 1) begin : lane
            wire signed [31:0] angle_in = $signed(angle[g*DATA_W +: DATA_W]);
            
            // 关键修复：用 state 直接计算迭代索引，无时序延迟
            // ST_ITER0=2, ST_ITER1=3, ..., ST_ITER7=9
            // iter_idx = state - 2，范围 0-7
            wire [3:0] iter_idx_full = (state >= ST_ITER0 && state <= ST_ITER7) ? 
                                       (state - ST_ITER0) : 4'd0;
            wire [2:0] iter_idx = iter_idx_full[2:0];
            
            wire signed [31:0] x_cur = x_reg[g];
            wire signed [31:0] y_cur = y_reg[g];
            wire signed [31:0] z_cur = z_reg[g];
            
            // 算术右移
            wire signed [31:0] x_sh = x_cur >>> iter_idx;
            wire signed [31:0] y_sh = y_cur >>> iter_idx;

            // CORDIC迭代（组合逻辑）
            reg signed [31:0] x_next, y_next, z_next;
            always @(*) begin
                if (z_cur >= 0) begin
                    x_next = x_cur - y_sh;
                    y_next = y_cur + x_sh;
                    z_next = z_cur - ATAN[iter_idx];
                end else begin
                    x_next = x_cur + y_sh;
                    y_next = y_cur - x_sh;
                    z_next = z_cur + ATAN[iter_idx];
                end
            end

            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    x_reg[g]   <= 32'sd0;
                    y_reg[g]   <= 32'sd0;
                    z_reg[g]   <= 32'sd0;
                    out_reg[g] <= 32'd0;
                end else begin
                    case (state)
                        ST_IDLE: ;
                        
                        ST_INIT: begin
                            x_reg[g] <= CORDIC_K;
                            y_reg[g] <= 32'sd0;
                            z_reg[g] <= angle_in;
                        end
                        
                        ST_ITER0, ST_ITER1, ST_ITER2, ST_ITER3,
                        ST_ITER4, ST_ITER5, ST_ITER6, ST_ITER7: begin
                            x_reg[g] <= x_next;
                            y_reg[g] <= y_next;
                            z_reg[g] <= z_next;
                        end
                        
                        ST_DONE: begin
                            out_reg[g] <= cos_sel_reg ? $unsigned(x_reg[g]) : $unsigned(y_reg[g]);
                        end
                        
                        default: ;
                    endcase
                end
            end
            
            always @(*) result[g*DATA_W +: DATA_W] = out_reg[g];
        end
    endgenerate

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= ST_IDLE;
            cos_sel_reg <= 1'b0;
            valid_out   <= 1'b0;
        end else begin
            valid_out <= 1'b0;
            
            case (state)
                ST_IDLE: begin
                    if (valid_in) begin
                        cos_sel_reg <= cos_sel;
                        state <= ST_INIT;
                    end
                end
                
                ST_INIT:  state <= ST_ITER0;
                ST_ITER0: state <= ST_ITER1;
                ST_ITER1: state <= ST_ITER2;
                ST_ITER2: state <= ST_ITER3;
                ST_ITER3: state <= ST_ITER4;
                ST_ITER4: state <= ST_ITER5;
                ST_ITER5: state <= ST_ITER6;
                ST_ITER6: state <= ST_ITER7;
                ST_ITER7: state <= ST_DONE;
                ST_DONE:  begin valid_out <= 1'b1; state <= ST_IDLE; end
                default:  state <= ST_IDLE;
            endcase
        end
    end

endmodule

