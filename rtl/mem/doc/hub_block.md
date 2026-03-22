这是整个 AI 加速器/GPU 系统的 **顶层集线器（Hub Top Level）**，作为 **"中央神经系统"** 协调所有子系统：计算集群（GPC）、内存层次（L2+HBM）、数据传输（Copy Engine）和任务调度（GigaThread Scheduler）。

---

## 1. 核心功能定位

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Host CPU (PCIe)                            │
│         配置寄存器写入 ←──→ Doorbell 网格启动信号                     │
└─────────────────┬───────────────────────────────────────────────────┘
                  │
                  ▼ PCIe x16
┌─────────────────────────────────────────────────────────────────────┐
│                    HUB_BLOCK (中央集线器)                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │ GigaThread   │  │   Crossbar   │  │   Config     │              │
│  │ Scheduler    │  │     4×4      │  │   Registers  │              │
│  │ (任务调度)   │  │  (路由交换)   │  │  (控制/状态)  │              │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                 │                       │
│         │    ┌────────────┴─────────────────┘                       │
│         │    │                                                      │
│         │    ▼                                                      │
│         │  ┌──────────────┐                                         │
│         │  │   L2 Cache   │ ←── Copy Engine (DMA)                  │
│         │  │  512KB 8-way │ ←── HBM 控制器 (4通道)                  │
│         │  └──────┬───────┘                                         │
│         │         │                                                 │
│         └─────────┤ 中断聚合                                         │
│                   ▼                                                 │
│         ┌─────────────────┐                                        │
│         │  GPC 0 │ GPC 1 │ GPC 2 │ GPC 3 │  ← 计算集群              │
│         │ SM阵列 │ SM阵列 │ SM阵列 │ SM阵列 │     (4个)             │
│         └─────────────────┘                                        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**三大核心职责**：
1. **任务调度**：接收 Host 的 Doorbell，通过 GigaThread Scheduler 分发 Kernel 到 4 个 GPC
2. **内存统一**：通过 Crossbar 连接 GPC 到 L2，再下行到 HBM，同时旁路 DMA（Copy Engine）
3. **系统控制**：PCIe 配置空间管理 + 中断聚合 + 性能计数器

---

## 2. 详细架构框图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              hub_block                                         │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │                        Configuration & Control                           │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                   │  │
│  │  │   cfg_addr   │  │   doorbell   │  │   hub_ctrl   │                   │  │
│  │  │   cfg_wdata  │  │   doorbell   │  │   hub_int_   │                   │  │
│  │  │   cfg_wr/rd  │  │   _valid     │  │   mask       │                   │  │
│  │  │      │       │  │      │       │  │   total_     │                   │  │
│  │  │      ▼       │  │      ▼       │  │   cycles     │                   │  │
│  │  │ Config Regs   │  │ Launch Ctrl  │  │ Performance │                   │  │
│  │  │ (MMIO Space) │  │  (Grid Dim)  │  │  Counters   │                   │  │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬──────┘                   │  │
│  └─────────┼─────────────────┼─────────────────┼──────────────────────────┘  │
│            │                 │                 │                              │
│            ▼                 ▼                 │                              │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │                     GigaThread Scheduler (u_scheduler)                   │  │
│  │  ┌────────────────┐        ┌────────────────┐                           │  │
│  │  │ Grid Dimensions│        │ CTA Dispatch   │──► gpc_dispatch_id[1:0]   │  │
│  │  │ (X/Y/Z)        │        │ Arbiter        │──► gpc_dispatch_valid     │  │
│  │  │ Kernel Args    │        │                │◄── gpc_dispatch_ready[3:0] │  │
│  │  │ Entry PC       │        │ Done Tracking  │◄── gpc_done_valid[3:0]     │  │
│  │  └────────────────┘        └────────────────┘                           │  │
│  └──────────────────────────┬──────────────────────────────────────────┘  │
│                             │                                                │
│  ┌──────────────────────────▼──────────────────────────────────────────┐  │
│  │                        Crossbar 4×4 (u_crossbar)                     │  │
│  │                                                                    │  │
│  │     GPC0 ──┐                                                       │  │
│  │     GPC1 ──┼──► [Input FIFO + RR Arbiter] ──┐                     │  │
│  │     GPC2 ──┤                                │                     │  │
│  │     GPC3 ──┘                                ▼                     │  │
│  │                                        ┌─────────┐               │  │
│  │                                        │ Routing │──► To L2       │  │
│  │                                        │ Logic   │    Bank 0-3     │  │
│  │                                        └─────────┘               │  │
│  │                                                                    │  │
│  └──────────────────────────┬──────────────────────────────────────────┘  │
│                             │                                                │
│  ┌──────────────────────────▼──────────────────────────────────────────┐  │
│  │                        L2 Cache (u_l2)                               │  │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐       │  │
│  │  │ Tag Array      │  │ Data Array     │  │ MSHR (Miss     │       │  │
│  │  │ (8-way, 512set)│  │ (512KB)        │  │ Handling)      │       │  │
│  │  └────────────────┘  └────────────────┘  └────────────────┘       │  │
│  │                                                                    │  │
│  │  ◄── Copy Engine Snoop (ce_snoop_*)                               │  │
│  │  ◄── Flush/Invalidate Commands (from cfg)                         │  │
│  │                                                                    │  │
│  └──────────────┬───────────────────────┬──────────────────────────────┘  │
│                 │                       │                                    │
│                 ▼                       ▼                                    │
│  ┌───────────────────────┐   ┌───────────────────────┐                      │
│  │   HBM Controller      │   │   Copy Engine         │                      │
│  │   (4 Channels)        │   │   (H2D/D2H DMA)       │                      │
│  │   u_hbm (行为模型)     │   │   (外部实例化)         │                      │
│  └───────────────────────┘   └───────────────────────┘                      │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                      Interrupt Aggregation                            │  │
│  │  sched_completion_irq ──┐                                           │  │
│  │  gpc_done_valid ────────┼──► [IRQ Arbiter] ──► hub_irq ──► PCIe MSI  │  │
│  │  ecc_error ─────────────┘         irq_vector                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 关键数据流详解

### A. Kernel 启动控制流（Doorbell 机制）
```
Host Driver                    hub_block                    GPC
    │                            │                          │
    │  1. 配置 Kernel 参数       │                          │
    │ ────────────────────> cfg_*                          │
    │                            │                          │
    │  2. 写 Doorbell (启动)    │                          │
    │ ────────────────────> doorbell_valid                 │
    │                            │                          │
    │                            │ 3. Scheduler 分配 CTA    │
    │                            │ ─────────────────────>   │
    │                            │    gpc_dispatch_valid    │
    │                            │    gpc_grid_base_addr    │
    │                            │                          │
    │                            │<──────────────────────   │
    │                            │    gpc_dispatch_ready    │
    │                            │                          │
    │                            │ 4. 分发完成中断          │
    │  5. 收到 MSI 中断         │ ────────────────────> hub_irq
    │<───────────────────────────┘                          │
```

### B. 内存访问数据流（GPC → L2 → HBM）
```
GPC (计算单元)          Crossbar           L2 Cache         HBM
    │                    │                  │               │
    │ gpc_req_valid      │                  │               │
    │ gpc_req_addr ─────►│                  │               │
    │                    │ Route to Bank    │               │
    │                    │ ────────────────►│ l2_req_valid  │
    │                    │                  │               │
    │                    │                  │ Cache Lookup  │
    │                    │                  │               │
    │                    │                  │ MISS ────────►│ hbm_req_valid
    │                    │                  │               │
    │                    │                  │◄──────────────│ hbm_resp_valid
    │                    │                  │ Refill        │
    │                    │◄─────────────────│ l2_resp_valid │
    │ gpc_resp_ready ◄───│                  │               │
    │                    │                  │               │
```

### C. DMA 数据流（Copy Engine 旁路）
```
Host Memory (PCIe)      Copy Engine          L2 Cache (Snoop)
        │                    │                    │
        │ pcie_rd_data       │                    │
        │ ──────────────────►│                    │
        │                    │ snoop_valid        │
        │                    │ snoop_addr ───────►│
        │                    │                    │ Check Hit
        │                    │◄───────────────────│ snoop_hit
        │                    │                    │
        │                    │ l2_wr_en ────────►│
        │                    │ l2_wdata ────────►│ Update Data
        │                    │                    │
```

---

## 4. 子模块连接关系

| 实例名 | 模块类型 | 连接对象 | 功能 |
|--------|----------|----------|------|
| `u_scheduler` | GigaThread Scheduler | 上游：Doorbell/Config<br>下游：4 个 GPC | CTA 任务分发与完成追踪 |
| `u_crossbar` | Crossbar 4×4 | 上游：4 GPC<br>下游：L2 Cache | 请求路由与仲裁 |
| `u_l2` | L2 Cache | 上游：Crossbar<br>下游：HBM<br>旁路：Copy Engine | 缓存存储与一致性 |
| *(外部)* | Copy Engine | 上游：PCIe<br>下游：L2 (Snoop) | Host/Device DMA |

---

## 5. 关键机制说明

### 1. Doorbell 网格启动（GPU-style Computing）
```verilog
// Host 通过写 Doorbell 触发 Kernel 启动
input  wire [63:0] doorbell_addr;   // Kernel 入口 PC
input  wire [31:0] doorbell_data;   // Kernel 参数/网格配置
input  wire        doorbell_valid;  // 启动脉冲

// Scheduler 将网格维度 (8×8×1) 分解为 CTA 分发到 GPC
.gpc_grid_dim_x(16'd8),
.gpc_grid_dim_y(16'd8),
.gpc_grid_dim_z(16'd1),
```

**机制**：类似 NVIDIA CUDA 的 `<<<grid, block>>>` 启动模型，硬件自动将 2D/3D 网格分解为协作线程阵列（CTA）分发到计算集群。

### 2. 配置寄存器映射（MMIO）
```verilog
case (cfg_addr[7:0])
    8'h00: hub_ctrl_reg;      // Bit0: Flush, Bit1: Invalidate
    8'h04: hub_int_mask;      // 中断屏蔽
    8'h10: total_cycles;      // 性能计数：总周期
    8'h20: perf_l2_hits;      // L2 命中统计（来自 L2 模块）
    8'h24: perf_l2_misses;    // L2 缺失统计
```

**用途**：驱动软件通过 PCIe BAR 空间读写这些寄存器，控制缓存刷新、使能中断、读取性能指标。

### 3. 中断聚合（Interrupt Aggregation）
```verilog
if (sched_completion_irq && !hub_int_mask[0]) begin
    hub_irq <= 1'b1;      // MSI 中断到 Host
    irq_vector <= 8'h01;  // Vector 0: 调度完成
end else if (|gpc_done_valid && !hub_int_mask[1]) begin
    hub_irq <= 1'b1;
    irq_vector <= 8'h02;  // Vector 1: GPC 计算完成
end
```

**机制**：支持多源中断（调度完成、GPC 完成、未来可扩展 ECC 错误），通过掩码寄存器独立控制使能。

### 4. 性能监控（内置 PMU）
- `total_cycles`：64 位全局周期计数器（用于计算 IPC/BW）
- `perf_l2_hits/misses`：直连 L2 模块的命中/缺失统计，用于分析缓存效率

---

## 6. 系统启动流程示例

```c
// 1. Host 驱动初始化
write_cfg(0x04, 0x0);          // 清除中断屏蔽
write_cfg(0x00, 0x3);          // Flush + Invalidate L2

// 2. 配置 Kernel
uint64_t kernel_pc = 0x10000;
write_cfg(DOORBELL_ADDR, kernel_pc);
write_cfg(DOORBELL_DATA, kernel_args);

// 3. 启动（触发 doorbell_valid）
write_pci_doorbell();          // 硬件置位 doorbell_valid

// 4. 等待中断（或轮询）
while (!read_irq_status());    // 等待 hub_irq 触发 MSI

// 5. 读取性能
uint32_t hits = read_cfg(0x20);
float hit_rate = (float)hits / (hits + read_cfg(0x24));
```

---

## 总结

`hub_block` 是 **完整的 AI 加速器 SoC 顶层模块**，集成了：
- **计算调度**（GigaThread）：多 GPC 任务分发
- **内存网络**（Crossbar + L2 + HBM）：高带宽数据供给
- **数据传输**（Copy Engine 接口）：Host 与 Device 间 DMA
- **系统管理**（PCIe 配置 + 中断）：软件可编程控制

它实现了从 **Host 命令** → **硬件执行** → **结果返回** 的完整闭环，是连接软件栈（CUDA/ROCm 风格）与硬件微架构的桥梁。