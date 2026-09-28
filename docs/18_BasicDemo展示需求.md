# Qt 卡顿排查辅助器 PRD 18：Basic Demo 展示需求

## 1. 定位
Basic Demo 不属于 QtEventWatcher 核心库，职责为：功能验证 + 诊断能力展示 + 开发测试工具。
- Demo 可以依赖 QtEventWatcherCore；Core 不反向依赖 Demo。
- Core 不允许出现 QWidget* / QLabel* / QTableWidget* / QMessageBox* 等 UI 依赖。
- 测试场景位于 examples/，不得成为 Core 依赖。

## 2. 页面结构

```text
Basic Demo
├── 概览
├── 慢事件
├── MetaCall
├── 高频事件
├── QSS
├── 运行环境        ← 数据来自 PRD 16
└── 测试场景
```

## 3. 概览页（默认首页）
- KPI 计数卡：慢事件、慢 MetaCall、高频事件、QSS 告警（含"过去 1 分钟"统计口径）。
- 最近异常列表：时间、类型、对象链路（如 `MouseMove → MainWindow`、`Worker → result()`）、耗时。
- 数据来源：PRD 17 最近异常缓冲。

## 4. 详细诊断页
四类明细表，字段规范见 PRD 17 §3：

| 页面 | 字段 |
|---|---|
| 慢事件 | Time / Event / Type / Receiver / Object / Cost / Threshold |
| MetaCall | Time / Sender / Signal / SignalId / Receiver / Object / Cost / Threshold |
| 高频事件 | Period / Event / Count / Total Cost / Count Threshold / Cost Threshold |
| QSS | Time / Operation / File 或 Object / Cost / Threshold / Count |

交互：单击查看详情（侧边抽屉 / Dialog，不跳页），双击打开详细信息，右键复制 / 导出 / 筛选。

## 5. 测试场景
与正式诊断功能视觉隔离，页面顶部明确 DEMO / TEST 标识：
- 制造慢事件 / 制造慢 MetaCall / 制造高频事件（MouseMove、Timer）/ 制造 QSS 更新。
- 仅用于功能验证、回归测试、Demo 演示。

## 6. 配置与日志约束
- 配置键与阈值保持 PRD 13 不变；Bit0~Bit3 定义不变，Runtime Diagnostics 不参与 Watch Function Mask。
- 日志仍是 Core 主要诊断输出（格式见 PRD 12）；Runtime Diagnostics 使用 `[RuntimeDiagnostics]` 前缀，且不得因每个 Event 重复输出。

## 7. 稳定性约束
Runtime Diagnostics（含依赖分析）异常时：不影响 Qt 主程序、不影响四个监控器；失败仅表现为"依赖分析失败"，不允许崩溃（详见 PRD 16 §8）。
