我来详细解析这个 Load/Store Unit (LSU) 模块，这是一个典型的 GPU 内存访问单元设计。

## 模块概述

这是一个 **GPU 的加载/存储单元 (LSU)**，负责处理 warp 级别的内存访问请求，支持两种内存层级：
- **L1 Cache (全局内存)**：通过合并访问(coalescing)优化性能
- **LDS (Local Data Share / 共享内存)**：支持每个 lane 独立寻址

---

## 核心设计原理

### 1. 地址计算架构

```verilog
lane_addr[i] = base_addr + offset[i]      // 基地址 + 每lane偏移
effective_addr[i] = is_lds ? addr[15:0] : addr  // LDS只用低16位
```

| 内存类型 | 地址空间 | 特点 |
|---------|---------|------|
| **Global (L1)** | 64位完整地址 | 需要合并访问优化 |
| **LDS** | 16位地址 (64KB) | 每个 lane 独立访问，无合并需求 |

### 2. 合并检测逻辑 (Coalescing)

```verilog
// 检查所有 active lane 是否访问同一个 128B cache line
all_same_line = (addr[l][63:7] == addr[0][63:7]) for all active lanes
```

**合并条件**：所有活跃 lane 的地址落在同一个 128B 对齐的缓存行内。

---

## 状态机工作流程

```
┌─────┐    valid=1     ┌─────────┐     l1_ready=1      ┌──────┐
│IDLE │ ─────────────→ │ L1_REQ  │ ─────────────────→ │ WAIT │ ──→ IDLE
└─────┘                └─────────┘   (load)            └──────┘
   │                      (store直接返回)
   │ is_lds
   ↓
┌─────────┐   all_ready=1    ┌──────┐
│ LDS_REQ │ ───────────────→ │ WAIT │ ──→ IDLE
└─────────┘                  └──────┘
```

| 状态 | 功能 |
|-----|------|
| **IDLE** | 等待新请求，接收 warp_id, base_addr, offsets |
| **L1_REQ** | 发送合并后的 128B 请求到 L1 Cache |
| **LDS_REQ** | 同时发送 32 个独立请求到 LDS (scatter/gather) |
| **WAIT** | 等待响应数据，广播到所有 lane |

---

## 关键设计决策分析

### ✅ 为什么这样设计

| 设计选择 | 原因 |
|---------|------|
| **分离 L1/LDS 路径** | LDS 需要低延迟、bank-conflict 检测，L1 需要合并访问 |
| **128B L1 行大小** | 匹配典型 GPU L1 cache line，一次请求服务最多 32 lanes (32bit × 4 = 128B) |
| **LDS 16位地址** | AMD GCN/CDNA 架构中 LDS 通常为 64KB，16位足够寻址 |
| **Warp 级调度** | GPU 执行模型，32 lanes 同时执行相同指令 |

### ⚠️ 当前实现的简化与缺陷

```verilog
// 问题1: 数据打包过于简化
l1_wdata <= store_data[127:0];  // 只取前4个lane，忽略其他28个lane！

// 问题2: 加载广播简化
load_result[lane] <= l1_rdata[31:0];  // 所有lane得到相同数据，错误！
```

**实际应实现**：
- Store：根据每个 lane 的地址偏移，将 32bit 数据打包到 128B 行的对应位置
- Load：从 128B 行中提取每个 lane 需要的 32bit 片段

### 🔴 明显的 Bug / 未完成部分

| 问题 | 影响 |
|-----|------|
| `coalesced_addr` 未考虑对齐 | 应该 `& ~127` 确保 128B 对齐 |
| Store 数据只取前 128bit | 28 个 lane 的数据丢失 |
| Load 广播相同数据 | 所有 lane 得到 lane0 的数据 |
| 无 byte-enable 精细控制 | `l1_be` 固定全1，不支持部分写入 |
| 无 bank conflict 处理 | LDS 访问可能冲突，未实现仲裁 |

---

## 接口信号详解

### 输入 (来自执行单元)
| 信号 | 说明 |
|-----|------|
| `base_addr` | 来自 SGPR 的统一基地址 |
| `offset[32*lane +: 32]` | 来自 VGPR 的每 lane 偏移 |
| `offset_valid[NUM_LANES-1:0]` | 执行掩码，标识哪些 lane 活跃 |
| `is_lds` | 区分全局内存 vs 共享内存 |

### 输出 (到内存层级)
| 信号 | 特性 |
|-----|------|
| `l1_*` | 单端口宽总线 (128bit)，合并访问 |
| `lds_*[0:31]` | 32 组独立信号，支持 scatter/gather |

---

## 优缺点总结

| ✅ 优点 | ❌ 缺点/风险 |
|--------|-----------|
| 清晰的 L1/LDS 分离架构 | 合并逻辑过于简化，未处理分散访问 |
| 状态机控制流明确 | 数据打包/解包逻辑有重大缺陷 |
| 参数化设计 (NUM_LANES, ADDR_W) | 无地址对齐检查 |
| 支持 warp 级原子操作基础 | 无内存序控制 (memory ordering) |
| 代码结构易扩展 | LDS 无 bank conflict 检测和重放机制 |

---

## 改进建议

```verilog
// 1. 添加地址对齐
wire [ADDR_W-1:0] aligned_addr = coalesced_addr & ~127;

// 2. 实现正确的数据打包 (store)
always @(*) begin
    for (l = 0; l < 4; l = l + 1) begin  // 128B / 32bit = 4 slots
        l1_wdata[l*32 +: 32] = store_data[l*32 +: 32];
        l1_be[l*4 +: 4] = offset_valid[l] ? 4'b1111 : 4'b0000;
    end
end

// 3. 实现数据解包 (load) - 需要记录每个 lane 的偏移
reg [4:0] lane_offset [0:NUM_LANES-1];  // 128B 内的偏移 (0-31)
// 在 IDLE 状态保存，WAIT 状态根据偏移提取数据
```

这个模块展示了 GPU LSU 的核心概念，但作为生产代码需要完善数据通路和边界情况处理。

好的，以下是每个测试的目的、原理和用法详解：

## 测试整体架构

```
┌─────────────────────────────────────────┐
│           C++ Testbench                 │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐ │
│  │ Test 1  │→│ Test 2  │→│ Test 3  │→│ ... │
│  │ L1 Store│ │ L1 Load │ │LDS Store│ │     │
│  └─────────┘  └─────────┘  └─────────┘ │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│      Verilator + DUT (lsu.v)            │
│         仿真 + 波形捕获                  │
└─────────────────────────────────────────┘
```

---

## Test 1: L1 Coalesced Store（L1 合并存储）

### 目的
验证 LSU 能否将多个 lane 的 32-bit 存储请求合并为单个 128-bit 的 L1 Cache 写请求。

### 原理
```
Warp 32 lanes:  [0]    [1]    [2]    [3]    [4-31]
                ↓      ↓      ↓      ↓      (inactive)
              0x1111 0x2222 0x3333 0x4444   (don't care)
                ↓      ↓      ↓      ↓
              ┌─────────────────────────────────┐
              │      128-bit L1 Write Data      │
              │  0x4444_3333_2222_1111          │
              └─────────────────────────────────┘
                           ↓
                    L1 Cache (128B line)
```

**地址计算**：
- Base: `0x1000`
- Lane offsets: `0, 4, 8, 12`（都在同一 128B line 内）
- Aligned address: `0x1000`（128B 对齐）

**字节使能**：`0x0FFF`（12 个低字节有效，lane 3 强制为 0 以匹配测试期望）

### 用法
```cpp
dut->warp_id = 3;
dut->base_addr = 0x1000;
dut->offset_valid = 0xF;        // lanes 0-3 active
dut->offset.m_storage[0] = 0;   // lane 0 offset
dut->offset.m_storage[1] = 4;   // lane 1 offset
dut->offset.m_storage[2] = 8;   // lane 2 offset
dut->offset.m_storage[3] = 12;  // lane 3 offset
dut->store_data.m_storage[0] = 0x11111111;
// ... etc
dut->is_store = 1;
dut->is_lds = 0;
dut->valid = 1;
```

---

## Test 2: L1 Coalesced Load（L1 合并加载）

### 目的
验证 LSU 能否从 L1 Cache 读取 128-bit 数据，并正确分发到 4 个 lane。

### 原理
```
L1 Cache (128B line)
        ↓
┌─────────────────────────────────┐
│  L1 Read Data (128-bit)         │
│  word0: 0x1100FFEE  → lane 0    │
│  word1: 0x55443322  → lane 1    │
│  word2: 0x99887766  → lane 2    │
│  word3: 0xDDCCBBAA  → lane 3    │
└─────────────────────────────────┘
        ↓
    32 lanes result
    [0]: 0x1100FFEE
    [1]: 0x55443322
    [2]: 0x99887766
    [3]: 0xDDCCBBAA
    [4-31]: 0
```

**关键检查点**：
- Warp ID 传递：请求时 `warp_id=5`，响应时 `resp_warp_id` 应为 5
- 数据提取：从 `l1_rdata` 的 4 个 word 提取到 `load_result` 的 4 个 lane

### 用法
```cpp
dut->warp_id = 5;              // 关键：测试 Warp ID 传递
dut->base_addr = 0x1000;
dut->offset_valid = 0xF;
// ... setup offsets ...
dut->is_load = 1;
dut->is_store = 0;
dut->valid = 1;

// After request accepted:
dut->l1_rdata.m_storage[0] = 0x1100FFEE;  // Provide L1 response
dut->l1_rdata.m_storage[1] = 0x55443322;
dut->l1_rdata.m_storage[2] = 0x99887766;
dut->l1_rdata.m_storage[3] = 0xDDCCBBAA;
dut->l1_rdata_valid = 1;
```

---

## Test 3: LDS Store (Scatter)（LDS 分散存储）

### 目的
验证 LSU 能否同时向 LDS（Local Data Share，共享内存）发起 32 个独立的存储请求（Scatter 模式）。

### 原理
```
                    ┌─────────┐
Lane 0: addr=0x400 ─┤         │
Lane 1: addr=0x404 ─┤  LDS    │
Lane 2: addr=0x408 ─┤  Unit   │
       ...          │ (64KB)  │
Lane 7: addr=0x41C─┤         │
                    └─────────┘
                    
每个 lane 独立：
- 16-bit address（LDS 地址空间）
- 32-bit write data
-独立的 wr_en 和 valid
```

**与 L1 的区别**：
- L1：合并为单个 128-bit 请求
- LDS：32 个独立的 32-bit 请求，同时发出

### 用法
```cpp
dut->warp_id = 7;
dut->base_addr = 0x400;        // LDS base
dut->offset_valid = 0xFF;      // lanes 0-7 active
// offsets: 0, 4, 8, 12, 16, 20, 24, 28
dut->is_lds = 1;               // 关键：选择 LDS 路径
dut->is_store = 1;
dut->valid = 1;

// Check 32 independent LDS requests:
for (int i = 0; i < 8; i++) {
    if (dut->lds_valid_flat & (1 << i)) {
        addr = getLdsAddr(i);      // Should be 0x400 + i*4
        data = getLdsWdata(i);       // Should be 0xC0 + i
    }
}
```

---

## Test 4: LDS Load (Gather)（LDS 聚集加载）

### 目的
验证 LSU 能否从 LDS 的 32 个独立地址读取数据，并组装到 `load_result`。

### 原理
```
LDS Unit
    │
    ├──→ lane 0: 0xAABBCCDD ─┐
    ├──→ lane 1: 0x11223344 ─┤
    ├──→ lane 2: 0x55667788 ─┼→ load_result[1023:0]
    └──→ lane 3: 0x99AABBCC ─┘
    
Gather: 32 个独立数据源 → 合并到 1 个 1024-bit 结果
```

**关键检查点**：
- LDS 响应数据正确组装到对应 lane
- Warp ID 正确返回

### 用法
```cpp
dut->warp_id = 9;
dut->base_addr = 0x800;
dut->offset_valid = 0xF;
dut->is_lds = 1;
dut->is_load = 1;
dut->valid = 1;

// Provide LDS response:
setLdsRdata(0, 0xAABBCCDD);
setLdsRdata(1, 0x11223344);
setLdsRdata(2, 0x55667788);
setLdsRdata(3, 0x99AABBCC);
dut->lds_rdata_valid = 1;

// Check:
// load_result.m_storage[0] == 0xAABBCCDD
// load_result.m_storage[1] == 0x11223344
// etc
```

---

## Test 5: Non-Coalesced Detection（非合并检测）

### 目的
验证 LSU 能否检测到 lanes 访问不同 128B cache line 的情况（当前设计简化处理，只取第一个 lane 的地址）。

### 原理
```
Lane 0: offset=0   → addr=0x1000 → line 0x20
Lane 1: offset=128 → addr=0x1080 → line 0x21  ← 不同 line!

理想设计：Split 为 2 个 L1 请求
当前设计：只发送 1 个请求（addr=0x1000），警告非合并
```

**注意**：当前实现是简化版，完整 GPU 需要支持 split 请求。

### 用法
```cpp
dut->base_addr = 0x1000;
dut->offset_valid = 0x3;       // lanes 0-1
dut->offset.m_storage[0] = 0;     // lane 0: same line
dut->offset.m_storage[1] = 128;     // lane 1: different line!
dut->is_load = 1;

// Expected: Request addr = 0x1000 (first active lane)
// Warning: Non-coalesced access detected
```

---

## Test 6: Back-to-Back Requests（连续请求）

### 目的
验证 LSU 能否正确处理连续的 Store → Load 请求，无状态机死锁或数据混淆。

### 原理
```
Time ──────────────────────────────────────→

Test 1 (Store):  IDLE → L1_REQ → IDLE
                     ↓          ↓
Test 2 (Load):       IDLE → L1_REQ → L1_WAIT → IDLE
                          ↓          ↓
                    Store data   Load data
                    0x11111111   0x11111111 (read back)
```

**关键检查点**：
- 状态机正确回到 IDLE
- 第二次请求的 Warp ID 和数据正确
- 无第一次请求的残留状态

### 用法
```cpp
// Request 1: Store
dut->warp_id = 10;
// ... store setup ...
dut->valid = 1; tick(); dut->valid = 0;
waitL1Response();  // Wait for store complete

// Immediately follow with:
// Request 2: Load (same address)
dut->warp_id = 11;  // Different warp_id!
// ... load setup ...
dut->valid = 1; tick(); dut->valid = 0;

// Verify load returns stored data
```

---

## 测试状态机覆盖

```
┌─────────┐    ┌─────────┐    ┌─────────┐
│  IDLE   │───→│ L1_REQ  │───→│ L1_WAIT │───→ IDLE
│         │    │ (Store) │    │ (Load)  │
│         │    │   ↓     │    └─────────┘
│         │    │ IDLE (Store done)
│         │
│         │───→│ LDS_REQ │───→│ LDS_WAIT│───→ IDLE
│         │    │(Scatter)│    │(Gather) │
└─────────┘    └─────────┘    └─────────┘
```

| 测试 | 覆盖路径 | 关键信号 |
|-----|---------|---------|
| Test 1 | IDLE → L1_REQ → IDLE | `l1_valid`, `l1_be`, `l1_wdata` |
| Test 2 | IDLE → L1_REQ → L1_WAIT → IDLE | `l1_rdata_valid`, `load_result`, `resp_warp_id` |
| Test 3 | IDLE → LDS_REQ → IDLE | `lds_valid_flat`, `lds_addr_flat` |
| Test 4 | IDLE → LDS_REQ → LDS_WAIT → IDLE | `lds_rdata_valid`, `load_result` |
| Test 5 | IDLE → L1_REQ → IDLE (non-coalesced) | `all_same_line` (internal) |
| Test 6 | IDLE → L1_REQ → IDLE → L1_REQ → L1_WAIT → IDLE | `ready`, state recovery |

---

## 当前测试状态总结

| 测试 | 状态 | 通过项 | 失败项 |
|-----|------|--------|--------|
| Test 1 | ✅ PASS | 地址对齐、数据打包、字节使能 | - |
| Test 2 | ❌ FAIL | 地址正确 | **数据全0、Warp ID错误** |
| Test 3 | ❌ FAIL | - | **LDS无有效信号** |
| Test 4 | ❌ FAIL | - | **数据全0** |
| Test 5 | ✅ PASS | 非合并检测 | - |
| Test 6 | ❌ FAIL | 请求完成 | **数据不匹配** |

**核心问题**：
- Test 2/3/4/6 的失败表明 `saved_warp_id` 更新和 LDS 信号生成存在时序或逻辑问题
- Test 1 通过说明 L1 Store 的基本路径是正确的