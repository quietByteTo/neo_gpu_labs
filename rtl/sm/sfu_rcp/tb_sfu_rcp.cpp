// ============================================================================
// Testbench for RCP Only SFU
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "Vsfu.h"

vluint64_t sim_time = 0;

// Helper: float <-> uint32_t
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

// Clock helper
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
    trace->open("sfu_rcp.vcd");
    #endif
    
    printf("=====================================\n");
    printf("SFU RCP Test (Newton-Raphson)\n");
    printf("=====================================\n\n");
    
    // Initialize
    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->sfu_op = 0;  // RCP
    
    // Reset
    printf("Resetting...\n");
    for (int i = 0; i < 5; i++) tick(dut, trace);
    dut->rst_n = 1;
    tick(dut, trace);
    printf("Ready = %d\n\n", dut->ready);
    
    // Test cases: input -> expected output
    float test_inputs[] = {1.0f, 2.0f, 4.0f, 1.5f, 0.5f, 10.0f};
    int num_tests = sizeof(test_inputs) / sizeof(test_inputs[0]);
    int passed = 0;
    
    for (int t = 0; t < num_tests; t++) {
        float input_val = test_inputs[t];
        float expected = 1.0f / input_val;
        uint32_t input_bits = f2u(input_val);
        
        printf("Test %d: RCP(%f) = ? (expected %f)\n", t+1, input_val, expected);
        
        // Setup input (all lanes same value for simplicity)
        for (int lane = 0; lane < 32; lane++) {
            dut->src[lane] = input_bits;
        }
        dut->sfu_op = 0;  // RCP
        dut->valid_in = 1;
        tick(dut, trace);
        dut->valid_in = 0;
        
        // Wait for computation (3 cycles: SEED->ITER1->ITER2)
        int cycles = 0;
        while (!dut->valid_out && cycles < 10) {
            tick(dut, trace);
            cycles++;
        }
        
        if (dut->valid_out) {
            uint32_t result_bits = dut->result[0];  // Check lane 0
            float result_val = u2f(result_bits);
            float error = fabs(result_val - expected) / expected;
            
            printf("  Result: %f (hex: %08X)\n", result_val, result_bits);
            printf("  Expected: %f (hex: %08X)\n", expected, f2u(expected));
            printf("  Cycles: %d, Relative error: %f%%\n", cycles, error * 100);
            
            if (error < 0.01) {  // 1% tolerance
                printf("  [PASS]\n");
                passed++;
            } else {
                printf("  [FAIL]\n");
            }
        } else {
            printf("  [TIMEOUT]\n");
        }
        printf("\n");
        
        // Wait one cycle between tests
        tick(dut, trace);
    }
    
    printf("=====================================\n");
    printf("Results: %d/%d passed\n", passed, num_tests);
    printf("=====================================\n");
    
    if (trace) {
        trace->close();
        delete trace;
    }
    delete dut;
    
    return (passed == num_tests) ? 0 : 1;
}