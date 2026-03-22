这是一个 **512KB 8路组相联 L2 缓存控制器** 模块，采用 **写回（Write-Back）** 策略，用于在 **GPC（通用处理集群）** 和 **HBM 高带宽内存** 之间提供高速数据缓冲。

---

## 1. 核心功能概述

| 特性 | 规格 |
|------|------|
| **容量** | 512KB |
| **组织方式** | 8路组相联（8-Way Set Associative） |
| **行大小** | 128字节（1024位），分8个bank × 128位 |
| **替换策略** | LRU（最近最少使用） |
| **写策略** | 写回（Write-Back，仅替换脏行时写内存） |
| **接口** | 4端口 Crossbar + HBM + Copy Engine监听 |

---

## 2. 内存组织详解

### 地址分解（48位物理地址）
```
|  Tag (40位)  | Set Index (9位) | Bank (3位) | Offset (4位) |
| 63        24 | 23           15 | 14      12 | 11         0 |
```
- **Sets 数量** = 512KB / (128B × 8路) = 512 组
- **Bank 组织**：每行128B分为8个bank，每个bank 128位（16字节），支持部分写（Byte Enable）

### 存储结构
```verilog
// Tag Array：每路存储 Valid + Dirty + Tag + LRU
valid_bits [512][8]     // 有效位
dirty_bits [512][8]     // 脏位（需写回标记）
tag_array  [512][8]     // 40位地址标签
lru_counter[512][8]     // 3位LRU计数器（0=最旧，7=最新）

// Data Array：分bank访问
data_array [512 sets][8 ways][8 banks] × 128-bit
```

---

## 3. 状态机操作流程

```
IDLE → LOOKUP → [HIT_RW | MISS_ALLOC → WRITEBACK → REFILL → HIT_RW]
  ↓
FLUSH/INVALIDATE (维护命令)
```

| 状态 | 功能 |
|------|------|
| **IDLE** | 等待请求，处理维护命令（Flush/Invalidate） |
| **LOOKUP** | 并行比较8路Tag，检测命中（Hit）或缺失（Miss） |
| **HIT_RW** | 命中处理：读直接返回，写标记脏位并更新LRU |
| **MISS_ALLOC** | 选择LRU牺牲行，检查是否需要写回 |
| **WRITEBACK** | 脏行写回HBM（8个周期，每次128位） |
| **REFILL** | 从HBM读取新行（8个周期填充8个bank） |
| **FLUSH** | 全缓存刷回：遍历所有Set和Way，写回脏行 |

---

## 4. 关键机制详解

### 命中检测（并行比较）
```verilog
// 组合逻辑，单周期完成8路Tag比较
for (way_i = 0; way_i < 8; way_i++) begin
    if (valid_bits[set][way_i] && tag_array[set][way_i] == req_tag)
        hit = 1;
end
```

### LRU 替换策略
- 每路维护3位计数器（0-7）
- **命中时**：命中路设为7，其他大于0的路减1
- **替换时**：选择计数器为0的路作为牺牲行

### 写回优化（Write-Back）
- 写操作只更新缓存，不立即写内存
- **脏位（Dirty Bit）** 标记被修改的缓存行
- **仅在替换脏行时** 才触发写回，减少HBM带宽压力

---

## 5. 接口功能

### GPC 请求端口（来自Crossbar）
- 128位数据宽度（匹配HBM突发长度）
- 支持字节使能（Byte Enable，16位对应16字节）
- 握手信号：`valid/ready`

### HBM 内存接口
- 4通道HBM支持（`HBM_CHANNELS=4`）
- 请求ID用于区分通道
- 突发传输：8个128位传输组成完整缓存行

### Copy Engine 监听接口（一致性支持）
```verilog
// 供Copy Engine查询地址是否在L2中（Snoop）
ce_snoop_addr  → 查询地址
ce_snoop_hit   → 返回是否命中（用于一致性决策）
```
- 支持 DMA/C2C（Chip-to-Chip）传输的缓存一致性
- 命中时可触发无效化或写回

### 缓存维护命令
| 命令 | 作用 |
|------|------|
| `flush_req` | 将所有脏行写回HBM（保持数据一致性） |
| `invalidate_req` | 直接清空所有有效位（不保留脏数据） |

---

## 6. 系统定位

这个L2缓存位于 **Crossbar 4×4**（你之前提供的模块）和 **HBM 控制器** 之间：

```
[GPC 0-3] 
    ↓
[Crossbar 4×4]  ← 路由到L2 Bank
    ↓
[L2 Cache]      ← 本模块（命中时短路，缺失时访问HBM）
    ↓
[HBM Controller]
    ↓
[HBM 2.5D/3D Memory]
```

**设计优势**：
- 8路组相联降低冲突缺失（Conflict Miss）
- 写回策略减少HBM写入带宽（提升50%+效率）
- LRU保证热点数据驻留
- 分bank结构支持128位部分访问，节省功耗

这是一个相当完整的工业级L2缓存设计，适用于AI加速器或高性能CPU的内存子系统。