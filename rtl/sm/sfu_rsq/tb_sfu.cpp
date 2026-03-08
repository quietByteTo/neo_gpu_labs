// ============================================================================
// Testbench for RCP and RSQ
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "Vsfu.h"

vluint64_t sim_time = 0;

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

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Vsfu* dut = new Vsfu;
    VerilatedVcdC* trace = nullptr;
    
    #ifdef TRACE
    Verilated::traceEverOn(true);
    trace = new VerilatedVcdC;
    dut->trace(trace, 5);
    trace->open("sfu.vcd");
    #endif
    
    printf("=====================================\n");
    printf("SFU RCP/RSQ Test\n");
    printf("=====================================\n\n");
    
    // Initialize
    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->sfu_op = 0;
    
    // Reset
    printf("Resetting...\n");
    for (int i = 0; i < 5; i++) tick(dut, trace);
    dut->rst_n = 1;
    tick(dut, trace);
    printf("Ready = %d\n\n", dut->ready);
    
    int passed = 0;
    int total = 0;
    
    // ========== Test RCP ==========
    printf("--- Testing RCP (1/x) ---\n");
    float rcp_tests[] = {1.0f, 2.0f, 4.0f, 1.5f, 0.5f, 10.0f};
    int num_rcp = sizeof(rcp_tests) / sizeof(float);
    
    for (int t = 0; t < num_rcp; t++) {
        float input_val = rcp_tests[t];
        float expected = 1.0f / input_val;
        total++;
        
        printf("RCP(%.2f) = ", input_val);
        
        for (int lane = 0; lane < 32; lane++) {
            dut->src[lane] = f2u(input_val);
        }
        dut->sfu_op = 0;  // RCP
        dut->valid_in = 1;
        tick(dut, trace);
        dut->valid_in = 0;
        
        int cycles = 0;
        while (!dut->valid_out && cycles < 10) {
            tick(dut, trace);
            cycles++;
        }
        
        if (dut->valid_out) {
            float result = u2f(dut->result[0]);
            float error = fabs(result - expected) / expected * 100;
            printf("%.6f (expected %.6f, error %.3f%%) ", result, expected, error);
            if (error < 1.0) {
                printf("[PASS]\n");
                passed++;
            } else {
                printf("[FAIL]\n");
            }
        } else {
            printf("TIMEOUT [FAIL]\n");
        }
    }
    printf("\n");
    
    // ========== Test RSQ ==========
    printf("--- Testing RSQ (1/sqrt(x)) ---\n");
    float rsq_tests[] = {1.0f, 4.0f, 9.0f, 16.0f, 0.25f, 100.0f};
    int num_rsq = sizeof(rsq_tests) / sizeof(float);
    
    for (int t = 0; t < num_rsq; t++) {
        float input_val = rsq_tests[t];
        float expected = 1.0f / sqrt(input_val);
        total++;
        
        printf("RSQ(%.2f) = ", input_val);
        
        for (int lane = 0; lane < 32; lane++) {
            dut->src[lane] = f2u(input_val);
        }
        dut->sfu_op = 1;  // RSQ
        dut->valid_in = 1;
        tick(dut, trace);
        dut->valid_in = 0;
        
        int cycles = 0;
        while (!dut->valid_out && cycles < 10) {
            tick(dut, trace);
            cycles++;
        }
        
        if (dut->valid_out) {
            float result = u2f(dut->result[0]);
            float error = fabs(result - expected) / expected * 100;
            printf("%.6f (expected %.6f, error %.3f%%) ", result, expected, error);
            if (error < 1.0) {
                printf("[PASS]\n");
                passed++;
            } else {
                printf("[FAIL]\n");
            }
        } else {
            printf("TIMEOUT [FAIL]\n");
        }
    }
    
    printf("\n=====================================\n");
    printf("Results: %d/%d passed\n", passed, total);
    printf("=====================================\n");
    
    if (trace) {
        trace->close();
        delete trace;
    }
    delete dut;
    
    return (passed == total) ? 0 : 1;
}
