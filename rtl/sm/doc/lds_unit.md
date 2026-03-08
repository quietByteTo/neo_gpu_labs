这是一个 **LDS（Local Data Share）单元**，即 GPU 的共享内存（Shared Memory）。让我详细解释原理并生成完整的 Verilator testbench。

---

## 核心原理

### 1. 组织架构

```
总容量: 64KB = 32 Banks × 2KB
每 Bank: 512 个 32-bit 字 (2KB)
地址映射: Addr[6:2] 选择 Bank (32 banks)，Addr[15:7] 选择 Bank 内地址
```

### 2. Bank 映射策略

| 地址位 | 作用 |
|--------|------|
| [1:0] | 字节偏移（未使用，字对齐） |
| [6:2] | **Bank 选择**（5 bits = 32 banks） |
| [15:7] | Bank 内字地址（9 bits = 512 words） |

**关键特性**：连续地址（stride-1）映射到不同 Bank，实现 **无冲突访问**。

### 3. 冲突处理

- **检测**：同一周期多个 lane 访问同一 bank
- **仲裁**：低 lane ID 优先（first-come-first-serve）
- **重放**：冲突的 lane 设置 `lds_ready=0`，需要重新发起请求

### 4. 原子操作

支持：ADD, MIN, MAX, AND, OR, XOR，返回操作前的旧值（read-modify-write）

---
