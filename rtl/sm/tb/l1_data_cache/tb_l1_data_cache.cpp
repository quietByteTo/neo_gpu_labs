// ============================================================================
// Verilator Testbench for l1_data_cache - Debug Version
// ============================================================================

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vl1_data_cache.h"

vluint64_t sim_time = 0;

void toggle_clock(Vl1_data_cache* dut, VerilatedVcdC* trace) {
    dut->clk = !dut->clk;
    dut->eval();
    trace->dump(sim_time++);
}

void posedge(Vl1_data_cache* dut, VerilatedVcdC* trace) {
    toggle_clock(dut, trace);
    toggle_clock(dut, trace);
}

void print_result(bool pass, const char* msg) {
    if (pass) printf("\033[32m[PASS]\033[0m %s\n", msg);
    else printf("\033[31m[FAIL]\033[0m %s\n", msg);
}

void set_wdata(Vl1_data_cache* dut, uint64_t val) {
    dut->req_wdata[0] = (uint32_t)(val & 0xFFFFFFFFULL);
    dut->req_wdata[1] = (uint32_t)(val >> 32);
    dut->req_wdata[2] = 0;
    dut->req_wdata[3] = 0;
}

void set_mem_fill(Vl1_data_cache* dut, uint64_t val) {
    for (int i = 0; i < 16; i++) dut->mem_fill_data[i] = (uint32_t)(val & 0xFFFFFFFFULL);
}

uint64_t get_rdata(Vl1_data_cache* dut) {
    return ((uint64_t)dut->resp_rdata[1] << 32) | (uint64_t)dut->resp_rdata[0];
}

uint64_t make_addr(uint64_t tag, uint64_t set, uint64_t offset) {
    return (tag << 14) | (set << 7) | offset;
}

// 执行Cache操作，带详细调试
bool do_cache_op_debug(Vl1_data_cache* dut, VerilatedVcdC* trace, 
                       uint64_t addr, uint64_t wdata, bool is_write,
                       uint64_t* rdata_out, const char* name, int max_cycles = 50) {
    
    printf("    [%s] Start: addr=0x%016lx, %s\n", name, addr, is_write ? "WRITE" : "READ");
    
    dut->req_valid = 1;
    dut->req_wr_en = is_write ? 1 : 0;
    dut->req_addr = addr;
    if (is_write) set_wdata(dut, wdata);
    
    bool saw_l2 = false;
    int l2_resp = 0;
    int cycles = 0;
    bool done = false;
    
    while (cycles < max_cycles && !done) {
        // L2响应逻辑
        if (dut->mem_req_valid) {
            saw_l2 = true;
            l2_resp = 10;
            printf("      [%s] L2 request at cycle %d\n", name, cycles);
        }
        
        if (l2_resp > 0) {
            dut->mem_fill_valid = 1;
            set_mem_fill(dut, 0xAAAAAAAAAAAAAAAAULL);
            l2_resp--;
        } else {
            dut->mem_fill_valid = 0;
        }
        
        posedge(dut, trace);
        cycles++;
        
        // 检查完成
        if (!is_write && dut->resp_valid) {
            if (rdata_out) *rdata_out = get_rdata(dut);
            printf("      [%s] Read complete: 0x%016lx\n", name, *rdata_out);
            done = true;
        }
        
        if (is_write && saw_l2 && cycles > 10 && dut->req_ready) {
            printf("      [%s] Write complete at cycle %d\n", name, cycles);
            done = true;
        }
    }
    
    dut->req_valid = 0;
    dut->req_wr_en = 0;
    dut->mem_fill_valid = 0;
    
    // 关键：大量等待确保状态机回到IDLE
    for (int i = 0; i < 20; i++) posedge(dut, trace);
    
    printf("    [%s] Done, total cycles=%d\n", name, cycles + 20);
    return done;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    Vl1_data_cache* dut = new Vl1_data_cache;
    Verilated::traceEverOn(true);
    VerilatedVcdC* m_trace = new VerilatedVcdC;
    dut->trace(m_trace, 5);
    m_trace->open("waveform.vcd");
    
    printf("========================================\n");
    printf("L1 Data Cache Testbench (Debug)\n");
    printf("========================================\n\n");
    
    int passed = 0, failed = 0;
    
    // 初始化
    dut->clk = 0;
    dut->rst_n = 0;
    dut->req_valid = 0;
    dut->req_wr_en = 0;
    dut->req_addr = 0;
    set_wdata(dut, 0);
    dut->req_be = 0xFFFF;
    dut->cache_en = 1;
    dut->flush_req = 0;
    dut->mem_req_ready = 1;
    dut->mem_fill_valid = 0;
    set_mem_fill(dut, 0);
    
    // 复位
    printf("Reset...\n");
    for (int i = 0; i < 10; i++) posedge(dut, m_trace);
    dut->rst_n = 1;
    for (int i = 0; i < 5; i++) posedge(dut, m_trace);
    printf("Ready\n\n");
    
    // ========== Test 1: 基本写后读 ==========
    printf("Test 1: Basic Write then Read\n");
    printf("----------------------------------------\n");
    
    uint64_t addr1 = make_addr(0x100, 0x10, 0x00);
    uint64_t write_data = 0xDEADBEEFCAFEBABEULL;
    uint64_t read_data = 0;
    
    bool ok1a = do_cache_op_debug(dut, m_trace, addr1, write_data, true, NULL, "W1");
    bool ok1b = do_cache_op_debug(dut, m_trace, addr1, 0, false, &read_data, "R1");
    
    bool ok1 = ok1a && ok1b && (read_data == write_data);
    print_result(ok1, "Test 1");
    if (ok1) passed++; else failed++;
    printf("\n");
    
    // ========== Test 2: 4-Way Set Associative（使用不同set避免冲突）==========
    printf("Test 2: 4-Way Set Associative (Different Sets)\n");
    printf("----------------------------------------\n");
    
    // 使用不同set，确保不冲突
    uint64_t test_data[4] = {
        0x1111111111111111ULL,
        0x2222222222222222ULL,
        0x3333333333333333ULL,
        0x4444444444444444ULL
    };
    uint64_t read_back[4] = {0};
    bool ok2 = true;
    
    // 写入4个不同set（避免同一set的替换问题）
    for (int i = 0; i < 4; i++) {
        uint64_t addr = make_addr(0x200 + i, 0x10 + i, 0);  // 不同tag，不同set
        char name[16];
        sprintf(name, "W2_%d", i);
        
        bool ok = do_cache_op_debug(dut, m_trace, addr, test_data[i], true, NULL, name);
        if (!ok) ok2 = false;
    }
    
    // 读回验证
    for (int i = 0; i < 4; i++) {
        uint64_t addr = make_addr(0x200 + i, 0x10 + i, 0);
        char name[16];
        sprintf(name, "R2_%d", i);
        
        bool ok = do_cache_op_debug(dut, m_trace, addr, 0, false, &read_back[i], name);
        if (read_back[i] != test_data[i]) {
            printf("    MISMATCH: exp 0x%016lx, got 0x%016lx\n", test_data[i], read_back[i]);
            ok2 = false;
        }
    }
    
    print_result(ok2, "Test 2");
    if (ok2) passed++; else failed++;
    printf("\n");
    
    // ========== Test 3: 同一Set多Way（真正的4-way测试）==========
    printf("Test 3: Same Set, 4 Ways\n");
    printf("----------------------------------------\n");
    
    uint64_t same_set = 0x20;
    uint64_t data3[4] = {0xAAAA1111AAAA1111ULL, 0xBBBB2222BBBB2222ULL,
                         0xCCCC3333CCCC3333ULL, 0xDDDD4444DDDD4444ULL};
    uint64_t read3[4] = {0};
    bool ok3 = true;
    
    // 先Flush确保干净
    printf("  Flushing cache first...\n");
    dut->flush_req = 1;
    posedge(dut, m_trace);
    dut->flush_req = 0;
    int fc = 0;
    while (!dut->flush_done && fc < 500) {
        posedge(dut, m_trace);
        fc++;
    }
    printf("  Flush done in %d cycles\n", fc);
    
    // 写入4个way
    for (int i = 0; i < 4; i++) {
        uint64_t addr = make_addr(0x300 + i, same_set, 0);
        char name[16];
        sprintf(name, "W3_%d", i);
        
        bool ok = do_cache_op_debug(dut, m_trace, addr, data3[i], true, NULL, name);
        if (!ok) ok3 = false;
    }
    
    // 读回
    for (int i = 0; i < 4; i++) {
        uint64_t addr = make_addr(0x300 + i, same_set, 0);
        char name[16];
        sprintf(name, "R3_%d", i);
        
        bool ok = do_cache_op_debug(dut, m_trace, addr, 0, false, &read3[i], name);
        if (read3[i] != data3[i]) {
            printf("    MISMATCH way %d: exp 0x%016lx, got 0x%016lx\n", i, data3[i], read3[i]);
            ok3 = false;
        }
    }
    
    print_result(ok3, "Test 3");
    if (ok3) passed++; else failed++;
    printf("\n");
    
    // ========== Test 4: Flush ==========
    printf("Test 4: Cache Flush\n");
    printf("----------------------------------------\n");
    
    dut->flush_req = 1;
    posedge(dut, m_trace);
    dut->flush_req = 0;
    
    int flush_cycles = 0;
    while (!dut->flush_done && flush_cycles < 500) {
        posedge(dut, m_trace);
        flush_cycles++;
    }
    
    bool ok4 = dut->flush_done;
    printf("  Flush took %d cycles\n", flush_cycles);
    print_result(ok4, "Test 4");
    if (ok4) passed++; else failed++;
    
    // ========== 总结 ==========
    printf("\n========================================\n");
    printf("Test Summary: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");
    
    m_trace->close();
    delete m_trace;
    delete dut;
    
    return failed > 0 ? 1 : 0;
}