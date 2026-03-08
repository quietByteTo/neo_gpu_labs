/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off CASEINCOMPLETE */
/* verilator lint_off WIDTHEXPAND */
/* verilator lint_off WIDTHTRUNC */
/* verilator lint_off CASEOVERLAP */

module sfu #(
    parameter NUM_LANES = 32,
    parameter DATA_W    = 32
) (
    input  wire                    clk,
    input  wire                    rst_n,
    input  wire [2:0]              sfu_op,
    input  wire [NUM_LANES*DATA_W-1:0] src,
    input  wire                    valid_in,
    output reg  [NUM_LANES*DATA_W-1:0] result,
    output reg                     valid_out,
    output wire                    ready
);

    localparam OP_RCP  = 3'd0;
    localparam OP_RSQ  = 3'd1;
    localparam OP_SIN  = 3'd2;
    localparam OP_COS  = 3'd3;

    localparam ST_IDLE     = 5'd0;
    localparam ST_LOAD     = 5'd1;
    localparam ST_INIT     = 5'd2;
    localparam ST_RCP_1    = 5'd3;
    localparam ST_RCP_2    = 5'd4;
    localparam ST_RSQ_1    = 5'd5;
    localparam ST_RSQ_2    = 5'd6;
    localparam ST_RSQ_3    = 5'd7;
    localparam ST_RSQ_4    = 5'd8;
    localparam ST_RSQ_5    = 5'd9;
    localparam ST_RSQ_6    = 5'd10;
    localparam ST_CORDIC_1 = 5'd11;
    localparam ST_CORDIC_2 = 5'd12;
    localparam ST_CORDIC_3 = 5'd13;
    localparam ST_CORDIC_4 = 5'd14;
    localparam ST_CORDIC_5 = 5'd15;
    localparam ST_CORDIC_6 = 5'd16;
    localparam ST_CORDIC_7 = 5'd17;
    localparam ST_CORDIC_8 = 5'd18;
    localparam ST_DONE     = 5'd19;
    
    reg [4:0] state;
    reg [2:0] op_reg;
    
    reg [31:0] lane_in  [0:NUM_LANES-1];
    reg [31:0] lane_out [0:NUM_LANES-1];
    reg [31:0] x_reg    [0:NUM_LANES-1];
    
    reg signed [31:0] cordic_x [0:NUM_LANES-1];
    reg signed [31:0] cordic_y [0:NUM_LANES-1];
    reg signed [31:0] cordic_z [0:NUM_LANES-1];
    
    genvar g;
    assign ready = (state == ST_IDLE);

    function automatic [31:0] fp32(input s, input [7:0] e, input [22:0] m);
        fp32 = {s, e, m};
    endfunction
    
    function automatic [7:0]  get_e(input [31:0] f); get_e = f[30:23]; endfunction
    function automatic [22:0] get_m(input [31:0] f); get_m = f[22:0]; endfunction
    function automatic        get_s(input [31:0] f); get_s = f[31]; endfunction

    function automatic [31:0] fp_mul(input [31:0] a, b);
        reg s; reg [8:0] e; reg [47:0] m;
        begin
            if (a[30:0] == 0 || b[30:0] == 0) return 32'h0;
            s = get_s(a) ^ get_s(b);
            e = {1'b0, get_e(a)} + {1'b0, get_e(b)} - 9'd127;
            m = ({1'b1, get_m(a)}) * ({1'b1, get_m(b)});
            if (m[47]) begin e = e + 1; m = m >> 1; end
            return fp32(s, e[7:0], m[45:23]);
        end
    endfunction
    
    // 改进的减法：正确处理 a < b 的情况，返回带符号的结果
    function automatic [31:0] fp_sub(input [31:0] a, b);
        reg [7:0] exp_a, exp_b, exp_diff;
        reg [23:0] man_a, man_b;
        reg [24:0] man_diff;
        reg [7:0] exp_out;
        reg [22:0] man_out;
        reg s;
        begin
            exp_a = get_e(a); exp_b = get_e(b);
            
            // 对齐尾数
            if (exp_a >= exp_b) begin
                exp_diff = exp_a - exp_b;
                man_a = {1'b1, get_m(a)};
                man_b = {1'b1, get_m(b)} >> exp_diff;
                exp_out = exp_a;
                s = get_s(a);  // 结果的符号与 a 相同
            end else begin
                exp_diff = exp_b - exp_a;
                man_a = {1'b1, get_m(a)} >> exp_diff;
                man_b = {1'b1, get_m(b)};
                exp_out = exp_b;
                s = ~get_s(a);  // 结果的符号与 a 相反（因为 b 更大）
            end
            
            // 计算差值
            if (man_a >= man_b) begin
                man_diff = man_a - man_b;
                s = get_s(a);
            end else begin
                man_diff = man_b - man_a;
                s = ~get_s(a);
            end
            
            if (man_diff == 0) return 32'h0;
            
            // 规格化
            if (man_diff[23]) man_out = man_diff[22:0];
            else if (man_diff[22]) begin man_out = {man_diff[21:0], 1'b0}; exp_out = exp_out - 1; end
            else if (man_diff[21]) begin man_out = {man_diff[20:0], 2'b00}; exp_out = exp_out - 2; end
            else begin man_out = {man_diff[19:0], 3'b000}; exp_out = exp_out - 3; end
            
            return fp32(s, exp_out, man_out);
        end
    endfunction

    // 保持 fp_sub_pos 用于 RCP（需要正数结果）
    function automatic [31:0] fp_sub_pos(input [31:0] a, b);
        reg [7:0] exp_a, exp_b, exp_diff;
        reg [23:0] man_a, man_b;
        reg [24:0] man_res;
        reg [7:0] exp_out;
        reg [22:0] man_out;
        begin
            exp_a = get_e(a); exp_b = get_e(b);
            if (exp_a >= exp_b) begin
                exp_diff = exp_a - exp_b;
                man_a = {1'b1, get_m(a)};
                man_b = {1'b1, get_m(b)} >> exp_diff;
                exp_out = exp_a;
            end else begin
                exp_diff = exp_b - exp_a;
                man_a = {1'b1, get_m(a)} >> exp_diff;
                man_b = {1'b1, get_m(b)};
                exp_out = exp_b;
            end
            man_res = (man_a >= man_b) ? (man_a - man_b) : (man_b - man_a);
            if (man_res == 0) return 32'h0;
            if (man_res[23]) man_out = man_res[22:0];
            else if (man_res[22]) begin man_out = {man_res[21:0], 1'b0}; exp_out = exp_out - 1; end
            else if (man_res[21]) begin man_out = {man_res[20:0], 2'b00}; exp_out = exp_out - 2; end
            else begin man_out = {man_res[19:0], 3'b000}; exp_out = exp_out - 3; end
            return fp32(1'b0, exp_out, man_out);
        end
    endfunction

    function automatic [31:0] rcp_seed(input [31:0] a);
        reg [7:0] exp_a;
        begin
            exp_a = get_e(a);
            if (exp_a == 0) return fp32(get_s(a), 8'hFF, 23'h0);
            return fp32(get_s(a), 8'd253 - exp_a, ~get_m(a));
        end
    endfunction
    
    function automatic [31:0] rcp_iter(input [31:0] a, input [31:0] x);
        reg [31:0] ax, two_minus_ax;
        begin
            ax = fp_mul(a, x);
            two_minus_ax = fp_sub_pos(32'h40000000, ax);
            return fp_mul(x, two_minus_ax);
        end
    endfunction

    function automatic [31:0] rsq_seed(input [31:0] a);
        reg [7:0] exp_a;
        reg [8:0] new_exp;
        begin
            exp_a = get_e(a);
            if (exp_a == 0) return fp32(1'b0, 8'hFF, 23'h0);
            new_exp = (9'd381 - {1'b0, exp_a}) >> 1;
            return fp32(1'b0, new_exp[7:0], ~get_m(a));
        end
    endfunction
    
    // 使用改进的 fp_sub 支持负数结果
    function automatic [31:0] rsq_iter(input [31:0] a, input [31:0] x);
        reg [31:0] x2, ax2, three_minus_ax2;
        reg [31:0] half;
        begin
            x2 = fp_mul(x, x);
            ax2 = fp_mul(a, x2);
            three_minus_ax2 = fp_sub(32'h40400000, ax2);  // 使用带符号减法
            x2 = fp_mul(x, three_minus_ax2);
            half = 32'h3F000000;  // 0.5
            return fp_mul(x2, half);
        end
    endfunction

    localparam signed [31:0] CORDIC_K = 32'sd39796;
    localparam signed [31:0] ATAN [0:7] = '{
        32'sd51471, 32'sd30386, 32'sd16054, 32'sd8144,
        32'sd4091, 32'sd2047, 32'sd1024, 32'sd512
    };

    function automatic signed [31:0] fp32_to_q16(input [31:0] f);
        reg s;
        reg [7:0] e;
        reg [22:0] m;
        reg [23:0] mant;
        reg [7:0] shift;
        reg signed [63:0] val;
        begin
            s = get_s(f);
            e = get_e(f);
            m = get_m(f);
            
            if (e == 0) return 32'sd0;
            
            mant = {1'b1, m};
            
            if (e >= 134) begin
                shift = e - 134;
                if (shift > 30) val = 63'sd2147483647;
                else val = mant << shift;
            end else begin
                shift = 134 - e;
                if (shift > 23) val = 63'sd0;
                else val = mant >> shift;
            end
            
            if (val > 63'sd2147483647) val = 63'sd2147483647;
            if (val < -63'sd2147483648) val = -63'sd2147483648;
            
            if (s) val = -val;
            
            return val[31:0];
        end
    endfunction
    
    function automatic [31:0] q16_to_fp32(input signed [31:0] q);
        reg s;
        reg [30:0] abs_q;
        reg [7:0] e;
        reg [22:0] m;
        reg [4:0] leading_zeros;
        begin
            if (q == 0) return 32'h0;
            
            s = q[31];
            abs_q = s ? -q : q;
            
            if (abs_q[30]) leading_zeros = 0;
            else if (abs_q[29]) leading_zeros = 1;
            else if (abs_q[28]) leading_zeros = 2;
            else if (abs_q[27]) leading_zeros = 3;
            else if (abs_q[26]) leading_zeros = 4;
            else if (abs_q[25]) leading_zeros = 5;
            else if (abs_q[24]) leading_zeros = 6;
            else if (abs_q[23]) leading_zeros = 7;
            else if (abs_q[22]) leading_zeros = 8;
            else if (abs_q[21]) leading_zeros = 9;
            else if (abs_q[20]) leading_zeros = 10;
            else if (abs_q[19]) leading_zeros = 11;
            else if (abs_q[18]) leading_zeros = 12;
            else if (abs_q[17]) leading_zeros = 13;
            else if (abs_q[16]) leading_zeros = 14;
            else if (abs_q[15]) leading_zeros = 15;
            else if (abs_q[14]) leading_zeros = 16;
            else if (abs_q[13]) leading_zeros = 17;
            else if (abs_q[12]) leading_zeros = 18;
            else if (abs_q[11]) leading_zeros = 19;
            else if (abs_q[10]) leading_zeros = 20;
            else if (abs_q[9]) leading_zeros = 21;
            else if (abs_q[8]) leading_zeros = 22;
            else if (abs_q[7]) leading_zeros = 23;
            else if (abs_q[6]) leading_zeros = 24;
            else if (abs_q[5]) leading_zeros = 25;
            else if (abs_q[4]) leading_zeros = 26;
            else if (abs_q[3]) leading_zeros = 27;
            else if (abs_q[2]) leading_zeros = 28;
            else if (abs_q[1]) leading_zeros = 29;
            else leading_zeros = 30;
            
            e = 8'd141 - {3'd0, leading_zeros};
            m = (abs_q << (leading_zeros + 1)) >> 8;
            
            return fp32(s, e, m);
        end
    endfunction

    generate
        for (g = 0; g < NUM_LANES; g = g + 1) begin : lane
            wire [31:0] fp32_in = src[g*DATA_W +: DATA_W];
            
            wire [3:0] iter_idx_full = (state >= ST_CORDIC_1 && state <= ST_CORDIC_8) ? 
                                       (state - ST_CORDIC_1) : 4'd0;
            wire [2:0] iter_idx = iter_idx_full[2:0];
            
            wire signed [31:0] x_cur = cordic_x[g];
            wire signed [31:0] y_cur = cordic_y[g];
            wire signed [31:0] z_cur = cordic_z[g];
            wire signed [31:0] x_sh = x_cur >>> iter_idx;
            wire signed [31:0] y_sh = y_cur >>> iter_idx;

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
                    lane_in[g]  <= 32'h0;
                    lane_out[g] <= 32'h0;
                    x_reg[g]    <= 32'h0;
                    cordic_x[g] <= 32'sd0;
                    cordic_y[g] <= 32'sd0;
                    cordic_z[g] <= 32'sd0;
                end else begin
                    case (state)
                        ST_IDLE: begin
                            if (valid_in)
                                lane_in[g] <= fp32_in;
                        end
                        
                        ST_LOAD: begin
                            case (sfu_op)
                                OP_SIN, OP_COS: begin
                                    cordic_z[g] <= fp32_to_q16(fp32_in);
                                end
                                default: ;
                            endcase
                        end
                        
                        ST_INIT: begin
                            case (op_reg)
                                OP_RCP: x_reg[g] <= rcp_seed(lane_in[g]);
                                OP_RSQ: x_reg[g] <= rsq_seed(lane_in[g]);
                                OP_SIN, OP_COS: begin
                                    cordic_x[g] <= CORDIC_K;
                                    cordic_y[g] <= 32'sd0;
                                end
                                default: ;
                            endcase
                        end
                        
                        ST_RCP_1: x_reg[g] <= rcp_iter(lane_in[g], x_reg[g]);
                        ST_RCP_2: x_reg[g] <= rcp_iter(lane_in[g], x_reg[g]);
                        
                        ST_RSQ_1: x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        ST_RSQ_2: x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        ST_RSQ_3: x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        ST_RSQ_4: x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        ST_RSQ_5: x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        ST_RSQ_6: x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        
                        ST_CORDIC_1, ST_CORDIC_2, ST_CORDIC_3, ST_CORDIC_4,
                        ST_CORDIC_5, ST_CORDIC_6, ST_CORDIC_7, ST_CORDIC_8: begin
                            cordic_x[g] <= x_next;
                            cordic_y[g] <= y_next;
                            cordic_z[g] <= z_next;
                        end
                        
                        ST_DONE: begin
                            case (op_reg)
                                OP_RCP: lane_out[g] <= x_reg[g];
                                OP_RSQ: lane_out[g] <= x_reg[g];
                                OP_SIN: lane_out[g] <= q16_to_fp32(cordic_y[g]);
                                OP_COS: lane_out[g] <= q16_to_fp32(cordic_x[g]);
                                default: lane_out[g] <= 32'h0;
                            endcase
                        end
                        
                        default: ;
                    endcase
                end
            end
            
            always @(*) result[g*DATA_W +: DATA_W] = lane_out[g];
        end
    endgenerate

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= ST_IDLE;
            op_reg    <= OP_RCP;
            valid_out <= 1'b0;
        end else begin
            valid_out <= 1'b0;
            
            case (state)
                ST_IDLE: begin
                    if (valid_in) begin
                        op_reg <= sfu_op;
                        state  <= ST_LOAD;
                    end
                end
                
                ST_LOAD:  state <= ST_INIT;
                
                ST_INIT: begin
                    case (op_reg)
                        OP_RCP: state <= ST_RCP_1;
                        OP_RSQ: state <= ST_RSQ_1;
                        OP_SIN, OP_COS: state <= ST_CORDIC_1;
                        default: state <= ST_DONE;
                    endcase
                end
                
                ST_RCP_1: state <= ST_RCP_2;
                ST_RCP_2: state <= ST_DONE;
                
                ST_RSQ_1: state <= ST_RSQ_2;
                ST_RSQ_2: state <= ST_RSQ_3;
                ST_RSQ_3: state <= ST_RSQ_4;
                ST_RSQ_4: state <= ST_RSQ_5;
                ST_RSQ_5: state <= ST_RSQ_6;
                ST_RSQ_6: state <= ST_DONE;
                
                ST_CORDIC_1: state <= ST_CORDIC_2;
                ST_CORDIC_2: state <= ST_CORDIC_3;
                ST_CORDIC_3: state <= ST_CORDIC_4;
                ST_CORDIC_4: state <= ST_CORDIC_5;
                ST_CORDIC_5: state <= ST_CORDIC_6;
                ST_CORDIC_6: state <= ST_CORDIC_7;
                ST_CORDIC_7: state <= ST_CORDIC_8;
                ST_CORDIC_8: state <= ST_DONE;
                
                ST_DONE: begin
                    valid_out <= 1'b1;
                    state     <= ST_IDLE;
                end
                
                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule

