// tb_gpc_cluster.cpp — 适配修复后 RTL 的测试平台

#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vgpc_cluster.h"

#include <cstdint>
#include <cstdio>

static vluint64_t g_time = 0;

static void set_wide128(VlWide<4> &w, uint32_t fill32) {
    for (int i = 0; i < 4; ++i) w[i] = fill32;
}

static void tick(Vgpc_cluster *dut, VerilatedVcdC *tf) {
    dut->clk = 0;
    dut->eval();
    if (tf) tf->dump(g_time++);
    dut->clk = 1;
    dut->eval();
    if (tf) tf->dump(g_time++);
}

static void reset_dut(Vgpc_cluster *dut, VerilatedVcdC *tf, int cycles) {
    dut->rst_n = 0;
    dut->dispatch_valid = 0;
    dut->grid_entry_pc = 0;
    dut->grid_dim_x = 0;
    dut->grid_dim_y = 0;
    dut->grid_dim_z = 0;
    dut->kernel_args = 0;
    dut->gpc_done_ready = 0;
    dut->l2_req_ready = 1;
    dut->l2_resp_valid = 0;
    dut->l2_resp_ready = 1;
    set_wide128(dut->l2_resp_data, 0);
    // 强制 SM idle，帮助通过 WAIT_DONE
    dut->sm_idle_status = (1 << 4) - 1;  // 0xF，所有4个SM idle
    for (int i = 0; i < cycles; ++i) tick(dut, tf);
    dut->rst_n = 1;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vgpc_cluster *dut = new Vgpc_cluster;
    VerilatedVcdC *tf = new VerilatedVcdC;
    dut->trace(tf, 99);
    tf->open("gpc_cluster.vcd");

    int failures = 0;
    auto fail = [&](const char *msg) {
        fprintf(stderr, "[FAIL] %s @ time %llu\n", msg, (unsigned long long)g_time);
        ++failures;
    };

    // 1) 复位检查
    reset_dut(dut, tf, 8);
    if (!dut->dispatch_ready) fail("after reset: expected dispatch_ready = 1");

    // 2) 启动任务 (2x2x1=4 blocks)
    const uint16_t total_blocks = 4;
    dut->grid_entry_pc = 0x10000ULL;
    dut->grid_dim_x = 2;
    dut->grid_dim_y = 2;
    dut->grid_dim_z = 1;
    dut->kernel_args = 0xDEADBEEFu;
    dut->dispatch_valid = 1;
    tick(dut, tf);
    dut->dispatch_valid = 0;

    if (dut->dispatch_ready) fail("after dispatch: expected dispatch_ready = 0");

    // 3) 监测分发过程
    uint16_t last_remaining = total_blocks;
    int dispatches_seen = 0;
    
    for (int k = 0; k < 100; ++k) {
        uint16_t curr = dut->blocks_remaining;
        
        if (curr != last_remaining) {
            printf("[INFO] blocks_remaining: %d -> %d @ time %llu\n", 
                   last_remaining, curr, (unsigned long long)g_time);
            dispatches_seen += (last_remaining - curr);
            last_remaining = curr;
        }

        // 保持 SM idle，确保 WAIT_DONE 能通过
        dut->sm_idle_status = 0xF;
        
        if (curr == 0 && dispatches_seen == total_blocks) {
            printf("[PASS] All %d blocks dispatched @ time %llu\n", total_blocks, (unsigned long long)g_time);
            break;
        }
        
        tick(dut, tf);
    }

    if (dispatches_seen != total_blocks) {
        fprintf(stderr, "[FAIL] Expected %d dispatches, but saw %d\n", total_blocks, dispatches_seen);
        failures++;
    }

    // 4) 等待 SIGNAL_DONE
    dut->gpc_done_ready = 1;
    bool done_handshaked = false;
    
    for (int k = 0; k < 100; ++k) {
        dut->sm_idle_status = 0xF;  // 持续保持 idle
        if (dut->gpc_done_valid) {
            tick(dut, tf);
            dut->gpc_done_ready = 0;
            done_handshaked = true;
            printf("[INFO] GPC done handshake @ time %llu\n", (unsigned long long)g_time);
            break;
        }
        tick(dut, tf);
    }

    if (!done_handshaked) fail("gpc_done_valid not asserted");

    // 等待回到 IDLE
    for (int k = 0; k < 20; ++k) tick(dut, tf);
    if (!dut->dispatch_ready) fail("expected dispatch_ready = 1 (back to IDLE)");

    // 5) L2 Smoke 测试
    dut->l2_req_ready = 1;
    int l2_req_count = 0;
    for (int k = 0; k < 50; ++k) {
        if (dut->l2_req_valid) {
            l2_req_count++;
            set_wide128(dut->l2_resp_data, 0x5A5A5A5Au);
            dut->l2_resp_valid = 1;
        } else {
            dut->l2_resp_valid = 0;
        }
        tick(dut, tf);
        dut->l2_resp_valid = 0;
    }
    printf("[INFO] L2 requests observed: %d\n", l2_req_count);

    tf->close();
    delete tf;
    delete dut;

    if (failures) {
        fprintf(stderr, "Done: %d failure(s)\n", failures);
        return 1;
    }
    printf("tb_gpc_cluster: PASSED\n");
    return 0;
}

