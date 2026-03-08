/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */
// ============================================================================
// SFU - RCP and RSQ (5 iterations for RSQ to handle large numbers)
// ============================================================================

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

    localparam OP_RCP = 3'd0;
    localparam OP_RSQ = 3'd1;
    
    // Extended states for 5 RSQ iterations
    localparam ST_IDLE   = 3'b000;
    localparam ST_SEED   = 3'b001;
    localparam ST_ITER1  = 3'b010;
    localparam ST_ITER2  = 3'b011;
    localparam ST_ITER3  = 3'b100;
    localparam ST_ITER4  = 3'b101;
    localparam ST_ITER5  = 3'b110;  // 5th iteration for RSQ
    localparam ST_DONE   = 3'b111;
    
    reg [2:0] state;
    reg [31:0] lane_in  [0:NUM_LANES-1];
    reg [31:0] lane_out [0:NUM_LANES-1];
    reg [31:0] x_reg    [0:NUM_LANES-1];
    reg [2:0] op_reg;
    
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

    // RCP
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

    // RSQ
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
    
    function automatic [31:0] rsq_iter(input [31:0] a, input [31:0] x);
        reg [31:0] x2, ax2, three_minus_ax2;
        begin
            x2 = fp_mul(x, x);
            ax2 = fp_mul(a, x2);
            three_minus_ax2 = fp_sub_pos(32'h40400000, ax2);
            x2 = fp_mul(x, three_minus_ax2);
            return fp_mul(x2, 32'h3F000000);
        end
    endfunction

    // Lane Logic
    generate
        for (g = 0; g < NUM_LANES; g = g + 1) begin : lane
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    lane_in[g] <= 0; lane_out[g] <= 0; x_reg[g] <= 0;
                end else begin
                    case (state)
                        ST_IDLE: if (valid_in) lane_in[g] <= src[g*DATA_W +: DATA_W];
                        
                        ST_SEED: begin
                            if (op_reg == OP_RCP)
                                x_reg[g] <= rcp_seed(lane_in[g]);
                            else
                                x_reg[g] <= rsq_seed(lane_in[g]);
                        end
                        
                        ST_ITER1: begin
                            if (op_reg == OP_RCP)
                                x_reg[g] <= rcp_iter(lane_in[g], x_reg[g]);
                            else
                                x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        end
                        
                        ST_ITER2: begin
                            if (op_reg == OP_RCP)
                                x_reg[g] <= rcp_iter(lane_in[g], x_reg[g]);
                            else
                                x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        end
                        
                        ST_ITER3: begin
                            if (op_reg == OP_RSQ)
                                x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        end
                        
                        ST_ITER4: begin
                            if (op_reg == OP_RSQ)
                                x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        end
                        
                        ST_ITER5: begin
                            if (op_reg == OP_RSQ)
                                x_reg[g] <= rsq_iter(lane_in[g], x_reg[g]);
                        end
                        
                        ST_DONE: lane_out[g] <= x_reg[g];
                        
                        default: ;
                    endcase
                end
            end
            always @(*) result[g*DATA_W +: DATA_W] = lane_out[g];
        end
    endgenerate

    // Control FSM
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= ST_IDLE;
            op_reg <= OP_RCP;
            valid_out <= 0;
        end else begin
            valid_out <= 0;
            case (state)
                ST_IDLE: if (valid_in) begin op_reg <= sfu_op; state <= ST_SEED; end
                ST_SEED: state <= ST_ITER1;
                ST_ITER1: state <= ST_ITER2;
                ST_ITER2: begin
                    if (op_reg == OP_RCP) state <= ST_DONE;
                    else state <= ST_ITER3;
                end
                ST_ITER3: state <= ST_ITER4;
                ST_ITER4: state <= ST_ITER5;
                ST_ITER5: state <= ST_DONE;
                ST_DONE: begin valid_out <= 1'b1; state <= ST_IDLE; end
                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_on UNUSEDSIGNAL */

