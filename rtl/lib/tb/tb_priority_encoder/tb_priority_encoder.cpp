// ============================================================================
// Verilator Testbench for priority_encoder
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vpriority_encoder.h"

#define MAX_SIM_TIME 100

vluint64_t sim_time = 0;

// 测试向量结构
struct TestCase {
    uint32_t in;
    uint32_t expected_out;
    bool expected_valid;
    const char* description;
};

int main(int argc, char** argv) {
    // 初始化 Verilator
    Verilated::commandArgs(argc, argv);
    
    // 创建 DUT 实例
    Vpriority_encoder* dut = new Vpriority_encoder;
    
    // 启用波形跟踪
    Verilated::traceEverOn(true);
    VerilatedVcdC* m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("waveform.vcd");
    
    // 定义测试用例（N=32）
    TestCase tests[] = {
        // in         , expected_out, valid, description
        {0x00000000  , 0          , false, "All zeros - no valid"},
        {0x00000001  , 0          , true , "Only bit 0 set"},
        {0x00000002  , 1          , true , "Only bit 1 set"},
        {0x00000010  , 4          , true , "Only bit 4 set"},
        {0x80000000  , 31         , true , "Only bit 31 set (MSB)"},
        {0x000000FF  , 0          , true , "Bits 0-7 set, lowest wins"},
        {0xF0000000  , 28         , true , "Bits 28-31 set, lowest wins"},
        {0xFFFFFFFF  , 0          , true , "All bits set, bit 0 wins (LSB priority)"},
        {0x0000FF00  , 8          , true , "Bits 8-15 set, bit 8 wins"},
        {0x12345678  , 3          , true , "Random pattern, find lowest"},
    };
    
    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;
    
    printf("========================================\n");
    printf("Priority Encoder Testbench (N=32)\n");
    printf("========================================\n\n");
    
    // 运行测试
    for (int t = 0; t < num_tests; t++) {
        // 应用输入
        dut->in = tests[t].in;
        
        // 评估组合逻辑（无时钟，只需 eval）
        dut->eval();
        
        // 记录波形
        m_trace->dump(sim_time++);
        
        // 检查结果
        bool check_out = (dut->out == tests[t].expected_out);
        bool check_valid = (dut->valid == tests[t].expected_valid);
        bool test_passed = check_out && check_valid;
        
        printf("Test %2d: %s\n", t + 1, tests[t].description);
        printf("  Input:    0x%08X (bin: ", tests[t].in);
        
        // 打印二进制
        for (int b = 31; b >= 0; b--) {
            printf("%d", (tests[t].in >> b) & 1);
            if (b % 4 == 0 && b != 0) printf("_");
        }
        printf(")\n");
        
        printf("  Expected: out=%2u, valid=%d\n", 
               tests[t].expected_out, tests[t].expected_valid);
        printf("  Actual:   out=%2u, valid=%d\n", 
               dut->out, dut->valid);
        
        if (test_passed) {
            printf("  Result:   \033[32mPASS\033[0m\n\n");
            passed++;
        } else {
            printf("  Result:   \033[31mFAIL\033[0m\n");
            if (!check_out) printf("            -> out mismatch!\n");
            if (!check_valid) printf("            -> valid mismatch!\n");
            printf("\n");
            failed++;
        }
    }
    
    // 边界测试：随机测试
    printf("----------------------------------------\n");
    printf("Random Test (100 iterations)\n");
    printf("----------------------------------------\n");
    
    srand(42);  // 固定种子，可重复
    for (int r = 0; r < 100; r++) {
        uint32_t random_in = rand();
        
        // 计算期望值（软件参考模型）
        uint32_t expected = 0;
        bool found = false;
        for (int b = 0; b < 32 && !found; b++) {
            if (random_in & (1u << b)) {
                expected = b;
                found = true;
            }
        }
        
        dut->in = random_in;
        dut->eval();
        m_trace->dump(sim_time++);
        
        if (dut->out != expected || dut->valid != (random_in != 0)) {
            printf("Random test %d FAILED: in=0x%08X\n", r, random_in);
            failed++;
        } else {
            passed++;
        }
    }
    
    // 总结
    printf("========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("Total:  %d\n", passed + failed);
    printf("Passed: \033[32m%d\033[0m\n", passed);
    printf("Failed: \033[31m%d\033[0m\n", failed);
    printf("========================================\n");
    
    // 清理
    m_trace->close();
    delete m_trace;
    delete dut;
    
    return failed > 0 ? 1 : 0;
}