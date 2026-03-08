这是一个 **Scoreboard（记分板）** 模块，用于 **GPU/CPU流水线中的数据冒险检测**。

---

## 核心原理

### 什么是 Scoreboard？
Scoreboard 是 **Tomasulo算法** 和 **动态调度** 中的核心组件，用于跟踪寄存器的 **"忙/闲"状态**，检测 **RAW（写后读）** 和 **WAW（写后写）** 冒险。

### 冒险类型

| 冒险类型 | 含义 | 检测条件 |
|---------|------|---------|
| **RAW** (Read After Write) | 指令要读的数据还没写完 | 源寄存器在busy_table中为1 |
| **WAW** (Write After Write) | 两条指令要写同一个寄存器 | 目的寄存器在busy_table中为1 |

### 数据结构

```
Busy Table: 32 Warps × 64 Registers = 2048 bits
            ┌─────────────────────────────────────┐
Warp 0      │ R0 │ R1 │ R2 │ ... │ R63 │  (64 bits)
            ├─────────────────────────────────────┤
Warp 1      │ R0 │ R1 │ R2 │ ... │ R63 │  (64 bits)
            ├─────────────────────────────────────┤
    ...     │              ...                    │
            ├─────────────────────────────────────┤
Warp 31     │ R0 │ R1 │ R2 │ ... │ R63 │  (64 bits)
            └─────────────────────────────────────┘
            
Index = warp_id × 64 + reg_addr  (0 ~ 2047)
```

---

## 模块信号详解

| 信号 | 方向 | 说明 |
|-----|------|------|
| `warp_id/rs0/rs1/rs2/rd` | Input | 当前检查的发射指令的Warp ID和寄存器地址 |
| `rd_wr_en` | Input | 该指令是否会写目的寄存器 |
| `stall_raw` | Output | 高电平表示检测到RAW冒险，需要阻塞 |
| `stall_waw` | Output | 高电平表示检测到WAW冒险，需要阻塞 |
| `wb_warp_id/wb_rd_addr/wb_valid` | Input | 写回阶段信号，用于清除busy位 |

### Bypass机制（关键优化）
```verilog
// 如果写回和发射在同一周期、同一Warp、同一寄存器，则不算冒险
!(wb_valid && wb_warp_id == warp_id && wb_rd_addr == rs0_addr)
```
这允许 **前递（Forwarding）**，避免不必要的阻塞。

---

## 工作流程

```
Cycle N:   指令发射
           ↓ 检查 busy_table[rs0], busy_table[rs1], busy_table[rd]
           ↓ 如果都为0，指令可以发射，设置 busy_table[rd] = 1
           ↓ 如果有1，stall_raw/stall_waw = 1，阻塞发射

Cycle N+M: 指令完成写回
           ↓ wb_valid = 1, 清除 busy_table[wb_rd] = 0
           ↓ 后续依赖该寄存器的指令可以发射
```


在 Scoreboard 模块中，这些信号代表 **指令的寄存器地址和 Warp ID**，用于数据冒险检测。让我详细解释：

---

## 信号含义

| 信号 | 位宽 | 含义 |
|-----|------|------|
| `warp_id` | 5位 (0-31) | **当前指令所属的 Warp ID** |
| `rs0_addr` | 6位 (0-63) | **源寄存器 0** (Source Register 0) |
| `rs1_addr` | 6位 (0-63) | **源寄存器 1** (Source Register 1) |
| `rs2_addr` | 6位 (0-63) | **源寄存器 2** (Source Register 2，可选) |
| `rd_addr` | 6位 (0-63) | **目的寄存器** (Destination Register) |

---

## 具体例子

### 指令：`ADD R5, R1, R2` (R5 = R1 + R2)

```
warp_id  = 3      // 这条指令属于 Warp 3
rs0_addr = 1      // 读 R1
rs1_addr = 2      // 读 R2
rs2_addr = 0      // 未使用（设为0）
rd_addr  = 5      // 写 R5
rd_wr_en = 1      // 确实要写入
```

### Scoreboard 检查：

1. **RAW 检查**：R1 或 R2 是否 busy？
   - `busy_table[3*64 + 1]` = busy_table[193] 
   - `busy_table[3*64 + 2]` = busy_table[194]
   - 如果任一=1，则 `stall_raw=1`

2. **WAW 检查**：R5 是否 busy？
   - `busy_table[3*64 + 5]` = busy_table[197]
   - 如果=1，则 `stall_waw=1`

3. **无冒险**：设置 R5 busy，`busy_table[197] = 1`

---

## 为什么需要 Warp ID？

GPU 有 **32个 Warp**，每个 Warp 有 **64个寄存器**。

```
物理索引 = warp_id × 64 + reg_addr

Warp 0: 索引 0-63
Warp 1: 索引 64-127
Warp 2: 索引 128-191
...
Warp 31: 索引 1984-2047
```

**不同 Warp 的同名寄存器是完全独立的！**

- Warp 0 的 R5 = `busy_table[5]`
- Warp 1 的 R5 = `busy_table[69]`  ← 不同位置

---

## 完整数据流

```
发射阶段 (Issue)
     │
     ▼
┌─────────────┐
│  Scoreboard │◄── warp_id, rs0, rs1, rs2, rd
│   检查冒险   │
└─────────────┘
     │
     ├──► stall_raw=1 ──► 阻塞，等待源寄存器就绪
     │
     ├──► stall_waw=1 ──► 阻塞，等待目的寄存器空闲
     │
     └──► 无冒险 ──► 设置 rd busy ──► 指令发射

执行阶段 (Execute) ──► 写回阶段 (Writeback)
                              │
                              ▼
                    ┌─────────────┐
                    │  wb_warp_id │
                    │  wb_rd_addr │──► 清除 busy_table
                    │   wb_valid  │
                    └─────────────┘
```

---

## 总结

| 信号 | 类比 CPU | 作用 |
|-----|---------|------|
| `warp_id` | 线程 ID / 核心 ID | 区分不同 Warp 的寄存器空间 |
| `rs0/rs1/rs2` | 源操作数寄存器 | 读数据，检查 RAW 冒险 |
| `rd` | 目的寄存器 | 写数据，检查 WAW 冒险，设置 busy |

这是 **SIMT/GPU 流水线** 的核心机制，确保同一条指令在不同 Warp 中并行执行时不会相互干扰。

