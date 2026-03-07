这段代码实现了一个**伪双端口 SRAM（Pseudo-Dual-Port SRAM）**，有两个访问端口，但共享同一个存储器阵列。让我详细解释：

---

## 整体架构

```
┌─────────────────────────────────────────┐
│           sram_2p (Pseudo-DP)           │
│                                         │
│   ┌─────────────┐    ┌─────────────┐   │
│   │   Port A    │    │   Port B    │   │
│   │  (R/W Port) │    │ (Read-Only) │   │
│   │             │    │             │   │
│   │  a_ce       │    │  b_ce       │   │
│   │  a_wr_en    │    │             │   │
│   │  a_addr     │    │  b_addr     │   │
│   │  a_wdata ───┼────┼──► b_rdata  │   │
│   │  a_rdata ◄──┘    │             │   │
│   └─────────────┘    └─────────────┘   │
│           │                    │        │
│           └────┐    ┌─────────┘        │
│                ▼    ▼                  │
│         ┌──────────────┐               │
│         │  mem[DEPTH]  │               │
│         │  (Shared     │               │
│         │   Storage)   │               │
│         └──────────────┘               │
│                                         │
└─────────────────────────────────────────┘
```

---

## 端口说明

| 端口 | 方向 | 功能 |
|------|------|------|
| `clk` | input | 时钟 |
| `rst_n` | input | 异步复位，低有效 |
| **Port A (R/W)** |
| `a_ce` | input | A 端口片选 |
| `a_wr_en` | input | A 端口写使能（1=写，0=读） |
| `a_addr` | input | A 端口地址 |
| `a_wdata` | input | A 端口写数据 |
| `a_rdata` | output | A 端口读数据 |
| **Port B (Read-Only)** |
| `b_ce` | input | B 端口片选 |
| `b_addr` | input | B 端口地址 |
| `b_rdata` | output | B 端口读数据 |

---

## 核心逻辑详解

### 1. Port A - 读写端口（Read-First 模式）

```verilog
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        a_rdata <= {DATA_W{1'b0}};
    end else if (a_ce) begin
        a_rdata <= mem[a_addr];     // 第一步：读旧数据
        if (a_wr_en) begin
            mem[a_addr] <= a_wdata; // 第二步：写新数据
        end
    end
end
```

**Read-First 行为**：
```
同一周期读写同一地址：
  读出的数据 = 旧数据（写入前的值）
  写入的数据 = 新数据（下一个周期才能读到）
```

**时序图**：
```
Cycle 1: A 写入 0xAAAA 到地址 0x10
         a_wr_en=1, a_wdata=0xAAAA
         a_rdata = mem[0x10] 的旧值（比如 0x0000）

Cycle 2: A 读取地址 0x10
         a_wr_en=0
         a_rdata = 0xAAAA（上一个周期写入的值）
```

---

### 2. Port B - 只读端口（带 Bypass 功能）

```verilog
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        b_rdata <= {DATA_W{1'b0}};
    end else if (b_ce) begin
        // Bypass 逻辑：检测冲突
        if (a_wr_en && a_ce && (a_addr == b_addr)) begin
            b_rdata <= a_wdata;     // 直接转发 A 的写数据
        end else begin
            b_rdata <= mem[b_addr]; // 正常读取存储器
        end
    end
end
```

**Bypass 功能**（关键特性）：
```
场景：A 写入地址 0x20，同时 B 读取地址 0x20

无 Bypass 时：
  B 读到的是 mem[0x20] 的旧值（因为 A 的写入要到周期结束才生效）
  
有 Bypass 时：
  B 直接拿到 a_wdata（新值），实现"写后立即可读"
```

**Bypass 条件**（3个条件同时满足）：
| 条件 | 含义 |
|------|------|
| `a_wr_en` | A 正在执行写操作 |
| `a_ce` | A 端口使能 |
| `a_addr == b_addr` | 两个端口访问同一地址 |

---

## 典型应用场景

### 场景 1：正常独立访问（无冲突）

```
Cycle 1: A 写入 0x10 到地址 0x00
Cycle 2: B 读取地址 0x00 → 得到 0x10（正常读取）
```

### 场景 2：同时访问不同地址（无冲突）

```
Cycle 1: A 写入 0xAAAA 到地址 0x00
         B 读取地址 0x10（不同地址）
         
结果：A 正常写入，B 正常读取（互不干扰）
```

### 场景 3：同时访问同一地址（Bypass 生效）⭐

```
Cycle 1: A 写入 0xBBBB 到地址 0x20
         B 读取地址 0x20（同一地址）
         
结果：
  - A 的 a_rdata = 旧值（Read-First）
  - B 的 b_rdata = 0xBBBB（Bypass 转发，立即看到新值）
```

**为什么需要 Bypass？**

在 GPU/AI 加速器中，经常需要：
- **生产者（Port A）** 写入数据
- **消费者（Port B）** 立即读取同一数据

Bypass 避免了"写后读延迟"，提高流水线效率。

---

## 与真双端口 SRAM 的区别

| 特性 | 真双端口 SRAM | 伪双端口 SRAM（本设计） |
|------|-------------|----------------------|
| 读写端口 | 两个独立 R/W | 一个 R/W + 一个 R/O |
| 面积 | 大（2套电路） | 小（1套存储器 + 旁路逻辑） |
| 同时写入 | 支持 | 不支持（Port B 只读） |
| 写后读延迟 | 1-2 周期 | Bypass 后 0 周期（对 Port B） |
| 应用场景 | 复杂多核系统 | GPU 共享内存、FIFO、简单缓冲 |

---

## 关键时序总结

```
操作类型                | 延迟        | 说明
-----------------------|------------|---------------------------
A 端口写后 A 端口读    | 1 周期      | Read-First，下个周期可见
A 端口写后 B 端口读    | 0 周期      | Bypass 直接转发
B 端口独立读取         | 1 周期      | 正常存储器访问
```

这个设计在面积和性能之间取得了平衡，是 GPU 共享内存（Shared Memory/L1 Data Cache）的典型实现方式。