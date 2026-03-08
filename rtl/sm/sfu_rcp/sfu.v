/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDSIGNAL */
// ============================================================================
// SFU - RCP Only (Corrected Sign Handling)
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

    localparam ST_IDLE   = 2'b00;
    localparam ST_SEED   = 2'b01;
    localparam ST_ITER1  = 2'b10;
    localparam ST_ITER2  = 2'b11;
    
    reg [1:0] state;
    reg [31:0] lane_in  [0:NUM_LANES-1];
    reg [31:0] lane_out [0:NUM_LANES-1];
    reg [31:0] x_reg    [0:NUM_LANES-1];
    
    genvar g;
    assign ready = (state == ST_IDLE);

    // FP pack/unpack
    function automatic [31:0] fp32(input s, input [7:0] e, input [22:0] m);
        fp32 = {s, e, m};
    endfunction
    function automatic [7:0]  get_e(input [31:0] f); get_e = f[30:23]; endfunction
    function automatic [22:0] get_m(input [31:0] f); get_m = f[22:0]; endfunction
    function automatic        get_s(input [31:0] f); get_s = f[31]; endfunction

    // =========================================================================
    // FP Multiply (Correct)
    // =========================================================================
    function automatic [31:0] fp_mul(input [31:0] a, b);
        reg s; 
        reg [8:0] e; 
        reg [47:0] m;
        begin
            if (a[30:0] == 0 || b[30:0] == 0) return 32'h0;
            s = get_s(a) ^ get_s(b);
            e = {1'b0, get_e(a)} + {1'b0, get_e(b)} - 9'd127;
            m = ({1'b1, get_m(a)}) * ({1'b1, get_m(b)});
            if (m[47]) begin e = e + 1; m = m >> 1; end
            return fp32(s, e[7:0], m[45:23]);
        end
    endfunction
    
    // =========================================================================
    // CORRECT FP Subtract for RCP: computes (a - b) where a >= b > 0
    // =========================================================================
    function automatic [31:0] fp_sub_pos(input [31:0] a, b);
        reg [7:0] exp_a, exp_b, exp_diff;
        reg [23:0] man_a, man_b;
        reg [24:0] man_res;  // Extra bit for borrow
        reg [7:0] exp_out;
        reg [22:0] man_out;
        begin
            exp_a = get_e(a);
            exp_b = get_e(b);
            
            // Align mantissas to larger exponent
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
            
            // Subtract
            if (man_a >= man_b)
                man_res = man_a - man_b;
            else
                man_res = man_b - man_a;
            
            // Handle zero
            if (man_res == 0) return 32'h00000000;
            
            // Normalize
            if (man_res[24]) begin
                // Shouldn't happen in subtraction
                man_out = man_res[23:1];
                exp_out = exp_out + 1;
            end else if (man_res[23]) begin
                man_out = man_res[22:0];
            end else begin
                // Count leading zeros - simplified
                if (man_res[22]) begin man_out = {man_res[21:0], 1'b0}; exp_out = exp_out - 1; end
                else if (man_res[21]) begin man_out = {man_res[20:0], 2'b00}; exp_out = exp_out - 2; end
                else if (man_res[20]) begin man_out = {man_res[19:0], 3'b000}; exp_out = exp_out - 3; end
                else begin man_out = {man_res[18:0], 4'b0000}; exp_out = exp_out - 4; end
            end
            
            return fp32(1'b0, exp_out, man_out);
        end
    endfunction

    // =========================================================================
    // RCP Functions
    // =========================================================================
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

    // =========================================================================
    // Lane Logic
    // =========================================================================
    generate
        for (g = 0; g < NUM_LANES; g = g + 1) begin : lane
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    lane_in[g] <= 0;
                    lane_out[g] <= 0;
                    x_reg[g] <= 0;
                end else begin
                    case (state)
                        ST_IDLE: if (valid_in) lane_in[g] <= src[g*DATA_W +: DATA_W];
                        ST_SEED:  x_reg[g] <= rcp_seed(lane_in[g]);
                        ST_ITER1: x_reg[g] <= rcp_iter(lane_in[g], x_reg[g]);
                        ST_ITER2: lane_out[g] <= rcp_iter(lane_in[g], x_reg[g]);
                        default: ;
                    endcase
                end
            end
            always @(*) result[g*DATA_W +: DATA_W] = lane_out[g];
        end
    endgenerate

    // Control
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= ST_IDLE;
            valid_out <= 0;
        end else begin
            valid_out <= 0;
            case (state)
                ST_IDLE: if (valid_in) state <= ST_SEED;
                ST_SEED:  state <= ST_ITER1;
                ST_ITER1: state <= ST_ITER2;
                ST_ITER2: begin valid_out <= 1; state <= ST_IDLE; end
                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
/* verilator lint_on UNUSEDSIGNAL */

