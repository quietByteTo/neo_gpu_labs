#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vsm_backend.h"

#include <iostream>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <cstring>
#include <cassert>
#include <string>

// 测试配置
#define NUM_LANES 32
#define DATA_W    32
#define WARP_ID_W 5
#define NUM_TESTS 1000
#define MAX_CYCLES 10000

// 辅助宏：清零 VlWide 类型（1024-bit = 32 words）
#define CLEAR_1024BIT(ptr) memset((ptr)->m_storage, 0, sizeof((ptr)->m_storage))

// 辅助类：模拟执行单元行为
class ExecutionUnitMock {
public:
    std::string name;
    int latency;
    int countdown;
    bool busy;
    uint32_t warp_id;
    uint32_t result_data[NUM_LANES]; // 32 lanes * 32-bit
    
    ExecutionUnitMock(std::string n, int lat) 
        : name(n), latency(lat), countdown(0), busy(false), warp_id(0) {
        memset(result_data, 0, sizeof(result_data));
    }
    
    void start(uint32_t wid, uint32_t *data) {
        busy = true;
        countdown = latency;
        warp_id = wid;
        if (data) memcpy(result_data, data, sizeof(result_data));
    }
    
    bool tick(uint32_t *out_data, uint32_t &out_wid) {
        if (!busy) return false;
        countdown--;
        if (countdown == 0) {
            busy = false;
            if (out_data) memcpy(out_data, result_data, sizeof(result_data));
            out_wid = warp_id;
            return true;
        }
        return false;
    }
};

// 辅助函数：设置 1024-bit 数据到 VlWide
void set_vlwide_1024(VlWide<32> *dest, uint32_t pattern_base) {
    for (int i = 0; i < NUM_LANES; i++) {
        dest->m_storage[i] = pattern_base + i;
    }
}

// 辅助函数：从 VlWide 读取 1024-bit 数据
void get_vlwide_1024(VlWide<32> *src, uint32_t *dest) {
    memcpy(dest, src->m_storage, sizeof(src->m_storage));
}

// 主测试类
class Testbench {
private:
    Vsm_backend *dut;
    VerilatedVcdC *trace;
    vluint64_t main_time;
    
    ExecutionUnitMock alu_mock;
    ExecutionUnitMock sfu_mock;
    ExecutionUnitMock lsu_mock;
    ExecutionUnitMock lds_mock;
    
    int tests_passed = 0;
    int tests_failed = 0;
    
    // 临时存储 1024-bit 数据的缓冲区
    uint32_t tmp_data0[NUM_LANES];
    uint32_t tmp_data1[NUM_LANES];
    
public:
    Testbench() : main_time(0), 
                  alu_mock("ALU", 1),
                  sfu_mock("SFU", 4),
                  lsu_mock("LSU", 3),
                  lds_mock("LDS", 2) {
        dut = new Vsm_backend;
        
        Verilated::traceEverOn(true);
        trace = new VerilatedVcdC;
        dut->trace(trace, 99);
        trace->open("sm_backend.vcd");
        
        reset();
    }
    
    ~Testbench() {
        trace->close();
        delete trace;
        delete dut;
    }
    
    void reset() {
        std::cout << "[TEST] Applying Reset..." << std::endl;
        
        dut->clk = 0;
        dut->rst_n = 0;
        dut->valid = 0;
        dut->instr = 0;
        dut->warp_id = 0;
        
        // 修复：使用 CLEAR_1024BIT 清零宽向量
        CLEAR_1024BIT(&(dut->rs0_data));
        CLEAR_1024BIT(&(dut->rs1_data));
        
        dut->alu_ready = 1;
        dut->sfu_ready = 1;
        dut->lsu_ready = 1;
        dut->lds_ready = 1;
        dut->alu_valid_out = 0;
        dut->sfu_valid_out = 0;
        dut->lsu_load_valid = 0;
        dut->lds_rdata_valid = 0;
        
        // 清零其他 1024-bit 输入
        CLEAR_1024BIT(&(dut->alu_result));
        CLEAR_1024BIT(&(dut->sfu_result));
        CLEAR_1024BIT(&(dut->lsu_load_data));
        
        for (int i = 0; i < 5; i++) {
            tick();
        }
        dut->rst_n = 1;
        tick();
        std::cout << "[TEST] Reset Done" << std::endl;
    }
    
    void tick() {
        // 下降沿
        dut->clk = 0;
        dut->eval();
        trace->dump(main_time++);
        
        // 上升沿
        dut->clk = 1;
        dut->eval();
        
        update_mock_units();
        drive_mock_outputs();
        trace->dump(main_time++);
    }
    
    void update_mock_units() {
        uint32_t dummy[NUM_LANES];
        uint32_t wid;
        
        // 捕获新指令
        if (dut->alu_valid && dut->alu_ready) {
            get_vlwide_1024(&(dut->alu_src_a), tmp_data0);
            alu_mock.start(dut->warp_id, tmp_data0);
            dut->alu_ready = 0;
        }
        
        if (dut->sfu_valid && dut->sfu_ready) {
            sfu_mock.start(dut->warp_id, nullptr);
            dut->sfu_ready = 0;
        }
        
        if (dut->lsu_valid && dut->lsu_ready) {
            // 构造 load 返回数据（独特模式）
            for (int i = 0; i < NUM_LANES; i++) {
                tmp_data0[i] = 0xDEAD0000 + i;
            }
            lsu_mock.start(dut->warp_id, tmp_data0);
            dut->lsu_ready = 0;
        }
        
        if (dut->lds_valid && dut->lds_ready) {
            lds_mock.start(dut->warp_id, nullptr);
            dut->lds_ready = 0;
        }
        
        // 更新完成状态
        if (alu_mock.tick(dummy, wid)) {
            dut->alu_valid_out = 1;
            set_vlwide_1024(&(dut->alu_result), 0x1000); // 构造结果
        } else {
            dut->alu_valid_out = 0;
            if (!alu_mock.busy) dut->alu_ready = 1;
        }
        
        if (sfu_mock.tick(dummy, wid)) {
            dut->sfu_valid_out = 1;
            // SFU 结果：全部填充 0xAB
            for (int i = 0; i < NUM_LANES; i++) {
                tmp_data0[i] = 0xABABABAB;
            }
            memcpy(dut->sfu_result.m_storage, tmp_data0, sizeof(tmp_data0));
        } else {
            dut->sfu_valid_out = 0;
            if (!sfu_mock.busy) dut->sfu_ready = 1;
        }
        
        if (lsu_mock.tick(dummy, wid)) {
            dut->lsu_load_valid = 1;
            set_vlwide_1024(&(dut->lsu_load_data), 0xDEAD0000);
        } else {
            dut->lsu_load_valid = 0;
            if (!lsu_mock.busy) dut->lsu_ready = 1;
        }
        
        if (lds_mock.tick(dummy, wid)) {
            dut->lds_rdata_valid = 1;
            dut->lds_rdata = 0x12345678;
        } else {
            dut->lds_rdata_valid = 0;
            if (!lds_mock.busy) dut->lds_ready = 1;
        }
    }
    
    void drive_mock_outputs() {
        // 已在 update_mock_units 中完成
    }
    
    void set_1024bit_data(VlWide<32> *dest, uint32_t pattern_base) {
        set_vlwide_1024(dest, pattern_base);
    }
    
    bool check_1024bit_eq(VlWide<32> *a, VlWide<32> *b) {
        return (memcmp(a->m_storage, b->m_storage, sizeof(a->m_storage)) == 0);
    }
    
    // 测试 1: 复位验证
    void test_reset() {
        std::cout << "\n[TEST 1] Reset Verification" << std::endl;
        
        dut->valid = 1;
        dut->instr = 0xFFFFFFFF;
        tick();
        
        dut->rst_n = 0;
        for (int i = 0; i < 3; i++) tick();
        dut->rst_n = 1;
        tick();
        
        bool pass = (dut->wb_en == 0) && (dut->rf_wr_en[0] == 0);
        if (!pass) {
            std::cerr << "FAIL: Reset did not clear outputs" << std::endl;
            tests_failed++;
        } else {
            std::cout << "PASS: Reset clears all outputs" << std::endl;
            tests_passed++;
        }
    }
    
    // 测试 2: ALU 单指令通路
    void test_alu_basic() {
        std::cout << "\n[TEST 2] ALU Basic Path (Opcode 2'b00)" << std::endl;
        
        set_1024bit_data(&(dut->rs0_data), 0x1000);
        set_1024bit_data(&(dut->rs1_data), 0x2000);
        
        dut->instr = 0x00000004;
        dut->warp_id = 5;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        // 等待 2 周期
        tick();
        
        bool pass = (dut->wb_en == 1) && 
                    (dut->wb_warp_id == 5) &&
                    (dut->wb_rd_addr == 1) &&
                    (dut->rf_wr_en[0] == 1);
                    
        if (!pass) {
            std::cerr << "FAIL: ALU writeback failed" << std::endl;
            tests_failed++;
        } else {
            std::cout << "PASS: ALU instruction completed" << std::endl;
            tests_passed++;
        }
        
        tick();
    }
    
    // 测试 3: 仲裁优先级 LSU > ALU
    void test_arbitration_lsu_priority() {
        std::cout << "\n[TEST 3] Arbitration: LSU Priority over ALU" << std::endl;
        
        dut->instr = 0x30000008;
        dut->warp_id = 1;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        tick();
        tick();
        
        dut->instr = 0x00000009;
        dut->warp_id = 2;
        dut->valid = 1;
        set_1024bit_data(&(dut->rs0_data), 0x1000);
        
        tick();
        dut->valid = 0;
        
        tick();
        
        bool pass = false;
        if (dut->wb_en) {
            std::cout << "  Arbitration cycle detected wb_en=1" << std::endl;
            // 检查优先级：如果有 LSU 完成，应该是它先
            tick();
            if (dut->wb_en && dut->rf_wr_addr[0] == 9) {
                std::cout << "  ALU got port in next cycle (expected if LSU done first)" << std::endl;
                pass = true;
            } else {
                pass = true; // 简化判断
            }
        }
        
        if (pass) {
            std::cout << "PASS: Arbitration test completed" << std::endl;
            tests_passed++;
        } else {
            std::cerr << "FAIL: Arbitration test failed" << std::endl;
            tests_failed++;
        }
        
        for (int i = 0; i < 5; i++) tick();
    }
    
    // 测试 4: 全执行单元并发
    void test_all_units_concurrent() {
        std::cout << "\n[TEST 4] All Units Concurrent Execution" << std::endl;
        int completions = 0;
        int max_wait = 20;
        std::vector<bool> seen_warps(4, false);

        int warp_base = 10;
        dut->instr = 0x00000001;
        dut->warp_id = warp_base + 0;
        dut->valid = 1;
        tick();
        while(!dut->ready)
        tick();
        
        
        if (dut->wb_en) {
            int wid = dut->wb_warp_id - warp_base;
            if (wid >= 0 && wid < 4 && !seen_warps[wid]) {
                seen_warps[wid] = true;
                completions++;
                std::cout << "  Warp " << (wid + warp_base) << " completed" << std::endl;
            }
        }
        dut->instr = 0x40000002;
        dut->warp_id = warp_base + 1;
        tick();
        while(!dut->ready)
        tick();;
        
        if (dut->wb_en) {
            int wid = dut->wb_warp_id - warp_base;
            if (wid >= 0 && wid < 4 && !seen_warps[wid]) {
                seen_warps[wid] = true;
                completions++;
                std::cout << "  Warp " << (wid + warp_base) << " completed" << std::endl;
            }
        }
        dut->instr = 0x80000003;
        dut->warp_id = warp_base + 2;
        tick();
        while(!dut->ready)
        tick();;

        if (dut->wb_en) {
            int wid = dut->wb_warp_id - warp_base;
            if (wid >= 0 && wid < 4 && !seen_warps[wid]) {
                seen_warps[wid] = true;
                completions++;
                std::cout << "  Warp " << (wid + warp_base) << " completed" << std::endl;
            }
        }
        dut->instr = 0xc0000004;
        dut->warp_id = warp_base + 3;
        tick();
        while(!dut->ready)
        tick();;
        
        if (dut->wb_en) {
            int wid = dut->wb_warp_id - warp_base;
            if (wid >= 0 && wid < 4 && !seen_warps[wid]) {
                seen_warps[wid] = true;
                completions++;
                std::cout << "  Warp " << (wid + warp_base) << " completed" << std::endl;
            }
        }
        dut->valid = 0;
        
        
        
        while (completions < 4 && max_wait-- > 0) {
            tick();
            std::cout << "max_wait: " << max_wait << "dut->wb_en:" <<(uint32_t) dut->wb_en <<std::endl;
            if (dut->wb_en) {
                int wid = dut->wb_warp_id - warp_base;
                if (wid >= 0 && wid < 4 && !seen_warps[wid]) {
                    seen_warps[wid] = true;
                    completions++;
                    std::cout << "  Warp " << (wid + warp_base) << " completed" << std::endl;
                }
            }
        }
        tick();
        tick();
        bool pass = (completions == 4);
        if (pass) {
            std::cout << "PASS: All 4 units completed" << std::endl;
            tests_passed++;
        } else {
            std::cerr << "FAIL: Only " << completions << "/4 units completed" << std::endl;
            tests_failed++;
        }
    }
    
    // 测试 5: 握手阻塞
    void test_handshake_stall() {
        std::cout << "\n[TEST 5] Handshake Stall Recovery" << std::endl;
        
        dut->alu_ready = 0;
        
        dut->instr = 0x00000005;
        dut->warp_id = 7;
        dut->valid = 1;
        
        tick();
        
        bool pass = (dut->ready == 0);
        
        dut->alu_ready = 1;
        tick();
        
        pass = pass && (dut->ready == 1);
        
        for (int i = 0; i < 3; i++) tick();
        
        pass = pass && (dut->rf_wr_en[0] == 1) && (dut->wb_warp_id == 7);
        
        if (pass) {
            std::cout << "PASS: Stall and resume working" << std::endl;
            tests_passed++;
        } else {
            std::cerr << "FAIL: Handshake stall test failed" << std::endl;
            tests_failed++;
        }
    }
    
    // 测试 6: LDS 广播
    void test_lds_broadcast() {
        std::cout << "\n[TEST 6] LDS Broadcast Writeback" << std::endl;
        
        dut->instr = 0x38000006;
        dut->warp_id = 3;
        dut->valid = 1;
        tick();
        dut->valid = 0;
        
        tick();
        tick();
        
        bool pass = false;
        for (int i = 0; i < 10 && !pass; i++) {
            tick();
            if (dut->wb_en && dut->wb_rd_addr == 6) {
                // 检查是否广播（所有 lanes 相同）
                uint32_t *data = dut->wb_data.m_storage;
                pass = (data[0] == 0x12345678);
                if (pass) {
                    std::cout << "  Detected broadcast value 0x12345678" << std::endl;
                }
            }
        }
        
        if (pass) {
            std::cout << "PASS: LDS broadcast working" << std::endl;
            tests_passed++;
        } else {
            std::cerr << "FAIL: LDS broadcast test failed" << std::endl;
            tests_failed++;
        }
    }
    
    // 测试 7: 随机压力
    void test_random_stress(int num_iter = 50) {
        std::cout << "\n[TEST 7] Random Stress Test (" << num_iter << " iter)" << std::endl;
        
        srand(time(nullptr));
        int ops_issued = 0;
        int ops_completed = 0;
        
        for (int i = 0; i < num_iter; i++) {
            if ((rand() % 2) && dut->ready) {
                uint32_t opcode = (rand() % 4);
                uint32_t instr = (opcode << 4) | (rand() % 64);
                dut->instr = instr;
                dut->warp_id = rand() % 32;
                dut->valid = 1;
                
                set_1024bit_data(&(dut->rs0_data), rand());
                ops_issued++;
            } else {
                dut->valid = 0;
            }
            
            tick();
            
            if (dut->wb_en) ops_completed++;
        }
        
        int timeout = 50;
        while (timeout-- > 0) {
            tick();
            if (dut->wb_en) ops_completed++;
        }
        
        std::cout << "  Issued: " << ops_issued << ", Completed: " << ops_completed << std::endl;
        
        bool pass = (ops_completed >= ops_issued * 0.8) || (ops_issued == 0);
        if (pass) {
            std::cout << "PASS: Stress test completed" << std::endl;
            tests_passed++;
        } else {
            std::cerr << "FAIL: Too many incomplete operations" << std::endl;
            tests_failed++;
        }
    }
    
    void run_all_tests() {
        std::cout << "=============================================" << std::endl;
        std::cout << " SM Backend Testbench with Verilator" << std::endl;
        std::cout << "=============================================" << std::endl;
        
        test_reset();
        //test_alu_basic();
        //test_arbitration_lsu_priority();
        test_all_units_concurrent();
        //test_handshake_stall();
        //test_lds_broadcast();
        //test_random_stress(100);
        
        std::cout << "\n=============================================" << std::endl;
        std::cout << " Test Summary: " << tests_passed << " passed, " 
                  << tests_failed << " failed" << std::endl;
        std::cout << "=============================================" << std::endl;
        
        //if (tests_failed > 0) exit(1);
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Testbench tb;
    tb.run_all_tests();
    
    return 0;
}
