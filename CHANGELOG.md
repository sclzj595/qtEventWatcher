# Changelog

本文件记录 QtEventWatcher 的版本演进。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号对应仓库迭代里程碑（详细规划与逐 Phase 实施记录见 [docs/21_版本规划与交付物.md](docs/21_版本规划与交付物.md)）。

## [Unreleased]

### 新增
- **Tauri 支持实证**（V7，docs/32 §8）：三探针对 Tauri（Rust 主进程 + 系统 WebView2）全通过——T1 窗口冻结直接适用（tao 主线程即消息泵）；T2 CDP 长任务经 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` 环境变量注入调试端口（`http://tauri.localhost/` 页面 target 实测 11 条 costMs=120 精确）；E2E 上行 `received=14 dropped=0` 零改动汇入 aggregator

### 修复
- T1b CPU 启发漏采孙进程：`TargetResolver::childPids`（一层子进程）→ **`descendantPids`**（BFS 3 层后代枚举，单快照建图+去重防 pid 环）——Tauri 进程树 `app.exe → msedgewebview2.exe → renderer` 的忙转 renderer 是孙进程，旧实现必漏；Electron（直接子进程）语义不变

## [v6.1.0] - 2026-09-30

### 新增
- **Scout 外部探针 S2**（V7 T2，examples/scout/CdpLongTaskProber）：CDP 长任务探针——`scout --cdp-port 9222` 直连 Electron/Edge/Chrome 页面（QNetworkAccessManager 发现 + QWebSocket + PerformanceObserver(longtask) 注入/回读），渲染长任务精确到毫秒；支持纯 CDP 模式（免 pid/name）、断连自动重发现、页面导航自动重注入、buffered 回放历史条目；行格式复用 slow event 前缀，aggregator/HTML/导出零改动渲染
- scout CLI：`--cdp-port` / `--cdp-target <substr>` / `--cdp-threshold ms`；Qt WebSockets 为可选依赖（缺失时禁用探针不阻断构建）
- **CI 首次打通**（GitHub Actions regression）：双 Qt 矩阵 windows-latest 常绿；Build 取证装置（失败时 error 行写入 step summary + ::error:: 注解 + build.log artifact）
- 双语 README 挂 CI 徽章 + VS2022 (17.13+) / Qt 6.5.x stdext 兼容性说明（QTBUG-111580 下游指引）

### 修复
- CI qt653-msvc 编译失败：Qt 6.5.x `qcompilerdetection.h` 引用新版 MSVC STL 已移除的 `stdext`（qvarlengtharray.h C2065）——workflow Install Qt 步对 aqt 安装树打直通补丁（checked iterator 纯调试安全包装，语义零差异；宏预定义方案因 Qt 头 MSVC 分支 C4005 重定义无效，已证伪）

## [v6.0.0] - 2026-09-30

### 新增
- **Scout 外部探针 S1**（产品线 B / V7，examples/scout）：进程外检测任意 Windows 桌面程序（Electron/WPF/Win32/Qt）卡顿——T1 窗口冻结（SendMessageTimeout 消息泵停摆，三态告警对齐 EventWatchdog，aggregator/HTML 零改动渲染）+ T1b CPU 启发（目标+子进程单核满转，episode 迟滞防抖），观测记录复用自监控日志行协议全链路上行
- `basic_demo --spin <ms>`：主线程 busy-loop 验证靶（Scout T1/T1b 双喂）
- **单元测试基建**（V6 Q1）：tests/unit/QEWT 轻量断言 + 25 用例 96 检查（解析族/抑制器合成时钟/kindOf 行为级/环形账目闭合/WatchConfig），回归脚本纳入 unit 步骤（四矩阵 16/16）
- **集成测试补强**（V6 Q4）：TestEventWatcher 新增 uplink/health 行为级断言（内嵌 QLocalServer：record.push 线协议/断线重连计数/health 载荷自洽）
- `scripts/static-check.ps1`：cppcheck 一键门禁（V6 Q2）
- 双语 README 质量徽章（tests/cppcheck/ASan）

### 变更
- **静态分析清零**（V6 Q2）：/W3→/W4 + -Wall -Wextra 四矩阵 0 警告；cppcheck（--library=qt + -DQT_VERSION + --error-exitcode）0 findings；修复 4 处 performance（成员入初始化列表/serverName() 返回 const 引用）
- **ASan 实证**（V6 Q3）：build-asan /MT 静态 runtime（Win11 24H2 动态 runtime 0xC0000142 环境降级），UnitTests/集成冒烟/Benchmark 全路径零报告；MSVC 无 LSan 照实声明（泄漏留待 Linux）
- CusApplication 配置改 unique_ptr<WatchConfig>（V4 C2 收尾）

## [v5.1.0] - 2026-09-29

### 新增
- **聚合 HTML 报告**（V5.1 C 线）：`AggregationReporter`（examples/aggregator 局部）——五章节自包含静态报告（概览 / 进程健康度 / TOP 接收者 / 冻结时间线 / 明细 200 条），零 JS 零依赖，`aggregator --html <file>` 触发，与 `--out` JSON 独立共存

## [v5.0.0] - 2026-09-29

### 新增
- **op=health 健康度上行**：UplinkClient 随 flushTick 5s 节拍上报 pushed/dropped/reconnects/lastSeq，aggregator 保存快照并进 JSON 导出
- `WatchRecordStore::hydrateRecord/parseFields/parseFrames` 公有静态——解析逻辑单点化，消费侧（aggregator）复用

### 变更
- **采集链路懒解析**：RecordSink 零解析，环形缓冲只存 kind/seq/time/raw，字段/栈帧解析后移到 snapshot 出口锁外 hydrate（"解析放消费侧"）
- **上行协议 raw 化**：record.push 仅传 kind/seq/time/raw，aggregator 消费侧重建 fields/frames（导出形状与 v4 逐字段一致）
- formatTime 秒段缓存（localtime_s 同秒只渲染一次）

### 修复
- **reconnects 计数缺陷**：对端进程被杀时本地无写错误可观察致计数恒 0——改为连接态变化统一检测（connected/everConnected 双标志）

### 性能
- 风暴抑制路径 mode=1：10.7 → 9.3~9.5µs；真实收益为架构性（风暴期零解析、环形内存降低、快照持锁缩短）

## [v4.0.0] - 2026-09-29

### 新增
- **一键四矩阵回归基建**：`scripts/run_regression.ps1`（构建+冒烟+基准漂移检测 ±25%）+ `baselines-v3.json` 基线 + GitHub Actions CI 模板
- **install / find_package 分发模型**：install 三件套 + find_package 包配置，examples/consume-test 双模型验收（四矩阵 × 双模型 8/8）
- **多进程聚合（D 线）**：`UplinkClient` 差量游标上行（NDJSON 批推 + 指数退避重连，热路径增量 0）+ `examples/aggregator` 中心收集器（per-pid 聚合 / tail / JSON 导出）

### 变更
- `AlarmSuppressWindowMs` 抑制窗口配置化（三通道热更新）
- `StackCaptureMode` 栈采集三档（0 命中即采 / 1 仅窗口首条 / 2 关闭，mode=1 风暴路径 -36%）
- `CusApplication` 改 `unique_ptr<WatchConfig>`，config 目录转 PRIVATE，公共面收窄为四目录

## [v3.0.0] - 2026-09-28

### 新增
- **慢事件调用栈采集**：`StackCapture`（RtlCaptureStackBackTrace ≤32 帧 + 模块归属缓存），记录与导出全链路带 frames
- **告警风暴抑制**：`AlarmSuppressor`（1s 窗口首条必出 + 懒冲刷汇总），风暴路径开销较全告警减半
- **冻结看门狗**：`EventWatchdog`（ANR 模式：主线程心跳 + 常驻线程轮询，三态告警），Watch_Fun bit4
- **数据消费**：SQLite 导出（meta/records/frames/statistics 四表）+ 自包含 HTML 报告（零 JS，概览/诊断/统计/冻结时间线/明细）

### 变更
- 公共 API 收紧：event/ipc/runtime 目录转 PRIVATE，白盒 include 显式化
- 终态基准：常驻 160~182ns / 正常路径 610~952ns / 风暴抑制路径 15.6~18.6µs / 完整告警路径 110~153µs（仅阈值命中事件付费）

## [v2.0.0] - 2026-09-28

### 新增
- 事件周期统计（跨周期合并 TOP / Exclusive Cost）
- QSS 加载监控与样式应用耗时统计
- 数据导出（CSV / JSON 全量回放）与九段报告（ReportExporter）
- IPC 实时调控（QLocalServer JSON 行协议，外部进程改配置）
- GUI 诊断 demo（examples/basic 七页 UI）与外部控制台（examples/ipc-console）
- 性能基准报告（docs/22：同进程对照法，四矩阵量化）

## [v1.0.0] - 2026-09-28

### 新增
- 基础事件监控：`CusApplication::notify()` 覆写 + 慢事件告警（spdlog 4 sink）
- 配置三通道：环境变量 / INI 热更新 / 运行时 setter
- MetaCall 跨线程信号监控（Qt 私有 API，5/6 双版本分开处理）
- Qt ≥ 5.14 编译期门控，Qt 5.15.2 / 6.5.3 × MSVC / MinGW 四矩阵适配

[unreleased]: https://gitee.com/liu-zhijiang_sc/qt-event-watcher/compare/v5.1.0...HEAD
[v5.1.0]: https://gitee.com/liu-zhijiang_sc/qt-event-watcher/blob/main/CHANGELOG.md
