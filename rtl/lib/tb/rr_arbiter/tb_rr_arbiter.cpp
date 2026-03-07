// ============================================================================
// Verilator Testbench for rr_arbiter
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vrr_arbiter.h"
#include "Vrr_arbiter_rr_arbiter.h"
#define MAX_SIM_TIME 200

vluint64_t sim_time = 0;
vluint64_t posedge_cnt = 0;

// 时钟边沿检测
void toggle_clock(Vrr_arbiter* dut) {
    dut->clk = !dut->clk;
    dut->eval();
    if (dut->clk == 1) posedge_cnt++;
}

void posedge(Vrr_arbiter* dut, VerilatedVcdC* trace) {
    toggle_clock(dut);
    trace->dump(sim_time++);
    toggle_clock(dut);
    trace->dump(sim_time++);
}

// 测试用例结构
struct TestCase {
    uint32_t req;           // 请求向量
    uint32_t expected_grant; // 期望的授权
    uint32_t expected_pointer_after; // advance 后的指针
    bool     advance;       // 是否更新指针
    const char* description;
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // 创建 DUT（默认 N=4）
    Vrr_arbiter* dut = new Vrr_arbiter;
    
    // 启用波形
    Verilated::traceEverOn(true);
    VerilatedVcdC* m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("waveform.vcd");
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->req = 0;
    dut->advance = 0;
    
    // 复位
    for (int i = 0; i < 5; i++) {
        posedge(dut, m_trace);
    }
    dut->rst_n = 1;
    posedge(dut, m_trace);
    
    printf("========================================\n");
    printf("Round-Robin Arbiter Testbench (N=4)\n");
    printf("========================================\n\n");
    
    int passed = 0;
    int failed = 0;
    
    // 测试 1: 基本功能 - 单请求
    printf("Test 1: Single request rotation\n");
    printf("----------------------------------------\n");
    
    TestCase tests1[] = {
        // req,  grant, ptr_after, adv,  description
        {0b0001, 0b0001, 0, true,  "req=0001, pointer=0, grant=0"},
        {0b0001, 0b0001, 0, true,  "req=0001, pointer=0, grant=0 (again)"},
        {0b0010, 0b0010, 1, true,  "req=0010, pointer=0, grant=1"},
        {0b0100, 0b0100, 2, true,  "req=0100, pointer=1, grant=2"},
        {0b1000, 0b1000, 3, true,  "req=1000, pointer=2, grant=3"},
        {0b0001, 0b0001, 0, true,  "req=0001, pointer=3, grant=0 (wrap)"},
    };
    
    for (int t = 0; t < 6; t++) {
        dut->req = tests1[t].req;
        dut->advance = tests1[t].advance;
        
        // 等待组合逻辑稳定
        dut->eval();
        
        printf("  %s: ", tests1[t].description);
        printf("grant=%04b (exp %04b), ", dut->grant, tests1[t].expected_grant);
        
        bool grant_ok = (dut->grant == tests1[t].expected_grant);
        
        // 时钟上升沿，advance 更新 pointer
        posedge(dut, m_trace);
        
        printf("pointer=%u (exp %u) ", dut->rr_arbiter->pointer, tests1[t].expected_pointer_after);
        
        bool ptr_ok = (dut->rr_arbiter->pointer == tests1[t].expected_pointer_after);
        
        if (grant_ok && ptr_ok) {
            printf("\033[32mPASS\033[0m\n");
            passed++;
        } else {
            printf("\033[31mFAIL\033[0m\n");
            if (!grant_ok) printf("    [grant mismatch] ");
            if (!ptr_ok) printf("    [pointer mismatch] ");
            printf("\n");
            failed++;
        }
    }
    
    printf("\n");
    
    // 测试 2: Round-Robin 公平性 - 多请求
    printf("Test 2: Round-Robin fairness (multiple requests)\n");
    printf("----------------------------------------\n");
    
    // 复位到已知状态
    dut->rst_n = 0;
    posedge(dut, m_trace);
    dut->rst_n = 1;
    posedge(dut, m_trace);
    // 第 1 轮
    // pointer=0, req=1111，应该 grant=0010 (2)
    dut->req = 0b1111;
    dut->advance = 1;
    dut->eval();

    printf("  pointer=%u, req=1111: grant=%04b (exp 0010) ", dut->rr_arbiter->pointer, dut->grant);
    if (dut->grant == 0b0010) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    posedge(dut, m_trace);  // pointer=0
    
    // 第 2 轮
    // pointer=1, req=1111，应该 grant=0100 (3)
    dut->req = 0b1111;
    dut->eval();

    // pointer=1, 扫描 2→3→0→1, 期望 grant=0100
    printf("  pointer=%u, req=1111: grant=%04b (exp 0100) ", dut->rr_arbiter->pointer, dut->grant);
    if (dut->grant == 0b0100) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    posedge(dut, m_trace);  // pointer=2
    
    // 第 3 轮: pointer=2, 期望 grant=1000
    // pointer=2, req=1111，应该 grant=1000 (4)
    dut->req = 0b1111;
    dut->eval();
    printf("  pointer=%u, req=1111: grant=%04b (exp 1000) ", dut->rr_arbiter->pointer, dut->grant);
    if (dut->grant == 0b1000) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    posedge(dut, m_trace);  // pointer=3
    
    // 第 4 轮: pointer=3, 期望 grant=0001 (环绕)
    // pointer=3, req=1111，应该 grant=0001 (1，环绕)
    dut->req = 0b1111;
    dut->eval();
    printf("  pointer=%u, req=1111: grant=%04b (exp 0001) ", dut->rr_arbiter->pointer, dut->grant);
    if (dut->grant == 0b0001) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    posedge(dut, m_trace);  // pointer=0
    
    // 第 5 轮: pointer=0  again, 期望 grant=0010
    dut->req = 0b1111;
    dut->eval();
    printf("  pointer=%u, req=1111: grant=%04b ", dut->rr_arbiter->pointer, dut->grant);
    if (dut->grant == 0b0010) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m (exp 0010)\n"); failed++; }

    posedge(dut, m_trace);

    printf("\n");
    
    // 测试 3: 无请求
    printf("Test 3: No request\n");
    printf("----------------------------------------\n");
    
    dut->req = 0b0000;
    dut->advance = 0;
    dut->eval();
    printf("  req=0000: grant=%04b (exp 0000), valid=%d ", 
           dut->grant, (dut->req != 0));
    if (dut->grant == 0b0000) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    posedge(dut, m_trace);
    
    printf("\n");
    
    // 测试 4: advance=0 时不更新 pointer
    printf("Test 4: No advance - pointer should not change\n");
    printf("----------------------------------------\n");
    
    dut->req = 0b0010;
    dut->advance = 0;
    uint32_t old_ptr = dut->rr_arbiter->pointer;
    dut->eval();
    printf("  req=0010, advance=0: grant=%04b, pointer=%u ", 
           dut->grant, dut->rr_arbiter->pointer);
    
    posedge(dut, m_trace);  // advance=0，pointer 不应变
    
    printf("after clk: pointer=%u (exp %u) ", dut->rr_arbiter->pointer, old_ptr);
    if (dut->rr_arbiter->pointer == old_ptr) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    
    printf("\n");
    
    // 测试 5: 随机测试
    printf("Test 5: Random test (100 iterations)\n");
    printf("----------------------------------------\n");
    
    srand(42);
    uint32_t last_grant = 0;
    int rr_violations = 0;
    
    for (int r = 0; r < 100; r++) {
        uint32_t random_req = rand() & 0xF;  // 4-bit
        if (random_req == 0) random_req = 0b0001;  // 避免无请求
        
        uint32_t old_pointer = dut->rr_arbiter->pointer;
        
        dut->req = random_req;
        dut->advance = 1;
        dut->eval();
        
        // 验证 Round-Robin：grant 应该在 old_pointer 之后
        uint32_t grant_idx = 0;
        for (int g = 0; g < 4; g++) {
            if ((dut->grant >> g) & 1) {
                grant_idx = g;
                break;
            }
        }
        
        // 检查：grant 应该是 (old_pointer+1)%N 到 N-1，然后 0 到 old_pointer 中第一个置位的
        bool valid_rr = false;
        for (int check = 1; check <= 4; check++) {
            uint32_t check_idx = (old_pointer + check) % 4;
            if ((random_req >> check_idx) & 1) {
                if (grant_idx == check_idx) valid_rr = true;
                break;
            }
        }
        
        if (!valid_rr && (random_req != 0)) {
            rr_violations++;
        }
        
        posedge(dut, m_trace);
    }
    
    printf("  Round-Robin violations: %d ", rr_violations);
    if (rr_violations == 0) { printf("\033[32mPASS\033[0m\n"); passed++; }
    else { printf("\033[31mFAIL\033[0m\n"); failed++; }
    
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