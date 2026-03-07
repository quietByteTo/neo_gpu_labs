// ============================================================================
// Verilator Testbench for sram_1p
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vsram_1p.h"

#define CLK_PERIOD 10
#define MAX_SIM_TIME 10000

vluint64_t sim_time = 0;

// 时钟辅助函数
void toggle_clock(Vsram_1p* dut, VerilatedVcdC* trace) {
    dut->clk = !dut->clk;
    dut->eval();
    trace->dump(sim_time++);
}

void posedge(Vsram_1p* dut, VerilatedVcdC* trace) {
    toggle_clock(dut, trace);  // 0->1
    toggle_clock(dut, trace);  // 1->0
}

// 打印测试结果
void print_result(bool pass, const char* msg) {
    if (pass) {
        printf("\033[32m[PASS]\033[0m %s\n", msg);
    } else {
        printf("\033[31m[FAIL]\033[0m %s\n", msg);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // 创建 DUT（小尺寸用于测试，ADDR_W=8, DEPTH=256）
    Vsram_1p* dut = new Vsram_1p;
    
    // 启用波形
    Verilated::traceEverOn(true);
    VerilatedVcdC* m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("waveform.vcd");
    
    // 参数检查（通过 DPI 或宏，这里直接硬编码对应 Verilog 参数）
    const int ADDR_W = 8;
    const int DATA_W = 32;
    const int DEPTH = 256;
    
    printf("========================================\n");
    printf("Single-Port SRAM Testbench\n");
    printf("Parameters: ADDR_W=%d, DATA_W=%d, DEPTH=%d\n", ADDR_W, DATA_W, DEPTH);
    printf("========================================\n\n");
    
    int passed = 0;
    int failed = 0;
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->ce = 0;
    dut->wr_en = 0;
    dut->addr = 0;
    dut->wdata = 0;
    
    // 复位
    printf("Reset sequence...\n");
    for (int i = 0; i < 5; i++) {
        posedge(dut, m_trace);
    }
    dut->rst_n = 1;
    posedge(dut, m_trace);
    printf("Reset complete.\n\n");
    
    // 测试 1: 基本写入和读取
    printf("Test 1: Basic Write and Read\n");
    printf("----------------------------------------\n");
    
    // 写入地址 0x10
    dut->ce = 1;
    dut->wr_en = 1;
    dut->addr = 0x10;
    dut->wdata = 0xDEADBEEF;
    posedge(dut, m_trace);
    
    // 写入地址 0x20
    dut->addr = 0x20;
    dut->wdata = 0xCAFEBABE;
    posedge(dut, m_trace);
    
    // 读取地址 0x10（Read-First: 应该读到旧数据，但第一次读是 0）
    dut->wr_en = 0;
    dut->addr = 0x10;
    posedge(dut, m_trace);  // 发起读，rdata 在下一个周期有效
    
    // 再给一个周期，rdata 稳定
    dut->ce = 0;  // 禁用，保持 rdata
    posedge(dut, m_trace);
    
    printf("  Read addr 0x10: rdata=0x%08X (exp 0xDEADBEEF) ", dut->rdata);
    if (dut->rdata == 0xDEADBEEF) {
        print_result(true, "Write/Read test");
        passed++;
    } else {
        print_result(false, "Write/Read test");
        failed++;
    }
    
    // 读取地址 0x20
    dut->ce = 1;
    dut->addr = 0x20;
    posedge(dut, m_trace);
    dut->ce = 0;
    posedge(dut, m_trace);
    
    printf("  Read addr 0x20: rdata=0x%08X (exp 0xCAFEBABE) ", dut->rdata);
    if (dut->rdata == 0xCAFEBABE) {
        print_result(true, "");
        passed++;
    } else {
        print_result(false, "");
        failed++;
    }
    
    printf("\n");
    
    // 测试 2: 连续写入和读取
    printf("Test 2: Sequential Write/Read\n");
    printf("----------------------------------------\n");
    
    // 连续写入 10 个地址
    for (int i = 0; i < 10; i++) {
        dut->ce = 1;
        dut->wr_en = 1;
        dut->addr = i;
        dut->wdata = 0x10000000 + i;
        posedge(dut, m_trace);
    }
    
    // 连续读取验证
    bool seq_ok = true;
    for (int i = 0; i < 10; i++) {
        dut->ce = 1;
        dut->wr_en = 0;
        dut->addr = i;
        posedge(dut, m_trace);
        dut->ce = 0;
        posedge(dut, m_trace);
        
        uint32_t expected = 0x10000000 + i;
        if (dut->rdata != expected) {
            printf("  Addr 0x%02X: rdata=0x%08X, exp=0x%08X\n", i, dut->rdata, expected);
            seq_ok = false;
        }
    }
    
    if (seq_ok) {
        print_result(true, "Sequential Write/Read test");
        passed++;
    } else {
        print_result(false, "Sequential Write/Read test");
        failed++;
    }
    
    printf("\n");
    
    // 测试 3: Read-First 行为验证（同一地址先写后读）
    printf("Test 3: Read-First Behavior\n");
    printf("----------------------------------------\n");
    
    // 先写入已知值
    dut->ce = 1;
    dut->wr_en = 1;
    dut->addr = 0x50;
    dut->wdata = 0x11111111;
    posedge(dut, m_trace);
    
    // 同一地址写入新值，同时读取（Read-First: 应该读到旧值 0x11111111）
    dut->wdata = 0x22222222;
    // 在一个周期内：先读（得到 0x11111111），后写（写入 0x22222222）
    posedge(dut, m_trace);
    
    // 检查 rdata（应该是旧值 0x11111111）
    uint32_t read_first_val = dut->rdata;
    
    // 再读一次，应该得到新值 0x22222222
    dut->wr_en = 0;
    posedge(dut, m_trace);
    dut->ce = 0;
    posedge(dut, m_trace);
    
    printf("  Read-first: rdata=0x%08X (exp 0x11111111) ", read_first_val);
    if (read_first_val == 0x11111111) {
        print_result(true, "Read-first behavior");
        passed++;
    } else {
        print_result(false, "Read-first behavior");
        failed++;
    }
    
    printf("  Second read: rdata=0x%08X (exp 0x22222222) ", dut->rdata);
    if (dut->rdata == 0x22222222) {
        print_result(true, "");
        passed++;
    } else {
        print_result(false, "");
        failed++;
    }
    
    printf("\n");
    
    // 测试 4: Chip Enable 功能
    printf("Test 4: Chip Enable (CE)\n");
    printf("----------------------------------------\n");
    
    // 写入一个值
    dut->ce = 1;
    dut->wr_en = 1;
    dut->addr = 0x60;
    dut->wdata = 0xAAAAAAAA;
    posedge(dut, m_trace);
    
    // CE=0 时读取（应该保持之前的 rdata）
    dut->ce = 0;
    dut->wr_en = 0;
    dut->addr = 0x60;
    uint32_t rdata_before = dut->rdata;
    posedge(dut, m_trace);
    posedge(dut, m_trace);
    
    printf("  CE=0: rdata unchanged? 0x%08X vs 0x%08X ", dut->rdata, rdata_before);
    if (dut->rdata == rdata_before) {
        print_result(true, "CE disable works");
        passed++;
    } else {
        print_result(false, "CE disable works");
        failed++;
    }
    
    // CE=1 时正常读取
    dut->ce = 1;
    posedge(dut, m_trace);
    dut->ce = 0;
    posedge(dut, m_trace);
    
    printf("  CE=1: rdata=0x%08X (exp 0xAAAAAAAA) ", dut->rdata);
    if (dut->rdata == 0xAAAAAAAA) {
        print_result(true, "");
        passed++;
    } else {
        print_result(false, "");
        failed++;
    }
    
    printf("\n");
    
    // 测试 5: 地址边界
    printf("Test 5: Address Boundary\n");
    printf("----------------------------------------\n");
    
    // 写入边界地址
    dut->ce = 1;
    dut->wr_en = 1;
    dut->addr = 0x00;
    dut->wdata = 0x00000000;
    posedge(dut, m_trace);
    
    dut->addr = DEPTH - 1;  // 0xFF for DEPTH=256
    dut->wdata = 0xFFFFFFFF;
    posedge(dut, m_trace);
    
    // 读取验证
    dut->wr_en = 0;
    dut->addr = 0x00;
    posedge(dut, m_trace);
    dut->ce = 0;
    posedge(dut, m_trace);
    
    printf("  Addr 0x00: rdata=0x%08X (exp 0x00000000) ", dut->rdata);
    if (dut->rdata == 0x00000000) {
        print_result(true, "Low boundary");
        passed++;
    } else {
        print_result(false, "Low boundary");
        failed++;
    }
    
    dut->ce = 1;
    dut->addr = DEPTH - 1;
    posedge(dut, m_trace);
    dut->ce = 0;
    posedge(dut, m_trace);
    
    printf("  Addr 0xFF: rdata=0x%08X (exp 0xFFFFFFFF) ", dut->rdata);
    if (dut->rdata == 0xFFFFFFFF) {
        print_result(true, "High boundary");
        passed++;
    } else {
        print_result(false, "High boundary");
        failed++;
    }
    
    printf("\n");
    
    // 测试 6: 随机测试（简化：读写不同地址，避免 Read-First 冲突）
    printf("Test 6: Random Access Test (100 iterations)\n");
    printf("----------------------------------------\n");
    
    srand(42);
    uint32_t shadow_mem[DEPTH] = {0};
    int rand_errors = 0;
    
    // 阶段 1：随机写入所有地址
    printf("  Phase 1: Random writes...\n");
    for (int i = 0; i < DEPTH; i++) {
        uint32_t rand_data = rand();
        
        dut->ce = 1;
        dut->wr_en = 1;
        dut->addr = i;
        dut->wdata = rand_data;
        posedge(dut, m_trace);
        
        shadow_mem[i] = rand_data;
    }
    
    // 阶段 2：随机读取验证
    printf("  Phase 2: Random reads...\n");
    for (int r = 0; r < 100; r++) {
        uint32_t rand_addr = rand() % DEPTH;
        
        // 发起读
        dut->ce = 1;
        dut->wr_en = 0;
        dut->addr = rand_addr;
        posedge(dut, m_trace);  // 发起读
        
        // 等待数据有效
        dut->ce = 0;
        posedge(dut, m_trace);  // 数据有效
        
        if (dut->rdata != shadow_mem[rand_addr]) {
            printf("  Error: addr=0x%02X, rdata=0x%08X, exp=0x%08X\n",
                   rand_addr, dut->rdata, shadow_mem[rand_addr]);
            rand_errors++;
        }
    }
    
    printf("  Random access errors: %d ", rand_errors);
    if (rand_errors == 0) {
        print_result(true, "Random test");
        passed++;
    } else {
        print_result(false, "Random test");
        failed++;
    }
    
    printf("\n");
    
    // 总结
    printf("========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("Total checks: %d\n", passed + failed);
    printf("Passed: \033[32m%d\033[0m\n", passed);
    printf("Failed: \033[31m%d\033[0m\n", failed);
    printf("========================================\n");
    
    // 清理
    m_trace->close();
    delete m_trace;
    delete dut;
    
    return failed > 0 ? 1 : 0;
}