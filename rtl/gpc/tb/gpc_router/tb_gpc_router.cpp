// tb_gpc_router.cpp — Verilator C++ testbench for gpc_router
//
// 覆盖意图：
//  1) 复位后 l2_req_valid 为低。
//  2) 单笔 SM 请求：握手成功后 L2 侧看到正确载荷与 l2_req_id；响应返回后 sm_resp_sm_id 正确。
//  3) L2 反压：l2_req_ready=0 时 sm_req_ready=0（OT 未满）。
//  4) 多笔顺序请求：ID 递增，顺序响应时 SM ID 与发起一致。
//
// 说明：DATA_W=128 时 Verilator 映射为 VlWide<4>；ADDR_W=64 一般为 vluint64_t/QData。
//       若你本机 Verilator 生成的端口类型不同，请按 Vgpc_router.h 微调赋值方式。
// 详见 rtl/gpc/doc/gpc_router.md。

#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vgpc_router.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static vluint64_t g_time = 0;

static void set_wide128(VlWide<4> &w, uint32_t fill32) {
    for (int i = 0; i < 4; ++i) {
        w[i] = fill32;
    }
}

static void tick(Vgpc_router *dut, VerilatedVcdC *tf) {
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

static void reset_dut(Vgpc_router *dut, VerilatedVcdC *tf, int cycles) {
    dut->rst_n = 0;
    dut->sm_req_valid = 0;
    dut->sm_req_addr = 0;
    set_wide128(dut->sm_req_data, 0);
    dut->sm_req_wr_en = 0;
    dut->sm_req_be = 0;
    dut->sm_req_sm_id = 0;
    dut->sm_resp_ready = 1;
    dut->l2_req_ready = 1;
    dut->l2_resp_valid = 0;
    set_wide128(dut->l2_resp_data, 0);
    dut->l2_resp_id = 0;
    for (int i = 0; i < cycles; ++i) {
        tick(dut, tf);
    }
    dut->rst_n = 1;
}

// 默认 NUM_SM=4 → SM_ID_W=2
static const uint32_t kSmMask = 0x3u;
// OT_ID_W = $clog2(32) = 5
static const uint32_t kOtMask = 0x1fu;

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vgpc_router *dut = new Vgpc_router;
    VerilatedVcdC *tf = new VerilatedVcdC;
    dut->trace(tf, 99);
    tf->open("gpc_router.vcd");

    int failures = 0;
    auto fail = [&](const char *msg) {
        fprintf(stderr, "[FAIL] %s @ time %llu\n", msg, (unsigned long long)g_time);
        ++failures;
    };

    // --- 1) 复位后：l2_req_valid 应为 0 ---
    reset_dut(dut, tf, 8);
    if (dut->l2_req_valid) {
        fail("after reset: expected l2_req_valid=0");
    }

    // L2 本地模型：记录已接受事务的 id 队列（顺序完成）
    std::vector<uint8_t> pending_ids;

    auto drive_l2_accept = [&]() {
        if (dut->l2_req_valid && dut->l2_req_ready) {
            pending_ids.push_back(static_cast<uint8_t>(dut->l2_req_id & kOtMask));
        }
    };

    // --- 2) 单笔请求：SM2，L2 立即 ready ---
    // 使用 addr[63:48]=0 作为“本地 L2”文档地址（RTL 当前不检查）
    dut->sm_req_addr = (0x0000ULL << 48) | 0x1000ULL;
    set_wide128(dut->sm_req_data, 0xAABBCCDDu);
    dut->sm_req_wr_en = 0;
    dut->sm_req_be = 0xFFFF;
    dut->sm_req_sm_id = 2 & kSmMask;
    dut->sm_req_valid = 1;
    dut->l2_req_ready = 1;

    tick(dut, tf);
    drive_l2_accept();

    if (!dut->sm_req_ready) {
        fail("single req: expected sm_req_ready=1 when l2 ready and OT not full");
    }
    if (!dut->l2_req_valid) {
        fail("single req: expected l2_req_valid=1 after accepted handshake");
    }
    if ((dut->l2_req_id & kOtMask) != 0) {
        fail("single req: expected first l2_req_id==0");
    }
    dut->sm_req_valid = 0;
    tick(dut, tf);
    drive_l2_accept();

    if (pending_ids.size() != 1u) {
        fail("model: expected one pending L2 txn");
    }

    // 响应：顺序返回
    uint8_t rid = pending_ids[0];
    pending_ids.clear();
    set_wide128(dut->l2_resp_data, 0x11223344u);
    dut->l2_resp_id = rid;
    dut->l2_resp_valid = 1;
    dut->sm_resp_ready = 1;
    tick(dut, tf);

    if (!dut->sm_resp_valid) {
        fail("resp: expected sm_resp_valid=1");
    }
    if ((dut->sm_resp_sm_id & kSmMask) != 2u) {
        fail("resp: expected sm_resp_sm_id==2");
    }
    dut->l2_resp_valid = 0;
    tick(dut, tf);

    // --- 3) L2 反压：未握手成功前 sm_req_ready 应为 0 ---
    dut->l2_req_ready = 0;
    dut->sm_req_addr = (0x0000ULL << 48) | 0x2000ULL;
    dut->sm_req_sm_id = 0;
    dut->sm_req_valid = 1;
    tick(dut, tf);
    if (dut->sm_req_ready) {
        fail("backpressure: expected sm_req_ready=0 when l2_req_ready=0");
    }
    dut->sm_req_valid = 0;
    dut->l2_req_ready = 1;
    tick(dut, tf);

    // --- 4) 连续两笔不同 SM：顺序响应 ---
    dut->sm_req_valid = 1;
    dut->sm_req_sm_id = 1;
    dut->sm_req_addr = (0x0000ULL << 48) | 0x3000ULL;
    tick(dut, tf);
    drive_l2_accept();
    dut->sm_req_valid = 0;
    tick(dut, tf);
    drive_l2_accept();

    dut->sm_req_valid = 1;
    dut->sm_req_sm_id = 3;
    dut->sm_req_addr = (0x0000ULL << 48) | 0x4000ULL;
    tick(dut, tf);
    drive_l2_accept();
    dut->sm_req_valid = 0;
    tick(dut, tf);
    drive_l2_accept();

    if (pending_ids.size() != 2u) {
        fail("multi: expected two pending ids");
    }

    dut->l2_resp_valid = 1;
    dut->l2_resp_id = pending_ids[0];
    set_wide128(dut->l2_resp_data, 0xAAAABBBBu);
    tick(dut, tf);
    if ((dut->sm_resp_sm_id & kSmMask) != 1u) {
        fail("multi resp0: sm_resp_sm_id should be 1");
    }
    dut->l2_resp_valid = 0;
    tick(dut, tf);

    dut->l2_resp_valid = 1;
    dut->l2_resp_id = pending_ids[1];
    set_wide128(dut->l2_resp_data, 0xCCCCDDDDu);
    tick(dut, tf);
    if ((dut->sm_resp_sm_id & kSmMask) != 3u) {
        fail("multi resp1: sm_resp_sm_id should be 3");
    }
    dut->l2_resp_valid = 0;
    tick(dut, tf);

    tf->close();
    delete tf;
    delete dut;

    if (failures == 0) {
        printf("tb_gpc_router: PASS\n");
        return 0;
    }
    fprintf(stderr, "tb_gpc_router: %d failure(s)\n", failures);
    return 1;
}
