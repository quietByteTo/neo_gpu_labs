// tb_tensor_core.cpp - 完整修复版本
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <iostream>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "Vtensor_core.h"

#define ARRAY_SIZE 4
#define FP16_W 16
#define FP32_W 32

vluint64_t main_time = 0;
double sc_time_stamp() { return main_time; }

void tick(Vtensor_core* top, VerilatedVcdC* tfp) {
    top->clk = 0; top->eval();
    if (tfp) tfp->dump(main_time++);
    top->clk = 1; top->eval();
    if (tfp) tfp->dump(main_time++);
}

// 清零所有矩阵输入（修复：16个元素）
void clear_mat(Vtensor_core* top) {
    for (int i = 0; i < 16; i++) {
        top->mat_a[i] = 0;
        top->mat_b[i] = 0;
        top->mat_c[i] = 0;
    }
}

void set_mat_a(Vtensor_core* top, int row, int col, uint16_t val) {
    int idx = row * ARRAY_SIZE + col;
    top->mat_a[idx] = val;
}

void set_mat_b(Vtensor_core* top, int row, int col, uint16_t val) {
    int idx = row * ARRAY_SIZE + col;
    top->mat_b[idx] = val;
}

void set_mat_c(Vtensor_core* top, int row, int col, uint32_t val) {
    int idx = row * ARRAY_SIZE + col;
    top->mat_c[idx] = val;
}

uint32_t get_mat_d(Vtensor_core* top, int row, int col) {
    int idx = row * ARRAY_SIZE + col;
    return top->mat_d[idx];
}

void print_mat_d(Vtensor_core* top, const char* name) {
    std::cout << name << " = [" << std::endl;
    for (int i = 0; i < ARRAY_SIZE; i++) {
        std::cout << "  ";
        for (int j = 0; j < ARRAY_SIZE; j++) {
            std::cout << get_mat_d(top, i, j) << " ";
        }
        std::cout << std::endl;
    }
    std::cout << "]" << std::endl;
}

// 新增：验证矩阵D的所有元素是否等于期望值
bool verify_mat_d_all(Vtensor_core* top, uint32_t expected) {
    for (int i = 0; i < ARRAY_SIZE; i++) {
        for (int j = 0; j < ARRAY_SIZE; j++) {
            if (get_mat_d(top, i, j) != expected) {
                return false;
            }
        }
    }
    return true;
}

// 新增：验证矩阵D是否为零矩阵
bool verify_mat_d_zero(Vtensor_core* top) {
    for (int i = 0; i < ARRAY_SIZE; i++) {
        for (int j = 0; j < ARRAY_SIZE; j++) {
            if (get_mat_d(top, i, j) != 0) {
                return false;
            }
        }
    }
    return true;
}

// 新增：测试间复位，确保状态机和缓冲清零
void reset_between_tests(Vtensor_core* top, VerilatedVcdC* tfp) {
    top->rst_n = 0;
    top->valid_in = 0;
    clear_mat(top);
    for (int i = 0; i < 3; i++) tick(top, tfp);  // 复位几个周期
    top->rst_n = 1;
    tick(top, tfp);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    
    Vtensor_core* top = new Vtensor_core;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("tensor_core.vcd");
    
    std::cout << "========================================" << std::endl;
    std::cout << "    Tensor Core Testbench Started       " << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 全局初始化
    top->rst_n = 0;
    top->valid_in = 0;
    top->precision = 0;
    top->layout = 0;
    clear_mat(top);
    
    // 初始复位
    for (int i = 0; i < 5; i++) tick(top, tfp);
    top->rst_n = 1;
    tick(top, tfp);
    
    int test_count = 0;
    int pass_count = 0;
    
    // Test 1: 单位矩阵乘法（A × I = A）
    std::cout << "\n[Test 1] Identity Matrix Multiplication..." << std::endl;
    {
        clear_mat(top);
        
        // A = [1, 2, 3, 4; 5, 6, 7, 8; 9, 10, 11, 12; 13, 14, 15, 16]
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                set_mat_a(top, i, j, (i * ARRAY_SIZE + j + 1));
            }
        }
        
        // B = I（单位矩阵）
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                set_mat_b(top, i, j, (i == j) ? 1 : 0);
            }
        }
        
        // C = 0（已清零）
        
        top->valid_in = 1;
        tick(top, tfp);
        top->valid_in = 0;
        
        int timeout = 20;
        while (!top->valid_out && timeout-- > 0) {
            tick(top, tfp);
        }
        
        assert(top->valid_out == 1);
        print_mat_d(top, "Result D");
        
        // 验证：D[i][j] 应该等于 A[i][j] = i*4 + j + 1
        bool pass = true;
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                uint32_t expected = i * ARRAY_SIZE + j + 1;
                if (get_mat_d(top, i, j) != expected) {
                    std::cout << "  FAIL: D[" << i << "][" << j << "] = " 
                              << get_mat_d(top, i, j) << ", expected " << expected << std::endl;
                    pass = false;
                }
            }
        }
        
        test_count++;
        if (pass) {
            pass_count++;
            std::cout << "  [PASS] Identity multiplication completed" << std::endl;
        } else {
            std::cout << "  [FAIL] Identity multiplication failed" << std::endl;
        }
    }
    
    // 测试间复位（关键：清空可能残留的状态）
    reset_between_tests(top, tfp);
    
    // Test 2: 零矩阵乘法
    std::cout << "\n[Test 2] Zero Matrix Multiplication..." << std::endl;
    {
        clear_mat(top);
        
        // A = 任意值（例如 (i+1)*(j+1)）
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                set_mat_a(top, i, j, (i + 1) * (j + 1));
            }
        }
        
        // B = 0（已清零）
        // C = 0（已清零）
        
        top->valid_in = 1;
        tick(top, tfp);
        top->valid_in = 0;
        
        int timeout = 20;
        while (!top->valid_out && timeout-- > 0) {
            tick(top, tfp);
        }
        
        assert(top->valid_out == 1);
        print_mat_d(top, "Result D (should be all 0)");
        
        // 验证：结果应该全为0
        bool pass = verify_mat_d_zero(top);
        
        test_count++;
        if (pass) {
            pass_count++;
            std::cout << "  [PASS] Zero multiplication completed" << std::endl;
        } else {
            std::cout << "  [FAIL] Zero multiplication failed - result not zero" << std::endl;
        }
    }
    
    // 测试间复位
    reset_between_tests(top, tfp);
    
    // Test 3: 累加测试
    std::cout << "\n[Test 3] Accumulation Test..." << std::endl;
    {
        clear_mat(top);
        
        // A = 全1
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                set_mat_a(top, i, j, 1);
            }
        }
        
        // B = 单位矩阵
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                set_mat_b(top, i, j, (i == j) ? 1 : 0);
            }
        }
        
        // C = 100
        for (int i = 0; i < ARRAY_SIZE; i++) {
            for (int j = 0; j < ARRAY_SIZE; j++) {
                set_mat_c(top, i, j, 100);
            }
        }
        
        top->valid_in = 1;
        tick(top, tfp);
        top->valid_in = 0;
        
        int timeout = 20;
        while (!top->valid_out && timeout-- > 0) {
            tick(top, tfp);
        }
        
        assert(top->valid_out == 1);
        print_mat_d(top, "Result D (should be all 104)");
        
        // 验证：D = A×B + C = 全1矩阵 + 100 = 全104
        // A×B: A是全1，B是单位矩阵，结果每行是B的对应行和 = [1,1,1,1]（因为单位矩阵每行只有一个1）
        // 实际上：A×B的第i行第j列 = sum_k(A[i][k] * B[k][j]) = sum_k(1 * (k==j ? 1 : 0)) = 1
        // 所以 A×B = 全1矩阵，D = 1 + 100 = 101？不对，让我重新计算...
        
        // 修正：A是全1（4x4），B是单位矩阵（4x4）
        // (A×B)[i][j] = sum_{k=0 to 3} A[i][k] * B[k][j] = sum_{k=0 to 3} 1 * (k==j ? 1 : 0) = 1
        // 所以 A×B = 全1矩阵，D = 1 + 100 = 101
        
        // 等等，如果B是单位矩阵，A×B = A，所以结果是全1 + 100 = 101？
        // 但之前输出显示104，让我再检查...
        
        // 实际上之前的"104"可能是错误的残留值。正确的应该是：
        // A×B = 全1矩阵（因为A的每行乘以B的每列，B只有对角线为1）
        // 所以每个元素 = 1*1 = 1
        // D = 1 + 100 = 101
        
        bool pass = verify_mat_d_all(top, 101);  // 修正期望值
        
        test_count++;
        if (pass) {
            pass_count++;
            std::cout << "  [PASS] Accumulation completed" << std::endl;
        } else {
            std::cout << "  [FAIL] Accumulation failed - expected all 101" << std::endl;
        }
    }
    
    // 测试间复位
    reset_between_tests(top, tfp);
    
    // Test 4: 连续计算（关键修复：等待ready后再发下一个）
    std::cout << "\n[Test 4] Back-to-back Computation..." << std::endl;
    {
        int num_tests = 3;
        int submitted = 0;
        int results = 0;
        int timeout = 200;
        
        // 阶段1：顺序提交所有测试（等待ready）
        while (submitted < num_tests && timeout-- > 0) {
            tick(top, tfp);
            
            if (top->ready) {
                clear_mat(top);
                for (int i = 0; i < ARRAY_SIZE; i++) {
                    for (int j = 0; j < ARRAY_SIZE; j++) {
                        set_mat_a(top, i, j, submitted * 10 + i + j);
                        set_mat_b(top, i, j, (i == j) ? 1 : 0);
                    }
                }
                top->valid_in = 1;
                tick(top, tfp);        // 发送请求
                top->valid_in = 0;     // 立即拉低
                std::cout << "  Test " << submitted << " accepted" << std::endl;
                submitted++;
            } else {
                top->valid_in = 0;
            }
        }
        top->valid_in = 0;
        
        // 阶段2：收集所有结果
        timeout = 100;
        while (results < num_tests && timeout-- > 0) {
            tick(top, tfp);
            if (top->valid_out) {
                results++;
                std::cout << "  Result " << results << " received" << std::endl;
            }
        }
        
        bool pass = (results == num_tests);
        test_count++;
        if (pass) {
            pass_count++;
            std::cout << "  [PASS] Back-to-back computation completed" << std::endl;
        } else {
            std::cout << "  [FAIL] Back-to-back computation failed: " 
                      << results << "/" << num_tests << " results received" << std::endl;
        }
    }
    
    // 测试间复位
    reset_between_tests(top, tfp);
    
    // Test 5: 状态机测试
    std::cout << "\n[Test 5] State Machine Test..." << std::endl;
    {
        bool pass = true;
        
        // 检查初始状态为ready
        if (!top->ready) {
            std::cout << "  FAIL: Initial ready should be 1" << std::endl;
            pass = false;
        }
        
        clear_mat(top);
        set_mat_a(top, 0, 0, 5);
        set_mat_b(top, 0, 0, 6);
        
        top->valid_in = 1;
        tick(top, tfp);
        
        // 检查开始计算后ready变0
        if (top->ready) {
            std::cout << "  FAIL: Ready should be 0 after valid_in" << std::endl;
            pass = false;
        }
        
        top->valid_in = 0;
        
        int timeout = 20;
        while (!top->valid_out && timeout-- > 0) {
            tick(top, tfp);
        }
        
        tick(top, tfp);  // 再多一个周期确保回到IDLE
        
        // 检查计算完成后ready变回1
        if (!top->ready) {
            std::cout << "  FAIL: Ready should be 1 after completion" << std::endl;
            pass = false;
        }
        
        test_count++;
        if (pass) {
            pass_count++;
            std::cout << "  [PASS] State machine transitions correct" << std::endl;
        } else {
            std::cout << "  [FAIL] State machine test failed" << std::endl;
        }
    }
    
    tick(top, tfp);
    tick(top, tfp);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  All Tests Completed: " << pass_count << "/" << test_count << std::endl;
    std::cout << "========================================" << std::endl;
    
    tfp->close();
    delete tfp;
    delete top;
    return (pass_count == test_count) ? 0 : 1;  // 如果有失败返回非0
}