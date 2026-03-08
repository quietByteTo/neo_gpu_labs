// register_file.v - 修复读实例选择
module register_file #(
    parameter NUM_WARPS  = 32,
    parameter NUM_REGS   = 64,
    parameter NUM_LANES  = 32,
    parameter DATA_W     = 32,
    parameter NUM_BANKS  = 4,
    parameter WARP_ID_W  = $clog2(NUM_WARPS),
    parameter REG_ADDR_W = $clog2(NUM_REGS)
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    input  wire [WARP_ID_W-1:0]      rd_warp_id [0:3],
    input  wire [REG_ADDR_W-1:0]     rd_addr    [0:3],
    input  wire                      rd_en      [0:3],
    output reg  [NUM_LANES*DATA_W-1:0] rd_data  [0:3],
    
    input  wire [WARP_ID_W-1:0]      wr_warp_id [0:1],
    input  wire [REG_ADDR_W-1:0]     wr_addr    [0:1],
    input  wire [NUM_LANES*DATA_W-1:0] wr_data  [0:1],
    input  wire [NUM_LANES-1:0]      wr_mask    [0:1],
    input  wire                      wr_en      [0:1],
    
    output wire                      bank_conflict
);

    // Bank Selection
    wire [1:0] rd_bank_sel [0:3];
    wire [1:0] wr_bank_sel [0:1];
    
    genvar g;
    generate
        for (g = 0; g < 4; g = g + 1) begin : rd_bank_calc
            assign rd_bank_sel[g] = rd_warp_id[g][1:0] ^ rd_addr[g][1:0];
        end
        for (g = 0; g < 2; g = g + 1) begin : wr_bank_calc
            assign wr_bank_sel[g] = wr_warp_id[g][1:0] ^ wr_addr[g][1:0];
        end
    endgenerate
    
    localparam BANK_ADDR_W = WARP_ID_W + REG_ADDR_W - 2;  // 9 bits
    
    wire [BANK_ADDR_W-1:0] rd_phy_addr [0:3];
    wire [BANK_ADDR_W-1:0] wr_phy_addr [0:1];
    
    generate
        for (g = 0; g < 4; g = g + 1) begin : rd_phy_calc
            assign rd_phy_addr[g] = {rd_warp_id[g], rd_addr[g][5:2]};
        end
        for (g = 0; g < 2; g = g + 1) begin : wr_phy_calc
            assign wr_phy_addr[g] = {wr_warp_id[g], wr_addr[g][5:2]};
        end
    endgenerate
    
    // 每个 bank 2 个 sram_2p 实例，通过地址 LSB 选择
    // 实例 0: 地址 LSB = 0 的数据
    // 实例 1: 地址 LSB = 1 的数据
    
    reg [NUM_LANES*DATA_W-1:0] sram_wdata [0:3][0:1];
    reg [BANK_ADDR_W-2:0]      sram_waddr [0:3][0:1];
    reg                        sram_wr_en [0:3][0:1];
    reg                        sram_rd_en [0:3][0:1];
    reg [BANK_ADDR_W-2:0]      sram_raddr [0:3][0:1];
    
    wire [NUM_LANES*DATA_W-1:0] sram_a_rdata [0:3][0:1];
    wire [NUM_LANES*DATA_W-1:0] sram_b_rdata [0:3][0:1];
    
    genvar b, s;
    generate
        for (b = 0; b < 4; b = b + 1) begin : bank_gen
            for (s = 0; s < 2; s = s + 1) begin : inst_gen
                sram_2p #(
                    .ADDR_W(BANK_ADDR_W-1),
                    .DATA_W(NUM_LANES*DATA_W),
                    .DEPTH(256)
                ) u_sram (
                    .clk(clk),
                    .rst_n(rst_n),
                    .a_ce(sram_rd_en[b][s] || sram_wr_en[b][s]),
                    .a_wr_en(sram_wr_en[b][s]),
                    .a_addr(sram_wr_en[b][s] ? sram_waddr[b][s] : sram_raddr[b][s]),
                    .a_wdata(sram_wdata[b][s]),
                    .a_rdata(sram_a_rdata[b][s]),
                    .b_ce(sram_rd_en[b][s]),
                    .b_addr(sram_raddr[b][s]),
                    .b_rdata(sram_b_rdata[b][s])
                );
            end
        end
    endgenerate
    
    // 控制逻辑
    integer i;
    always @(*) begin
        for (i = 0; i < 4; i = i + 1) begin
            sram_wr_en[i][0] = 1'b0;
            sram_wr_en[i][1] = 1'b0;
            sram_rd_en[i][0] = 1'b0;
            sram_rd_en[i][1] = 1'b0;
        end
        
        // 读端口分配：根据地址 LSB 选择实例
        for (i = 0; i < 4; i = i + 1) begin
            if (rd_en[i]) begin
                case (i)
                    0, 2: begin  // 使用实例 0 或 1，取决于地址 LSB
                        sram_rd_en[rd_bank_sel[i]][rd_phy_addr[i][0]] = 1'b1;
                        sram_raddr[rd_bank_sel[i]][rd_phy_addr[i][0]] = rd_phy_addr[i][BANK_ADDR_W-1:1];
                    end
                    1, 3: begin  // 同样根据地址 LSB 选择
                        sram_rd_en[rd_bank_sel[i]][rd_phy_addr[i][0]] = 1'b1;
                        sram_raddr[rd_bank_sel[i]][rd_phy_addr[i][0]] = rd_phy_addr[i][BANK_ADDR_W-1:1];
                    end
                endcase
            end
        end
        
        // 写端口分配
        if (wr_en[0]) begin
            sram_wr_en[wr_bank_sel[0]][wr_phy_addr[0][0]] = 1'b1;
            sram_waddr[wr_bank_sel[0]][wr_phy_addr[0][0]] = wr_phy_addr[0][BANK_ADDR_W-1:1];
            sram_wdata[wr_bank_sel[0]][wr_phy_addr[0][0]] = wr_data[0];
        end
        
        if (wr_en[1]) begin
            sram_wr_en[wr_bank_sel[1]][wr_phy_addr[1][0]] = 1'b1;
            sram_waddr[wr_bank_sel[1]][wr_phy_addr[1][0]] = wr_phy_addr[1][BANK_ADDR_W-1:1];
            sram_wdata[wr_bank_sel[1]][wr_phy_addr[1][0]] = wr_data[1];
        end
    end
    
    // 读数据选择和 Bypass
    reg [NUM_LANES*DATA_W-1:0] raw_rd_data [0:3];
    integer r, w;
    
    always @(*) begin
        // 从 SRAM 获取数据：根据地址 LSB 选择实例
        for (r = 0; r < 4; r = r + 1) begin
            case (r)
                0: raw_rd_data[0] = (rd_phy_addr[0][0]) ? 
                                    sram_a_rdata[rd_bank_sel[0]][1] : 
                                    sram_a_rdata[rd_bank_sel[0]][0];
                1: raw_rd_data[1] = (rd_phy_addr[1][0]) ? 
                                    sram_a_rdata[rd_bank_sel[1]][1] : 
                                    sram_a_rdata[rd_bank_sel[1]][0];
                2: raw_rd_data[2] = (rd_phy_addr[2][0]) ? 
                                    sram_b_rdata[rd_bank_sel[2]][1] : 
                                    sram_b_rdata[rd_bank_sel[2]][0];
                3: raw_rd_data[3] = (rd_phy_addr[3][0]) ? 
                                    sram_b_rdata[rd_bank_sel[3]][1] : 
                                    sram_b_rdata[rd_bank_sel[3]][0];
            endcase
        end
        
        // Bypass 检查
        for (r = 0; r < 4; r = r + 1) begin
            rd_data[r] = raw_rd_data[r];
            if (rd_en[r]) begin
                for (w = 0; w < 2; w = w + 1) begin
                    if (wr_en[w] && rd_warp_id[r] == wr_warp_id[w] && 
                        rd_addr[r] == wr_addr[w]) begin
                        rd_data[r] = wr_data[w];
                    end
                end
            end
        end
    end
    
    assign bank_conflict = ((rd_bank_sel[0] == rd_bank_sel[1]) && rd_en[0] && rd_en[1]) ||
                           ((rd_bank_sel[0] == rd_bank_sel[2]) && rd_en[0] && rd_en[2]) ||
                           ((rd_bank_sel[0] == rd_bank_sel[3]) && rd_en[0] && rd_en[3]) ||
                           ((rd_bank_sel[1] == rd_bank_sel[2]) && rd_en[1] && rd_en[2]) ||
                           ((rd_bank_sel[1] == rd_bank_sel[3]) && rd_en[1] && rd_en[3]) ||
                           ((rd_bank_sel[2] == rd_bank_sel[3]) && rd_en[2] && rd_en[3]);

endmodule

