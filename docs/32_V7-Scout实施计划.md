# 32 V7 Scout 外部探针实施计划（产品线 B 第一期）

> 主题：把 QtEventWatcher 扩充为**通用程序卡顿检测器**——不止监控 Qt 自身，进程外检测任意
> Windows 桌面软件（Electron / WPF / Win32 / Qt）的卡顿。V6 规划期（docs/31 §4）立项为产品线 B，
> V6 收官后正式启动。本文 = 实施计划 + S1 交付记录。

## 1. 核心洞察

卡顿的通用本质是**主线程 / 消息泵停摆**，与 GUI 框架无关：Electron 主进程、Qt、WPF、Win32
全部是 Windows 消息循环驱动，窗口"未响应"就是跨栈统一的卡顿信号。Windows 自带检测原语
（SendMessageTimeout 超时 ≈ 任务管理器"未响应"同源），**无需注入、无需被监控程序配合**。

## 2. 探针分层

| 层 | 能力 | 原语 | 状态 |
|---|---|---|---|
| T1 | 窗口冻结（跨栈通用） | EnumWindows + SendMessageTimeout(WM_NULL, SMTO_ABORTIFHUNG, threshold)，阈值自控（IsHungAppWindow 内置 5s 不可调故弃用） | ✅ S1 交付 |
| T1b | CPU 启发（"忙死"：窗口仍响应但单核满转，Electron renderer 即子进程） | GetProcessTimes 差分采样（目标+直接子进程，单核口径），连续 N 拍超阈值告警 | ✅ S1 交付 |
| T2 | Electron renderer 精确卡顿（长任务级） | CDP（--remote-debugging-port → PerformanceObserver longtask），QtWebSockets 已确认可用（Qt5.15.2 MSVC）；本机可用 msedge 充当 Chromium 验证靶 | ⏸ S2 另立计划 |
| T3 | 全系统级（ETW） | 复杂度不成比例 | 不做 |

## 3. 架构决策：零 src 公共面改动

观测记录走与自监控**完全相同**的日志行协议，进程内闭环复用采集链：

```
scout 探针 ──QEW_LOG_WARN──▶ WatchLogger ──▶ RecordSink（前缀 kindOf 采集）
             ──▶ WatchRecordStore 环形 ──▶ UplinkClient（NDJSON record.push/health）
             ──▶ aggregator（host_pid 分组 / JSON / HTML 冻结时间线）——零改动
```

- freeze 三态行逐字段对齐 EventWatchdog（`freeze started thresholdMs=/stalledMs=/receiver=/type=/inProgress=`；
  `ongoing elapsedMs=`；`recovered totalMs=`）→ aggregator/HTML 冻结时间线直接渲染
- CPU 启发复用 `[EventWatcher] slow event` 行，`event=cpuSpin type=99 source=scout` 诚实标注
  启发式来源；不新增 Kind 枚举（跨层波及 kindOf/aggregator/HTML，违背克制原则）
- src/config、src/ipc 经白盒 include 消费（V4 C1/D2 收紧后的 PRIVATE 面，与 tests/examples 口径一致）

## 4. S1 交付物（examples/scout/）

| 文件 | 职责 |
|---|---|
| `main.cpp` | console 应用；CLI `--pid/--name/--threshold(2000)/--interval(250)/--cpu-threshold(95)/--cpu-runs(3)/--uplink/--flush-ms/--duration`；组装 WatchConfig→WatchLogger→UplinkClient→双探针 |
| `WindowFreezeProber.h/.cpp` | T1：常驻 worker 线程（hung 拍最坏占满 threshold，不能占主循环）；name→pid 全量匹配 + EnumWindows 可见顶层窗口 + SendMessageTimeout；三态状态机（started stalledMs 取阈值下界=诚实下界）；目标消失收口 `freeze lost` |
| `CpuSampler.h/.cpp` | T1b：主线程 QTimer 采样；单核口径 `cpu% = Δ(kernel+user)/Δt`；episode 迟滞（连续 runs 拍超阈值发一条，**连续 4 拍低于阈值**才收口——单拍噪声不重置）；目标消失断点复位 |
| `TargetResolver.h` | header-only：resolvePids（name 匹配/pid 探活）+ childPids（直接子进程）+ processNameOf |
| `CMakeLists.txt` | console exe + QtEventWatcherCore + 白盒 include src/config、src/ipc（无需 sql 插件部署） |

微改两处：`examples/CMakeLists.txt` 挂载 scout；`examples/basic` 新增 `--spin <ms>`
（主线程 busy-loop：CPU 单核满转 + 消息泵停摆，同帧喂 T1 冻结与 T1b CPU 的验证靶）。

## 5. S1 交付记录（2026-09-30）

### 5.1 实测证据（qt5152-msvc，basic_demo --spin 6000 为靶）

- **控制台直测**：cpuSpin 单条（costMs=100.8>95）+ freeze started→ongoing→recovered
  （totalMs=3731ms，spin 实际 6s，差值 = threshold 探测粒度的诚实滞后）
- **uplink e2e**：scout --uplink QtEventWatcherAggregator → aggregator 收到 4 条
  （SlowEvent×1 + Freeze×3，host_pid=scout pid），health 自洽
  `pushed=4 == received=4 == lastSeq=4, dropped=0, reconnects=0`——**零改动闭环成立**

### 5.2 迭代修复

- **CpuSampler 迟滞收口**：首测 6s 单次忙转被采样噪声（单拍 <95%）撕成 3 条告警——
  加"连续 4 拍低于阈值才关闭 episode"迟滞后恰好 1 条
- **cppcheck uninitMemberVar ×7**：探针成员头文件内默认初始化（`= 0` / `{false}`）

### 5.3 质量门禁

- 四矩阵构建全绿（scout MinGW 同标准 Win32 API 无适配）
- 回归 16/16 PASS：qt653-msvc slow plain-user-event 单次 +38.2% DRIFT 复测即消
  （单次摆动噪声，V5 教训再次验证）；scout 零 src 改动热路径零增量
- static-check 0 findings；UnitTests 96/0 不受影响

## 6. S2 展望（后续计划）

- T2 CDP：/json/list 发现 target → QWebSocket 连接 → Runtime.evaluate 注入
  PerformanceObserver('longtask') → 周期拉取 buffer → costMs=long task 时长
- 验证靶：`msedge --remote-debugging-port=9222`（同为 Chromium，无需 Electron 环境）
- aggregator HTML 外部进程专属章节（receiver 按 host_pid 分组已有基础）、--title 窗口过滤
