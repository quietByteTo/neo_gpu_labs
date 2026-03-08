这是一个 **Tensor Core（张量核心）** 模块，基于 **脉动阵列（Systolic Array）** 架构，用于 GPU 的矩阵乘法加速。让我详细解释其结构、原理和用法。

---

## 核心架构

### 1. 4×4 脉动阵列

```
┌─────┬─────┬─────┬─────┐
│ MAC │ MAC │ MAC │ MAC │  ← Weight[0][*]  stationary
├─────┼─────┼─────┼─────┤
│ MAC │ MAC │ MAC │ MAC │  ← Weight[1][*]  stationary
├─────┼─────┼─────┼─────┤
│ MAC │ MAC │ MAC │ MAC │  ← Weight[2][*]  stationary
├─────┼─────┼─────┼─────┤
│ MAC │ MAC │ MAC │ MAC │  ← Weight[3][*]  stationary
└─────┴─────┴─────┴─────┘
  ↑     ↑     ↑     ↑
Activation 逐列流入（Systolic flow）
```

### 2. 数据流：Weight Stationary（权重 stationary）

| 阶段 | 操作 |
|------|------|
| **LOAD** | 将 B 矩阵（权重）加载到阵列中，保持 stationary |
| **COMP** | A 矩阵（激活）逐行/列流入，与权重相乘并累加 |
| **DONE** | 输出结果 D = A × B + C |

---

## 端口详解

### 配置端口

| 端口 | 位宽 | 说明 |
|------|------|------|
| `precision` | 2-bit | 精度模式：00=FP16, 01=TF32, 10=INT8 |
| `layout` | 3-bit | 数据布局：行优先/列优先 |

### 数据端口

| 端口 | 位宽 | 说明 |
|------|------|------|
| `mat_a` | 256-bit | 输入矩阵 A（4×4 FP16 = 16×16bit） |
| `mat_b` | 256-bit | 输入矩阵 B（4×4 FP16 = 16×16bit） |
| `mat_c` | 512-bit | 累加矩阵 C（4×4 FP32 = 16×32bit） |
| `mat_d` | 512-bit | 输出矩阵 D（4×4 FP32） |

### 控制端口

| 端口 | 方向 | 说明 |
|------|------|------|
| `valid_in` | Input | 输入数据有效 |
| `ready` | Output | 核心空闲，可接收新数据 |
| `valid_out` | Output | 输出数据有效 |

---

## 计算原理

### 矩阵乘法：D = A × B + C

```
A (4×4)     B (4×4)     C (4×4)     D (4×4)
┌───┐       ┌───┐       ┌───┐       ┌───┐
│ a │   ×   │ b │   +   │ c │   =   │ d │
└───┘       └───┘       └───┘       └───┘

d[i][j] = Σ(a[i][k] × b[k][j]) + c[i][j],  k=0..3
```

### 脉动阵列执行流程

```
Cycle 0 (LOAD):  加载 B[0][*] 到第0行, B[1][*] 到第1行...

Cycle 1 (COMP):  A[0][0] 进入 → 乘 B[0][0], 累加到 ACC[0][0]
Cycle 2 (COMP):  A[0][1] 进入 → 乘 B[0][1], 累加到 ACC[0][1]
                 A[1][0] 进入 → 乘 B[1][0], 累加到 ACC[1][0]
Cycle 3 (COMP):  A[0][2], A[1][1], A[2][0] 同时进入（波前传播）
...

Cycle N (DONE): 输出 D 矩阵
```

---

## 状态机

```
┌─────┐    valid_in    ┌─────┐    cycle=0    ┌─────┐
│ IDLE │ ─────────────→ │ LOAD │ ───────────→ │ COMP │
└─────┘                └─────┘               └─────┘
   ↑                      │                      │
   │                      │                      │ cycle=LATENCY-1
   │                      ↓                      ↓
   │                   ┌─────┐               ┌─────┐    cycle=0
   └───────────────────│ DONE │ ←───────────│ ... │ ←──────────┘
                        └─────┘               └─────┘
                        valid_out=1
```

| 状态 | 描述 |
|------|------|
| `IDLE` | 空闲，等待输入 |
| `LOAD` | 加载权重矩阵 B 到阵列 |
| `COMP` | 脉动计算，A 流入，MAC 累加 |
| `DONE` | 打包输出 D 矩阵 |

---

## 使用示例

### 示例 1：基本 FP16 矩阵乘法

```verilog
// 定义 4×4 矩阵（FP16）
// A = [1, 2, 3, 4; 5, 6, 7, 8; ...] （行优先）
wire [255:0] mat_a = {
    16'h0004, 16'h0003, 16'h0002, 16'h0001,  // Row 0: 1,2,3,4
    16'h0008, 16'h0007, 16'h0006, 16'h0005,  // Row 1: 5,6,7,8
    16'h000C, 16'h000B, 16'h000A, 16'h0009,  // Row 2: 9,10,11,12
    16'h0010, 16'h000F, 16'h000E, 16'h000D   // Row 3: 13,14,15,16
};

// B = 单位矩阵
wire [255:0] mat_b = {
    16'h0001, 16'h0000, 16'h0000, 16'h0000,
    16'h0000, 16'h0001, 16'h0000, 16'h0000,
    16'h0000, 16'h0000, 16'h0001, 16'h0000,
    16'h0000, 16'h0000, 16'h0000, 16'h0001
};

// C = 0
wire [511:0] mat_c = 512'd0;

// 启动计算
precision = 2'b00;  // FP16
layout    = 3'b000; // Row-major
mat_a     = mat_a;
mat_b     = mat_b;
mat_c     = mat_c;
valid_in  = 1'b1;

// 等待 ready=1，然后 valid_in=0

// 4 周期后 valid_out=1，读取 mat_d
// D = A × I + 0 = A
```

### 示例 2：累加模式（D = A×B + C）

```verilog
// C 矩阵初始化为上一次的结果
mat_c = prev_result;

// 新的 A, B
mat_a = new_activation;
mat_b = new_weights;

valid_in = 1'b1;

// 结果：D = A×B + C（常用于 LSTM/Transformer 的累加）
```

### 示例 3：连续计算（Pipeline）

```verilog
// Cycle 0: 第一个矩阵块
valid_in = 1'b1; mat_a = block0_a; mat_b = block0_b; mat_c = block0_c;

// Cycle 1: 第二个矩阵块（如果 ready=1）
valid_in = 1'b1; mat_a = block1_a; mat_b = block1_b; mat_c = block1_c;

// Cycle 4: 第一个结果输出
if (valid_out) result0 = mat_d;

// Cycle 5: 第二个结果输出
if (valid_out) result1 = mat_d;
```

---

## 关键特性

| 特性 | 说明 |
|------|------|
| **Weight Stationary** | 权重保持不动，减少内存访问 |
| **Systolic Flow** | 数据流动计算，最大化并行度 |
| **FP16→FP32** | 低精度输入，高精度累加，防止溢出 |
| **固定延迟** | LATENCY=4，可精确预测输出时间 |

---

## 与 CUDA Tensor Core 对比

| 特性 | 本模块 | NVIDIA Tensor Core |
|------|--------|-------------------|
| 阵列大小 | 4×4 | 16×16 (Volta) / 8×8 (Ampere) |
| 精度 | FP16/TF32/INT8 | FP16/TF32/BF16/INT8/FP64 |
| 累加 | FP32 | FP32 |
| 吞吐量 | 16 MAC/cycle | 256 MAC/cycle (Volta) |

这是一个 **简化教学模型**，实际 GPU Tensor Core 更大、更复杂，支持更多精度和更大的矩阵块（如 16×16×16）。

