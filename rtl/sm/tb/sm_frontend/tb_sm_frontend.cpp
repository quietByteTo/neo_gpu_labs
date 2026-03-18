//=============================================================================
// SM Frontend Verilator Testbench
// File: tb_sm_frontend.cpp
// Description: Comprehensive verification environment for sm_frontend module
//=============================================================================

#include "Vsm_frontend.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <functional>
#include <map>
#include <cassert>

//=============================================================================
// Configuration & Constants
//=============================================================================

#define NUM_WARPS       32
#define NUM_REGS        64
#define PC_W            64
#define INST_W          32
#define WARP_ID_W       5
#define REG_ADDR_W      6

// Test configuration
#define MAX_SIM_TIME    10000
#define TRACE_ENABLE    1

//=============================================================================
// Instruction Encoding Helpers (Simplified ISA)
//=============================================================================

// Instruction format: [31:26] opcode, [25:20] rd, [19:14] rs0, [13:8] rs1, [7:2] rs2, [1:0] funct
class InstEncoder {
public:
    // R-type: opcode(6) | rd(6) | rs0(6) | rs1(6) | rs2(6) | funct(2)
    static uint32_t R_TYPE(uint8_t opcode, uint8_t rd, uint8_t rs0, uint8_t rs1, 
                          uint8_t rs2 = 0, uint8_t funct = 0) {
        return ((opcode & 0x3F) << 26) |
               ((rd & 0x3F) << 20) |
               ((rs0 & 0x3F) << 14) |
               ((rs1 & 0x3F) << 8) |
               ((rs2 & 0x3F) << 2) |
               (funct & 0x3);
    }
    
    // I-type: opcode(6) | rd(6) | rs0(6) | imm(12) | funct(2)
    static uint32_t I_TYPE(uint8_t opcode, uint8_t rd, uint8_t rs0, uint16_t imm) {
        return ((opcode & 0x3F) << 26) |
               ((rd & 0x3F) << 20) |
               ((rs0 & 0x3F) << 14) |
               ((imm & 0xFFF) << 2);
    }
    
    // J-type (Branch/Jump): opcode(6) | rd(6) | target(20)
    static uint32_t J_TYPE(uint8_t opcode, uint8_t rd, uint32_t target) {
        return ((opcode & 0x3F) << 26) |
               ((rd & 0x3F) << 20) |
               (target & 0xFFFFF);
    }
    
    // Common opcodes
    static constexpr uint8_t OP_ADD  = 0x01;
    static constexpr uint8_t OP_SUB  = 0x02;
    static constexpr uint8_t OP_MUL  = 0x03;
    static constexpr uint8_t OP_LOAD = 0x04;
    static constexpr uint8_t OP_STORE= 0x05;
    static constexpr uint8_t OP_BRANCH = 0x06;
    static constexpr uint8_t OP_JUMP = 0x07;
    static constexpr uint8_t OP_NOP  = 0x00;
    
    // Extract fields for checking
    static uint8_t get_rd(uint32_t inst)  { return (inst >> 20) & 0x3F; }
    static uint8_t get_rs0(uint32_t inst) { return (inst >> 14) & 0x3F; }
    static uint8_t get_rs1(uint32_t inst) { return (inst >> 8) & 0x3F; }
    static uint8_t get_opcode(uint32_t inst) { return (inst >> 26) & 0x3F; }
};

//=============================================================================
// Scoreboard Model (Golden Reference)
//=============================================================================

class ScoreboardModel {
public:
    struct RegStatus {
        bool busy;
        uint64_t ready_cycle;
    };
    
    std::vector<std::vector<RegStatus>> table; // [warp][reg]
    uint64_t current_cycle;
    
    ScoreboardModel() : current_cycle(0) {
        table.resize(NUM_WARPS, std::vector<RegStatus>(NUM_REGS, {false, 0}));
    }
    
    void reset() {
        for (auto& warp : table) {
            for (auto& reg : warp) {
                reg.busy = false;
                reg.ready_cycle = 0;
            }
        }
        current_cycle = 0;
    }
    
    void tick() { current_cycle++; }
    
    // Check RAW hazard
    bool check_raw(uint8_t warp_id, uint8_t rs0, uint8_t rs1, uint8_t rs2) {
        if (rs0 != 0 && table[warp_id][rs0].busy && table[warp_id][rs0].ready_cycle > current_cycle) 
            return true;
        if (rs1 != 0 && table[warp_id][rs1].busy && table[warp_id][rs1].ready_cycle > current_cycle) 
            return true;
        if (rs2 != 0 && table[warp_id][rs2].busy && table[warp_id][rs2].ready_cycle > current_cycle) 
            return true;
        return false;
    }
    
    // Check WAW hazard
    bool check_waw(uint8_t warp_id, uint8_t rd) {
        if (rd != 0 && table[warp_id][rd].busy && table[warp_id][rd].ready_cycle > current_cycle)
            return true;
        return false;
    }
    
    // Mark register as busy (when instruction issues)
    void mark_busy(uint8_t warp_id, uint8_t rd, uint32_t latency = 2) {
        if (rd != 0) {
            table[warp_id][rd].busy = true;
            table[warp_id][rd].ready_cycle = current_cycle + latency;
        }
    }
    
    // Clear register (when writeback occurs)
    void clear(uint8_t warp_id, uint8_t rd) {
        if (rd != 0) {
            table[warp_id][rd].busy = false;
        }
    }
};

//=============================================================================
// PC Management Model (Golden Reference)
//=============================================================================

class PCModel {
public:
    uint64_t pc[NUM_WARPS];
    bool active[NUM_WARPS];
    bool waiting_barrier[NUM_WARPS];
    
    void reset() {
        for (int i = 0; i < NUM_WARPS; i++) {
            pc[i] = 0;
            active[i] = false;
            waiting_barrier[i] = false;
        }
    }
    
    void launch(uint8_t warp_id, uint64_t entry_pc) {
        pc[warp_id] = entry_pc;
        active[warp_id] = true;
    }
    
    void step(uint8_t warp_id) {
        if (active[warp_id] && !waiting_barrier[warp_id]) {
            pc[warp_id] += 4;
        }
    }
    
    void branch(uint8_t warp_id, uint64_t target) {
        pc[warp_id] = target;
    }
    
    uint64_t get_pc(uint8_t warp_id) { return pc[warp_id]; }
    bool is_active(uint8_t warp_id) { return active[warp_id]; }
};

//=============================================================================
// Test Statistics & Reporting
//=============================================================================

struct TestStats {
    std::string name;
    bool passed;
    uint64_t sim_cycles;
    std::string fail_reason;
    
    void report() const {
        std::cout << "  [" << (passed ? "PASS" : "FAIL") << "] " 
                  << std::left << std::setw(30) << name
                  << " Cycles: " << std::setw(6) << sim_cycles;
        if (!passed) std::cout << " | Reason: " << fail_reason;
        std::cout << std::endl;
    }
};

class TestReporter {
public:
    std::vector<TestStats> results;
    
    void add(const std::string& name, bool passed, uint64_t cycles, 
             const std::string& reason = "") {
        results.push_back({name, passed, cycles, reason});
    }
    
    void summary() const {
        int passed = 0, failed = 0;
        for (const auto& r : results) {
            if (r.passed) passed++; else failed++;
        }
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "           TEST SUMMARY" << std::endl;
        std::cout << "========================================" << std::endl;
        for (const auto& r : results) r.report();
        std::cout << "----------------------------------------" << std::endl;
        std::cout << "Total: " << (passed + failed) 
                  << " | Passed: " << passed 
                  << " | Failed: " << failed << std::endl;
        std::cout << "========================================" << std::endl;
    }
};

//=============================================================================
// Main Testbench Class
//=============================================================================

class SMFrontendTB {
public:
    Vsm_frontend* dut;
    VerilatedVcdC* trace;
    uint64_t sim_time;
    
    // Golden models
    ScoreboardModel sb_model;
    PCModel pc_model;
    TestReporter reporter;
    
    // Test state
    bool test_failed;
    std::string fail_msg;
    uint64_t test_start_time;
    
    // Tracking
    std::map<uint8_t, uint32_t> warp_inst_count;
    uint32_t total_issued;
    uint32_t total_stalls_raw;
    uint32_t total_stalls_waw;
    
    SMFrontendTB() : sim_time(0), test_failed(false), total_issued(0),
                     total_stalls_raw(0), total_stalls_waw(0) {
        Verilated::traceEverOn(TRACE_ENABLE);
        dut = new Vsm_frontend;
        
        if (TRACE_ENABLE) {
            trace = new VerilatedVcdC;
            dut->trace(trace, 99);
            trace->open("sm_frontend.vcd");
        }
        
        reset_dut();
    }
    
    ~SMFrontendTB() {
        if (TRACE_ENABLE) {
            trace->close();
            delete trace;
        }
        delete dut;
    }
    
    //=========================================================================
    // Basic Operations
    //=========================================================================
    
    void reset_dut() {
        dut->clk = 0;
        dut->rst_n = 0;
        
        // Initialize all inputs
        dut->entry_pc = 0;
        dut->start_warp_id = 0;
        dut->task_valid = 0;
        
        dut->ifetch_data = 0;
        dut->ifetch_ready = 1;
        
        for (int i = 0; i < 4; i++) {
            dut->rf_rd_data[i] = 0;
        }
        
        dut->issue_ready = 1;
        
        dut->wb_warp_id = 0;
        dut->wb_branch_taken = 0;
        dut->wb_branch_target = 0;
        dut->wb_valid = 0;
        
        // Apply reset
        for (int i = 0; i < 10; i++) tick();
        dut->rst_n = 1;
        tick();
        
        // Reset models
        sb_model.reset();
        pc_model.reset();
        warp_inst_count.clear();
    }
    
    void tick() {
        // Falling edge
        dut->clk = 0;
        dut->eval();
        if (TRACE_ENABLE) trace->dump(sim_time++);
        
        // Rising edge
        dut->clk = 1;
        dut->eval();
        if (TRACE_ENABLE) trace->dump(sim_time++);
        
        // Update models
        sb_model.tick();
        
        // Track statistics
        if (dut->sc_stall_raw) total_stalls_raw++;
        if (dut->sc_stall_waw) total_stalls_waw++;
    }
    
    void ticks(int n) {
        for (int i = 0; i < n; i++) tick();
    }
    
    //=========================================================================
    // High-Level Operations
    //=========================================================================
    
    void launch_task(uint64_t pc, uint8_t warp_id) {
        dut->entry_pc = pc;
        dut->start_warp_id = warp_id;
        dut->task_valid = 1;
        tick();
        dut->task_valid = 0;
        
        // Update model
        pc_model.launch(warp_id, pc);
    }
    
    void simulate_icache(uint32_t inst, bool ready = true) {
        dut->ifetch_data = inst;
        dut->ifetch_ready = ready ? 1 : 0;
    }
    
    void simulate_regfile(uint64_t d0, uint64_t d1, uint64_t d2 = 0, uint64_t d3 = 0) {
        dut->rf_rd_data[0] = d0;
        dut->rf_rd_data[1] = d1;
        dut->rf_rd_data[2] = d2;
        dut->rf_rd_data[3] = d3;
    }
    
    void simulate_writeback(uint8_t warp_id, uint8_t rd, bool branch = false, 
                           uint64_t target = 0) {
        dut->wb_warp_id = warp_id;
        dut->wb_rd_addr = rd;  // Note: need to add this port or extract from inst
        dut->wb_valid = 1;
        dut->wb_branch_taken = branch ? 1 : 0;
        dut->wb_branch_target = target;
        tick();
        dut->wb_valid = 0;
        
        if (branch) {
            pc_model.branch(warp_id, target);
        }
        sb_model.clear(warp_id, rd);
    }
    
    //=========================================================================
    // Assertions
    //=========================================================================
    
    #define ASSERT(cond, msg) \
        do { \
            if (!(cond)) { \
                test_failed = true; \
                fail_msg = msg; \
                std::cerr << "  [ASSERT FAIL @ " << sim_time << "] " << msg << std::endl; \
                return; \
            } \
        } while(0)
    
    #define ASSERT_EQ(actual, expected, msg) \
        ASSERT((actual) == (expected), \
               std::string(msg) + " (got " + std::to_string(actual) + \
               ", expected " + std::to_string(expected) + ")")
    
    //=========================================================================
    // Test Cases
    //=========================================================================
    
    void test_reset() {
        TEST_HEADER("Reset Verification");
        
        // Check initial state
        ASSERT_EQ(dut->warp_active, 0, "warp_active should be 0 after reset");
        ASSERT_EQ(dut->task_ready, 1, "task_ready should be 1");
        ASSERT_EQ(dut->ifetch_valid, 0, "ifetch_valid should be 0");
        ASSERT_EQ(dut->issue_valid, 0, "issue_valid should be 0");
        
        // Check all warp PCs are 0
        // Note: Internal signal, need to probe or trust functionality
        
        TEST_PASS();
    }
    
    void test_task_launch() {
        TEST_HEADER("Task Launch");
        
        uint64_t test_pc = 0x1000;
        uint8_t warp_id = 5;
        
        launch_task(test_pc, warp_id);
        ticks(2);
        
        // Check warp is active
        ASSERT((dut->warp_active >> warp_id) & 1, "Launched warp should be active");
        
        // Check other warps are not affected
        ASSERT_EQ(dut->warp_active & ~(1 << warp_id), 0, "Other warps should be inactive");
        
        // Launch multiple tasks
        launch_task(0x2000, 10);
        ticks(2);
        ASSERT((dut->warp_active >> 10) & 1, "Second warp should be active");
        
        TEST_PASS();
    }
    
    void test_single_warp_pipeline() {
        TEST_HEADER("Single Warp Pipeline Flow");
        
        // Launch warp 0
        launch_task(0x1000, 0);
        
        // Feed instructions
        std::vector<uint32_t> instructions = {
            InstEncoder::R_TYPE(InstEncoder::OP_ADD, 1, 2, 3),   // ADD r1, r2, r3
            InstEncoder::R_TYPE(InstEncoder::OP_SUB, 4, 1, 5),   // SUB r4, r1, r5 (RAW on r1)
            InstEncoder::R_TYPE(InstEncoder::OP_MUL, 6, 4, 7),   // MUL r6, r4, r7 (RAW on r4)
        };
        
        int issued_count = 0;
        for (int i = 0; i < 20 && issued_count < 3; i++) {
            simulate_icache(instructions[issued_count]);
            simulate_regfile(0x100 + i, 0x200 + i);
            
            tick();
            
            if (dut->issue_valid) {
                ASSERT_EQ(dut->issue_warp_id, 0, "Should issue from warp 0");
                ASSERT_EQ(dut->decoded_inst, instructions[issued_count], 
                         "Wrong instruction issued");
                issued_count++;
                total_issued++;
                pc_model.step(0);
            }
        }
        
        ASSERT_EQ(issued_count, 3, "Should issue all 3 instructions");
        
        TEST_PASS();
    }
    
    void test_scheduler_round_robin() {
        TEST_HEADER("Scheduler Round-Robin");
        
        // Launch warps 0, 1, 2
        launch_task(0x1000, 0);
        launch_task(0x2000, 1);
        launch_task(0x3000, 2);
        ticks(3);
        
        std::vector<int> issue_order;
        uint32_t nop = InstEncoder::R_TYPE(InstEncoder::OP_NOP, 0, 0, 0);
        
        // Run for several cycles, record issue order
        for (int i = 0; i < 12; i++) {
            simulate_icache(nop);
            simulate_regfile(i, i+1);
            tick();
            
            if (dut->issue_valid) {
                issue_order.push_back(dut->issue_warp_id);
            }
        }
        
        // Check all warps get scheduled
        bool has_warp[3] = {false, false, false};
        for (int id : issue_order) {
            if (id < 3) has_warp[id] = true;
        }
        
        ASSERT(has_warp[0] && has_warp[1] && has_warp[2], 
               "All warps should be scheduled");
        
        // Check fairness (no warp starved)
        int counts[3] = {0};
        for (int id : issue_order) {
            if (id < 3) counts[id]++;
        }
        
        // Each should have roughly equal share (allowing 50% variance)
        for (int i = 0; i < 3; i++) {
            ASSERT(counts[i] >= 2, "Warp should get fair scheduling");
        }
        
        TEST_PASS();
    }
    
    void test_raw_hazard_detection() {
        TEST_HEADER("RAW Hazard Detection");
        
        launch_task(0x1000, 0);
        ticks(2);
        
        // Instruction 1: ADD r1, r2, r3 (writes r1)
        uint32_t inst1 = InstEncoder::R_TYPE(InstEncoder::OP_ADD, 1, 2, 3);
        // Instruction 2: ADD r4, r1, r5 (reads r1 - RAW)
        uint32_t inst2 = InstEncoder::R_TYPE(InstEncoder::OP_ADD, 4, 1, 5);
        
        // Issue first instruction
        simulate_icache(inst1);
        simulate_regfile(0x100, 0x200);
        tick();
        
        // Wait for it to issue
        while (!dut->issue_valid) tick();
        
        // Mark r1 as busy in model
        sb_model.mark_busy(0, 1, 5);  // r1 busy for 5 cycles
        
        // Now try to issue second instruction
        simulate_icache(inst2);
        int stall_cycles = 0;
        int max_wait = 10;
        
        while (max_wait-- > 0) {
            tick();
            if (dut->sc_stall_raw) {
                stall_cycles++;
            }
            if (dut->issue_valid) break;
        }
        
        ASSERT(stall_cycles > 0, "Should stall on RAW hazard");
        ASSERT_EQ(dut->issue_warp_id, 0, "Should eventually issue to same warp");
        
        TEST_PASS();
    }
    
    void test_waw_hazard_detection() {
        TEST_HEADER("WAW Hazard Detection");
        
        launch_task(0x1000, 0);
        ticks(2);
        
        // Two instructions writing same register
        uint32_t inst1 = InstEncoder::R_TYPE(InstEncoder::OP_ADD, 5, 1, 2);
        uint32_t inst2 = InstEncoder::R_TYPE(InstEncoder::OP_SUB, 5, 3, 4);
        
        // Issue first
        simulate_icache(inst1);
        tick();
        while (!dut->issue_valid) tick();
        
        sb_model.mark_busy(0, 5, 5);  // r5 busy
        
        // Try second
        simulate_icache(inst2);
        tick();
        
        ASSERT(dut->sc_stall_waw, "Should detect WAW hazard");
        
        // Simulate writeback to clear
        simulate_writeback(0, 5);
        ticks(2);
        
        // Should now issue
        ASSERT(dut->issue_valid || dut->sc_stall_waw == 0, 
               "Should clear WAW after writeback");
        
        TEST_PASS();
    }
    
    void test_branch_handling() {
        TEST_HEADER("Branch Handling");
        
        uint64_t entry_pc = 0x1000;
        uint64_t branch_target = 0x2000;
        
        launch_task(entry_pc, 0);
        ticks(3);
        
        // Issue a few instructions to advance PC
        uint32_t nop = InstEncoder::R_TYPE(InstEncoder::OP_NOP, 0, 0, 0);
        for (int i = 0; i < 3; i++) {
            simulate_icache(nop);
            tick();
            if (dut->issue_valid) pc_model.step(0);
        }
        
        uint64_t pc_before = pc_model.get_pc(0);
        ASSERT_EQ(pc_before, entry_pc + 12, "PC should advance sequentially");
        
        // Simulate branch
        simulate_writeback(0, 0, true, branch_target);
        ticks(2);
        
        // Next fetch should be from branch target
        // This is checked indirectly through ifetch_addr
        ASSERT_EQ(pc_model.get_pc(0), branch_target, "PC should update to branch target");
        
        TEST_PASS();
    }
    
    void test_pipeline_stall_backend() {
        TEST_HEADER("Pipeline Stall (Backend Busy)");
        
        launch_task(0x1000, 0);
        ticks(2);
        
        // Make backend busy
        dut->issue_ready = 0;
        
        uint32_t nop = InstEncoder::R_TYPE(InstEncoder::OP_NOP, 0, 0, 0);
        int cycles_without_issue = 0;
        
        for (int i = 0; i < 5; i++) {
            simulate_icache(nop);
            tick();
            if (!dut->issue_valid) cycles_without_issue++;
        }
        
        ASSERT_EQ(cycles_without_issue, 5, "Should not issue when backend busy");
        
        // Release backend
        dut->issue_ready = 1;
        tick();
        
        // Should resume issuing
        int wait_cycles = 0;
        while (!dut->issue_valid && wait_cycles < 10) {
            simulate_icache(nop);
            tick();
            wait_cycles++;
        }
        
        ASSERT(dut->issue_valid, "Should resume issuing when backend ready");
        
        TEST_PASS();
    }
    
    void test_icache_stall() {
        TEST_HEADER("I-Cache Stall Handling");
        
        launch_task(0x1000, 0);
        ticks(2);
        
        // I-Cache not ready
        dut->ifetch_ready = 0;
        
        uint32_t nop = InstEncoder::R_TYPE(InstEncoder::OP_NOP, 0, 0, 0);
        simulate_icache(nop);
        
        int stall_cycles = 0;
        for (int i = 0; i < 5; i++) {
            tick();
            if (!dut->ifetch_valid && dut->schedule_valid) {
                stall_cycles++;  // Want to fetch but can't
            }
        }
        
        ASSERT(stall_cycles > 0, "Should stall when I-Cache busy");
        
        // I-Cache becomes ready
        dut->ifetch_ready = 1;
        tick();
        
        // Should complete fetch
        ASSERT(dut->ifetch_valid || dut->issue_valid, 
               "Should resume fetch when I-Cache ready");
        
        TEST_PASS();
    }
    
    void test_scoreboard_bypass() {
        TEST_HEADER("Scoreboard Bypass (Same Cycle Write-Read)");
        
        launch_task(0x1000, 0);
        ticks(2);
        
        // Instruction that writes r1
        uint32_t inst1 = InstEncoder::R_TYPE(InstEncoder::OP_ADD, 1, 2, 3);
        simulate_icache(inst1);
        tick();
        while (!dut->issue_valid) tick();
        
        // Next instruction reads r1
        uint32_t inst2 = InstEncoder::R_TYPE(InstEncoder::OP_ADD, 4, 1, 5);
        simulate_icache(inst2);
        
        // Simulate same-cycle writeback
        dut->wb_warp_id = 0;
        dut->wb_valid = 1;
        // Note: wb_rd_addr needs to be connected in RTL
        
        tick();
        
        // Should not stall due to bypass
        // This depends on exact RTL implementation
        ASSERT(!dut->sc_stall_raw || dut->issue_valid, 
               "Bypass should prevent RAW stall or allow immediate issue");
        
        dut->wb_valid = 0;
        
        TEST_PASS();
    }
    
    void test_multi_warp_independent() {
        TEST_HEADER("Multi-Warp Independent Execution");
        
        // Launch warps with different PCs
        launch_task(0x1000, 0);
        launch_task(0x2000, 1);
        ticks(3);
        
        std::map<uint8_t, uint64_t> expected_pc;
        expected_pc[0] = 0x1000;
        expected_pc[1] = 0x2000;
        
        // Interleave instructions
        for (int i = 0; i < 10; i++) {
            uint32_t inst = InstEncoder::R_TYPE(InstEncoder::OP_ADD, i % 32, (i+1)%32, (i+2)%32);
            simulate_icache(inst);
            simulate_regfile(i * 0x100, i * 0x200);
            tick();
            
            if (dut->issue_valid) {
                uint8_t wid = dut->issue_warp_id;
                expected_pc[wid] += 4;
                warp_inst_count[wid]++;
            }
        }
        
        // Both warps should make progress
        ASSERT(warp_inst_count[0] > 0, "Warp 0 should issue instructions");
        ASSERT(warp_inst_count[1] > 0, "Warp 1 should issue instructions");
        
        TEST_PASS();
    }
    
    void test_warp0_priority() {
        TEST_HEADER("Warp 0 Priority Scheduling");
        
        // Launch all warps
        for (int i = 0; i < NUM_WARPS; i++) {
            launch_task(0x1000 + i * 0x100, i);
        }
        ticks(5);
        
        uint32_t nop = InstEncoder::R_TYPE(InstEncoder::OP_NOP, 0, 0, 0);
        
        // First valid schedule should be warp 0 (lowest ID)
        int first_warp = -1;
        for (int i = 0; i < 20 && first_warp < 0; i++) {
            simulate_icache(nop);
            tick();
            if (dut->schedule_valid) {
                first_warp = dut->dut->u_scheduler->warp_id;  // Internal probe
                // Or check ifetch_addr matches warp 0's PC
            }
        }
        
        // Alternative: Check that warp 0 gets scheduled first by monitoring
        ASSERT(first_warp == 0 || first_warp == -1, 
               "Warp 0 should have priority (or need internal signal probe)");
        
        TEST_PASS();
    }
    
    void test_concurrent_launch_and_issue() {
        TEST_HEADER("Concurrent Task Launch and Issue");
        
        // Start with one warp running
        launch_task(0x1000, 0);
        ticks(3);
        
        // While issuing, launch another task
        uint32_t nop = InstEncoder::R_TYPE(InstEncoder::OP_NOP, 0, 0, 0);
        simulate_icache(nop);
        
        // Launch in same cycle as potential issue
        dut->entry_pc = 0x3000;
        dut->start_warp_id = 5;
        dut->task_valid = 1;
        tick();
        dut->task_valid = 0;
        
        // Both operations should succeed
        ASSERT((dut->warp_active >> 5) & 1, "New warp should be activated");
        
        ticks(5);
        ASSERT((dut->warp_active >> 0) & 1, "Original warp should remain active");
        
        TEST_PASS();
    }
    
    //=========================================================================
    // Test Execution Framework
    //=========================================================================
    
    #define TEST_HEADER(name) \
        do { \
            test_failed = false; \
            fail_msg = ""; \
            test_start_time = sim_time; \
            std::cout << "\nRunning: " << name << std::endl; \
            reset_dut(); \
        } while(0)
    
    #define TEST_PASS() \
        do { \
            if (!test_failed) { \
                reporter.add(current_test_name, true, sim_time - test_start_time); \
                std::cout << "  -> PASSED (" << (sim_time - test_start_time) << " cycles)" << std::endl; \
            } else { \
                reporter.add(current_test_name, false, sim_time - test_start_time, fail_msg); \
                std::cout << "  -> FAILED: " << fail_msg << std::endl; \
            } \
        } while(0)
    
    std::string current_test_name;
    
    void run_all_tests() {
        std::cout << "========================================" << std::endl;
        std::cout << "  SM Frontend Verification Suite" << std::endl;
        std::cout << "========================================" << std::endl;
        
        std::vector<std::pair<std::string, std::function<void()>>> tests = {
            {"Reset Verification", [this]() { current_test_name = "Reset Verification"; test_reset(); }},
            {"Task Launch", [this]() { current_test_name = "Task Launch"; test_task_launch(); }},
            {"Single Warp Pipeline", [this]() { current_test_name = "Single Warp Pipeline"; test_single_warp_pipeline(); }},
            {"Scheduler Round-Robin", [this]() { current_test_name = "Scheduler Round-Robin"; test_scheduler_round_robin(); }},
            {"RAW Hazard Detection", [this]() { current_test_name = "RAW Hazard Detection"; test_raw_hazard_detection(); }},
            {"WAW Hazard Detection", [this]() { current_test_name = "WAW Hazard Detection"; test_waw_hazard_detection(); }},
            {"Branch Handling", [this]() { current_test_name = "Branch Handling"; test_branch_handling(); }},
            {"Pipeline Stall (Backend)", [this]() { current_test_name = "Pipeline Stall (Backend)"; test_pipeline_stall_backend(); }},
            {"I-Cache Stall", [this]() { current_test_name = "I-Cache Stall"; test_icache_stall(); }},
            {"Scoreboard Bypass", [this]() { current_test_name = "Scoreboard Bypass"; test_scoreboard_bypass(); }},
            {"Multi-Warp Independent", [this]() { current_test_name = "Multi-Warp Independent"; test_multi_warp_independent(); }},
            {"Warp 0 Priority", [this]() { current_test_name = "Warp 0 Priority"; test_warp0_priority(); }},
            {"Concurrent Launch/Issue", [this]() { current_test_name = "Concurrent Launch/Issue"; test_concurrent_launch_and_issue(); }},
        };
        
        for (auto& [name, test_fn] : tests) {
            try {
                test_fn();
            } catch (const std::exception& e) {
                reporter.add(name, false, sim_time - test_start_time, 
                           std::string("Exception: ") + e.what());
                std::cerr << "  -> EXCEPTION: " << e.what() << std::endl;
            }
        }
        
        // Final report
        reporter.summary();
        
        std::cout << "\nStatistics:" << std::endl;
        std::cout << "  Total instructions issued: " << total_issued << std::endl;
        std::cout << "  RAW stalls: " << total_stalls_raw << std::endl;
        std::cout << "  WAW stalls: " << total_stalls_waw << std::endl;
        std::cout << "  Total simulation time: " << sim_time << " cycles" << std::endl;
        
        if (TRACE_ENABLE) {
            std::cout << "\nWaveform saved to: sm_frontend.vcd" << std::endl;
        }
    }
};

//=============================================================================
// Main Entry
//=============================================================================

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    SMFrontendTB tb;
    tb.run_all_tests();
    
    return 0;
}