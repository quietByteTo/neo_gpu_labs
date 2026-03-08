// tb_lds_unit.cpp
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <iostream>
#include <cassert>
#include <cstdint>
#include "Vlds_unit.h"

#define NUM_LANES 32
#define NUM_BANKS 32
#define DATA_W    32

vluint64_t main_time = 0;
double sc_time_stamp() { return main_time; }

void tick(Vlds_unit* top, VerilatedVcdC* tfp) {
    top->clk = 0; top->eval();
    if (tfp) tfp->dump(main_time++);
    top->clk = 1; top->eval();
    if (tfp) tfp->dump(main_time++);
}

// 辅助函数：设置 lane 请求
void set_lane_req(Vlds_unit* top, int lane, uint16_t addr, uint32_t data, bool wr, bool valid) {
    top->lds_addr[lane] = addr;
    top->lds_wdata[lane] = data;
    top->lds_wr_en[lane] = wr;
    top->lds_valid[lane] = valid;
}

// 清除所有请求
void clear_all_req(Vlds_unit* top) {
    for (int i = 0; i < NUM_LANES; i++) {
        top->lds_valid[i] = 0;
        top->lds_wr_en[i] = 0;
    }
    top->atomic_en = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    
    Vlds_unit* top = new Vlds_unit;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("lds_unit.vcd");
    
    std::cout << "========================================" << std::endl;
    std::cout << "      LDS Unit Testbench Started        " << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 初始化
    top->rst_n = 0;
    top->atomic_op = 0;
    top->atomic_en = 0;
    clear_all_req(top);
    
    for (int i = 0; i < 5; i++) tick(top, tfp);
    top->rst_n = 1;
    tick(top, tfp);
    
    int test_count = 0;
    int pass_count = 0;
    
    // Test 1: Basic Write and Read
    std::cout << "\n[Test 1] Basic Write/Read..." << std::endl;
    // Lane 0 writes to addr 0x100
    set_lane_req(top, 0, 0x100, 0xDEADBEEF, 1, 1);
    tick(top, tfp);
    clear_all_req(top);
    tick(top, tfp);  // 等待响应
    
    // Lane 0 reads from addr 0x100
    set_lane_req(top, 0, 0x100, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata_valid == 1);
    assert(top->lds_rdata[0] == 0xDEADBEEF);
    assert(top->lds_ready[0] == 1);
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Basic write/read OK" << std::endl;
    
    // Test 2: Stride-1 Conflict-Free Access (32 consecutive addresses)
    std::cout << "\n[Test 2] Stride-1 Conflict-Free Access..." << std::endl;
    // 32 lanes access 32 consecutive words (addr 0x200-0x27F)
    // Bank = addr[6:2], consecutive addresses have different bank[6:2]
    for (int i = 0; i < NUM_LANES; i++) {
        set_lane_req(top, i, 0x200 + i*4, 0x10000000 + i, 1, 1);
    }
    tick(top, tfp);
    clear_all_req(top);
    tick(top, tfp);
    
    // Read back
    for (int i = 0; i < NUM_LANES; i++) {
        set_lane_req(top, i, 0x200 + i*4, 0, 0, 1);
    }
    tick(top, tfp);
    assert(top->conflict_detected == 0);  // No conflict for stride-1
    for (int i = 0; i < NUM_LANES; i++) {
        assert(top->lds_ready[i] == 1);
        assert(top->lds_rdata[i] == (uint32_t)(0x10000000 + i));
    }
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Stride-1 access conflict-free" << std::endl;
    
    // Test 3: Bank Conflict Detection
    std::cout << "\n[Test 3] Bank Conflict Detection..." << std::endl;
    // All lanes access same bank (addr[6:2] = 0)
    // addr 0x000, 0x080, 0x100, 0x180... all have bank = 0
    for (int i = 0; i < NUM_LANES; i++) {
        set_lane_req(top, i, i * 0x80, 0x20000000 + i, 1, 1);  // Same bank 0
    }
    tick(top, tfp);
    assert(top->conflict_detected == 1);
    assert(top->conflict_count == 31);  // 31 conflicts (lane 0 wins)
    assert(top->lds_ready[0] == 1);     // Lane 0 granted
    assert(top->lds_ready[1] == 0);     // Lane 1 stalled
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Conflict detected, lane 0 wins" << std::endl;
    
    // Test 4: Conflict Replay
    std::cout << "\n[Test 4] Conflict Replay..." << std::endl;
    // First: lane 0 and 1 both try to write same bank
    // 0x300[6:2] = 0, 0x301[6:2] = 0 (same bank!)
    set_lane_req(top, 0, 0x300, 0xAAAA0000, 1, 1);
    set_lane_req(top, 1, 0x301, 0xBBBB1111, 1, 1);  // 0x301[6:2] = 0, same bank as 0x300
    tick(top, tfp);
    assert(top->lds_ready[0] == 1);
    assert(top->lds_ready[1] == 0);  // Conflict
    clear_all_req(top);
    tick(top, tfp);

    // Replay: lane 1 retries
    set_lane_req(top, 1, 0x301, 0xBBBB1111, 1, 1);
    tick(top, tfp);
    assert(top->lds_ready[1] == 1);  // Now granted
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Conflict replay successful" << std::endl;
    
    // Test 5: Atomic ADD
    std::cout << "\n[Test 5] Atomic ADD..." << std::endl;
    // Initialize: write 100 to addr 0x400
    set_lane_req(top, 0, 0x400, 100, 1, 1);
    tick(top, tfp);
    clear_all_req(top);
    tick(top, tfp);
    
    // Atomic ADD: add 50, return old value (100)
    top->atomic_op = 1;  // ADD
    top->atomic_en = 1;
    set_lane_req(top, 0, 0x400, 50, 1, 1);  // wr_en=1 for atomic
    tick(top, tfp);
    assert(top->lds_rdata[0] == 100);  // Return old value
    clear_all_req(top);
    top->atomic_en = 0;
    tick(top, tfp);
    
    // Verify: read should be 150
    set_lane_req(top, 0, 0x400, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 150);
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Atomic ADD OK" << std::endl;
    
    // Test 6: Atomic MIN/MAX
    std::cout << "\n[Test 6] Atomic MIN/MAX..." << std::endl;
    // Write 100
    set_lane_req(top, 0, 0x500, 100, 1, 1);
    tick(top, tfp);
    clear_all_req(top);
    tick(top, tfp);
    
    // Atomic MIN with 50 -> result 50
    top->atomic_op = 2;  // MIN
    top->atomic_en = 1;
    set_lane_req(top, 0, 0x500, 50, 1, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 100);  // Old value
    clear_all_req(top);
    top->atomic_en = 0;
    tick(top, tfp);
    
    // Read verify
    set_lane_req(top, 0, 0x500, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 50);
    clear_all_req(top);
    
    // Atomic MAX with 200 -> result 200
    top->atomic_op = 3;  // MAX
    top->atomic_en = 1;
    set_lane_req(top, 0, 0x500, 200, 1, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 50);  // Old value
    clear_all_req(top);
    top->atomic_en = 0;
    tick(top, tfp);
    
    // Read verify
    set_lane_req(top, 0, 0x500, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 200);
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Atomic MIN/MAX OK" << std::endl;
    
    // Test 7: Atomic AND/OR/XOR
    std::cout << "\n[Test 7] Atomic Bitwise Ops..." << std::endl;
    // Write 0xFF00FF00
    set_lane_req(top, 0, 0x600, 0xFF00FF00, 1, 1);
    tick(top, tfp);
    clear_all_req(top);
    tick(top, tfp);
    
    // AND with 0x0F0F0F0F -> 0x0F000F00
    top->atomic_op = 4;  // AND
    top->atomic_en = 1;
    set_lane_req(top, 0, 0x600, 0x0F0F0F0F, 1, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 0xFF00FF00);  // Old
    clear_all_req(top);
    top->atomic_en = 0;
    tick(top, tfp);
    
    set_lane_req(top, 0, 0x600, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 0x0F000F00);
    clear_all_req(top);
    
    // OR with 0xF0F0F0F0 -> 0xFFF0FFF0
    top->atomic_op = 5;  // OR
    top->atomic_en = 1;
    set_lane_req(top, 0, 0x600, 0xF0F0F0F0, 1, 1);
    tick(top, tfp);
    clear_all_req(top);
    top->atomic_en = 0;
    tick(top, tfp);
    
    set_lane_req(top, 0, 0x600, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 0xFFF0FFF0);
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Atomic AND/OR OK" << std::endl;
    
    // Test 8: Multi-Lane Atomic (conflict expected)
    std::cout << "\n[Test 8] Multi-Lane Atomic Conflict..." << std::endl;
    // Initialize counter to 0
    set_lane_req(top, 0, 0x700, 0, 1, 1);
    tick(top, tfp);
    clear_all_req(top);
    tick(top, tfp);
    
    // All lanes try atomic ADD (only lane 0 succeeds)
    top->atomic_op = 1;  // ADD
    top->atomic_en = 1;
    for (int i = 0; i < NUM_LANES; i++) {
        set_lane_req(top, i, 0x700, 1, 1, 1);  // All add 1
    }
    tick(top, tfp);
    assert(top->conflict_detected == 1);
    assert(top->lds_ready[0] == 1);
    assert(top->lds_rdata[0] == 0);  // Old value
    clear_all_req(top);
    top->atomic_en = 0;
    tick(top, tfp);
    
    // Verify counter = 1 (only lane 0 succeeded)
    set_lane_req(top, 0, 0x700, 0, 0, 1);
    tick(top, tfp);
    assert(top->lds_rdata[0] == 1);
    clear_all_req(top);
    test_count++; pass_count++;
    std::cout << "  [PASS] Multi-lane atomic conflict handled" << std::endl;
    
    // Test 9: Address Mapping Verification
    std::cout << "\n[Test 9] Address Mapping..." << std::endl;
    // Verify bank = addr[6:2]
    // addr 0x000: bank 0, addr 0x004: bank 1, ..., addr 0x07C: bank 31
    for (int i = 0; i < NUM_BANKS; i++) {
        set_lane_req(top, 0, i * 4, 0x30000000 + i, 1, 1);
        tick(top, tfp);
        clear_all_req(top);
        tick(top, tfp);
    }
    // Read back
    for (int i = 0; i < NUM_BANKS; i++) {
        set_lane_req(top, 0, i * 4, 0, 0, 1);
        tick(top, tfp);
        assert(top->lds_rdata[0] == (uint32_t)(0x30000000 + i));
        clear_all_req(top);
    }
    test_count++; pass_count++;
    std::cout << "  [PASS] Address mapping correct" << std::endl;
    
    // Finish
    tick(top, tfp);
    tick(top, tfp);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  All Tests Completed: " << pass_count << "/" << test_count << std::endl;
    std::cout << "========================================" << std::endl;
    
    tfp->close();
    delete tfp;
    delete top;
    return 0;
}
