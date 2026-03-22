// tb_main.cpp
// Verilator C++ Testbench for giga_thread_scheduler

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vgiga_thread_scheduler.h"  // 由 verilator 生成

#include <iostream>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <queue>

// 配置参数
#define NUM_GPC 4
#define MAX_CYCLES 100000
#define VCD_DUMP 1

class GPCModel {
public:
    int id;
    bool ready;
    int processing_cta;  // -1 表示空闲
    std::queue<int> completion_queue;  // 模拟处理延迟
    
    GPCModel(int _id) : id(_id), ready(true), processing_cta(-1) {}
    
    // 每个周期调用，模拟处理延迟后发出完成信号
    bool tick(int &completed_cta_id) {
        if (!completion_queue.empty()) {
            completion_queue.front()--;
            if (completion_queue.front() <= 0) {
                completed_cta_id = id;  // 简化：用 GPC ID 代表完成了某个 CTA
                completion_queue.pop();
                return true;
            }
        }
        return false;
    }
    
    void accept_cta(int delay) {
        completion_queue.push(delay);  // delay 周期后完成
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // 实例化 DUT
    Vgiga_thread_scheduler* dut = new Vgiga_thread_scheduler;
    
    // VCD 波形配置
    #if VCD_DUMP
    VerilatedVcdC* tfp = new VerilatedVcdC;
    Verilated::traceEverOn(true);
    dut->trace(tfp, 99);
    tfp->open("waveform.vcd");
    #endif
    
    // 初始化随机种子
    srand(time(nullptr));
    
    // 仿真变量
    vluint64_t sim_time = 0;
    int test_passed = 1;
    
    // 测试状态
    enum TestPhase { RESET, IDLE, RUNNING, CHECKING, DONE };
    TestPhase phase = RESET;
    
    // 统计变量
    int host_launches = 0;
    int total_ctas_to_send = 0;
    int ctas_dispatched_stat = 0;
    int ctas_completed_stat = 0;
    int grid_dim_x = 0, grid_dim_y = 0, grid_dim_z = 0;
    int current_cta_x = 0, current_cta_y = 0, current_cta_z = 0;
    
    // GPC 模型
    std::vector<GPCModel> gpc_models;
    for (int i = 0; i < NUM_GPC; i++) {
        gpc_models.emplace_back(i);
    }
    
    // 辅助函数：设置主机启动信号
    auto launch_grid = [&](int x, int y, int z, uint64_t base_addr) {
        dut->host_grid_dim_x = x;
        dut->host_grid_dim_y = y;
        dut->host_grid_dim_z = z;
        dut->host_grid_base_addr = base_addr;
        dut->host_kernel_args = 0xDEADBEEF;
        dut->host_launch_valid = 1;
        total_ctas_to_send = x * y * z;
        grid_dim_x = x; grid_dim_y = y; grid_dim_z = z;
        current_cta_x = 0; current_cta_y = 0; current_cta_z = 0;
        std::cout << "[Host] Launching Grid: " << x << "x" << y << "x" << z 
                  << " = " << total_ctas_to_send << " CTAs" << std::endl;
    };
    
    // 初始化输入
    dut->clk = 0;
    dut->rst_n = 0;
    dut->host_launch_valid = 0;
    dut->host_grid_base_addr = 0;
    dut->host_grid_dim_x = 0;
    dut->host_grid_dim_y = 0;
    dut->host_grid_dim_z = 0;
    dut->host_kernel_args = 0;
    
    for (int i = 0; i < NUM_GPC; i++) {
        dut->gpc_dispatch_ready[i] = 1;  // 默认所有 GPC ready
        dut->gpc_done_valid[i] = 0;
    }
    
    // 主仿真循环
    while (sim_time < MAX_CYCLES && phase != DONE) {
        // 时钟边沿处理（上升沿前）
        if (dut->clk == 0) {
            // 下降沿后的逻辑更新
            
            // GPC 模型更新（在时钟上升沿前计算完成信号）
            for (int i = 0; i < NUM_GPC; i++) {
                int completed_id;
                if (gpc_models[i].tick(completed_id)) {
                    dut->gpc_done_valid[i] = 1;  // 发出完成脉冲
                    ctas_completed_stat++;
                    std::cout << "[GPC " << i << "] CTA Completed (total: " 
                              << ctas_completed_stat << "/" << total_ctas_to_send << ")" << std::endl;
                } else {
                    dut->gpc_done_valid[i] = 0;
                }
                
                // 随机反压：20% 概率 GPC 不 ready（测试流控）
                if (rand() % 5 == 0) {
                    dut->gpc_dispatch_ready[i] = 0;
                } else {
                    dut->gpc_dispatch_ready[i] = 1;
                }
            }
            
            // 主机行为模型
            switch (phase) {
                case RESET:
                    if (sim_time > 20) {
                        dut->rst_n = 1;
                        phase = IDLE;
                        std::cout << "[TB] Reset released" << std::endl;
                    }
                    break;
                    
                case IDLE:
                    if (dut->host_launch_ready) {
                        // 启动测试案例：3x2x2 = 12 CTAs
                        launch_grid(3, 2, 2, 0x10000000);
                        phase = RUNNING;
                        host_launches++;
                    }
                    break;
                    
                case RUNNING:
                    dut->host_launch_valid = 0;  // 脉冲只维持一个周期
                    
                    // 检查分发计数
                    if (dut->gpc_dispatch_valid) {
                        ctas_dispatched_stat++;
                        
                        // 验证 3D 坐标递增逻辑
                        int expected_x = (ctas_dispatched_stat - 1) % grid_dim_x;
                        int temp = (ctas_dispatched_stat - 1) / grid_dim_x;
                        int expected_y = temp % grid_dim_y;
                        int expected_z = temp / grid_dim_y;
                        
                        if (dut->gpc_block_x != expected_x || 
                            dut->gpc_block_y != expected_y || 
                            dut->gpc_block_z != expected_z) {
                            std::cerr << "[ERROR] CTA coordinate mismatch! Expected (" 
                                      << expected_x << "," << expected_y << "," << expected_z 
                                      << ") Got (" << dut->gpc_block_x << "," 
                                      << dut->gpc_block_y << "," << dut->gpc_block_z << ")" << std::endl;
                            test_passed = 0;
                        }
                        
                        // 验证 Round-Robin GPC 选择
                        int expected_gpc = (ctas_dispatched_stat - 1) % NUM_GPC;
                        if (dut->gpc_target_id != expected_gpc) {
                            std::cerr << "[ERROR] Round-Robin violation! Expected GPC " 
                                      << expected_gpc << " Got " << (int)dut->gpc_target_id << std::endl;
                            test_passed = 0;
                        }
                        
                        // 通知 GPC 模型接受了一个 CTA（随机处理延迟 1-5 周期）
                        gpc_models[dut->gpc_target_id].accept_cta(1 + rand() % 5);
                        
                        // 验证基础地址透传
                        if (dut->gpc_entry_pc != 0x10000000) {
                            std::cerr << "[ERROR] Entry PC mismatch!" << std::endl;
                            test_passed = 0;
                        }
                    }
                    
                    // 检查完成
                    if (dut->completion_irq) {
                        std::cout << "[TB] Completion IRQ detected! Status: 0x" 
                                  << std::hex << dut->completion_status << std::dec << std::endl;
                        phase = CHECKING;
                    }
                    break;
                    
                case CHECKING:
                    // 验证所有 CTA 都已完成
                    if (ctas_dispatched_stat == total_ctas_to_send && 
                        ctas_completed_stat == total_ctas_to_send) {
                        std::cout << "[TB] Test Case 1 PASSED: All CTAs dispatched and completed" << std::endl;
                        
                        // 可以在这里启动第二个测试案例（Back-to-back）
                        if (host_launches < 2) {
                            launch_grid(4, 1, 1, 0x20000000);  // 第二个小 grid
                            phase = RUNNING;
                            ctas_dispatched_stat = 0;
                            ctas_completed_stat = 0;
                        } else {
                            phase = DONE;
                        }
                    } else {
                        std::cerr << "[ERROR] Mismatch! Dispatched: " << ctas_dispatched_stat 
                                  << ", Completed: " << ctas_completed_stat 
                                  << ", Expected: " << total_ctas_to_send << std::endl;
                        test_passed = 0;
                        phase = DONE;
                    }
                    break;
                    
                default:
                    break;
            }
        }
        
        // 时钟翻转
        dut->clk = !dut->clk;
        
        // 评估 DUT
        dut->eval();
        
        // Dump 波形
        #if VCD_DUMP
        if (tfp) tfp->dump(sim_time);
        #endif
        
        sim_time++;
    }
    
    // 最终检查
    if (phase != DONE) {
        std::cerr << "[ERROR] Simulation timeout!" << std::endl;
        test_passed = 0;
    }
    
    // 清理
    dut->final();
    #if VCD_DUMP
    if (tfp) tfp->close();
    delete tfp;
    #endif
    delete dut;
    
    // 报告结果
    if (test_passed) {
        std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
        return 0;
    } else {
        std::cout << "\n=== TEST FAILED ===" << std::endl;
        return 1;
    }
}