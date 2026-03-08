// tb_warp_scheduler.cpp
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <iostream>
#include <bitset>
#include <cassert>
#include "Vwarp_scheduler.h"

#define NUM_WARPS 32

vluint64_t main_time = 0;

double sc_time_stamp() {
    return main_time;
}

void tick(Vwarp_scheduler* top, VerilatedVcdC* tfp) {
    top->clk = 0;
    top->eval();
    if (tfp) tfp->dump(main_time++);
    
    top->clk = 1;
    top->eval();
    if (tfp) tfp->dump(main_time++);
}

void reset(Vwarp_scheduler* top, VerilatedVcdC* tfp) {
    top->rst_n = 0;
    top->stall = 0;
    top->warp_valid = 0;
    top->warp_ready = 0;
    
    for (int i = 0; i < 5; i++) {
        tick(top, tfp);
    }
    top->rst_n = 1;
    tick(top, tfp);
}

// 辅助函数：设置特定warp为valid和ready
void set_warp(Vwarp_scheduler* top, int warp_id, bool valid, bool ready) {
    if (valid) {
        top->warp_valid |= (1 << warp_id);
    } else {
        top->warp_valid &= ~(1 << warp_id);
    }
    
    if (ready) {
        top->warp_ready |= (1 << warp_id);
    } else {
        top->warp_ready &= ~(1 << warp_id);
    }
}

void clear_all(Vwarp_scheduler* top) {
    top->warp_valid = 0;
    top->warp_ready = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    
    Vwarp_scheduler* top = new Vwarp_scheduler;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    
    top->trace(tfp, 99);
    tfp->open("waveform.vcd");
    
    std::cout << "========================================" << std::endl;
    std::cout << "  Warp Scheduler Testbench (Verilator)  " << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Test 1: Reset
    std::cout << "\n[Test 1] Reset Check..." << std::endl;
    reset(top, tfp);
    assert(top->schedule_valid == 0);
    std::cout << "  [PASS] Reset OK" << std::endl;
    
    // Test 2: Single Warp Ready
    std::cout << "\n[Test 2] Single Warp (ID=5)..." << std::endl;
    clear_all(top);
    set_warp(top, 5, true, true);
    tick(top, tfp);
    assert(top->schedule_valid == 1);
    assert(top->warp_id == 5);
    std::cout << "  [PASS] Selected Warp ID = " << (int)top->warp_id << std::endl;
    
    // Test 3: Multiple Warps - Priority Check (lower ID wins)
    std::cout << "\n[Test 3] Multiple Warps Priority..." << std::endl;
    clear_all(top);
    set_warp(top, 10, true, true);  // ID 10
    set_warp(top, 5, true, true);   // ID 5 (lower, should win)
    set_warp(top, 20, true, true);  // ID 20
    tick(top, tfp);
    assert(top->schedule_valid == 1);
    assert(top->warp_id == 5);  // 最低ID优先
    std::cout << "  [PASS] Selected lowest ID = " << (int)top->warp_id << std::endl;
    
    // Test 4: Only Valid but not Ready
    std::cout << "\n[Test 4] Valid but not Ready..." << std::endl;
    clear_all(top);
    set_warp(top, 3, true, false);  // Valid but not ready
    tick(top, tfp);
    assert(top->schedule_valid == 0);
    std::cout << "  [PASS] No valid selection when not ready" << std::endl;
    
    // Test 5: Stall Functionality
    std::cout << "\n[Test 5] Stall Functionality..." << std::endl;
    clear_all(top);
    set_warp(top, 7, true, true);
    tick(top, tfp);  // 第一次选择ID=7
    assert(top->warp_id == 7);
    
    // 启用stall，改变输入
    top->stall = 1;
    set_warp(top, 2, true, true);  // 添加更高优先级的warp
    tick(top, tfp);  // 应该保持ID=7
    assert(top->warp_id == 7);
    assert(top->schedule_valid == 1);
    std::cout << "  [PASS] Stall holds selection (ID=" << (int)top->warp_id << ")" << std::endl;
    
    // 解除stall，应该切换到ID=2
    top->stall = 0;
    tick(top, tfp);
    assert(top->warp_id == 2);
    std::cout << "  [PASS] After stall release, selected ID = " << (int)top->warp_id << std::endl;
    
    // Test 6: Dynamic Change
    std::cout << "\n[Test 6] Dynamic Warp Change..." << std::endl;
    clear_all(top);
    set_warp(top, 15, true, true);
    tick(top, tfp);
    assert(top->warp_id == 15);
    
    // 移除warp 15，添加warp 8
    set_warp(top, 15, false, false);
    set_warp(top, 8, true, true);
    tick(top, tfp);
    assert(top->warp_id == 8);
    std::cout << "  [PASS] Dynamic change to ID = " << (int)top->warp_id << std::endl;
    
    // Test 7: All Warps
    std::cout << "\n[Test 7] All 32 Warps..." << std::endl;
    clear_all(top);
    top->warp_valid = 0xFFFFFFFF;
    top->warp_ready = 0xFFFFFFFF;
    tick(top, tfp);
    assert(top->warp_id == 0);  // 最低ID
    std::cout << "  [PASS] All warps ready, selected ID = " << (int)top->warp_id << std::endl;
    
    // Test 8: No Valid Warp
    std::cout << "\n[Test 8] No Valid Warp..." << std::endl;
    clear_all(top);
    tick(top, tfp);
    assert(top->schedule_valid == 0);
    std::cout << "  [PASS] No valid selection when empty" << std::endl;
    
    // Test 9: Random Pattern
    std::cout << "\n[Test 9] Random Pattern..." << std::endl;
    clear_all(top);
    // 设置warp 3, 7, 12, 25
    set_warp(top, 3, true, true);
    set_warp(top, 7, true, true);
    set_warp(top, 12, true, true);
    set_warp(top, 25, true, true);
    tick(top, tfp);
    assert(top->warp_id == 3);
    std::cout << "  [PASS] Random pattern, selected lowest ID = " << (int)top->warp_id << std::endl;
    
    // 移除3，检查是否选7
    set_warp(top, 3, false, false);
    tick(top, tfp);
    assert(top->warp_id == 7);
    std::cout << "  [PASS] After remove 3, selected ID = " << (int)top->warp_id << std::endl;
    
    // 多周期stall测试
    std::cout << "\n[Test 10] Multi-cycle Stall..." << std::endl;
    clear_all(top);
    set_warp(top, 9, true, true);
    tick(top, tfp);
    uint8_t saved_id = top->warp_id;
    
    top->stall = 1;
    for (int i = 0; i < 10; i++) {
        set_warp(top, i, true, true);  // 不断添加更高优先级warp
        tick(top, tfp);
        assert(top->warp_id == saved_id);
        assert(top->schedule_valid == 1);
    }
    std::cout << "  [PASS] 10-cycle stall maintained ID = " << (int)saved_id << std::endl;
    
    top->stall = 0;
    tick(top, tfp);
    assert(top->warp_id == 0);  // 现在0是最高优先级
    std::cout << "  [PASS] After long stall, selected ID = " << (int)top->warp_id << std::endl;
    
    // 完成
    tick(top, tfp);
    tick(top, tfp);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  All Tests PASSED! (" << 10 << "/10)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    tfp->close();
    delete tfp;
    delete top;
    
    return 0;
}