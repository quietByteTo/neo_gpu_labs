这是一个 **Warp Scheduler（Warp调度器）** 模块，用于GPU或SIMT（单指令多线程）架构中，从32个Warp中选择一个就绪的Warp进行调度。

## 模块功能解释

### 核心功能
| 信号 | 说明 |
|------|------|
| `warp_valid` | 哪些Warp是激活的（还有线程要执行） |
| `warp_ready` | 哪些Warp已就绪（无数据依赖、未阻塞） |
| `eligible_warps` | 可调度Warp = valid & ready |
| `stall` | 后端阻塞信号，为高时保持当前选择 |
| `warp_id` | 选中的Warp ID（当前优先级：ID小的优先） |
| `schedule_valid` | 本次调度是否有效 |

### 调度策略
- **当前策略**：固定优先级（Fixed Priority），ID小的Warp优先
- **可扩展**：代码中预留了 `priority_mask` 用于实现Round-Robin（轮询）

---

## 完整的 Verilator Testbench

我将为你创建一个完整的Verilator验证环境，包括：
1. **Priority Encoder 模块**（依赖项）
2. **Warp Scheduler 顶层**
3. **C++ Testbench**（使用Verilator）
4. **Makefile**

### 1. Priority Encoder 模块 (`priority_encoder.v`)

```verilog
// priority_encoder.v - 优先级编码器，选择最低位的1
module priority_encoder #(
    parameter N = 32,           // 输入位宽
    parameter W = $clog2(N)     // 输出位宽
)(
    input  wire [N-1:0]   in,
    output reg  [W-1:0]   out,
    output reg            valid
);

    integer i;
    always @(*) begin
        out   = {W{1'b0}};
        valid = 1'b0;
        // 从低位到高位扫描，选择第一个1（ID小的优先）
        for (i = 0; i < N; i = i + 1) begin
            if (in[i] && !valid) begin
                out   = i[W-1:0];
                valid = 1'b1;
            end
        end
    end

endmodule
```

### 2. Warp Scheduler 模块 (`warp_scheduler.v`)

```verilog
// warp_scheduler.v
module warp_scheduler #(
    parameter NUM_WARPS = 32,
    parameter WARP_ID_W = $clog2(NUM_WARPS)
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    input  wire [NUM_WARPS-1:0]  warp_valid,
    input  wire [NUM_WARPS-1:0]  warp_ready,
    
    input  wire                  stall,
    output reg  [WARP_ID_W-1:0]  warp_id,
    output reg                   schedule_valid
);

    wire [NUM_WARPS-1:0] eligible_warps;
    wire [WARP_ID_W-1:0] selected_id;
    wire                 has_selection;
    
    assign eligible_warps = warp_valid & warp_ready;
    
    priority_encoder #(
        .N(NUM_WARPS),
        .W(WARP_ID_W)
    ) u_prio_enc (
        .in(eligible_warps),
        .out(selected_id),
        .valid(has_selection)
    );
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            warp_id        <= {WARP_ID_W{1'b0}};
            schedule_valid <= 1'b0;
        end else begin
            if (!stall) begin
                warp_id        <= selected_id;
                schedule_valid <= has_selection;
            end
        end
    end

endmodule
```

### 3. Verilator C++ Testbench (`tb_warp_scheduler.cpp`)

```cpp
// tb_warp_scheduler.cpp
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <iostream>
#include <bitset>
#include <cassert>
#include "Vwarp_scheduler.h"

#define NUM_WARPS 32

vluint64_t main_time = 0;

double sc_time_stamp() {
    return main_time;
}

void tick(Vwarp_scheduler* top, VerilatedVcdC* tfp) {
    top->clk = 0;
    top->eval();
    if (tfp) tfp->dump(main_time++);
    
    top->clk = 1;
    top->eval();
    if (tfp) tfp->dump(main_time++);
}

void reset(Vwarp_scheduler* top, VerilatedVcdC* tfp) {
    top->rst_n = 0;
    top->stall = 0;
    top->warp_valid = 0;
    top->warp_ready = 0;
    
    for (int i = 0; i < 5; i++) {
        tick(top, tfp);
    }
    top->rst_n = 1;
    tick(top, tfp);
}

// 辅助函数：设置特定warp为valid和ready
void set_warp(Vwarp_scheduler* top, int warp_id, bool valid, bool ready) {
    if (valid) {
        top->warp_valid |= (1 << warp_id);
    } else {
        top->warp_valid &= ~(1 << warp_id);
    }
    
    if (ready) {
        top->warp_ready |= (1 << warp_id);
    } else {
        top->warp_ready &= ~(1 << warp_id);
    }
}

void clear_all(Vwarp_scheduler* top) {
    top->warp_valid = 0;
    top->warp_ready = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    
    Vwarp_scheduler* top = new Vwarp_scheduler;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    
    top->trace(tfp, 99);
    tfp->open("waveform.vcd");
    
    std::cout << "========================================" << std::endl;
    std::cout << "  Warp Scheduler Testbench (Verilator)  " << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Test 1: Reset
    std::cout << "\n[Test 1] Reset Check..." << std::endl;
    reset(top, tfp);
    assert(top->schedule_valid == 0);
    std::cout << "  [PASS] Reset OK" << std::endl;
    
    // Test 2: Single Warp Ready
    std::cout << "\n[Test 2] Single Warp (ID=5)..." << std::endl;
    clear_all(top);
    set_warp(top, 5, true, true);
    tick(top, tfp);
    assert(top->schedule_valid == 1);
    assert(top->warp_id == 5);
    std::cout << "  [PASS] Selected Warp ID = " << (int)top->warp_id << std::endl;
    
    // Test 3: Multiple Warps - Priority Check (lower ID wins)
    std::cout << "\n[Test 3] Multiple Warps Priority..." << std::endl;
    clear_all(top);
    set_warp(top, 10, true, true);  // ID 10
    set_warp(top, 5, true, true);   // ID 5 (lower, should win)
    set_warp(top, 20, true, true);  // ID 20
    tick(top, tfp);
    assert(top->schedule_valid == 1);
    assert(top->warp_id == 5);  // 最低ID优先
    std::cout << "  [PASS] Selected lowest ID = " << (int)top->warp_id << std::endl;
    
    // Test 4: Only Valid but not Ready
    std::cout << "\n[Test 4] Valid but not Ready..." << std::endl;
    clear_all(top);
    set_warp(top, 3, true, false);  // Valid but not ready
    tick(top, tfp);
    assert(top->schedule_valid == 0);
    std::cout << "  [PASS] No valid selection when not ready" << std::endl;
    
    // Test 5: Stall Functionality
    std::cout << "\n[Test 5] Stall Functionality..." << std::endl;
    clear_all(top);
    set_warp(top, 7, true, true);
    tick(top, tfp);  // 第一次选择ID=7
    assert(top->warp_id == 7);
    
    // 启用stall，改变输入
    top->stall = 1;
    set_warp(top, 2, true, true);  // 添加更高优先级的warp
    tick(top, tfp);  // 应该保持ID=7
    assert(top->warp_id == 7);
    assert(top->schedule_valid == 1);
    std::cout << "  [PASS] Stall holds selection (ID=" << (int)top->warp_id << ")" << std::endl;
    
    // 解除stall，应该切换到ID=2
    top->stall = 0;
    tick(top, tfp);
    assert(top->warp_id == 2);
    std::cout << "  [PASS] After stall release, selected ID = " << (int)top->warp_id << std::endl;
    
    // Test 6: Dynamic Change
    std::cout << "\n[Test 6] Dynamic Warp Change..." << std::endl;
    clear_all(top);
    set_warp(top, 15, true, true);
    tick(top, tfp);
    assert(top->warp_id == 15);
    
    // 移除warp 15，添加warp 8
    set_warp(top, 15, false, false);
    set_warp(top, 8, true, true);
    tick(top, tfp);
    assert(top->warp_id == 8);
    std::cout << "  [PASS] Dynamic change to ID = " << (int)top->warp_id << std::endl;
    
    // Test 7: All Warps
    std::cout << "\n[Test 7] All 32 Warps..." << std::endl;
    clear_all(top);
    top->warp_valid = 0xFFFFFFFF;
    top->warp_ready = 0xFFFFFFFF;
    tick(top, tfp);
    assert(top->warp_id == 0);  // 最低ID
    std::cout << "  [PASS] All warps ready, selected ID = " << (int)top->warp_id << std::endl;
    
    // Test 8: No Valid Warp
    std::cout << "\n[Test 8] No Valid Warp..." << std::endl;
    clear_all(top);
    tick(top, tfp);
    assert(top->schedule_valid == 0);
    std::cout << "  [PASS] No valid selection when empty" << std::endl;
    
    // Test 9: Random Pattern
    std::cout << "\n[Test 9] Random Pattern..." << std::endl;
    clear_all(top);
    // 设置warp 3, 7, 12, 25
    set_warp(top, 3, true, true);
    set_warp(top, 7, true, true);
    set_warp(top, 12, true, true);
    set_warp(top, 25, true, true);
    tick(top, tfp);
    assert(top->warp_id == 3);
    std::cout << "  [PASS] Random pattern, selected lowest ID = " << (int)top->warp_id << std::endl;
    
    // 移除3，检查是否选7
    set_warp(top, 3, false, false);
    tick(top, tfp);
    assert(top->warp_id == 7);
    std::cout << "  [PASS] After remove 3, selected ID = " << (int)top->warp_id << std::endl;
    
    // 多周期stall测试
    std::cout << "\n[Test 10] Multi-cycle Stall..." << std::endl;
    clear_all(top);
    set_warp(top, 9, true, true);
    tick(top, tfp);
    uint8_t saved_id = top->warp_id;
    
    top->stall = 1;
    for (int i = 0; i < 10; i++) {
        set_warp(top, i, true, true);  // 不断添加更高优先级warp
        tick(top, tfp);
        assert(top->warp_id == saved_id);
        assert(top->schedule_valid == 1);
    }
    std::cout << "  [PASS] 10-cycle stall maintained ID = " << (int)saved_id << std::endl;
    
    top->stall = 0;
    tick(top, tfp);
    assert(top->warp_id == 0);  // 现在0是最高优先级
    std::cout << "  [PASS] After long stall, selected ID = " << (int)top->warp_id << std::endl;
    
    // 完成
    tick(top, tfp);
    tick(top, tfp);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  All Tests PASSED! (" << 10 << "/10)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    tfp->close();
    delete tfp;
    delete top;
    
    return 0;
}
```

### 4. Makefile

```makefile
# Makefile for Verilator Simulation

# Verilator flags
VERILATOR = verilator
VERILATOR_FLAGS = -Wall --trace --cc --exe --build -j 0

# Source files
VERILOG_SRC = priority_encoder.v warp_scheduler.v
CPP_SRC = tb_warp_scheduler.cpp

# Output
TARGET = Vwarp_scheduler

.PHONY: all clean run wave

all: $(TARGET)

$(TARGET): $(VERILOG_SRC) $(CPP_SRC)
	$(VERILATOR) $(VERILATOR_FLAGS) $(VERILOG_SRC) $(CPP_SRC) --top-module warp_scheduler

run: $(TARGET)
	./obj_dir/$(TARGET)

wave: run
	gtkwave waveform.vcd &

clean:
	rm -rf obj_dir waveform.vcd
```

---

## 运行步骤

```bash
# 1. 创建目录并放入上述4个文件
mkdir warp_scheduler_test && cd warp_scheduler_test

# 2. 编译
make

# 3. 运行测试
make run

# 4. 查看波形（需要安装GTKWave）
make wave
```

---

## 预期输出

```
========================================
  Warp Scheduler Testbench (Verilator)  
========================================

[Test 1] Reset Check...
  [PASS] Reset OK

[Test 2] Single Warp (ID=5)...
  [PASS] Selected Warp ID = 5

[Test 3] Multiple Warps Priority...
  [PASS] Selected lowest ID = 5

[Test 4] Valid but not Ready...
  [PASS] No valid selection when not ready

[Test 5] Stall Functionality...
  [PASS] Stall holds selection (ID=7)
  [PASS] After stall release, selected ID = 2

[Test 6] Dynamic Warp Change...
  [PASS] Dynamic change to ID = 8

[Test 7] All 32 Warps...
  [PASS] All warps ready, selected ID = 0

[Test 8] No Valid Warp...
  [PASS] No valid selection when empty

[Test 9] Random Pattern...
  [PASS] Random pattern, selected lowest ID = 3
  [PASS] After remove 3, selected ID = 7

[Test 10] Multi-cycle Stall...
  [PASS] 10-cycle stall maintained ID = 9
  [PASS] After long stall, selected ID = 0

========================================
  All Tests PASSED! (10/10)
========================================
```

这个testbench覆盖了：
- ✅ 复位功能
- ✅ 单Warp调度
- ✅ 优先级编码（低ID优先）
- ✅ Valid/Ready条件检查
- ✅ Stall保持功能
- ✅ 动态Warp变化
- ✅ 全32 Warp场景
- ✅ 空场景处理
- ✅ 多周期Stall