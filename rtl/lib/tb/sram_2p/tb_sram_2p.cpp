// ============================================================================
// Verilator Testbench for sram_2p (Pseudo-Dual-Port SRAM)
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vsram_2p.h"

vluint64_t sim_time = 0;

// 时钟辅助函数
void toggle_clock(Vsram_2p* dut, VerilatedVcdC* trace) {
    dut->clk = !dut->clk;
    dut->eval();
    trace->dump(sim_time++);
}

void posedge(Vsram_2p* dut, VerilatedVcdC* trace) {
    toggle_clock(dut, trace);  // 0->1
    toggle_clock(dut, trace);  // 1->0
}

void print_result(bool pass, const char* msg) {
    if (pass) {
        printf("\033[32m[PASS]\033[0m %s\n", msg);
    } else {
        printf("\033[31m[FAIL]\033[0m %s\n", msg);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // 创建 DUT（小尺寸用于测试）
    Vsram_2p* dut = new Vsram_2p;
    
    // 启用波形
    Verilated::traceEverOn(true);
    VerilatedVcdC* m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("waveform.vcd");
    
    const int ADDR_W = 8;
    const int DATA_W = 32;
    const int DEPTH = 256;
    
    printf("========================================\n");
    printf("Pseudo-Dual-Port SRAM Testbench\n");
    printf("Parameters: ADDR_W=%d, DATA_W=%d, DEPTH=%d\n", ADDR_W, DATA_W, DEPTH);
    printf("========================================\n\n");
    
    int passed = 0;
    int failed = 0;
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->a_ce = 0; dut->a_wr_en = 0; dut->a_addr = 0; dut->a_wdata = 0;
    dut->b_ce = 0; dut->b_addr = 0;
    
    // 复位
    printf("Reset sequence...\n");
    for (int i = 0; i < 5; i++) posedge(dut, m_trace);
    dut->rst_n = 1;
    posedge(dut, m_trace);
    printf("Reset complete.\n\n");
    
    // 测试 1: Port A 独立写入和读取
    printf("Test 1: Port A Basic Write/Read\n");
    printf("----------------------------------------\n");
    
    // 写入
    dut->a_ce = 1; dut->a_wr_en = 1;
    dut->a_addr = 0x10; dut->a_wdata = 0xAAAA1111;
    posedge(dut, m_trace);
    
    dut->a_addr = 0x20; dut->a_wdata = 0xBBBB2222;
    posedge(dut, m_trace);
    
    // 读取（2周期延迟）
    dut->a_wr_en = 0;
    dut->a_addr = 0x10;
    posedge(dut, m_trace);  // 发起读
    dut->a_ce = 0;
    posedge(dut, m_trace);  // 数据有效
    
    printf("  Port A read 0x10: 0x%08X (exp 0xAAAA1111) ", dut->a_rdata);
    if (dut->a_rdata == 0xAAAA1111) { print_result(true, ""); passed++; }
    else { print_result(false, ""); failed++; }
    
    dut->a_ce = 1; dut->a_addr = 0x20;
    posedge(dut, m_trace);
    dut->a_ce = 0;
    posedge(dut, m_trace);
    
    printf("  Port A read 0x20: 0x%08X (exp 0xBBBB2222) ", dut->a_rdata);
    if (dut->a_rdata == 0xBBBB2222) { print_result(true, ""); passed++; }
    else { print_result(false, ""); failed++; }
    
    printf("\n");
    
    // 测试 2: Port B 独立读取
    printf("Test 2: Port B Basic Read\n");
    printf("----------------------------------------\n");
    
    // 先用 Port A 写入
    dut->a_ce = 1; dut->a_wr_en = 1;
    dut->a_addr = 0x30; dut->a_wdata = 0xCCCC3333;
    posedge(dut, m_trace);
    dut->a_ce = 0;
    posedge(dut, m_trace);
    
    // Port B 读取
    dut->b_ce = 1; dut->b_addr = 0x30;
    posedge(dut, m_trace);  // 发起读
    dut->b_ce = 0;
    posedge(dut, m_trace);  // 数据有效
    
    printf("  Port B read 0x30: 0x%08X (exp 0xCCCC3333) ", dut->b_rdata);
    if (dut->b_rdata == 0xCCCC3333) { print_result(true, ""); passed++; }
    else { print_result(false, ""); failed++; }
    
    printf("\n");
    
    // 测试 3: 同时访问不同地址（无冲突）
    printf("Test 3: Simultaneous Access (Different Addresses)\n");
    printf("----------------------------------------\n");
    
    // 准备数据
    dut->a_ce = 1; dut->a_wr_en = 1;
    dut->a_addr = 0x40; dut->a_wdata = 0xDDDD4444;
    posedge(dut, m_trace);
    dut->a_ce = 0;
    posedge(dut, m_trace);
    
    // A读0x40，B读0x30（不同地址）
    dut->a_ce = 1; dut->a_wr_en = 0; dut->a_addr = 0x40;
    dut->b_ce = 1; dut->b_addr = 0x30;
    posedge(dut, m_trace);  // 同时发起
    dut->a_ce = 0; dut->b_ce = 0;
    posedge(dut, m_trace);  // 数据有效
    
    bool a_ok = (dut->a_rdata == 0xDDDD4444);
    bool b_ok = (dut->b_rdata == 0xCCCC3333);
    
    printf("  Port A read 0x40: 0x%08X (exp 0xDDDD4444) ", dut->a_rdata);
    print_result(a_ok, "");
    printf("  Port B read 0x30: 0x%08X (exp 0xCCCC3333) ", dut->b_rdata);
    print_result(b_ok, "");
    
    if (a_ok && b_ok) passed++; else failed++;
    
    printf("\n");
    
    // 测试 4: 同时访问相同地址 - Bypass 功能（关键测试）
    printf("Test 4: Write-Read Bypass (Same Address Collision)\n");
    printf("----------------------------------------\n");
    
    // 场景：Port A 写入地址 0x50，同时 Port B 读取地址 0x50
    // Bypass 应该让 B 立即看到新数据（0xEEEE5555），而不是旧数据
    
    dut->a_ce = 1; dut->a_wr_en = 1;
    dut->a_addr = 0x50; dut->a_wdata = 0xEEEE5555;
    dut->b_ce = 1; dut->b_addr = 0x50;  // 同一地址
    
    printf("  A writes 0xEEEE5555 to 0x50, B reads 0x50 same cycle\n");
    posedge(dut, m_trace);  // 同时发生
    
    dut->a_ce = 0; dut->b_ce = 0;
    posedge(dut, m_trace);  // B数据有效（Bypass）
    
    printf("  Port B rdata: 0x%08X (exp 0xEEEE5555 with bypass) ", dut->b_rdata);
    if (dut->b_rdata == 0xEEEE5555) {
        print_result(true, "Bypass works!");
        passed++;
    } else {
        print_result(false, "Bypass failed!");
        printf("    (Got old data, bypass not working)\n");
        failed++;
    }
    
    // 验证 A 是 Read-First（读到旧数据）
    printf("  Port A rdata: 0x%08X (Read-First, old data) ", dut->a_rdata);
    // A 应该读到写入前的数据（可能是 0x00000000 或其他）
    printf("(any value OK for Read-First)\n");
    
    printf("\n");
    
    // 测试 5: 无 Bypass 情况（A 不写入）
    printf("Test 5: Normal Read (No Write Collision)\n");
    printf("----------------------------------------\n");
    
    // 先写入
    dut->a_ce = 1; dut->a_wr_en = 1;
    dut->a_addr = 0x60; dut->a_wdata = 0xFFFF6666;
    posedge(dut, m_trace);
    dut->a_ce = 0;
    posedge(dut, m_trace);
    
    // B 读取，A 不写入
    dut->b_ce = 1; dut->b_addr = 0x60;
    dut->a_ce = 1; dut->a_wr_en = 0; dut->a_addr = 0x60;  // A 只读
    posedge(dut, m_trace);
    dut->a_ce = 0; dut->b_ce = 0;
    posedge(dut, m_trace);
    
    printf("  Port B read 0x60: 0x%08X (exp 0xFFFF6666) ", dut->b_rdata);
    if (dut->b_rdata == 0xFFFF6666) { print_result(true, ""); passed++; }
    else { print_result(false, ""); failed++; }
    
    printf("\n");
    
    // 测试 6: CE 控制
    printf("Test 6: Chip Enable Control\n");
    printf("----------------------------------------\n");
    
    dut->a_ce = 0; dut->b_ce = 0;
    uint32_t a_old = dut->a_rdata;
    uint32_t b_old = dut->b_rdata;
    
    // 改变地址，但 CE=0
    dut->a_addr = 0xFF; dut->b_addr = 0xFF;
    posedge(dut, m_trace);
    posedge(dut, m_trace);
    
    bool a_ce_ok = (dut->a_rdata == a_old);
    bool b_ce_ok = (dut->b_rdata == b_old);
    
    printf("  CE=0: outputs unchanged? A=%s, B=%s ", 
           a_ce_ok ? "YES" : "NO", b_ce_ok ? "YES" : "NO");
    if (a_ce_ok && b_ce_ok) { print_result(true, ""); passed++; }
    else { print_result(false, ""); failed++; }
    
    printf("\n");
    
    // 测试 7: 随机测试
    printf("Test 7: Random Access Test\n");
    printf("----------------------------------------\n");
    
    srand(42);
    uint32_t shadow_mem[DEPTH] = {0};
    int errors = 0;
    
    // 阶段 1：随机写入（只用 Port A）
    for (int i = 0; i < DEPTH; i++) {
        shadow_mem[i] = rand();
        
        dut->a_ce = 1; dut->a_wr_en = 1;
        dut->a_addr = i;
        dut->a_wdata = shadow_mem[i];
        posedge(dut, m_trace);
    }
    dut->a_ce = 0;
    posedge(dut, m_trace);
    
    // 阶段 2：随机读取验证（Port A 和 Port B 交替）
    for (int r = 0; r < 200; r++) {
        uint32_t addr = rand() % DEPTH;
        bool use_port_a = rand() % 2;
        
        if (use_port_a) {
            dut->a_ce = 1; dut->a_wr_en = 0;
            dut->a_addr = addr;
            posedge(dut, m_trace);
            dut->a_ce = 0;
            posedge(dut, m_trace);
            
            if (dut->a_rdata != shadow_mem[addr]) errors++;
        } else {
            dut->b_ce = 1;
            dut->b_addr = addr;
            posedge(dut, m_trace);
            dut->b_ce = 0;
            posedge(dut, m_trace);
            
            if (dut->b_rdata != shadow_mem[addr]) errors++;
        }
    }
    
    printf("  Random access errors: %d ", errors);
    if (errors == 0) { print_result(true, "Random test"); passed++; }
    else { print_result(false, "Random test"); failed++; }
    
    printf("\n");
    
    // 总结
    printf("========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("Total checks: %d\n", passed + failed);
    printf("Passed: \033[32m%d\033[0m\n", passed);
    printf("Failed: \033[31m%d\033[0m\n", failed);
    printf("========================================\n");
    
    m_trace->close();
    delete m_trace;
    delete dut;
    
    return failed > 0 ? 1 : 0;
}