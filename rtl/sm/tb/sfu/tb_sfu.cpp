// ============================================================================
// Verilator Testbench for SFU (Special Function Unit)
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "Vsfu.h"

vluint64_t sim_time = 0;

// SFU操作编码
#define OP_RCP  0
#define OP_RSQ  1
#define OP_SIN  2
#define OP_COS  3
#define OP_LOG2 4
#define OP_EXP2 5
#define TRACE
// 浮点辅助函数
uint32_t float_to_bits(float f) {
    union { float f; uint32_t i; } u;
    u.f = f;
    return u.i;
}

float bits_to_float(uint32_t i) {
    union { float f; uint32_t i; } u;
    u.i = i;
    return u.f;
}

// 检查浮点结果（允许误差）
bool check_float(float actual, float expected, float tolerance = 0.01f) {
    if (std::isnan(expected) && std::isnan(actual)) return true;
    if (std::isinf(expected) && std::isinf(actual)) return true;
    float diff = std::fabs(actual - expected);
    float rel_diff = diff / (std::fabs(expected) + 1e-6f);
    return diff < tolerance || rel_diff < tolerance;
}

// 时钟翻转
void posedge(Vsfu* dut, VerilatedVcdC* trace) {
    dut->clk = 0;
    dut->eval();
    if (trace) trace->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    if (trace) trace->dump(sim_time++);
}

// 发送SFU请求
void send_sfu_req(Vsfu* dut, VerilatedVcdC* trace, uint32_t op, uint32_t* data) {
    dut->clk = 0;
    dut->eval();
    
    dut->sfu_op = op;
    dut->valid_in = 1;
    for (int i = 0; i < 32; i++) {
        dut->src[i] = data[i];
    }
    
    dut->eval();
    if (trace) trace->dump(sim_time++);
    
    dut->clk = 1;
    dut->eval();
    if (trace) trace->dump(sim_time++);
    
    dut->valid_in = 0;
}

// 等待结果
bool wait_sfu_result(Vsfu* dut, VerilatedVcdC* trace, uint32_t* out, int max_cycles = 10) {
    for (int i = 0; i < max_cycles; i++) {
        posedge(dut, trace);
        if (dut->valid_out) {
            for (int j = 0; j < 32; j++) {
                out[j] = dut->result[j];
            }
            return true;
        }
    }
    return false;
}

// 打印结果（前4个和后4个lane）
void print_sfu_result(uint32_t* result, const char* name) {
    printf("%s: [", name);
    for (int i = 0; i < 4; i++) {
        float f = bits_to_float(result[i]);
        printf("%g%s", f, i < 3 ? ", " : "");
    }
    printf(" ... ");
    for (int i = 28; i < 32; i++) {
        float f = bits_to_float(result[i]);
        printf("%g%s", f, i < 31 ? ", " : "");
    }
    printf("]\n");
}

// 检查32个lane的浮点结果
bool check_sfu_lanes(uint32_t* actual, uint32_t* expected, const char* test_name, float tolerance = 0.01f) {
    bool pass = true;
    for (int i = 0; i < 32; i++) {
        float act = bits_to_float(actual[i]);
        float exp = bits_to_float(expected[i]);
        if (!check_float(act, exp, tolerance)) {
            printf("  [FAIL] Lane %d: expected %g, got %g\n", i, exp, act);
            pass = false;
            if (i >= 2) break;
        }
    }
    if (pass) {
        printf("\033[32m[PASS]\033[0m %s\n", test_name);
    } else {
        printf("\033[31m[FAIL]\033[0m %s\n", test_name);
    }
    return pass;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Vsfu* dut = new Vsfu;
    VerilatedVcdC* m_trace = nullptr;
    
    #ifdef TRACE
    Verilated::traceEverOn(true);
    m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("sfu.vcd");
    #endif
    
    printf("========================================\n");
    printf("SFU Testbench (32 lanes, Special Functions)\n");
    printf("========================================\n\n");
    
    int passed = 0, failed = 0;
    uint32_t src[32], result[32], expected[32];
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->sfu_op = 0;
    for (int i = 0; i < 32; i++) dut->src[i] = 0;
    
    // 复位
    printf("Reset...\n");
    for (int i = 0; i < 5; i++) posedge(dut, m_trace);
    dut->rst_n = 1;
    posedge(dut, m_trace);
    printf("Reset done, ready=%d\n\n", dut->ready);
    
    // ========== Test 1: RCP (Reciprocal 1/x) ==========
    printf("Test 1: RCP (Reciprocal)\n");
    for (int i = 0; i < 32; i++) {
        float input = 1.0f + i * 0.5f;  // 1.0, 1.5, 2.0, ...
        src[i] = float_to_bits(input);
        expected[i] = float_to_bits(1.0f / input);  // 1.0, 0.666..., 0.5, ...
    }
    send_sfu_req(dut, m_trace, OP_RCP, src);
    printf("  Input: [1, 1.5, 2, 2.5 ...] (32 lanes)\n");
    
    if (wait_sfu_result(dut, m_trace, result)) {
        print_sfu_result(result, "  Result");
        // RCP精度要求较低（Newton-Raphson迭代）
        if (check_sfu_lanes(result, expected, "RCP", 0.1f)) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m RCP timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 2: RSQ (Reciprocal Sqrt 1/sqrt(x)) ==========
    printf("Test 2: RSQ (Reciprocal Sqrt)\n");
    for (int i = 0; i < 32; i++) {
        float input = 1.0f + i;  // 1.0, 2.0, 3.0, ...
        src[i] = float_to_bits(input);
        expected[i] = float_to_bits(1.0f / std::sqrt(input));
    }
    send_sfu_req(dut, m_trace, OP_RSQ, src);
    printf("  Input: [1, 2, 3, 4 ...] (32 lanes)\n");
    
    if (wait_sfu_result(dut, m_trace, result)) {
        print_sfu_result(result, "  Result");
        if (check_sfu_lanes(result, expected, "RSQ", 0.1f)) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m RSQ timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 3: SIN ==========
    printf("Test 3: SIN (Sine)\n");
    for (int i = 0; i < 32; i++) {
        float angle = (float)i * 0.1f;  // 0, 0.1, 0.2, ... (radians)
        src[i] = float_to_bits(angle);
        expected[i] = float_to_bits(std::sin(angle));
    }
    send_sfu_req(dut, m_trace, OP_SIN, src);
    printf("  Input: [0, 0.1, 0.2, 0.3 ...] radians\n");
    
    if (wait_sfu_result(dut, m_trace, result)) {
        print_sfu_result(result, "  Result");
        // 注意：当前RTL是pass-through，需要实际实现
        if (check_sfu_lanes(result, expected, "SIN", 0.01f)) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m SIN timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 4: COS ==========
    printf("Test 4: COS (Cosine)\n");
    for (int i = 0; i < 32; i++) {
        float angle = (float)i * 0.1f;
        src[i] = float_to_bits(angle);
        expected[i] = float_to_bits(std::cos(angle));
    }
    send_sfu_req(dut, m_trace, OP_COS, src);
    printf("  Input: [0, 0.1, 0.2, 0.3 ...] radians\n");
    
    if (wait_sfu_result(dut, m_trace, result)) {
        print_sfu_result(result, "  Result");
        if (check_sfu_lanes(result, expected, "COS", 0.01f)) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m COS timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 5: LOG2 ==========
    printf("Test 5: LOG2 (Log base 2)\n");
    for (int i = 0; i < 32; i++) {
        float input = 1.0f + i * 0.25f;  // 1.0, 1.25, 1.5, ...
        src[i] = float_to_bits(input);
        expected[i] = float_to_bits(std::log2(input));
    }
    send_sfu_req(dut, m_trace, OP_LOG2, src);
    printf("  Input: [1, 1.25, 1.5, 1.75 ...]\n");
    
    if (wait_sfu_result(dut, m_trace, result)) {
        print_sfu_result(result, "  Result");
        if (check_sfu_lanes(result, expected, "LOG2", 0.01f)) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m LOG2 timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 6: EXP2 ==========
    printf("Test 6: EXP2 (2^x)\n");
    for (int i = 0; i < 32; i++) {
        float input = (float)i * 0.1f;  // 0, 0.1, 0.2, ...
        src[i] = float_to_bits(input);
        expected[i] = float_to_bits(std::exp2(input));  // 2^0=1, 2^0.1≈1.07, ...
    }
    send_sfu_req(dut, m_trace, OP_EXP2, src);
    printf("  Input: [0, 0.1, 0.2, 0.3 ...]\n");
    
    if (wait_sfu_result(dut, m_trace, result)) {
        print_sfu_result(result, "  Result");
        if (check_sfu_lanes(result, expected, "EXP2", 0.01f)) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m EXP2 timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 7: Back-to-back requests ==========
    printf("Test 7: Back-to-back SFU requests\n");
    printf("  Note: SFU is non-pipelined, second request should wait\n");
    
    // 第一个请求：RCP
    for (int i = 0; i < 32; i++) {
        float input = 2.0f + i;
        src[i] = float_to_bits(input);
        expected[i] = float_to_bits(1.0f / input);
    }
    send_sfu_req(dut, m_trace, OP_RCP, src);
    printf("  Sent RCP request\n");
    
    // 尝试立即发送第二个请求（应该被拒绝，因为ready=0）
    posedge(dut, m_trace);
    printf("  After 1 cycle: ready=%d (should be 0)\n", dut->ready);
    
    // 等待第一个完成
    uint32_t result1[32], result2[32];
    int cycles = 0;
    while (!dut->valid_out && cycles < 10) {
        posedge(dut, m_trace);
        cycles++;
    }
    printf("  RCP completed in %d cycles\n", cycles);
    
    if (dut->valid_out) {
        for (int i = 0; i < 32; i++) result1[i] = dut->result[i];
        bool ok1 = check_float(bits_to_float(result1[0]), 0.5f, 0.1f);
        printf("  RCP result[0]=%g, expected=0.5, %s\n", 
               bits_to_float(result1[0]), ok1 ? "OK" : "FAIL");
        
        // 现在发送第二个请求
        for (int i = 0; i < 32; i++) {
            src[i] = float_to_bits(4.0f + i);
            expected[i] = float_to_bits(1.0f / (4.0f + i));
        }
        send_sfu_req(dut, m_trace, OP_RCP, src);
        printf("  Sent second RCP request\n");
        
        if (wait_sfu_result(dut, m_trace, result2)) {
            bool ok2 = check_float(bits_to_float(result2[0]), 0.25f, 0.1f);
            printf("  Second RCP result[0]=%g, expected=0.25, %s\n",
                   bits_to_float(result2[0]), ok2 ? "OK" : "FAIL");
            
            if (ok1 && ok2) {
                printf("\033[32m[PASS]\033[0m Back-to-back requests\n");
                passed++;
            } else {
                printf("\033[31m[FAIL]\033[0m Back-to-back\n");
                failed++;
            }
        } else {
            printf("\033[31m[FAIL]\033[0m Second request timeout\n");
            failed++;
        }
    } else {
        printf("\033[31m[FAIL]\033[0m First request timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 8: Boundary values ==========
    printf("Test 8: Boundary values (RCP of 1.0 and large number)\n");
    for (int i = 0; i < 32; i++) {
        float input = (i == 0) ? 1.0f : 1000.0f + i;
        src[i] = float_to_bits(input);
        expected[i] = float_to_bits(1.0f / input);
    }
    send_sfu_req(dut, m_trace, OP_RCP, src);
    
    if (wait_sfu_result(dut, m_trace, result)) {
        float r0 = bits_to_float(result[0]);
        float r1 = bits_to_float(result[1]);
        bool ok = check_float(r0, 1.0f, 0.01f) && check_float(r1, 0.001f, 0.0001f);
        printf("  RCP(1.0)=%g, RCP(1001)=%g\n", r0, r1);
        if (ok) {
            printf("\033[32m[PASS]\033[0m Boundary values\n");
            passed++;
        } else {
            printf("\033[31m[FAIL]\033[0m Boundary values\n");
            failed++;
        }
    } else {
        printf("\033[31m[FAIL]\033[0m Boundary timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== 总结 ==========
    printf("========================================\n");
    printf("Test Summary: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");
    printf("Note: Current SFU RTL uses simplified pass-through\n");
    printf("      for SIN/COS/LOG2/EXP2. Real implementation\n");
    printf("      needs CORDIC or lookup tables.\n");
    
    #ifdef TRACE
    if (m_trace) {
        m_trace->close();
        delete m_trace;
    }
    #endif
    delete dut;
    
    return failed > 0 ? 1 : 0;
}