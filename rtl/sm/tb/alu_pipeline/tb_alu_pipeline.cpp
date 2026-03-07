// ============================================================================
// Verilator Testbench for alu_pipeline - Complete Version
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "Valu_pipeline.h"

vluint64_t sim_time = 0;

// 指令编码
#define OP_ADD  0x0
#define OP_SUB  0x1
#define OP_MUL  0x2
#define OP_MAD  0x3
#define OP_AND  0x4
#define OP_OR   0x5
#define OP_SHL  0x6
#define OP_SHR  0x7
#define OP_MIN  0x8
#define OP_MAX  0x9

#define TYPE_INT32 0x0
#define TRACE
// 构造指令
uint32_t make_instr(uint8_t op, uint8_t dtype = TYPE_INT32) {
    return (0x0 << 28) | (op << 24) | (dtype << 20);
}

// 时钟翻转
void toggle_clock(Valu_pipeline* dut, VerilatedVcdC* trace) {
    dut->clk = !dut->clk;
    dut->eval();
    if (trace) trace->dump(sim_time++);
}

void posedge(Valu_pipeline* dut, VerilatedVcdC* trace) {
    dut->clk = 0;
    dut->eval();
    if (trace) trace->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    if (trace) trace->dump(sim_time++);
}

// 正确设置1024bit向量（Verilator打包为32个32bit字）
void set_src_a(Valu_pipeline* dut, uint32_t* data) {
    for (int i = 0; i < 32; i++) {
        dut->src_a[i] = data[i];
    }
}

void set_src_b(Valu_pipeline* dut, uint32_t* data) {
    for (int i = 0; i < 32; i++) {
        dut->src_b[i] = data[i];
    }
}

void set_src_c(Valu_pipeline* dut, uint32_t* data) {
    for (int i = 0; i < 32; i++) {
        dut->src_c[i] = data[i];
    }
}

// 获取结果
void get_result(Valu_pipeline* dut, uint32_t* out) {
    for (int i = 0; i < 32; i++) {
        out[i] = dut->result[i];
    }
}

// 打印结果
void print_lanes(uint32_t* data, const char* name) {
    printf("%s: [", name);
    for (int i = 0; i < 4; i++) {
        printf("0x%08x%s", data[i], i < 3 ? ", " : "");
    }
    printf(" ... ");
    for (int i = 28; i < 32; i++) {
        printf("0x%08x%s", data[i], i < 31 ? ", " : "");
    }
    printf("]\n");
}

// 检查32个lane
bool check_lanes(uint32_t* actual, uint32_t* expected, const char* test_name) {
    bool pass = true;
    for (int i = 0; i < 32; i++) {
        if (actual[i] != expected[i]) {
            printf("  [FAIL] Lane %d: expected 0x%08x, got 0x%08x\n", 
                   i, expected[i], actual[i]);
            pass = false;
            if (i >= 3) break; // 只显示前3个错误
        }
    }
    if (pass) {
        printf("\033[32m[PASS]\033[0m %s\n", test_name);
    } else {
        printf("\033[31m[FAIL]\033[0m %s\n", test_name);
    }
    return pass;
}

// 发送指令（修正时序：在时钟低电平设置，上升沿采样）
void send_instr(Valu_pipeline* dut, VerilatedVcdC* trace, 
                uint32_t instr, uint32_t* a, uint32_t* b, uint32_t* c = nullptr) {
    // 在时钟低电平设置输入（setup time）
    dut->clk = 0;
    dut->eval();
    
    dut->valid_in = 1;
    dut->instr = instr;
    set_src_a(dut, a);
    set_src_b(dut, b);
    if (c) set_src_c(dut, c);
    
    dut->eval();
    if (trace) trace->dump(sim_time++);
    
    // 上升沿采样
    dut->clk = 1;
    dut->eval();
    if (trace) trace->dump(sim_time++);
    
    // 立即撤除valid（单周期脉冲）
    dut->valid_in = 0;
}

// 等待结果（流水线深度3，需要3-4周期）
bool wait_result(Valu_pipeline* dut, VerilatedVcdC* trace, uint32_t* out, int max_cycles = 10) {
    for (int i = 0; i < max_cycles; i++) {
        // 先下降沿
        dut->clk = 0;
        dut->eval();
        if (trace) trace->dump(sim_time++);
        
        // 再上升沿（采样点）
        dut->clk = 1;
        dut->eval();
        if (trace) trace->dump(sim_time++);
        
        if (dut->valid_out) {
            get_result(dut, out);
            return true;
        }
    }
    return false;
}

// 调试：打印流水线状态
void debug_pipeline(Valu_pipeline* dut) {
    printf("  valid_in=%d, ready_in=%d, valid_out=%d\n", 
           dut->valid_in, dut->ready_in, dut->valid_out);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Valu_pipeline* dut = new Valu_pipeline;
    VerilatedVcdC* m_trace = nullptr;
    
    // 可选：开启波形
    #ifdef TRACE
    Verilated::traceEverOn(true);
    m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("alu_pipeline.vcd");
    #endif
    
    printf("========================================\n");
    printf("ALU Pipeline Testbench (32 lanes)\n");
    printf("========================================\n\n");
    
    int passed = 0, failed = 0;
    uint32_t a[32], b[32], c[32], result[32], expected[32];
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->instr = 0;
    for (int i = 0; i < 32; i++) {
        dut->src_a[i] = 0;
        dut->src_b[i] = 0;
        dut->src_c[i] = 0;
    }
    
    // 复位（5个周期）
    printf("Reset...\n");
    for (int i = 0; i < 5; i++) {
        dut->clk = 0;
        dut->eval();
        if (m_trace) m_trace->dump(sim_time++);
        dut->clk = 1;
        dut->eval();
        if (m_trace) m_trace->dump(sim_time++);
    }
    dut->rst_n = 1;
    dut->eval();
    printf("Reset done, ready_in=%d\n\n", dut->ready_in);
    
    // ========== Test 1: ADD ==========
    printf("Test 1: ADD (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 0x10000000 + i;
        b[i] = 0x00000001;
        expected[i] = 0x10000001 + i;
    }
    send_instr(dut, m_trace, make_instr(OP_ADD), a, b);
    
    printf("  Sent ADD, waiting for result...\n");
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "ADD")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m ADD timeout (valid_out never high)\n");
        debug_pipeline(dut);
        failed++;
    }
    printf("\n");
    
    // ========== Test 2: SUB ==========
    printf("Test 2: SUB (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 0x00000100 + i * 16;
        b[i] = 0x00000010;
        expected[i] = 0x000000F0 + i * 16;
    }
    send_instr(dut, m_trace, make_instr(OP_SUB), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "SUB")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m SUB timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 3: MUL ==========
    printf("Test 3: MUL (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = i + 1;
        b[i] = (i + 1) * 2;
        expected[i] = (i + 1) * (i + 1) * 2;
    }
    send_instr(dut, m_trace, make_instr(OP_MUL), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "MUL")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m MUL timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 4: MAD ==========
    printf("Test 4: MAD a*b+c (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 2 + i;
        b[i] = 3;
        c[i] = 10;
        expected[i] = (2 + i) * 3 + 10;
    }
    send_instr(dut, m_trace, make_instr(OP_MAD), a, b, c);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "MAD")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m MAD timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 5: AND ==========
    printf("Test 5: AND (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 0xFF00FF00 | (i << 16);
        b[i] = 0x0F0F0F0F;
        expected[i] = a[i] & b[i];
    }
    send_instr(dut, m_trace, make_instr(OP_AND), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "AND")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m AND timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 6: OR ==========
    printf("Test 6: OR (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 0xF0000000 | i;
        b[i] = 0x0F000000 | i;
        expected[i] = a[i] | b[i];
    }
    send_instr(dut, m_trace, make_instr(OP_OR), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "OR")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m OR timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 7: SHL ==========
    printf("Test 7: SHL by 4 (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 0x0000000F + i;
        b[i] = 4;
        expected[i] = a[i] << 4;
    }
    send_instr(dut, m_trace, make_instr(OP_SHL), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "SHL")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m SHL timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 8: SHR ==========
    printf("Test 8: SHR by 2 (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = 0xFF000000 + (i << 20);
        b[i] = 2;
        expected[i] = a[i] >> 2;
    }
    send_instr(dut, m_trace, make_instr(OP_SHR), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "SHR")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m SHR timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 9: MIN ==========
    printf("Test 9: MIN signed (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = (i < 16) ? (0x80000000 + i) : i;  // 前16个负数
        b[i] = 100;
        int32_t sa = (int32_t)a[i];
        int32_t sb = (int32_t)b[i];
        expected[i] = (sa < sb) ? a[i] : b[i];
    }
    send_instr(dut, m_trace, make_instr(OP_MIN), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "MIN")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m MIN timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 10: MAX ==========
    printf("Test 10: MAX signed (32 lanes)\n");
    for (int i = 0; i < 32; i++) {
        a[i] = i * 10;
        b[i] = 150;
        int32_t sa = (int32_t)a[i];
        int32_t sb = (int32_t)b[i];
        expected[i] = (sa > sb) ? a[i] : b[i];
    }
    send_instr(dut, m_trace, make_instr(OP_MAX), a, b);
    
    if (wait_result(dut, m_trace, result)) {
        print_lanes(result, "  Result");
        if (check_lanes(result, expected, "MAX")) passed++; else failed++;
    } else {
        printf("\033[31m[FAIL]\033[0m MAX timeout\n");
        failed++;
    }
    printf("\n");
    
    // ========== Test 11: Pipeline Back-to-Back ==========
    printf("Test 11: Pipeline back-to-back (3 instructions)\n");
    
    // 清空流水线
    for (int i = 0; i < 5; i++) posedge(dut, m_trace);
    
    // 指令1: ADD (1+2=3)
    for (int i = 0; i < 32; i++) { a[i] = 1; b[i] = 2; }
    send_instr(dut, m_trace, make_instr(OP_ADD), a, b);
    
    // 指令2: SUB (10-3=7) - 在指令1的Execute阶段进入
    for (int i = 0; i < 32; i++) { a[i] = 10; b[i] = 3; }
    send_instr(dut, m_trace, make_instr(OP_SUB), a, b);
    
    // 指令3: MUL (5*6=30) - 在指令1的Writeback阶段进入
    for (int i = 0; i < 32; i++) { a[i] = 5; b[i] = 6; }
    send_instr(dut, m_trace, make_instr(OP_MUL), a, b);
    
    // 收集3个结果
    uint32_t r1[32], r2[32], r3[32];
    int results = 0;
    bool r1_ok = false, r2_ok = false, r3_ok = false;
    
    for (int i = 0; i < 6 && results < 3; i++) {
        if (dut->valid_out) {
            results++;
            if (results == 1) {
                get_result(dut, r1);
                r1_ok = (r1[0] == 3);
                printf("  Result 1 (ADD): %s (lane0=0x%08x, exp=0x%08x)\n", 
                       r1_ok ? "OK" : "FAIL", r1[0], 3);
            } else if (results == 2) {
                get_result(dut, r2);
                r2_ok = (r2[0] == 7);
                printf("  Result 2 (SUB): %s (lane0=0x%08x, exp=0x%08x)\n", 
                       r2_ok ? "OK" : "FAIL", r2[0], 7);
            } else {
                get_result(dut, r3);
                r3_ok = (r3[0] == 30);
                printf("  Result 3 (MUL): %s (lane0=0x%08x, exp=0x%08x)\n", 
                       r3_ok ? "OK" : "FAIL", r3[0], 30);
            }
        }
        posedge(dut, m_trace);
    }
    
    if (results == 3 && r1_ok && r2_ok && r3_ok) {
        printf("\033[32m[PASS]\033[0m Pipeline back-to-back\n");
        passed++;
    } else {
        printf("\033[31m[FAIL]\033[0m Pipeline: got %d results, r1=%d, r2=%d, r3=%d\n", 
               results, r1_ok, r2_ok, r3_ok);
        failed++;
    }
    printf("\n");
    
    // ========== Test 12: Pipeline with 1-cycle gap ==========
    printf("Test 12: Pipeline with 1-cycle gap\n");
    
    // 清空流水线
    for (int i = 0; i < 5; i++) posedge(dut, m_trace);
    
    // 指令1: ADD (100+200=300)
    for (int i = 0; i < 32; i++) { a[i] = 100; b[i] = 200; }
    send_instr(dut, m_trace, make_instr(OP_ADD), a, b);
    
    // 等待1周期
    //posedge(dut, m_trace);
    
    // 指令2: SUB (500-123=377)
    for (int i = 0; i < 32; i++) { a[i] = 500; b[i] = 123; }
    send_instr(dut, m_trace, make_instr(OP_SUB), a, b);
    
    // 等待1周期
    //posedge(dut, m_trace);
    
    // 指令3: MUL (7*8=56)
    for (int i = 0; i < 32; i++) { a[i] = 7; b[i] = 8; }
    send_instr(dut, m_trace, make_instr(OP_MUL), a, b);
    
    // 收集结果
    uint32_t rg1[32], rg2[32], rg3[32];
    bool rg1_ok = false, rg2_ok = false, rg3_ok = false;
    int rg_results = 0;
    
    for (int i = 0; i < 6 && rg_results < 3; i++) {
        if (dut->valid_out) {
            rg_results++;
            if (rg_results == 1) {
                get_result(dut, rg1);
                rg1_ok = (rg1[0] == 300);
                printf("  Result 1 (ADD): %s (lane0=0x%08x, exp=0x%08x)\n", 
                       rg1_ok ? "OK" : "FAIL", rg1[0], 300);
            } else if (rg_results == 2) {
                get_result(dut, rg2);
                rg2_ok = (rg2[0] == 377);
                printf("  Result 2 (SUB): %s (lane0=0x%08x, exp=0x%08x)\n", 
                       rg2_ok ? "OK" : "FAIL", rg2[0], 377);
            } else {
                get_result(dut, rg3);
                rg3_ok = (rg3[0] == 56);
                printf("  Result 3 (MUL): %s (lane0=0x%08x, exp=0x%08x)\n", 
                       rg3_ok ? "OK" : "FAIL", rg3[0], 56);
            }
        }
        posedge(dut, m_trace);
    }
    
    if (rg_results == 3 && rg1_ok && rg2_ok && rg3_ok) {
        printf("\033[32m[PASS]\033[0m Pipeline with gap\n");
        passed++;
    } else {
        printf("\033[31m[FAIL]\033[0m Pipeline gap: got %d results\n", rg_results);
        failed++;
    }
    printf("\n");
    
    // ========== 总结 ==========
    printf("========================================\n");
    printf("Test Summary: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");
    
    #ifdef TRACE
    if (m_trace) {
        m_trace->close();
        delete m_trace;
    }
    #endif
    delete dut;
    
    return failed > 0 ? 1 : 0;
}