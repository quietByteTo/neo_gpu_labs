## 2. 验证计划文档 (test_plan.md)

```markdown
# SM Frontend 验证计划

## 1. 验证目标

### 1.1 功能正确性
- 确保所有设计功能符合架构规范
- 验证边界条件和异常处理
- 确认时序收敛性

### 1.2 覆盖率目标
| 类型 | 目标 | 方法 |
|:---|:---|:---|
| 代码覆盖率 | >95% | Verilator coverage |
| 功能覆盖率 | 100% 关键路径 | 定向测试+随机测试 |
| 场景覆盖率 | 所有典型场景 | 测试用例矩阵 |

---

## 2. 模块功能规格

### 2.1 核心功能
```
┌─────────────────────────────────────────┐
│  PC管理: 32个独立PC寄存器 (64-bit)      │
│  调度器: 优先级/Round-Robin选择就绪warp │
│  取指:   向I-Cache发起请求              │
│  译码:   提取寄存器地址和操作码         │
│  记分板: RAW/WAW冒险检测                │
│  发射:   向后端发送指令和操作数         │
└─────────────────────────────────────────┘
```

### 2.2 关键时序
| 操作 | 延迟 | 说明 |
|:---|:---|:---|
| Task Launch → Active | 1 cycle | `task_valid`采样后 |
| Schedule → Fetch | 0 cycle | 组合逻辑 |
| Fetch → Issue | 2 cycles | 取指+译码+冒险检查 |
| Issue → Writeback | N cycles | 由执行单元决定 |
| Branch → PC Update | 1 cycle | 写回阶段同步更新 |

---

## 3. 验证策略

### 3.1 分层验证
```
Level 1: 单元测试 (Unit Tests)
    └── 单个功能点独立验证
    └── 黄金模型对比
    
Level 2: 集成测试 (Integration Tests)  
    └── 多模块协同工作
    └── 数据流端到端检查
    
Level 3: 系统测试 (System Tests)
    └── 完整应用场景
    └── 性能/压力测试
```

### 3.2 验证方法
| 方法 | 用途 | 实现 |
|:---|:---|:---|
| **定向测试** | 验证特定功能点 | 手工编写测试向量 |
| **随机测试** | 发现边界问题 | 随机指令序列 |
| **回环测试** | 验证数据完整性 | 执行结果写回对比 |
| **形式验证** | 关键属性证明 | SVA断言（未来） |

---

## 4. 测试用例详述

### 4.1 基础功能测试

#### TC-001: Reset Verification
**目的**: 验证复位后初始状态正确

**为什么重要**: 
- 确保上电后处于已知状态
- 防止X态传播导致不确定行为

**验证方法**:
```cpp
1. 施加复位 (rst_n = 0)
2. 保持10个时钟周期
3. 检查所有输出:
   - warp_active == 0
   - task_ready == 1
   - ifetch_valid == 0
   - issue_valid == 0
```

**通过标准**: 所有信号符合预期初始值

---

#### TC-002: Task Launch
**目的**: 验证任务正确启动

**为什么重要**:
- 这是GPU工作的入口点
- 错误启动会导致整个计算错误

**验证方法**:
```cpp
1. 选择warp_id = 5, entry_pc = 0x1000
2. 置位task_valid一个周期
3. 检查:
   - 周期1: warp_active[5]置位
   - 周期2: 该warp可被调度
   - PC值正确加载
```

**通过标准**: warp激活，PC正确，不影响其他warp

---

### 4.2 流水线测试

#### TC-003: Single Warp Pipeline Flow
**目的**: 验证单warp指令流水

**为什么重要**:
- 基础功能验证
- 建立性能基准（CPI）

**验证方法**:
```
Cycle 0: Launch warp 0
Cycle 1-2: Scheduler selects warp 0
Cycle 3:   Fetch request to I-Cache
Cycle 4:   I-Cache returns instruction
Cycle 5:   Decode, check scoreboard
Cycle 6:   Read register file
Cycle 7:   Issue to backend
```

**检查点**:
- 每周期PC += 4
- 指令正确译码
- 操作数正确传递

---

#### TC-004: Scheduler Round-Robin
**目的**: 验证调度器公平性

**为什么重要**:
- 防止warp饿死（starvation）
- 确保SIMT利用率

**验证方法**:
```cpp
1. 启动3个warp (0, 1, 2)
2. 运行12个周期
3. 记录每个warp的发射次数
4. 检查分布: 每个warp应获得 ~4次机会
```

**通过标准**: 无warp被连续忽略超过2个周期

---

### 4.3 冒险检测测试

#### TC-005: RAW Hazard Detection
**目的**: 验证读后写冒险正确阻塞

**为什么重要**:
- 数据正确性保障
- 避免使用陈旧数据

**场景设计**:
```asm
I1: ADD r1, r2, r3    # Cycle 0 issue, writes r1 at Cycle 3
I2: ADD r4, r1, r5    # Cycle 1 tries to read r1 - HAZARD!
```

**验证方法**:
```cpp
1. 发射I1，标记r1为busy (latency=3)
2. 下一周期尝试发射I2
3. 检查sc_stall_raw == 1
4. 等待3周期（模拟执行）
5. 检查I2成功发射
```

**通过标准**: 正确检测stall，无错误发射

---

#### TC-006: WAW Hazard Detection
**目的**: 验证写后写冒险正确阻塞

**为什么重要**:
- 保证指令顺序
- 避免寄存器状态混乱

**场景设计**:
```asm
I1: ADD r5, r1, r2    # writes r5
I2: SUB r5, r3, r4    # also writes r5 - WAW!
```

**通过标准**: I2在I1写回前被阻塞

---

### 4.4 控制流测试

#### TC-007: Branch Handling
**目的**: 验证分支跳转处理

**为什么重要**:
- 控制流正确性
- 性能影响（分支预测失败惩罚）

**验证方法**:
```cpp
1. 顺序执行3条指令 (PC: 0x1000 → 0x100C)
2. 模拟分支指令写回:
   - wb_branch_taken = 1
   - wb_branch_target = 0x2000
3. 检查下一取指地址 = 0x2000
4. 验证PC数组正确更新
```

**通过标准**: PC正确跳转，流水线正确刷新

---

### 4.5 阻塞处理测试

#### TC-008: Backend Stall
**目的**: 验证后端忙时的流水线暂停

**为什么重要**:
- 流控机制
- 防止数据丢失

**验证方法**:
```cpp
1. 正常发射几条指令
2. 置位issue_ready = 0
3. 运行5周期，检查无issue_valid
4. 置位issue_ready = 1
5. 检查1-2周期内恢复发射
```

**通过标准**: 无缝暂停和恢复

---

#### TC-009: I-Cache Stall
**目的**: 验证I-Cache未就绪处理

**为什么重要**:
- 内存层次性能
- 隐藏延迟能力

**验证方法**:
```cpp
1. 正常取指
2. 置位ifetch_ready = 0
3. 检查ifetch_valid保持或重试
4. 恢复ifetch_ready = 1
5. 验证指令流连续性
```

---

### 4.6 高级功能测试

#### TC-010: Scoreboard Bypass
**目的**: 验证同周期写读旁路

**为什么重要**:
- 性能优化
- 避免假冒险

**场景**:
```
Cycle N:   Instruction writes r1 (WB stage)
           Next instruction reads r1 (Issue stage)
           
Without bypass: Stall (false RAW)
With bypass:    Allow issue (data forwarded)
```

**通过标准**: 无不必要的stall

---

#### TC-011: Multi-Warp Independent Execution
**目的**: 验证多warp独立执行

**为什么重要**:
- SIMT核心功能
- 资源隔离

**验证方法**:
```cpp
1. 启动warp 0 (PC=0x1000) 和 warp 1 (PC=0x2000)
2. 运行20周期
3. 检查:
   - 两个warp都发射了指令
   - PC独立递增
   - 记分板独立工作
```

---

#### TC-012: Concurrent Launch and Issue
**目的**: 验证任务启动与发射并发

**为什么重要**:
- 动态并行性
- 任务调度灵活性

**验证方法**:
```cpp
1. warp 0正在运行并发射指令
2. 同一周期启动warp 5
3. 检查:
   - warp 0继续发射
   - warp 5正确激活
   - 无资源冲突
```

---

## 5. 覆盖率计划

### 5.1 代码覆盖率检查点
```cpp
// PC管理
covergroup pc_cg;
    coverpoint pc_value {
        bins low = {[0:32'hFFFF]};
        bins mid = {[32'h10000:32'hFFFFFFFF]};
        bins high = {[32'h100000000:64'hFFFFFFFFFFFFFFFF]};
    }
    coverpoint warp_id { bins w[32] = {[0:31]}; }
endgroup

// 记分板
covergroup sb_cg;
    coverpoint hazard_type { bins raw = {1}, waw = {2}, none = {0}; }
    coverpoint bypass_used { bins yes = {1}, no = {0}; }
endgroup

// 调度器
covergroup sched_cg;
    coverpoint selected_warp { bins all[32] = {[0:31]}; }
    coverpoint stall_reason { 
        bins backend = {1}, 
        bins raw = {2}, 
        bins waw = {3},
        bins icache = {4} 
    };
endgroup
```

### 5.2 功能覆盖矩阵

| 功能 | 测试用例 | 覆盖状态 |
|:---|:---|:---:|
| 复位 | TC-001 | ⬜ |
| 任务启动 | TC-002 | ⬜ |
| 单warp流水 | TC-003 | ⬜ |
| 多warp调度 | TC-004, TC-011 | ⬜ |
| RAW冒险 | TC-005 | ⬜ |
| WAW冒险 | TC-006 | ⬜ |
| 分支处理 | TC-007 | ⬜ |
| 后端阻塞 | TC-008 | ⬜ |
| I-Cache阻塞 | TC-009 | ⬜ |
| 记分板旁路 | TC-010 | ⬜ |
| 优先级调度 | TC-012 | ⬜ |
| 并发操作 | TC-013 | ⬜ |

---

## 6. 回归测试流程

### 6.1 每日回归
```bash
make clean
make build
make run
# 检查所有测试通过
# 检查覆盖率报告
```

### 6.2 发布前回归
```bash
# 1. 完整随机测试（10,000次随机序列）
./sm_frontend_tb --random-tests=10000

# 2. 压力测试（所有warp满载）
./sm_frontend_tb --stress-test

# 3. 边界测试（极端PC值，寄存器地址）
./sm_frontend_tb --boundary-test

# 4. 生成最终报告
make coverage-report
```

---

## 7. 调试指南

### 7.1 波形分析要点
```
关键信号分组:
1. 控制: clk, rst_n
2. 任务: task_valid, task_ready, start_warp_id, entry_pc
3. 调度: schedule_valid, selected_warp (probe)
4. 取指: ifetch_valid, ifetch_addr, ifetch_ready, ifetch_data
5. 发射: issue_valid, issue_warp_id, decoded_inst
6. 冒险: sc_stall_raw, sc_stall_waw
7. 写回: wb_valid, wb_warp_id, wb_branch_taken
```

### 7.2 常见问题

| 现象 | 可能原因 | 调试方法 |
|:---|:---|:---|
| warp不激活 | task_valid时序错误 | 检查setup/hold |
| 指令不发射 | 记分板持续stall | 检查busy_table状态 |
| PC不更新 | 分支信号干扰 | 隔离分支逻辑 |
| 调度不公平 | 优先级编码器错误 | 检查eligible_warps |

---

## 8. 附录：修改建议

### 8.1 RTL增强（为更好验证）
```verilog
// 建议添加调试接口
`ifdef VERILATOR
    // 内部信号探针
    output wire [PC_W-1:0] debug_pc [0:NUM_WARPS-1],
    output wire [NUM_WARPS*NUM_REGS-1:0] debug_busy_table,
    output wire [WARP_ID_W-1:0] debug_selected_warp,
`endif
```

### 8.2 测试平台增强
- 添加UVM风格sequence
- 实现自动对比（scoreboarding）
- 支持多核并行仿真
```