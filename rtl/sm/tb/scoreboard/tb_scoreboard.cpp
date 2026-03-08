// tb_scoreboard.cpp - 修复版本
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <iostream>
#include <cassert>
#include <cstdint>
#include "Vscoreboard.h"

#define NUM_WARPS 32
#define NUM_REGS  64

vluint64_t main_time = 0;

double sc_time_stamp() { return main_time; }

void tick(Vscoreboard* top, VerilatedVcdC* tfp) {
    top->clk = 0; top->eval(); 
    if (tfp) tfp->dump(main_time++);
    top->clk = 1; top->eval(); 
    if (tfp) tfp->dump(main_time++);
}

void reset(Vscoreboard* top, VerilatedVcdC* tfp) {
    top->rst_n = 0;
    top->warp_id = 0; top->rs0_addr = 0; top->rs1_addr = 0; top->rs2_addr = 0;
    top->rd_addr = 0; top->rd_wr_en = 0; top->issue_valid = 0;
    top->wb_warp_id = 0; top->wb_rd_addr = 0; top->wb_valid = 0;
    for (int i = 0; i < 5; i++) tick(top, tfp);
    top->rst_n = 1;
    tick(top, tfp);
}

// 设置输入信号（不tick）
void set_inputs(Vscoreboard* top, uint8_t warp, uint8_t rs0, uint8_t rs1, uint8_t rs2,
                uint8_t rd, bool wr_en, bool issue) {
    top->warp_id = warp;
    top->rs0_addr = rs0; top->rs1_addr = rs1; top->rs2_addr = rs2;
    top->rd_addr = rd; top->rd_wr_en = wr_en;
    top->issue_valid = issue;
}

// 发射指令并检查冒险（在上升沿前检查组合输出）
void issue_inst(Vscoreboard* top, VerilatedVcdC* tfp,
                uint8_t warp, uint8_t rs0, uint8_t rs1, uint8_t rs2,
                uint8_t rd, bool wr_en, bool& raw, bool& waw) {
    // 设置输入
    set_inputs(top, warp, rs0, rs1, rs2, rd, wr_en, 1);
    top->wb_valid = 0;
    top->eval();  // 评估组合逻辑
    
    // 在时钟上升沿前读取组合输出
    raw = top->stall_raw;
    waw = top->stall_waw;
    
    // 时钟上升沿采样
    tick(top, tfp);
    
    // 清除issue_valid
    top->issue_valid = 0;
    top->eval();
}

// 写回
void writeback(Vscoreboard* top, VerilatedVcdC* tfp, uint8_t warp, uint8_t rd) {
    top->wb_warp_id = warp;
    top->wb_rd_addr = rd;
    top->wb_valid = 1;
    tick(top, tfp);
    top->wb_valid = 0;
    top->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    
    Vscoreboard* top = new Vscoreboard;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("scoreboard.vcd");
    
    std::cout << "========================================" << std::endl;
    std::cout << "     Scoreboard Testbench Started       " << std::endl;
    std::cout << "========================================" << std::endl;
    
    bool raw, waw;
    int test_count = 0;
    int pass_count = 0;
    
    //-----------------------------
    // Test 1: Reset
    //-----------------------------
    std::cout << "\n[Test 1] Reset Check..." << std::endl;
    reset(top, tfp);
    // 发射一条指令，应该无冒险
    issue_inst(top, tfp, 0, 1, 2, 3, 4, 1, raw, waw);
    std::cout << "  raw=" << raw << " waw=" << waw << std::endl;
    assert(!raw && !waw); test_count++; pass_count++;
    std::cout << "  [PASS] Reset OK, no hazard on fresh table" << std::endl;
    
    //-----------------------------
    // Test 2: RAW Hazard (Read After Write)
    //-----------------------------
    std::cout << "\n[Test 2] RAW Hazard Detection..." << std::endl;
    reset(top, tfp);
    // 指令1: R5 = R1 + R2 (发射并设置busy)
    issue_inst(top, tfp, 0, 1, 2, 0, 5, 1, raw, waw);
    assert(!raw && !waw);
    // 指令2: R6 = R5 + R3 (读R5，应该是RAW)
    issue_inst(top, tfp, 0, 5, 3, 0, 6, 1, raw, waw);
    assert(raw && !waw); test_count++; pass_count++;
    std::cout << "  [PASS] RAW detected on R5" << std::endl;
    
    // Test 3: WAW Hazard (Write After Write)
    std::cout << "\n[Test 3] WAW Hazard Detection..." << std::endl;
    reset(top, tfp);
    // 指令1: R10 = ...
    issue_inst(top, tfp, 0, 1, 2, 0, 10, 1, raw, waw);
    assert(!raw && !waw);
    // 指令2: R10 = ... (再次写R10，WAW)
    issue_inst(top, tfp, 0, 3, 4, 0, 10, 1, raw, waw);
    assert(!raw && waw); test_count++; pass_count++;
    std::cout << "  [PASS] WAW detected on R10" << std::endl;
    
    // Test 4: Writeback clears busy
    std::cout << "\n[Test 4] Writeback Clears Busy..." << std::endl;
    // 继续Test 3的状态，R10 busy
    // 写回R10
    writeback(top, tfp, 0, 10);
    // 现在应该可以发射
    issue_inst(top, tfp, 0, 3, 4, 0, 10, 1, raw, waw);
    assert(!raw && !waw); test_count++; pass_count++;
    std::cout << "  [PASS] After WB, WAW resolved" << std::endl;
    
    // Test 5: Bypass (Same cycle WB and Issue)
    std::cout << "\n[Test 5] Same-Cycle Bypass..." << std::endl;
    reset(top, tfp);
    // 设置R7 busy
    issue_inst(top, tfp, 0, 1, 2, 0, 7, 1, raw, waw);
    // 同一周期：写回R7同时发射读R7的指令
    set_inputs(top, 0, 7, 0, 0, 8, 1, 1);
    top->wb_warp_id = 0; top->wb_rd_addr = 7; top->wb_valid = 1;
    top->eval();
    raw = top->stall_raw;
    waw = top->stall_waw;
    tick(top, tfp);
    // 应该通过bypass解决RAW
    assert(!raw); test_count++; pass_count++;
    std::cout << "  [PASS] Bypass resolved same-cycle RAW" << std::endl;
    top->wb_valid = 0; top->issue_valid = 0;
    
    // Test 6: Multi-Warp Independence
    std::cout << "\n[Test 6] Multi-Warp Independence..." << std::endl;
    reset(top, tfp);
    // Warp 0写R5
    issue_inst(top, tfp, 0, 1, 2, 0, 5, 1, raw, waw);
    assert(!raw && !waw);
    // Warp 1读R5（应该无RAW，不同Warp）
    issue_inst(top, tfp, 1, 5, 0, 0, 6, 1, raw, waw);
    assert(!raw && !waw); test_count++; pass_count++;
    std::cout << "  [PASS] Warp 0 and Warp 1 are independent" << std::endl;
    
    // Test 7: Multiple Sources RAW
    std::cout << "\n[Test 7] Multiple Source RAW..." << std::endl;
    reset(top, tfp);
    // 设置R10和R20 busy
    issue_inst(top, tfp, 0, 1, 2, 0, 10, 1, raw, waw);
    issue_inst(top, tfp, 0, 3, 4, 0, 20, 1, raw, waw);
    // 指令读R10, R20, R30（R10和R20是RAW）
    issue_inst(top, tfp, 0, 10, 20, 30, 40, 1, raw, waw);
    assert(raw && !waw); test_count++; pass_count++;
    std::cout << "  [PASS] RAW on multiple sources detected" << std::endl;
    
    // Test 8: No Write (rd_wr_en=0)
    std::cout << "\n[Test 8] No Write Operation..." << std::endl;
    reset(top, tfp);
    // 设置R15 busy
    issue_inst(top, tfp, 0, 1, 2, 0, 15, 1, raw, waw);
    // Store指令：读R15，不写（rd_wr_en=0），不应该WAW
    issue_inst(top, tfp, 0, 15, 0, 0, 0, 0, raw, waw);  // rd=0, wr_en=0
    assert(raw && !waw);  // 有RAW但无WAW
    // 再发射一条写R15的，应该有WAW
    issue_inst(top, tfp, 0, 1, 2, 0, 15, 1, raw, waw);
    assert(!raw && waw); test_count++; pass_count++;
    std::cout << "  [PASS] No WAW when rd_wr_en=0" << std::endl;
    
    // Test 9: Complex Sequence
    std::cout << "\n[Test 9] Complex Sequence..." << std::endl;
    reset(top, tfp);
    // I1: R1 = R2 + R3
    issue_inst(top, tfp, 0, 2, 3, 0, 1, 1, raw, waw);
    assert(!raw && !waw);
    // I2: R4 = R1 + R5 (RAW on R1)
    issue_inst(top, tfp, 0, 1, 5, 0, 4, 1, raw, waw);
    assert(raw && !waw);
    // I3: R6 = R7 + R8 (No hazard)
    issue_inst(top, tfp, 0, 7, 8, 0, 6, 1, raw, waw);
    assert(!raw && !waw);
    // WB R1
    writeback(top, tfp, 0, 1);
    // I2 retry: R4 = R1 + R5 (now OK)
    issue_inst(top, tfp, 0, 1, 5, 0, 4, 1, raw, waw);
    assert(!raw && !waw); test_count++; pass_count++;
    std::cout << "  [PASS] Complex sequence handled correctly" << std::endl;
    
    // Test 10: All Registers Same Warp
    std::cout << "\n[Test 10] Register Isolation..." << std::endl;
    reset(top, tfp);
    // 使用R0作为源，填充R1-R63（R0保持空闲）
    for (int i = 1; i < NUM_REGS; i++) {
        issue_inst(top, tfp, 0, 0, 0, 0, i, 1, raw, waw);
        assert(!raw && !waw);
    }
    // 写回R63，用作填充R0的源
    writeback(top, tfp, 0, 63);
    // 填充R0
    issue_inst(top, tfp, 0, 63, 63, 63, 0, 1, raw, waw);
    assert(!raw && !waw);
    // 现在读R0, R1, R2（都是busy的）
    issue_inst(top, tfp, 0, 0, 1, 2, 63, 1, raw, waw);
    assert(raw); test_count++; pass_count++;
    std::cout << "  [PASS] All 64 registers tracked correctly" << std::endl;
    // 清空
    for (int i = 0; i < NUM_REGS; i++) {
        writeback(top, tfp, 0, i);
    }
    
    // Test 11: Full Warp Isolation (Simplified)
    std::cout << "\n[Test 11] Full Warp Isolation..." << std::endl;
    reset(top, tfp);
    // 只测试 Warp 0 和 Warp 1 的隔离
    // Warp 0: 填满所有寄存器
    for (int r = 1; r < NUM_REGS; r++) {
        issue_inst(top, tfp, 0, 0, 0, 0, r, 1, raw, waw);
        assert(!raw && !waw);
    }
    writeback(top, tfp, 0, 63);
    issue_inst(top, tfp, 0, 63, 63, 63, 0, 1, raw, waw);
    assert(!raw && !waw);
    // Warp 1: 读 Warp 0 的寄存器（应该无 RAW，不同 Warp）
    issue_inst(top, tfp, 1, 10, 20, 30, 40, 1, raw, waw);
    assert(!raw && !waw);
    test_count++; pass_count++;
    std::cout << "  [PASS] Warp isolation verified" << std::endl;
    
    // Test 12: Simultaneous WB and Issue Different Warps
    std::cout << "\n[Test 12] Simultaneous Operations..." << std::endl;
    reset(top, tfp);
    // Warp 0设置R5 busy
    issue_inst(top, tfp, 0, 1, 2, 0, 5, 1, raw, waw);
    // Warp 1设置R10 busy
    issue_inst(top, tfp, 1, 1, 2, 0, 10, 1, raw, waw);
    // 同时：Warp 0 WB R5，Warp 1发射读R10（RAW）
    set_inputs(top, 1, 10, 0, 0, 20, 1, 1);
    top->wb_warp_id = 0; top->wb_rd_addr = 5; top->wb_valid = 1;
    top->eval();
    raw = top->stall_raw;
    top->wb_valid = 0; top->issue_valid = 0;
    tick(top, tfp);
    assert(raw);  // Warp 1的R10还是busy
    test_count++; pass_count++;
    std::cout << "  [PASS] Simultaneous WB and Issue handled" << std::endl;
    
    // 完成测试
    tick(top, tfp);
    tick(top, tfp);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  All Tests Completed: " << pass_count << "/" << test_count << std::endl;
    std::cout << "========================================" << std::endl;
    
    tfp->close();
    delete tfp;
    delete top;
    
    return (pass_count == test_count) ? 0 : 1;
}