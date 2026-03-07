// ============================================================================
// Verilator Testbench for sync_fifo
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vsync_fifo.h"

vluint64_t sim_time = 0;

// 时钟辅助函数
void toggle_clock(Vsync_fifo* dut, VerilatedVcdC* trace) {
    dut->clk = !dut->clk;
    dut->eval();
    trace->dump(sim_time++);
}

void posedge(Vsync_fifo* dut, VerilatedVcdC* trace) {
    toggle_clock(dut, trace);
    toggle_clock(dut, trace);
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
    
    // 创建 DUT
    Vsync_fifo* dut = new Vsync_fifo;
    
    // 启用波形
    Verilated::traceEverOn(true);
    VerilatedVcdC* m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("waveform.vcd");
    
    const int DATA_W = 32;
    const int DEPTH = 16;
    const int PTR_W = 5;  // $clog2(16) + 1 = 5
    
    printf("========================================\n");
    printf("Synchronous FIFO Testbench\n");
    printf("Parameters: DATA_W=%d, DEPTH=%d\n", DATA_W, DEPTH);
    printf("========================================\n\n");
    
    int passed = 0;
    int failed = 0;
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->wr_en = 0;
    dut->wdata = 0;
    dut->rd_en = 0;
    
    // 复位
    printf("Reset sequence...\n");
    for (int i = 0; i < 5; i++) posedge(dut, m_trace);
    dut->rst_n = 1;
    posedge(dut, m_trace);
    printf("Reset complete. empty=%d, full=%d, count=%d\n\n", 
           dut->empty, dut->full, dut->count);
    
    // 测试 1: 基本写入和读取
    printf("Test 1: Basic Write and Read\n");
    printf("----------------------------------------\n");
    
    // 写入 5 个数据
    printf("  Writing 5 values...\n");
    for (int i = 0; i < 5; i++) {
        dut->wr_en = 1;
        dut->wdata = 0x10000000 + i;
        posedge(dut, m_trace);
    }
    dut->wr_en = 0;
    posedge(dut, m_trace);
    
    printf("  count=%d (exp 5), empty=%d, full=%d\n", dut->count, dut->empty, dut->full);
    bool test1_ok = (dut->count == 5) && !dut->empty && !dut->full;
    
    // 读取 5 个数据
    printf("  Reading 5 values...\n");
    for (int i = 0; i < 5; i++) {
        dut->rd_en = 1;
        posedge(dut, m_trace);
        printf("    Read %d: 0x%08X (exp 0x%08X) ", i, dut->rdata, 0x10000000 + i);
        if (dut->rdata == (uint32_t)(0x10000000 + i)) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            test1_ok = false;
        }
    }
    dut->rd_en = 0;
    posedge(dut, m_trace);
    
    printf("  count=%d (exp 0), empty=%d\n", dut->count, dut->empty);
    test1_ok = test1_ok && (dut->count == 0) && dut->empty;
    
    if (test1_ok) { print_result(true, "Basic Write/Read"); passed++; }
    else { print_result(false, "Basic Write/Read"); failed++; }
    
    printf("\n");
    
    // 测试 2: FIFO 满检测
    printf("Test 2: FIFO Full Detection\n");
    printf("----------------------------------------\n");
    
    // 写满 FIFO
    printf("  Filling FIFO (%d entries)...\n", DEPTH);
    int write_count = 0;
    while (!dut->full && write_count < DEPTH + 5) {
        dut->wr_en = 1;
        dut->wdata = 0x20000000 + write_count;
        posedge(dut, m_trace);
        write_count++;
    }
    dut->wr_en = 0;
    posedge(dut, m_trace);
    
    printf("  Written %d entries, full=%d, count=%d\n", write_count, dut->full, dut->count);
    bool test2_ok = dut->full && (dut->count == DEPTH);
    
    if (test2_ok) { print_result(true, "FIFO Full"); passed++; }
    else { print_result(false, "FIFO Full"); failed++; }
    
    printf("\n");
    
    // 测试 3: 满时继续写入（应该被忽略）
    printf("Test 3: Write When Full\n");
    printf("----------------------------------------\n");
    
    uint32_t old_count = dut->count;
    dut->wr_en = 1;
    dut->wdata = 0xDEADBEEF;  // 这个写入应该被忽略
    posedge(dut, m_trace);
    dut->wr_en = 0;
    posedge(dut, m_trace);
    
    printf("  Attempt write when full: count=%d (exp %d), full=%d\n", 
           dut->count, DEPTH, dut->full);
    bool test3_ok = (dut->count == old_count) && dut->full;
    
    if (test3_ok) { print_result(true, "Write blocked when full"); passed++; }
    else { print_result(false, "Write blocked when full"); failed++; }
    
    printf("\n");
    
    // 测试 4: FIFO 空检测
    printf("Test 4: FIFO Empty Detection\n");
    printf("----------------------------------------\n");
    
    // 读空 FIFO
    printf("  Draining FIFO...\n");
    int read_count = 0;
    while (!dut->empty && read_count < DEPTH + 5) {
        dut->rd_en = 1;
        posedge(dut, m_trace);
        read_count++;
    }
    dut->rd_en = 0;
    posedge(dut, m_trace);
    
    printf("  Read %d entries, empty=%d, count=%d\n", read_count, dut->empty, dut->count);
    bool test4_ok = dut->empty && (dut->count == 0);
    
    if (test4_ok) { print_result(true, "FIFO Empty"); passed++; }
    else { print_result(false, "FIFO Empty"); failed++; }
    
    printf("\n");
    
    // 测试 5: 空时继续读取（应该被忽略）
    printf("Test 5: Read When Empty\n");
    printf("----------------------------------------\n");
    
    uint32_t old_rdata = dut->rdata;
    dut->rd_en = 1;
    posedge(dut, m_trace);
    dut->rd_en = 0;
    posedge(dut, m_trace);
    
    printf("  Attempt read when empty: count=%d (exp 0), empty=%d\n", 
           dut->count, dut->empty);
    bool test5_ok = (dut->count == 0) && dut->empty;
    
    if (test5_ok) { print_result(true, "Read blocked when empty"); passed++; }
    else { print_result(false, "Read blocked when empty"); failed++; }
    
    printf("\n");
    
    // 测试 6: 同时读写（流水线平衡）
    printf("Test 6: Simultaneous Read and Write\n");
    printf("----------------------------------------\n");
    
    // 先写入一些数据
    for (int i = 0; i < 3; i++) {
        dut->wr_en = 1;
        dut->wdata = 0x30000000 + i;
        posedge(dut, m_trace);
    }
    dut->wr_en = 0;
    posedge(dut, m_trace);
    
    uint32_t initial_count = dut->count;
    printf("  Initial count=%d\n", initial_count);
    
    // 同时读写 5 个周期
    printf("  Simultaneous rd/wr for 5 cycles...\n");
    for (int i = 0; i < 5; i++) {
        dut->wr_en = 1;
        dut->rd_en = 1;
        dut->wdata = 0x40000000 + i;
        uint32_t old_count = dut->count;
        posedge(dut, m_trace);
        printf("    Cycle %d: count=%d->%d, rdata=0x%08X\n", 
               i, old_count, dut->count, dut->rdata);
    }
    dut->wr_en = 0;
    dut->rd_en = 0;
    posedge(dut, m_trace);
    
    printf("  Final count=%d (exp %d, should be same)\n", dut->count, initial_count);
    bool test6_ok = (dut->count == initial_count);
    
    if (test6_ok) { print_result(true, "Simultaneous rd/wr"); passed++; }
    else { print_result(false, "Simultaneous rd/wr"); failed++; }
    
    printf("\n");
    
    // 测试 7: 数据一致性（顺序验证）
    printf("Test 7: Data Ordering (FIFO property)\n");
    printf("----------------------------------------\n");
    
    // 复位
    dut->rst_n = 0;
    posedge(dut, m_trace);
    dut->rst_n = 1;
    posedge(dut, m_trace);
    
    // 写入 0-9
    printf("  Writing 0-9...\n");
    for (int i = 0; i < 10; i++) {
        dut->wr_en = 1;
        dut->wdata = i;
        posedge(dut, m_trace);
    }
    dut->wr_en = 0;
    posedge(dut, m_trace);
    
    // 读取，验证顺序
    printf("  Reading and verifying order...\n");
    bool order_ok = true;
    for (int i = 0; i < 10; i++) {
        dut->rd_en = 1;
        posedge(dut, m_trace);
        if (dut->rdata != (uint32_t)i) {
            printf("    FAIL: expected %d, got %d\n", i, dut->rdata);
            order_ok = false;
        }
    }
    dut->rd_en = 0;
    posedge(dut, m_trace);
    
    if (order_ok) { print_result(true, "FIFO ordering"); passed++; }
    else { print_result(false, "FIFO ordering"); failed++; }
    
    printf("\n");
    
    // 测试 8: 边界测试（绕回指针）
    printf("Test 8: Pointer Wrap-around\n");
    printf("----------------------------------------\n");
    
    // 复位
    dut->rst_n = 0;
    posedge(dut, m_trace);
    dut->rst_n = 1;
    posedge(dut, m_trace);
    
    // 写入 DEPTH 个数据，读指针会绕回
    printf("  Filling to full, then drain twice...\n");
    bool wrap_ok = true;
    
    for (int round = 0; round < 2; round++) {
        // 写满
        for (int i = 0; i < DEPTH; i++) {
            dut->wr_en = 1;
            dut->wdata = (round * 0x10000000) + i;
            posedge(dut, m_trace);
        }
        dut->wr_en = 0;
        posedge(dut, m_trace);
        
        if (!dut->full) {
            printf("    Round %d: not full after %d writes\n", round, DEPTH);
            wrap_ok = false;
        }
        
        // 读空
        for (int i = 0; i < DEPTH; i++) {
            dut->rd_en = 1;
            posedge(dut, m_trace);
            uint32_t expected = (round * 0x10000000) + i;
            if (dut->rdata != expected) {
                printf("    Round %d, idx %d: exp 0x%08X, got 0x%08X\n",
                       round, i, expected, dut->rdata);
                wrap_ok = false;
            }
        }
        dut->rd_en = 0;
        posedge(dut, m_trace);
        
        if (!dut->empty) {
            printf("    Round %d: not empty after drain\n", round);
            wrap_ok = false;
        }
    }
    
    if (wrap_ok) { print_result(true, "Pointer wrap-around"); passed++; }
    else { print_result(false, "Pointer wrap-around"); failed++; }
    
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