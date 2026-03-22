这是一个 GPU 全局网格/CTA 调度器（Global Grid/CTA Scheduler）的 Verilog 实现。下面我为你提供完整的架构分析、详细说明以及基于 **Verilator** 的 C++ Testbench 实现。

---

## 1. 架构总览

该模块是 GPU 的 **Host-Device 接口层**核心组件，负责将主机下发的 Kernel Grid 拆解为独立的 CTA（Cooperative Thread Array，线程块），并以 Round-Robin 方式分发到多个 GPC（Graphics Processing Cluster）。

```
┌─────────────────┐         ┌──────────────────────────┐         ┌─────────┐
│   Host (PCIe)   │◄───────►│  giga_thread_scheduler   │◄───────►│  GPC 0  │
│  Doorbell Reg   │         │  (This Module)           │         │  GPC 1  │
│  Grid Launch    │         │                          │         │  GPC 2  │
└─────────────────┘         └──────────────────────────┘         │  GPC 3  │
                                        │                         └─────────┘
                                        ▼
                              ┌─────────────────┐
                              │  Completion IRQ │
                              │  (Doorbell Back)│
                              └─────────────────┘
```

### 状态机（State Machine）
```
IDLE ─────► SETUP ─────► DISTRIBUTE ─────► WAIT_COMPLETE ─────► WRITE_DOORBELL ─────► INTERRUPT ─────► CLEANUP ─────► (IDLE)
   ▲                                                                                                            │
   └────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 端口说明

| 信号组 | 信号名 | 方向 | 位宽 | 描述 |
|--------|--------|------|------|------|
| **System** | `clk` | Input | 1 | 全局时钟 |
| | `rst_n` | Input | 1 | 异步复位（低有效） |
| **Host Command** | `host_grid_base_addr` | Input | 64 | Kernel 代码入口地址 |
| | `host_grid_dim_x/y/z` | Input | 16 | Grid 的 3D 维度（CTA 总数 = X×Y×Z） |
| | `host_kernel_args` | Input | 32 | Kernel 参数指针 |
| | `host_launch_valid` | Input | 1 | Doorbell 触发信号（脉冲） |
| | `host_launch_ready` | Output | 1 | 调度器空闲，可接收新 Grid |
| **GPC Dispatch** | `gpc_entry_pc` | Output | 64 | 分发给 GPC 的入口 PC |
| | `gpc_block_x/y/z` | Output | 16 | CTA 的 3D 索引 |
| | `gpc_kernel_args` | Output | 32 | Kernel 参数透传 |
| | `gpc_target_id` | Output | $clog2(NUM_GPC) | 目标 GPC ID（Round-Robin 选择） |
| | `gpc_dispatch_valid` | Output | 1 | 分发有效信号 |
| | `gpc_dispatch_ready` | Input | NUM_GPC | 各 GPC 的就绪信号（独热或 one-hot ready） |
| **GPC Completion** | `gpc_done_valid` | Input | NUM_GPC | 各 GPC 完成脉冲（Per-GPC done） |
| | `gpc_done_ready` | Output | NUM_GPC | 始终为 1，表示永远可接收完成信号 |
| **Host Interrupt** | `completion_irq` | Output | 1 | Grid 完成中断脉冲 |
| | `completion_status` | Output | 32 | 完成状态码（0x1=Success） |
| **Debug** | `scheduler_state` | Output | 3 | 当前状态机状态 |
| | `ctas_remaining` | Output | 10 | 剩余待分发 CTA 数 |
| | `ctas_dispatched` | Output | 10 | 已分发 CTA 数 |
| | `ctas_completed` | Output | 10 | 已完成 CTA 数 |

---

## 3. 模块内部结构

### 3.1 寄存器组
- **Grid Config Regs**：在 `SETUP` 阶段捕获主机下发的网格配置
- **CTA Counters**：3D 计数器 `(x, y, z)` 用于生成 CTA 全局索引
- **GPC Outstanding Trackers**：`gpc_cta_count[i]` 记录每个 GPC 正在执行的 CTA 数量（用于高级流控，当前版本用于调试）

### 3.2 调度算法
- **Round-Robin**：`next_gpc_id` 循环递增，确保负载均衡
- **3D 遍历顺序**：X 优先，其次是 Y，最后是 Z（符合 CUDA/Metal 线性化惯例）

---

## 4. 数据流说明

### 阶段 1：Grid Launch（IDLE → SETUP）
1. 主机配置 `host_grid_*` 信号
2. 拉高 `host_launch_valid`（Doorbell）
3. 模块捕获配置，计算 `total_ctas = X × Y × Z`，进入 `DISTRIBUTE`

### 阶段 2：CTA Distribution（DISTRIBUTE）
```
while (dispatched_count < total_ctas):
    wait (gpc_dispatch_ready[next_gpc_id] == 1)
    output (gpc_block_x/y/z, gpc_entry_pc) to GPC
    increment 3D counters (x->y->z)
    dispatched_count++
    next_gpc_id = (next_gpc_id + 1) % NUM_GPC
```

### 阶段 3：Wait for Completion（WAIT_COMPLETE）
- 监听 `gpc_done_valid[i]` 脉冲
- 每个脉冲使 `completed_count++`
- 当 `completed_count == total_ctas`，进入 `WRITE_DOORBELL`

### 阶段 4：Completion Doorbell（INTERRUPT）
- 向主机发送 `completion_irq` 脉冲
- 写状态码 `completion_status = 0x1`

---

## 5. 功能说明

| 功能 | 说明 |
|------|------|
| **Grid 管理** | 支持最大 1024 个 CTA，3D 网格维度各 16-bit（最大 65535） |
| **动态分发** | 每个周期可向一个就绪的 GPC 分发一个 CTA |
| **流控机制** | 通过 `gpc_dispatch_ready` 反压，防止 GPC buffer 溢出 |
| **完成追踪** | 精确计数每个 GPC 返回的 done 信号，确保无丢失 |
| **中断机制** | Grid 全部完成后产生单周期脉冲中断，可配置为 MSI/MSI-X |
| **状态可见性** | 提供实时计数器用于调试和性能分析 |

---

## 6. 验证说明（Verification Plan）

### 6.1 必验功能清单

| ID | 验证项 | 优先级 | 验证方法 |
|----|--------|--------|----------|
| V1 | 复位后状态机回到 IDLE，信号初始化正确 | P0 | 直接观察 |
| V2 | 单次 Grid Launch（1 CTA）完整流程 | P0 | 时序检查 |
| V3 | 大 Grid（1024 CTAs）分发到 4 个 GPC | P0 | 统计 Round-Robin 均匀性 |
| V4 | GPC ready 反压测试（随机置低 ready） | P0 | 延迟注入 |
| V5 | 3D 计数器边界检查（X/Y/Z 维度边界） | P1 | Corner Case |
| V6 | 完成信号乱序到达（Out-of-order completion） | P1 | 随机延迟 |
| V7 | 连续两个 Grid 背靠背启动（Back-to-back） | P1 | 状态机清理检查 |
| V8 | 中断脉冲宽度和状态码正确性 | P2 | 协议检查 |

### 6.2 关键断言（Assertions）
```verilog
// 1. 分发计数不超过总数
assert (dispatched_count <= total_ctas);

// 2. 完成计数不超过分发计数
assert (completed_count <= dispatched_count);

// 3. INTERRUPT 状态只持续一个周期
assert (state == INTERRUPT |-> ##1 state == CLEANUP);

// 4. 分发时 GPC ID 有效
assert (gpc_dispatch_valid |-> gpc_target_id < NUM_GPC);
```

---

## 7. Verilator Testbench（C++）

下面提供完整的、可直接编译的 C++ Testbench。该测试平台包含：
- 时钟/复位生成
- 主机事务生成器（随机 Grid 配置）
- GPC 行为模型（带随机延迟的完成响应）
- 记分板（Scoreboard）验证分发与完成计数一致性



## 8. 验证策略详解

### 8.1 主机激励生成器（Driver）
- **Scenario 1**：标准 3D Grid（3×2×2=12 CTAs），验证 3D 线性化索引
- **Scenario 2**：1D Grid（1024×1×1），最大容量测试
- **Scenario 3**：0 延迟背靠背启动，验证状态机清理逻辑

### 8.2 GPC 行为模型（BFM）
- **随机延迟**：每个 CTA 处理时间随机（1-10 周期），模拟真实计算负载
- **反压注入**：随机拉低 `gpc_dispatch_ready`，验证调度器停顿逻辑

### 8.3 记分板（Scoreboard）检查点
1. **分发检查**：验证 `(x,y,z)` 坐标按 X→Y→Z 顺序递增
2. **负载均衡**：验证每个 GPC 接收的 CTA 数量差不超过 1（Round-Robin 验证）
3. **完成完整性**：验证 `completed_count == dispatched_count == total_ctas`
4. **中断时序**：验证 `completion_irq` 只在 `INTERRUPT` 状态为高，且持续单周期

### 8.4 覆盖率目标
- 状态机所有状态转换至少触发一次
- 每个 GPC 都至少接收过 CTA 和发送过完成信号
- Corner case：`total_ctas = 1`（最小 Grid）

---

这个 Testbench 提供了完整的验证框架，你可以根据需要添加更多测试案例（如错误注入、复位恢复测试等）。需要我详细解释某个特定验证场景的实现细节吗？