#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vneo_gpu_top.h"  // Verilator 生成的头文件
#include <iostream>
#include <vector>
#include <cstdint>
#include <cassert>

// ============================================================================
// 配置参数
// ============================================================================
constexpr uint64_t SYS_CLK_PERIOD_PS   = 1000;  // 1GHz = 1000ps
constexpr uint64_t PCIE_CLK_PERIOD_PS  = 4000;  // 250MHz = 4000ps
constexpr uint64_t SIM_TIME_MAX_PS    = 100000000; // 100us
constexpr uint32_t HBM_SIZE_MB        = 4096;
constexpr uint64_t HBM_SIZE_BYTES     = (uint64_t)HBM_SIZE_MB * 1024 * 1024;

// ============================================================================
// 内存模型（模拟 HBM）
// ============================================================================
class MemoryModel {
private:
    std::vector<uint8_t> data;
    
public:
    MemoryModel() : data(HBM_SIZE_BYTES, 0) {}
    
    void write(uint64_t addr, uint32_t value, uint8_t be) {
        if (addr >= HBM_SIZE_BYTES) return;
        for (int i = 0; i < 4; i++) {
            if (be & (1 << i)) {
                data[addr + i] = (value >> (i * 8)) & 0xFF;
            }
        }
    }
    
    uint32_t read(uint64_t addr) {
        if (addr >= HBM_SIZE_BYTES) return 0xDEADBEEF;
        uint32_t val = 0;
        for (int i = 0; i < 4; i++) {
            val |= (data[addr + i] << (i * 8));
        }
        return val;
    }
    
    void load_kernel(uint64_t addr, const std::vector<uint32_t>& kernel_code) {
        for (size_t i = 0; i < kernel_code.size(); i++) {
            write(addr + i*4, kernel_code[i], 0xF);
        }
    }
};

// ============================================================================
// PCIe TLP 生成器
// ============================================================================
class PCIeTLPGenerator {
private:
    Vneo_gpu_top* dut;
    MemoryModel* mem;
    uint64_t time_ps;
    
    // Doorbell 地址定义
    static constexpr uint64_t DOORBELL_ADDR = 0x1000;
    static constexpr uint64_t CFG_BAR0_ADDR = 0x0000;
    
public:
    PCIeTLPGenerator(Vneo_gpu_top* _dut, MemoryModel* _mem) 
        : dut(_dut), mem(_mem), time_ps(0) {}
    
    // 通过 DPI 发送写请求（Memory Write TLP）
    void send_memory_write(uint64_t addr, uint32_t data, uint8_t be = 0xF) {
        // 设置 DPI 寄存器
        dut->dpi_rx_addr = addr;
        dut->dpi_rx_data = data;
        dut->dpi_rx_is_write = 1;
        dut->dpi_rx_be = be;
        dut->dpi_rx_valid = 1;
        
        // 保持一个时钟周期
        dut->clk_pcie = 1;
        dut->eval();
        dut->clk_pcie = 0;
        dut->eval();
        
        dut->dpi_rx_valid = 0; // 清除有效位
    }
    
    // 发送读请求
    void send_memory_read(uint64_t addr) {
        dut->dpi_rx_addr = addr;
        dut->dpi_rx_data = 0;
        dut->dpi_rx_is_write = 0;
        dut->dpi_rx_valid = 1;
        
        dut->clk_pcie = 1;
        dut->eval();
        dut->clk_pcie = 0;
        dut->eval();
        
        dut->dpi_rx_valid = 0;
    }
    
    // 发送门铃（启动 Kernel）
    void ring_doorbell(uint64_t grid_base, uint16_t dim_x, uint16_t dim_y) {
        std::cout << "[PCIe] Ringing doorbell @ " << time_ps << "ps" << std::endl;
        std::cout << "       Grid Base: 0x" << std::hex << grid_base << std::dec << std::endl;
        std::cout << "       Dim: " << dim_x << "x" << dim_y << std::endl;
        
        // 写配置寄存器（假设偏移 0x00-0x1F 为配置）
        send_memory_write(CFG_BAR0_ADDR + 0x00, grid_base & 0xFFFFFFFF);
        send_memory_write(CFG_BAR0_ADDR + 0x04, (grid_base >> 32) & 0xFFFFFFFF);
        send_memory_write(CFG_BAR0_ADDR + 0x08, (uint32_t)dim_x);
        send_memory_write(CFG_BAR0_ADDR + 0x0C, (uint32_t)dim_y);
        
        // 写门铃地址触发调度
        send_memory_write(DOORBELL_ADDR, 0x1); // 任何非零值即可
    }
    
    // 检查 TX 输出（来自 GPU 的中断或完成通知）
    bool check_tx_output(uint32_t& data) {
        if (dut->pcie_tx_tlp_valid) {
            data = dut->pcie_tx_tlp_data;
            return true;
        }
        return false;
    }
    
    void set_time(uint64_t t) { time_ps = t; }
};

// ============================================================================
// Testbench 主类
// ============================================================================
class Testbench {
private:
    Vneo_gpu_top* dut;
    VerilatedVcdC* tfp;
    MemoryModel hbm;
    PCIeTLPGenerator pcie;
    
    uint64_t sys_clk_ps;
    uint64_t pcie_clk_ps;
    uint64_t main_time_ps;
    int toggle_sys;
    int toggle_pcie;
    
    // 统计
    uint64_t tick_count;
    bool finished;
    
public:
    Testbench() : pcie(dut, &hbm), sys_clk_ps(0), pcie_clk_ps(0), 
                  main_time_ps(0), toggle_sys(0), toggle_pcie(0), 
                  tick_count(0), finished(false) {
        
        // 实例化 DUT
        Verilated::traceEverOn(true);
        dut = new Vneo_gpu_top;
        
        // 波形转储设置
        tfp = new VerilatedVcdC;
        dut->trace(tfp, 99); // 深度 99
        tfp->open("neo_gpu_waveform.vcd");
        
        // 初始化信号
        dut->rst_n = 0;
        dut->clk_sys = 0;
        dut->clk_pcie = 0;
        dut->pwr_sleep_req = 0;
        dut->dbg_en = 0;
        
        // DPI 信号初始化
        dut->dpi_rx_valid = 0;
        dut->dpi_tx_data = 0;
        dut->dpi_tx_valid = 0;
    }
    
    ~Testbench() {
        dut->final();
        tfp->close();
        delete dut;
        delete tfp;
    }
    
    // 复位序列
    void reset() {
        std::cout << "[TB] Starting Reset Sequence..." << std::endl;
        dut->rst_n = 0;
        
        // 复位 100ns
        for (int i = 0; i < 100; i++) {
            tick(SYS_CLK_PERIOD_PS / 2);
        }
        
        dut->rst_n = 1;
        std::cout << "[TB] Reset Released @ " << main_time_ps << "ps" << std::endl;
    }
    
    // 单步推进（以较小时间片为单位）
    void tick(uint64_t step_ps) {
        main_time_ps += step_ps;
        
        // 生成 sys_clk (1GHz)
        sys_clk_ps += step_ps;
        if (sys_clk_ps >= SYS_CLK_PERIOD_PS / 2) {
            sys_clk_ps -= SYS_CLK_PERIOD_PS / 2;
            toggle_sys = !toggle_sys;
            dut->clk_sys = toggle_sys;
        }
        
        // 生成 pcie_clk (250MHz)
        pcie_clk_ps += step_ps;
        if (pcie_clk_ps >= PCIE_CLK_PERIOD_PS / 2) {
            pcie_clk_ps -= PCIE_CLK_PERIOD_PS / 2;
            toggle_pcie = !toggle_pcie;
            dut->clk_pcie = toggle_pcie;
            
            // 在 PCIe 时钟沿更新 DPI 时间
            pcie.set_time(main_time_ps);
        }
        
        // 评估 DUT
        dut->eval();
        tfp->dump(main_time_ps);
        tick_count++;
    }
    
    // 运行测试场景
    void run_test() {
        // 准备测试数据：加载简单 Kernel 到 HBM 地址 0x10000
        std::vector<uint32_t> kernel_code = {
            0x00000001, // NOP
            0x00000002, // LOAD
            0x00000003, // ADD
            0x00000004, // STORE
            0xFFFFFFFF  // EXIT
        };
        hbm.load_kernel(0x10000, kernel_code);
        
        // 延迟一段时间后启动
        for (int i = 0; i < 50; i++) tick(SYS_CLK_PERIOD_PS);
        
        // 发送门铃启动 Kernel（2x2 grid）
        pcie.ring_doorbell(0x10000, 2, 2);
        
        // 运行直到完成或超时
        uint64_t last_activity = main_time_ps;
        uint32_t tx_data;
        
        while (main_time_ps < SIM_TIME_MAX_PS) {
            tick(SYS_CLK_PERIOD_PS); // 以系统时钟为步长推进
            
            // 检查 GPU 输出
            if (pcie.check_tx_output(tx_data)) {
                std::cout << "[TB] GPU TX Data: 0x" << std::hex << tx_data 
                          << " @ " << std::dec << main_time_ps << "ps" << std::endl;
                last_activity = main_time_ps;
            }
            
            // 检查完成（通过状态寄存器或中断）
            if (dut->status_gpu_busy == 0 && main_time_ps > 50000) {
                std::cout << "[TB] GPU reports IDLE, test complete!" << std::endl;
                break;
            }
            
            // 超时检测（10us 无活动）
            if (main_time_ps - last_activity > 10000000) {
                std::cout << "[TB] Warning: 10us without activity, stopping" << std::endl;
                break;
            }
        }
        
        finished = true;
    }
    
    void report() {
        std::cout << "========================================" << std::endl;
        std::cout << "Simulation Report:" << std::endl;
        std::cout << "  Total Time: " << main_time_ps << " ps (" 
                  << main_time_ps / 1000000.0 << " us)" << std::endl;
        std::cout << "  Clock Ticks: " << tick_count << std::endl;
        std::cout << "  Status: " << (finished ? "FINISHED" : "TIMEOUT") << std::endl;
        std::cout << "========================================" << std::endl;
    }
};

// ============================================================================
// Main Entry
// ============================================================================
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    std::cout << "========================================" << std::endl;
    std::cout << "  NeoGPU Verilator Testbench" << std::endl;
    std::cout << "========================================" << std::endl;
    
    Testbench tb;
    tb.reset();
    tb.run_test();
    tb.report();
    
    return 0;
}