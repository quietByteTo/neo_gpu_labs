// tb_sm_core_top.cpp — Verilator C++ testbench for sm_core_top
//
// 本测试覆盖的特性（按场景）：
//  1) 复位：task_done 在无非活跃 warp 时应为 1；perf_inst_count 为 0。
//  2) 任务握手：task_valid 与 task_ready 的 valid/ready 行为（前端当前实现中 task_ready 常为 1）。
//  3) 任务启动后：warp_status 中对应 start_warp_id 位被置位（活跃掩码）。
//  4) 发射计数：perf_inst_count 在流水线成功握手发射时应单调递增（issue_valid && issue_ready）。
//  5) L1 从设备模型：当 l1_req_valid 置位时置位 l1_req_ready；对 load 路径可返回 l1_resp_valid/数据（便于后续接真 LSU 用例）。
//  6) task_done：仅当 warp_status 全 0 时为 1；当前 RTL 若从不清除 active_mask，则启动任务后可能长期为 0——测试区分「复位后」与「启动后」行为。
//
// 说明：顶层 ifetch_data 在 RTL 内为占位 0，指令流固定；本 bench 以黑盒观测顶层信号为主。

#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vsm_core_top.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static vluint64_t g_time = 0;

static void tick(Vsm_core_top *dut, VerilatedVcdC *tf) {
    dut->clk = 0;
    dut->eval();
    if (tf) {
        tf->dump(g_time++);
    }
    dut->clk = 1;
    dut->eval();
    if (tf) {
        tf->dump(g_time++);
    }
}

static void reset_dut(Vsm_core_top *dut, VerilatedVcdC *tf, int cycles) {
    dut->rst_n = 0;
    dut->task_valid = 0;
    dut->entry_pc = 0;
    dut->start_warp_id = 0;
    dut->num_warps = 0;
    dut->l1_req_ready = 1;
    dut->l1_resp_valid = 0;
    memset(&dut->l1_resp_data, 0, sizeof(dut->l1_resp_data));
    for (int i = 0; i < cycles; ++i) {
        tick(dut, tf);
    }
    dut->rst_n = 1;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vsm_core_top *dut = new Vsm_core_top;
    VerilatedVcdC *tf = new VerilatedVcdC;
    dut->trace(tf, 99);
    tf->open("sm_core_top.vcd");

    int failures = 0;
    auto fail = [&](const char *msg) {
        fprintf(stderr, "[FAIL] %s @ time %llu\n", msg, (unsigned long long)g_time);
        ++failures;
    };

    // --- Test 1: after reset, no task -> task_done should be 1 (all warps inactive) ---
    reset_dut(dut, tf, 5);
    if (!dut->task_done) {
        fail("after reset: expected task_done==1");
    }
    if (dut->perf_inst_count != 0) {
        fail("after reset: expected perf_inst_count==0");
    }

    // --- Test 2: task handshake pulse ---
    dut->entry_pc = 0x1000ULL;
    dut->start_warp_id = 3;
    dut->num_warps = 1;
    dut->task_valid = 1;
    tick(dut, tf);
    if (!dut->task_ready) {
        fail("task_valid: expected task_ready==1 (current frontend model)");
    }
    dut->task_valid = 0;
    tick(dut, tf);

    // --- Test 3: warp bit for start_warp_id set ---
    if (((dut->warp_status >> 3) & 1u) == 0) {
        fail("after launch: warp_status bit[3] should be 1");
    }
    if (dut->task_done) {
        fail("with active warp: task_done should be 0");
    }

    // --- Test 4 & 5: run cycles — perf counter increases; L1 stub keeps memory path live ---
    uint32_t p0 = dut->perf_inst_count;
    for (int c = 0; c < 500; ++c) {
        if (dut->l1_req_valid) {
            dut->l1_req_ready = 1;
            memset(&dut->l1_resp_data, 0, sizeof(dut->l1_resp_data));
            dut->l1_resp_valid = 1;
        } else {
            dut->l1_resp_valid = 0;
        }
        tick(dut, tf);
    }
    uint32_t p1 = dut->perf_inst_count;
    if (p1 <= p0) {
        fail("expected perf_inst_count to increase after many cycles (issue handshake)");
    }

    tf->close();
    delete tf;
    delete dut;

    if (failures == 0) {
        printf("tb_sm_core_top: all checks passed (perf %u -> %u).\n", p0, p1);
        return 0;
    }
    fprintf(stderr, "tb_sm_core_top: %d check(s) failed.\n", failures);
    return 1;
}
