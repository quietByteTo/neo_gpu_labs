// ============================================================================
// Testbench for SFU - RCP, RSQ, SIN, COS (Integrated)
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "Vsfu.h"

vluint64_t sim_time = 0;

#define OP_RCP  0
#define OP_RSQ  1
#define OP_SIN  2
#define OP_COS  3

uint32_t f2u(float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    return conv.u;
}

float u2f(uint32_t u) {
    union { float f; uint32_t u; } conv;
    conv.u = u;
    return conv.f;
}

void tick(Vsfu* dut, VerilatedVcdC* trace) {
    dut->clk = 0;
    dut->eval();
    if (trace) trace->dump(sim_time++);
    
    dut->clk = 1;
    dut->eval();
    if (trace) trace->dump(sim_time++);
}

bool run_sfu(Vsfu* dut, VerilatedVcdC* trace, int op, float input, 
             float* result, int max_cycles = 20) {
    for (int i = 0; i < 32; i++) {
        dut->src[i] = f2u(input);
    }
    dut->sfu_op = op;
    dut->valid_in = 1;
    tick(dut, trace);
    dut->valid_in = 0;
    
    int cycles = 0;
    while (!dut->valid_out && cycles < max_cycles) {
        tick(dut, trace);
        cycles++;
    }
    
    if (dut->valid_out) {
        *result = u2f(dut->result[0]);
        return true;
    }
    return false;
}

bool check(float actual, float expected, float tol_percent) {
    if (std::isnan(expected)) return std::isnan(actual);
    if (std::isinf(expected)) return std::isinf(actual);
    if (fabs(expected) < 0.001f) return fabs(actual) < 0.02f;
    float rel_err = fabs(actual - expected) / (fabs(expected) + 1e-6f) * 100;
    return rel_err < tol_percent;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Vsfu* dut = new Vsfu;
    VerilatedVcdC* trace = nullptr;
    
    Verilated::traceEverOn(true);
    trace = new VerilatedVcdC;
    dut->trace(trace, 5);
    trace->open("sfu.vcd");
    
    printf("========================================\n");
    printf("SFU Test - RCP / RSQ / SIN / COS\n");
    printf("========================================\n\n");
    
    int passed = 0, total = 0;
    
    dut->clk = 0; dut->rst_n = 0; dut->valid_in = 0;
    for (int i = 0; i < 5; i++) tick(dut, trace);
    dut->rst_n = 1;
    tick(dut, trace);
    
    // RCP Test
    printf("--- RCP (Reciprocal 1/x) ---\n");
    struct { float in, exp; } rcp_tests[] = {
        {1.0f, 1.0f}, {2.0f, 0.5f}, {4.0f, 0.25f},
        {1.5f, 0.6667f}, {0.5f, 2.0f}, {10.0f, 0.1f}
    };
    for (auto& t : rcp_tests) {
        float res; total++;
        printf("RCP(%.2f) = ", t.in);
        if (run_sfu(dut, trace, OP_RCP, t.in, &res, 6)) {
            float err = fabs(res-t.exp)/t.exp*100;
            printf("%.6f (exp %.4f, err %.2f%%) %s\n", 
                   res, t.exp, err,
                   check(res, t.exp, 2.0) ? "[PASS]" : "[FAIL]");
            if (check(res, t.exp, 2.0)) passed++;
        } else printf("TIMEOUT [FAIL]\n");
    }
    printf("\n");
    
    // RSQ Test
    printf("--- RSQ (Reciprocal Sqrt 1/sqrt(x)) ---\n");
    struct { float in, exp; } rsq_tests[] = {
        {1.0f, 1.0f}, {4.0f, 0.5f}, {9.0f, 0.3333f},
        {16.0f, 0.25f}, {0.25f, 2.0f}, {100.0f, 0.1f}
    };
    for (auto& t : rsq_tests) {
        float res; total++;
        printf("RSQ(%.2f) = ", t.in);
        if (run_sfu(dut, trace, OP_RSQ, t.in, &res, 8)) {
            float err = fabs(res-t.exp)/t.exp*100;
            printf("%.6f (exp %.4f, err %.2f%%) %s\n", 
                   res, t.exp, err,
                   check(res, t.exp, 3.0) ? "[PASS]" : "[FAIL]");
            if (check(res, t.exp, 3.0)) passed++;
        } else printf("TIMEOUT [FAIL]\n");
    }
    printf("\n");
    
    // SIN Test
    printf("--- SIN (Sine) ---\n");
    struct { float in, exp; } sin_tests[] = {
        {0.0f, 0.0f}, {0.5f, 0.4794f}, {1.0f, 0.8415f},
        {1.5708f, 1.0f}, {-0.5f, -0.4794f}, {0.7854f, 0.7071f}
    };
    for (auto& t : sin_tests) {
        float res; total++;
        printf("SIN(%.4f) = ", t.in);
        if (run_sfu(dut, trace, OP_SIN, t.in, &res, 12)) {
            float err = fabs(res-t.exp)/(fabs(t.exp)+0.001f)*100;
            printf("%.6f (exp %.4f, err %.2f%%) %s\n", 
                   res, t.exp, err,
                   check(res, t.exp, 3.0) ? "[PASS]" : "[FAIL]");
            if (check(res, t.exp, 3.0)) passed++;
        } else printf("TIMEOUT [FAIL]\n");
    }
    printf("\n");
    
    // COS Test
    printf("--- COS (Cosine) ---\n");
    struct { float in, exp; } cos_tests[] = {
        {0.0f, 1.0f}, {0.5f, 0.8776f}, {1.0f, 0.5403f},
        {1.5708f, 0.0f}, {-0.5f, 0.8776f}, {0.7854f, 0.7071f}
    };
    for (auto& t : cos_tests) {
        float res; total++;
        printf("COS(%.4f) = ", t.in);
        if (run_sfu(dut, trace, OP_COS, t.in, &res, 12)) {
            float err = fabs(res-t.exp)/(fabs(t.exp)+0.001f)*100;
            printf("%.6f (exp %.4f, err %.2f%%) %s\n", 
                   res, t.exp, err,
                   check(res, t.exp, 3.0) ? "[PASS]" : "[FAIL]");
            if (check(res, t.exp, 3.0)) passed++;
        } else printf("TIMEOUT [FAIL]\n");
    }
    printf("\n");
    
    printf("========================================\n");
    printf("Results: %d/%d passed\n", passed, total);
    printf("========================================\n");
    
    if (trace) { trace->close(); delete trace; }
    delete dut;
    
    return (passed == total) ? 0 : 1;
}

