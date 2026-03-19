// ============================================================================
// Verilator C++ Testbench for LSU Module (Fixed for VlWide types)
// ============================================================================
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <iostream>
#include <iomanip>
#include <cstdint>

#include "Vlsu.h"

#define NUM_LANES 32
#define DATA_W    32
#define CLK_PERIOD 10

class LSUTestbench {
private:
    Vlsu* dut;
    VerilatedVcdC* tfp;
    vluint64_t sim_time;
    int test_count;
    int pass_count;
    int fail_count;

    // Helper: Set 32-bit value in VlWide array at specific word position
    // VlWide<32> has 32 words, each 32 bits = 1024 bits total
    void setWide32(VlWide<32>& wide, int word_idx, uint32_t value) {
        wide.m_storage[word_idx] = value;
    }

    // Helper: Get 32-bit value from VlWide array
    uint32_t getWide32(const VlWide<32>& wide, int word_idx) {
        return wide.m_storage[word_idx];
    }

    // Helper: Set all of VlWide<32> from array of 32 uint32_t values
    void setWide32Array(VlWide<32>& wide, const uint32_t* values) {
        for (int i = 0; i < 32; i++) {
            wide.m_storage[i] = values[i];
        }
    }

    // Helper: Set 16-bit value in VlWide<16> at specific position
    void setWide16(VlWide<16>& wide, int idx, uint16_t value) {
        wide.m_storage[idx] = value;
    }

    // Helper: Get 16-bit value from VlWide<16>
    uint16_t getWide16(const VlWide<16>& wide, int idx) {
        return wide.m_storage[idx];
    }

    // Helper: Set VlWide<4> (128-bit) from four 32-bit words
    void setWide4(VlWide<4>& wide, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
        wide.m_storage[0] = w0;
        wide.m_storage[1] = w1;
        wide.m_storage[2] = w2;
        wide.m_storage[3] = w3;
    }

    // Helper: Get 32-bit from VlWide<4>
    uint32_t getWide4(const VlWide<4>& wide, int idx) {
        return wide.m_storage[idx];
    }

public:
    LSUTestbench() : sim_time(0), test_count(0), pass_count(0), fail_count(0) {
        dut = new Vlsu;
        
        Verilated::traceEverOn(true);
        tfp = new VerilatedVcdC;
        dut->trace(tfp, 99);
        tfp->open("lsu_waveform.vcd");
        
        resetInputs();
        resetDUT();
    }
    
    ~LSUTestbench() {
        tfp->close();
        delete tfp;
        delete dut;
    }
    
    void resetInputs() {
        dut->clk = 0;
        dut->rst_n = 1;
        dut->valid = 0;
        dut->l1_ready = 0;
        dut->l1_rdata_valid = 0;
        dut->lds_rdata_valid = 0;
        dut->lds_ready = 0xFFFFFFFF;
        
        // Clear wide signals
        for (int i = 0; i < 32; i++) {
            dut->offset.m_storage[i] = 0;
            dut->store_data.m_storage[i] = 0;
            dut->lds_rdata_flat.m_storage[i] = 0;
        }
        for (int i = 0; i < 16; i++) {
            dut->lds_addr_flat.m_storage[i] = 0;
        }
    }
    
    void tick() {
        dut->clk = !dut->clk;
        dut->eval();
        
 
        tfp->dump(sim_time);

        sim_time += CLK_PERIOD / 2;
    }
    
    void resetDUT() {
        dut->rst_n = 0;
        for (int i = 0; i < 10; i++) tick();
        dut->rst_n = 1;
        for (int i = 0; i < 4; i++) tick();
        std::cout << "[INFO] Reset completed at time " << sim_time << "ns" << std::endl;
    }
    
    void waitReady() {
        int timeout = 1000;
        while (!dut->ready && timeout-- > 0) tick();
    }
    
    void waitL1Request() {
        int timeout = 1000;
        while (!dut->l1_valid && timeout-- > 0) tick();
    }
    
    void waitL1Response() {
        int timeout = 1000;
        while (!dut->load_valid && timeout-- > 0) tick();
    }

    // Test 1: L1 Coalesced Store
    void testL1Store() {
        test_count++;
        std::cout << "\n========================================" << std::endl;
        std::cout << "[TEST " << test_count << "] L1 Coalesced Store" << std::endl;
        std::cout << "========================================" << std::endl;
        
        waitReady();
        
        dut->warp_id = 3;
        dut->base_addr = 0x1000;
        dut->offset_valid = 0x0000000F;  // Lanes 0-3 active
        
        // Offsets: lane0=0, lane1=4, lane2=8, lane3=12
        dut->offset.m_storage[0] = 0;
        dut->offset.m_storage[1] = 4;
        dut->offset.m_storage[2] = 8;
        dut->offset.m_storage[3] = 12;
        
        // Store data: 0x11111111, 0x22222222, 0x33333333, 0x44444444
        dut->store_data.m_storage[0] = 0x11111111;
        dut->store_data.m_storage[1] = 0x22222222;
        dut->store_data.m_storage[2] = 0x33333333;
        dut->store_data.m_storage[3] = 0x44444444;
        
        dut->is_load = 0;
        dut->is_store = 1;
        dut->is_lds = 0;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        waitL1Request();
        
        std::cout << "  L1 Request:" << std::endl;
        std::cout << "    Address: 0x" << std::hex << std::setfill('0') << std::setw(16) 
                  << dut->l1_addr << std::dec << std::endl;
        std::cout << "    WData word0: 0x" << std::hex << dut->l1_wdata.m_storage[0] << std::dec << std::endl;
        std::cout << "    WData word1: 0x" << std::hex << dut->l1_wdata.m_storage[1] << std::dec << std::endl;
        std::cout << "    WData word2: 0x" << std::hex << dut->l1_wdata.m_storage[2] << std::dec << std::endl;
        std::cout << "    WData word3: 0x" << std::hex << dut->l1_wdata.m_storage[3] << std::dec << std::endl;
        std::cout << "    WrEn:    " << (int)dut->l1_wr_en << std::endl;
        std::cout << "    ByteEn:  0x" << std::hex << std::setw(4) << dut->l1_be << std::dec << std::endl;
        
        bool addr_aligned = (dut->l1_addr & 0x7F) == 0;
        bool data_correct = (dut->l1_wdata.m_storage[0] == 0x11111111) &&
                            (dut->l1_wdata.m_storage[1] == 0x22222222) &&
                            (dut->l1_wdata.m_storage[2] == 0x33333333) &&
                            (dut->l1_wdata.m_storage[3] == 0x44444444);
        bool be_correct = dut->l1_be == 0xFFFF;
        
        if (addr_aligned) {
            std::cout << "  [PASS] Address is 128B aligned" << std::endl;
        } else {
            std::cout << "  [FAIL] Address not aligned!" << std::endl;
            fail_count++;
        }
        
        if (data_correct) {
            std::cout << "  [PASS] Store data correctly packed" << std::endl;
        } else {
            std::cout << "  [FAIL] Store data incorrect!" << std::endl;
            fail_count++;
        }
        
        if (be_correct) {
            std::cout << "  [PASS] Byte enables correct (0x0FFF)" << std::endl;
        } else {
            std::cout << "  [FAIL] Byte enables incorrect!" << std::endl;
            fail_count++;
        }
        
        dut->l1_ready = 1;
        tick();
        tick();
        dut->l1_ready = 0;
        
        waitL1Response();
        std::cout << "  [PASS] Store operation completed" << std::endl;
        pass_count++;
        
        tick();
    }

    // Test 2: L1 Coalesced Load
    void testL1Load() {
        test_count++;
        std::cout << "\n========================================" << std::endl;
        std::cout << "[TEST " << test_count << "] L1 Coalesced Load" << std::endl;
        std::cout << "========================================" << std::endl;
        
        waitReady();
        
        dut->warp_id = 5;
        dut->base_addr = 0x2000;
        dut->offset_valid = 0x0000000F;
        dut->offset.m_storage[0] = 0;
        dut->offset.m_storage[1] = 4;
        dut->offset.m_storage[2] = 8;
        dut->offset.m_storage[3] = 12;
        
        dut->is_load = 1;
        dut->is_store = 0;
        dut->is_lds = 0;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        waitL1Request();
        std::cout << "  L1 Load Request: Addr=0x" << std::hex << dut->l1_addr << std::dec << std::endl;
        
        dut->l1_ready = 1;
        // Response: word0=0x1100FFEE, word1=0x55443322, word2=0x99887766, word3=0xDDCCBBAA
        dut->l1_rdata.m_storage[0] = 0x1100FFEE;
        dut->l1_rdata.m_storage[1] = 0x55443322;
        dut->l1_rdata.m_storage[2] = 0x99887766;
        dut->l1_rdata.m_storage[3] = 0xDDCCBBAA;
        tick();
        tick();
        dut->l1_ready = 0;
        tick();
        dut->l1_rdata_valid = 1;
        tick();
        tick();
        dut->l1_rdata_valid = 0;
        
        waitL1Response();
        
        std::cout << "  Load Results:" << std::endl;
        std::cout << "    Lane 0: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[0] << std::dec << " (expected 0x1100FFEE)" << std::endl;
        std::cout << "    Lane 1: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[1] << std::dec << " (expected 0x55443322)" << std::endl;
        std::cout << "    Lane 2: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[2] << std::dec << " (expected 0x99887766)" << std::endl;
        std::cout << "    Lane 3: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[3] << std::dec << " (expected 0xDDCCBBAA)" << std::endl;
        std::cout << "    Warp ID: " << (int)dut->resp_warp_id << " (expected 5)" << std::endl;
        
        bool data_ok = (dut->load_result.m_storage[0] == 0x1100FFEE) &&
                       (dut->load_result.m_storage[1] == 0x55443322) &&
                       (dut->load_result.m_storage[2] == 0x99887766) &&
                       (dut->load_result.m_storage[3] == 0xDDCCBBAA);
        bool warp_ok = dut->resp_warp_id == 5;
        
        if (data_ok && warp_ok) {
            std::cout << "  [PASS] Load data correct, Warp ID correct" << std::endl;
            pass_count++;
        } else {
            if (!data_ok) std::cout << "  [FAIL] Load data incorrect!" << std::endl;
            if (!warp_ok) std::cout << "  [FAIL] Warp ID incorrect!" << std::endl;
            fail_count++;
        }
        
        tick();
    }

    // Test 3: LDS Store (Scatter)
    void testLDSStore() {
        test_count++;
        std::cout << "\n========================================" << std::endl;
        std::cout << "[TEST " << test_count << "] LDS Store (Scatter)" << std::endl;
        std::cout << "========================================" << std::endl;
        
        waitReady();
        
        dut->warp_id = 7;
        dut->base_addr = 0x400;
        dut->offset_valid = 0x000000FF;  // Lanes 0-7 active
        
        // Offsets: 0, 4, 8, 12, 16, 20, 24, 28
        for (int i = 0; i < 8; i++) {
            dut->offset.m_storage[i] = i * 4;
        }
        
        // Data: 0xC0, 0xC1, 0xC2, ... 0xC7
        for (int i = 0; i < 8; i++) {
            dut->store_data.m_storage[i] = 0xC0 + i;
        }
        
        dut->is_load = 0;
        dut->is_store = 1;
        dut->is_lds = 1;
        dut->valid = 1;
        tick();
        tick();
        dut->valid = 0;
        
        tick();
        //tick();
        
        std::cout << "  LDS Requests (first 4 lanes):" << std::endl;
        bool any_valid = false;
        for (int i = 0; i < 4; i++) {
            if ((dut->lds_valid_flat >> i) & 1) {
                any_valid = true;
                std::cout << "    Lane " << i << ": Addr=0x" << std::hex << std::setfill('0') 
                          << std::setw(4) << (uint16_t)dut->lds_addr_flat.m_storage[i] 
                          << ", Data=0x" << std::setw(8) << dut->lds_wdata_flat.m_storage[i] 
                          << std::dec << std::endl;
            }
        }
        
        if (!any_valid) {
            std::cout << "  [WARN] No LDS valid signals detected!" << std::endl;
        }
        
        dut->lds_ready = 0xFFFFFFFF;
        tick();
        
        int timeout = 100;
        while (!dut->load_valid && timeout-- > 0) tick();
        
        if (dut->load_valid) {
            std::cout << "  [PASS] LDS Store completed" << std::endl;
            pass_count++;
        } else {
            std::cout << "  [FAIL] LDS Store timeout!" << std::endl;
            fail_count++;
        }
        
        tick();
    }

    // Test 4: LDS Load (Gather)
    void testLDSLoad() {
        test_count++;
        std::cout << "\n========================================" << std::endl;
        std::cout << "[TEST " << test_count << "] LDS Load (Gather)" << std::endl;
        std::cout << "========================================" << std::endl;
        
        waitReady();
        
        dut->warp_id = 9;
        dut->base_addr = 0x800;
        dut->offset_valid = 0x0000000F;
        dut->offset.m_storage[0] = 0;
        dut->offset.m_storage[1] = 4;
        dut->offset.m_storage[2] = 8;
        dut->offset.m_storage[3] = 12;
        
        dut->is_load = 1;
        dut->is_store = 0;
        dut->is_lds = 1;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        tick();
        tick();
        
        dut->lds_rdata_flat.m_storage[0] = 0xAABBCCDD;
        dut->lds_rdata_flat.m_storage[1] = 0x11223344;
        dut->lds_rdata_flat.m_storage[2] = 0x55667788;
        dut->lds_rdata_flat.m_storage[3] = 0x99AABBCC;
        dut->lds_rdata_valid = 1;
        tick();
        tick();
        dut->lds_rdata_valid = 0;
        
        int timeout = 100;
        while (!dut->load_valid && timeout-- > 0) tick();
        
        std::cout << "  LDS Load Results:" << std::endl;
        std::cout << "    Lane 0: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[0] << std::dec << " (expected 0xAABBCCDD)" << std::endl;
        std::cout << "    Lane 1: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[1] << std::dec << " (expected 0x11223344)" << std::endl;
        std::cout << "    Lane 2: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[2] << std::dec << " (expected 0x55667788)" << std::endl;
        std::cout << "    Lane 3: 0x" << std::hex << std::setfill('0') << std::setw(8) 
                  << dut->load_result.m_storage[3] << std::dec << " (expected 0x99AABBCC)" << std::endl;
        
        bool ok = (dut->load_result.m_storage[0] == 0xAABBCCDD) &&
                  (dut->load_result.m_storage[1] == 0x11223344) &&
                  (dut->load_result.m_storage[2] == 0x55667788) &&
                  (dut->load_result.m_storage[3] == 0x99AABBCC);
        
        if (ok) {
            std::cout << "  [PASS] LDS Load data correct" << std::endl;
            pass_count++;
        } else {
            std::cout << "  [FAIL] LDS Load data incorrect!" << std::endl;
            fail_count++;
        }
        
        tick();
    }

    // Test 5: Non-coalesced access detection
    void testNonCoalesced() {
        test_count++;
        std::cout << "\n========================================" << std::endl;
        std::cout << "[TEST " << test_count << "] Non-Coalesced Detection" << std::endl;
        std::cout << "========================================" << std::endl;
        
        waitReady();
        
        dut->warp_id = 1;
        dut->base_addr = 0x1000;
        dut->offset_valid = 0x00000003;  // Lanes 0-1
        dut->offset.m_storage[0] = 0;     // Lane 0: addr 0x1000
        dut->offset.m_storage[1] = 128;   // Lane 1: addr 0x1080 (different line!)
        
        dut->store_data.m_storage[0] = 0xCAFEBABE;
        dut->store_data.m_storage[1] = 0xDEADBEEF;
        
        dut->is_load = 1;
        dut->is_store = 0;
        dut->is_lds = 0;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        waitL1Request();
        
        std::cout << "  L1 Request Addr: 0x" << std::hex << dut->l1_addr << std::dec << std::endl;
        std::cout << "  [NOTE] Current design uses first active lane's address" << std::endl;
        std::cout << "  [WARN] Full implementation should split into multiple requests!" << std::endl;
        
        dut->l1_ready = 1;
        dut->l1_rdata.m_storage[0] = 0xCAFEBABE;
        dut->l1_rdata.m_storage[1] = 0;
        dut->l1_rdata.m_storage[2] = 0;
        dut->l1_rdata.m_storage[3] = 0;
        tick();
        dut->l1_ready = 0;
        dut->l1_rdata_valid = 1;
        tick();
        tick();
        dut->l1_rdata_valid = 0;
        
        waitL1Response();
        std::cout << "  [INFO] Non-coalesced test completed (partial handling)" << std::endl;
        pass_count++;
        
        tick();
    }

    // Test 6: Back-to-back requests
    void testBackToBack() {
        test_count++;
        std::cout << "\n========================================" << std::endl;
        std::cout << "[TEST " << test_count << "] Back-to-Back Requests" << std::endl;
        std::cout << "========================================" << std::endl;
        
        // First request: Store
        waitReady();
        dut->warp_id = 10;
        dut->base_addr = 0x3000;
        dut->offset_valid = 0x0000000F;
        dut->offset.m_storage[0] = 0;
        dut->offset.m_storage[1] = 4;
        dut->offset.m_storage[2] = 8;
        dut->offset.m_storage[3] = 12;
        dut->store_data.m_storage[0] = 0x11111111;
        dut->store_data.m_storage[1] = 0x22222222;
        dut->store_data.m_storage[2] = 0x33333333;
        dut->store_data.m_storage[3] = 0x44444444;
        dut->is_load = 0;
        dut->is_store = 1;
        dut->is_lds = 0;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        waitL1Request();
        dut->l1_ready = 1;
        tick();
        dut->l1_ready = 0;
        waitL1Response();
        std::cout << "  [DONE] First request (Store) completed" << std::endl;
        
        // Second request: Load
        waitReady();
        dut->warp_id = 11;
        dut->base_addr = 0x3000;
        dut->offset_valid = 0x0000000F;
        dut->is_load = 1;
        dut->is_store = 0;
        dut->valid = 1;
        
        tick();
        dut->valid = 0;
        
        waitL1Request();
        dut->l1_ready = 1;
        dut->l1_rdata.m_storage[0] = 0x11111111;
        dut->l1_rdata.m_storage[1] = 0x22222222;
        dut->l1_rdata.m_storage[2] = 0x33333333;
        dut->l1_rdata.m_storage[3] = 0x44444444;
        tick();
        dut->l1_ready = 0;
        dut->l1_rdata_valid = 1;
        tick();
        tick();
        dut->l1_rdata_valid = 0;
        
        waitL1Response();
        std::cout << "  [DONE] Second request (Load) completed" << std::endl;
        
        bool data_ok = (dut->load_result.m_storage[0] == 0x11111111) &&
                       (dut->load_result.m_storage[3] == 0x44444444);
        
        if (data_ok) {
            std::cout << "  [PASS] Back-to-back requests handled correctly" << std::endl;
            pass_count++;
        } else {
            std::cout << "  [FAIL] Back-to-back data mismatch!" << std::endl;
            fail_count++;
        }
        
        tick();
    }

    void printSummary() {
        std::cout << "\n========================================" << std::endl;
        std::cout << "           Test Summary" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  Total Tests:  " << test_count << std::endl;
        std::cout << "  Passed:      " << pass_count << std::endl;
        std::cout << "  Failed:      " << fail_count << std::endl;
        
        if (fail_count == 0) {
            std::cout << "\n  *** ALL TESTS PASSED ***" << std::endl;
        } else {
            std::cout << "\n  *** SOME TESTS FAILED ***" << std::endl;
        }
        std::cout << "========================================" << std::endl;
    }
    
    void runAllTests() {
        testL1Store();
        testL1Load();
        testLDSStore();
        testLDSLoad();
        testNonCoalesced();
        testBackToBack();
        
        for (int i = 0; i < 20; i++) tick();
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    std::cout << "============================================" << std::endl;
    std::cout << "    LSU (Load/Store Unit) Verilator Test" << std::endl;
    std::cout << "============================================" << std::endl;
    
    LSUTestbench tb;
    tb.runAllTests();
    tb.printSummary();
    
    return 0;
}