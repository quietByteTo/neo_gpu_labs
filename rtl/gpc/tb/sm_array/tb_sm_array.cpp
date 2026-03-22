// tb_sm_array.cpp — Verilator C++ testbench for sm_array
//
// 覆盖意图：
//  1) 复位后基本输出可观测。
//  2) 向指定 task_sm_id 投递任务，检查 task_ready / task_valid 握手。
//  3) L1 从设备：在 l1_req_valid 时接受或反压；响应返回时带上 l1_resp_sm_id。
//
// 说明：sm_array 将各 SM 的 l1_req_ready 与 l1_resp_sm_id / l1_resp_ready 关联；
//       激励侧需配合设置 l1_resp_sm_id，使目标 SM 能获得“请求 ready”路径上的允许。
//       详见 rtl/gpc/doc/sm_array.md。

#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vsm_array.h"

#include <cstdint>
#include <cstdio>

static vluint64_t g_time = 0;

// l1_resp_data 为 128bit，Verilator 映射为 VlWide<4>（按 32bit 字）
static void set_wide128(VlWide<4> &w, uint32_t fill32) {
    for (int i = 0; i < 4; ++i) {
        w[i] = fill32;
    }
}

static void tick(Vsm_array *dut, VerilatedVcdC *tf) {
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

static void reset_dut(Vsm_array *dut, VerilatedVcdC *tf, int cycles) {
    dut->rst_n = 0;
    dut->task_valid = 0;
    dut->task_entry_pc = 0;
    dut->task_warp_id = 0;
    dut->task_num_warps = 0;
    dut->task_sm_id = 0;
    dut->l1_req_ready = 1;
    dut->l1_resp_valid = 0;
    dut->l1_resp_ready = 1;
    dut->l1_resp_sm_id = 0;
    set_wide128(dut->l1_resp_data, 0);
    for (int i = 0; i < cycles; ++i) {
        tick(dut, tf);
    }
    dut->rst_n = 1;
}

// 默认参数 NUM_SM=4 → SM_ID_W=2，与 rtl/gpc/sm_array.v 一致
static const uint32_t kSmIdMask = 0x3u;

/// 将 l1_resp_sm_id 设为 sm，以便该 SM 的 l1_req_ready 路径与当前 RTL 一致地有效
static void arm_sm_for_req_ready(Vsm_array *dut, uint32_t sm_id) {
    dut->l1_resp_sm_id = sm_id & kSmIdMask;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vsm_array *dut = new Vsm_array;
    VerilatedVcdC *tf = new VerilatedVcdC;
    dut->trace(tf, 99);
    tf->open("sm_array.vcd");

    int failures = 0;
    auto fail = [&](const char *msg) {
        fprintf(stderr, "[FAIL] %s @ time %llu\n", msg, (unsigned long long)g_time);
        ++failures;
    };

    // --- 1) 复位后 idle：无任务时 sm_idle 期望为全 1（无 FIFO 输出）---
    reset_dut(dut, tf, 8);
    // sm_idle[i] = !sm_task_valid[i] — 无任务时通常为全 1
    if (dut->sm_idle != 0xF) {  // 默认 NUM_SM=4
        fail("after reset: expected sm_idle all 1 (4 SMs)");
    }

    // --- 2) 向 SM0 投递单任务：握手一拍 ---
    arm_sm_for_req_ready(dut, 0);
    dut->task_entry_pc = 0x1000ULL;
    dut->task_warp_id = 0;
    dut->task_num_warps = 1;
    dut->task_sm_id = 0;
    dut->task_valid = 1;
    tick(dut, tf);
    if (!dut->task_ready) {
        fail("SM0 task: expected task_ready high when FIFO accepts");
    }
    dut->task_valid = 0;
    tick(dut, tf);

    // --- 3) 向 SM1 投递（需切换 task_sm_id 与 resp_sm_id 以配合 ready 路径）---
    arm_sm_for_req_ready(dut, 1);
    dut->task_entry_pc = 0x2000ULL;
    dut->task_warp_id = 1;
    dut->task_num_warps = 1;
    dut->task_sm_id = 1;
    dut->task_valid = 1;
    tick(dut, tf);
    if (!dut->task_ready) {
        fail("SM1 task: expected task_ready high");
    }
    dut->task_valid = 0;
    tick(dut, tf);

    // --- 4) 跑若干周期让内部状态前进（黑盒 smoke）---
    arm_sm_for_req_ready(dut, 0);
    for (int k = 0; k < 20; ++k) {
        tick(dut, tf);
    }

    // --- 5) L1 从模型 smoke：反压可开可关；有请求时可打一拍响应（sm_id 需与仲裁目标一致时再收紧检查）---
    dut->l1_req_ready = 1;
    arm_sm_for_req_ready(dut, 0);
    for (int k = 0; k < 50; ++k) {
        if (dut->l1_req_valid) {
            set_wide128(dut->l1_resp_data, 0xA5A5A5A5u);
            dut->l1_resp_valid = 1;
            arm_sm_for_req_ready(dut, 0);  // 假定服务 SM0；多 SM 并发时需与 grant 对齐
        } else {
            dut->l1_resp_valid = 0;
        }
        tick(dut, tf);
        dut->l1_resp_valid = 0;
    }

    tf->close();
    delete tf;
    delete dut;

    if (failures) {
        fprintf(stderr, "Done: %d failure(s)\n", failures);
        return 1;
    }
    printf("tb_sm_array: basic checks passed (see sm_array.vcd).\n");
    return 0;
}
