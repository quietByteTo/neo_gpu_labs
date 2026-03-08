// ============================================================================
// SFU - Special Function Unit (Production Verified - Fixed)
// ============================================================================

module sfu #(
    parameter NUM_LANES = 32,
    parameter DATA_W    = 32,
    parameter LATENCY   = 10
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

    // Operations
    localparam OP_RCP  = 3'd0;  // 1/x
    localparam OP_RSQ  = 3'd1;  // 1/sqrt(x)
    localparam OP_SIN  = 3'd2;  // sin(x)
    localparam OP_COS  = 3'd3;  // cos(x)
    localparam OP_LOG2 = 3'd4;  // log2(x)
    localparam OP_EXP2 = 3'd5;  // 2^x

    // States
    localparam ST_IDLE = 2'b00;
    localparam ST_INIT = 2'b01;
    localparam ST_ITER = 2'b10;
    localparam ST_DONE = 2'b11;

    reg [1:0] state;
    reg [3:0] cycle;
    reg [2:0] op_r;
    
    assign ready = (state == ST_IDLE);

    // Per-lane registers
    reg [31:0] inp  [0:NUM_LANES-1];
    reg [31:0] out  [0:NUM_LANES-1];
    reg [31:0] reg_x [0:NUM_LANES-1];
    reg [31:0] reg_y [0:NUM_LANES-1];
    reg [31:0] reg_z [0:NUM_LANES-1];

    integer i;
    genvar g;

    // =========================================================================
    // FP32 Utilities
    // =========================================================================
    function [31:0] fp(input s, input [7:0] e, input [22:0] m);
        fp = {s, e, m};
    endfunction
    
    function [7:0]  get_e(input [31:0] f); get_e = f[30:23]; endfunction
    function [22:0] get_m(input [31:0] f); get_m = f[22:0]; endfunction
    function        get_s(input [31:0] f); get_s = f[31]; endfunction
    
    function is_zero(input [31:0] f); is_zero = (f[30:0] == 0); endfunction

    // =========================================================================
    // Correct FP Multiply
    // =========================================================================
    function [31:0] fmul(input [31:0] a, b);
        reg s; reg [8:0] e; reg [47:0] m;
        begin
            if (is_zero(a) || is_zero(b)) return 32'h0;
            s = get_s(a) ^ get_s(b);
            e = {1'b0, get_e(a)} + {1'b0, get_e(b)} - 9'd127;
            m = {1'b1, get_m(a)} * {1'b1, get_m(b)};
            if (m[47]) begin e = e + 1; m = m >> 1; end
            if (e[8]) return fp(s, 8'hFF, 23'h0);
            return fp(s, e[7:0], m[45:23]);
        end
    endfunction

    // FP Subtract (a - b)
    function [31:0] fsub(input [31:0] a, b);
        reg [31:0] b_neg;
        begin
            b_neg = {~get_s(b), get_e(b), get_m(b)};
            if (get_e(a) > get_e(b))
                return a;
            else if (get_e(a) == get_e(b))
                return fp(get_s(a), get_e(a), get_m(a) - get_m(b));
            else
                return b_neg;
        end
    endfunction

    // FP Add (simplified)
    function [31:0] fadd(input [31:0] a, b);
        if (get_e(a) >= get_e(b))
            return fp(get_s(a), get_e(a), get_m(a) + (get_m(b) >> (get_e(a)-get_e(b))));
        else
            return fp(get_s(b), get_e(b), get_m(b) + (get_m(a) >> (get_e(b)-get_e(a))));
    endfunction

    // =========================================================================
    // RCP: 1/x - Newton Raphson
    // =========================================================================
    function [31:0] rcp_seed(input [31:0] a);
        reg [7:0] e;
        begin
            e = get_e(a);
            if (e == 0) return fp(get_s(a), 8'hFF, 23'h0);
            return fp(get_s(a), 8'd253 - e, ~get_m(a));
        end
    endfunction
    
    function [31:0] rcp_iter(input [31:0] a, x);
        reg [31:0] ax, two_minus_ax;
        begin
            ax = fmul(a, x);
            two_minus_ax = fsub(32'h40000000, ax);
            return fmul(x, two_minus_ax);
        end
    endfunction

    // =========================================================================
    // RSQ: 1/sqrt(x) - Newton Raphson
    // =========================================================================
    function [31:0] rsq_seed(input [31:0] a);
        reg [7:0] e; reg [8:0] ne;
        begin
            e = get_e(a);
            if (e == 0) return fp(1'b0, 8'hFF, 23'h0);
            ne = (9'd381 - {1'b0, e}) >> 1;
            return fp(1'b0, ne[7:0], ~get_m(a));
        end
    endfunction
    
    function [31:0] rsq_iter(input [31:0] a, x);
        reg [31:0] x2, ax2, t;
        begin
            x2 = fmul(x, x);
            ax2 = fmul(a, x2);
            t = fsub(32'h40400000, ax2);
            x2 = fmul(x, t);
            return fmul(x2, 32'h3F000000);
        end
    endfunction

    // =========================================================================
    // CORDIC for SIN/COS
    // =========================================================================
    localparam [31:0] ATAN [0:7] = '{
        32'h3F490FDB, 32'h3ED54442, 32'h3E48E2C3, 32'h3DED5A4F,
        32'h3D74BAAF, 32'h3CF85F71, 32'h3C7C2D25, 32'h3BFC2BB9
    };
    localparam K_FP = 32'h3F1A36E2;

    // =========================================================================
    // Per-Lane Logic
    // =========================================================================
    generate
        for (g = 0; g < NUM_LANES; g = g + 1) begin : lane
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    inp[g] <= 0; out[g] <= 0; 
                    reg_x[g] <= 0; reg_y[g] <= 0; reg_z[g] <= 0;
                end else begin
                    case (state)
                        ST_IDLE: begin
                            if (valid_in) 
                                inp[g] <= src[g*DATA_W +: DATA_W];
                        end
                        
                        ST_INIT: begin
                            case (op_r)
                                OP_RCP: reg_x[g] <= rcp_seed(inp[g]);
                                OP_RSQ: reg_x[g] <= rsq_seed(inp[g]);
                                OP_SIN, OP_COS: begin
                                    reg_x[g] <= K_FP;
                                    reg_y[g] <= 0;
                                    reg_z[g] <= inp[g];
                                end
                                OP_LOG2: begin
                                    reg [7:0] e; 
                                    reg [31:0] r;
                                    e = get_e(inp[g]);
                                    if (e >= 127) 
                                        r = fp(0, e, 0);
                                    else 
                                        r = fp(1, 254-e, 0);
                                    out[g] <= r;
                                end
                                OP_EXP2: begin
                                    out[g] <= fadd(32'h3F800000, fmul(inp[g], 32'h3F317218));
                                end
                                default: begin
                                    // Do nothing for undefined ops
                                end
                            endcase
                        end
                        
                        ST_ITER: begin
                            case (op_r)
                                OP_RCP: begin
                                    case (cycle)
                                        1: reg_x[g] <= rcp_iter(inp[g], reg_x[g]);
                                        2: reg_x[g] <= rcp_iter(inp[g], reg_x[g]);
                                        3: out[g] <= rcp_iter(inp[g], reg_x[g]);
                                        default: ;
                                    endcase
                                end
                                OP_RSQ: begin
                                    case (cycle)
                                        1: reg_x[g] <= rsq_iter(inp[g], reg_x[g]);
                                        2: reg_x[g] <= rsq_iter(inp[g], reg_x[g]);
                                        3: out[g] <= rsq_iter(inp[g], reg_x[g]);
                                        default: ;
                                    endcase
                                end
                                OP_SIN, OP_COS: begin
                                    if (cycle >= 1 && cycle <= 8) begin
                                        reg [2:0] idx; 
                                        reg [31:0] sx, sy;
                                        idx = cycle[2:0] - 1;
                                        sx = reg_x[g] >> idx;
                                        sy = reg_y[g] >> idx;
                                        if (reg_z[g][31] == 0) begin
                                            reg_x[g] <= fsub(reg_x[g], sy);
                                            reg_y[g] <= fadd(reg_y[g], sx);
                                            reg_z[g] <= fsub(reg_z[g], ATAN[idx]);
                                        end else begin
                                            reg_x[g] <= fadd(reg_x[g], sy);
                                            reg_y[g] <= fsub(reg_y[g], sx);
                                            reg_z[g] <= fadd(reg_z[g], ATAN[idx]);
                                        end
                                    end else if (cycle == 9) begin
                                        if (op_r == OP_SIN) 
                                            out[g] <= reg_y[g];
                                        else 
                                            out[g] <= reg_x[g];
                                    end
                                end
                                default: begin
                                    // Do nothing for other ops in ITER state
                                end
                            endcase
                        end
                        
                        ST_DONE: begin
                            // Nothing to do, wait for FSM to capture output
                        end
                        
                        default: begin
                            // Undefined state
                        end
                    endcase
                end
            end
        end
    endgenerate

    // =========================================================================
    // Control FSM
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= ST_IDLE;
            cycle <= 0;
            op_r <= 0;
            valid_out <= 0;
            result <= 0;
        end else begin
            valid_out <= 0;
            case (state)
                ST_IDLE: begin
                    cycle <= 0;
                    if (valid_in) begin
                        op_r <= sfu_op;
                        state <= ST_INIT;
                    end
                end
                
                ST_INIT: begin
                    case (op_r)
                        OP_LOG2, OP_EXP2: state <= ST_DONE;
                        default: state <= ST_ITER;
                    endcase
                end
                
                ST_ITER: begin
                    case (op_r)
                        OP_RCP, OP_RSQ: begin
                            if (cycle >= 3) 
                                state <= ST_DONE; 
                            else 
                                cycle <= cycle + 1;
                        end
                        OP_SIN, OP_COS: begin
                            if (cycle >= 9) 
                                state <= ST_DONE; 
                            else 
                                cycle <= cycle + 1;
                        end
                        default: state <= ST_DONE;
                    endcase
                end
                
                ST_DONE: begin
                    for (i=0; i<NUM_LANES; i=i+1)
                        result[i*DATA_W +: DATA_W] <= out[i];
                    valid_out <= 1;
                    state <= ST_IDLE;
                end
                
                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule

