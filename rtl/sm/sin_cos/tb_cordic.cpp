// ============================================================================
// Testbench for CORDIC SIN/COS (Fixed Point Q16.16)
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "Vcordic_sincos.h"

vluint64_t sim_time = 0;

// 定点数转换
const float Q_SCALE = 65536.0f;  // 2^16

uint32_t f2q(float f) {
    return (uint32_t)(int32_t)(f * Q_SCALE);
}

float q2f(uint32_t q) {
    return (float)(int32_t)q / Q_SCALE;
}

// Clock tick
void tick(Vcordic_sincos* dut, VerilatedVcdC* trace) {
    dut->clk = 0;
    dut->eval();
    if (trace) trace->dump(sim_time++);
    
    dut->clk = 1;
    dut->eval();
    if (trace) trace->dump(sim_time++);
}

// Run CORDIC
bool run_cordic(Vcordic_sincos* dut, VerilatedVcdC* trace, float angle_rad, bool cos_sel, 
                float* result, int max_cycles = 15) {
    // 输入转换为定点数
    int32_t angle_q = (int32_t)(angle_rad * Q_SCALE);
    
    for (int i = 0; i < 32; i++) {
        dut->angle[i] = angle_q;
    }
    dut->cos_sel = cos_sel;
    dut->valid_in = 1;
    tick(dut, trace);
    dut->valid_in = 0;
    
    int cycles = 0;
    while (!dut->valid_out && cycles < max_cycles) {
        tick(dut, trace);
        cycles++;
    }
    
    if (dut->valid_out) {
        *result = q2f(dut->result[0]);
        return true;
    }
    return false;
}

bool check(float actual, float expected, float tol_percent) {
    if (fabs(expected) < 0.01f) return fabs(actual - expected) < 0.02f;
    float rel_err = fabs(actual - expected) / fabs(expected) * 100;
    return rel_err < tol_percent;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Vcordic_sincos* dut = new Vcordic_sincos;
    VerilatedVcdC* trace = nullptr;
    
    Verilated::traceEverOn(true);
    trace = new VerilatedVcdC;
    dut->trace(trace, 5);
    trace->open("cordic.vcd");
    
    printf("========================================\n");
    printf("CORDIC SIN/COS Test (Fixed Point Q16.16)\n");
    printf("========================================\n\n");
    
    int passed = 0, total = 0;
    
    // Reset
    dut->clk = 0; dut->rst_n = 0; dut->valid_in = 0;
    for (int i = 0; i < 5; i++) tick(dut, trace);
    dut->rst_n = 1;
    tick(dut, trace);
    
    // SIN Test
    printf("--- SIN ---\n");
    struct { float angle, expected; } tests[] = {
        {0.0f, 0.0f},
        {0.5f, 0.4794f},
        {1.0f, 0.8415f},
        {1.5708f, 1.0f},
        {-0.5f, -0.4794f},
        {0.7854f, 0.7071f},
        {0.25f, 0.2474f},
        {-1.0f, -0.8415f}
    };
    
    for (auto& t : tests) {
        float res; total++;
        printf("SIN(%.4f) = ", t.angle);
        if (run_cordic(dut, trace, t.angle, 0, &res, 15)) {
            float err = fabs(res - t.expected) / (fabs(t.expected) + 0.001f) * 100;
            printf("%.6f (exp %.4f, err %.2f%%) %s\n", 
                   res, t.expected, err,
                   check(res, t.expected, 3.0) ? "[PASS]" : "[FAIL]");
            if (check(res, t.expected, 3.0)) passed++;
        } else {
            printf("TIMEOUT [FAIL]\n");
        }
    }
    printf("\n");
    
    // COS Test
    printf("--- COS ---\n");
    struct { float angle, expected; } cos_tests[] = {
        {0.0f, 1.0f},
        {0.5f, 0.8776f},
        {1.0f, 0.5403f},
        {1.5708f, 0.0f},
        {-0.5f, 0.8776f},
        {0.7854f, 0.7071f}
    };
    
    for (auto& t : cos_tests) {
        float res; total++;
        printf("COS(%.4f) = ", t.angle);
        if (run_cordic(dut, trace, t.angle, 1, &res, 15)) {
            float err = fabs(res - t.expected) / (fabs(t.expected) + 0.001f) * 100;
            printf("%.6f (exp %.4f, err %.2f%%) %s\n", 
                   res, t.expected, err,
                   check(res, t.expected, 3.0) ? "[PASS]" : "[FAIL]");
            if (check(res, t.expected, 3.0)) passed++;
        } else {
            printf("TIMEOUT [FAIL]\n");
        }
    }
    printf("\n");
    
    printf("========================================\n");
    printf("Results: %d/%d passed\n", passed, total);
    printf("========================================\n");
    
    if (trace) { trace->close(); delete trace; }
    delete dut;
    
    return (passed == total) ? 0 : 1;
}

