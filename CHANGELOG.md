# Changelog

本文件记录 QtEventWatcher 的版本演进。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号对应仓库迭代里程碑（详细规划与逐 Phase 实施记录见 [docs/21_版本规划与交付物.md](docs/21_版本规划与交付物.md)）。

## [v7.0.0] - 2026-10-02

### 新增
- **Scout V1——卡顿检测功能与单元测试全链路**（docs/34）：
  - **ProbeLogic.h 纯逻辑抽离**：冻结三态状态机（`FreezeTracker`）、CPU episode 迟滞（`CpuEpisodeTracker`）、receiver/excluded 工具从平台壳抽出为 header-only 零平台依赖，T1/T1b/T3 三消费方重构，日志行格式逐字节不变；alive 惰性求值语义（仅冻结恢复拍付探活成本）与消亡竞态语义（recovered 前真判存活）统一
  - **雷达 per-target CPU 探针**：`--radar` 每拍冻结探活后同循环做 per-target CPU 差分采样（Windows `GetProcessTimes` 100ns 累计差分 / Linux `/proc/<pid>/stat` utime+stime），episode 迟滞复用 `CpuEpisodeTracker`，cpuSpin 告警对齐 slow event 行格式 + `radar=1` 标注（receiver=name@pid 实例级唯一，同名多开各自成 episode）；全非阻塞 O(N) 不破坏单线程轮询模型
  - **雷达配置文件持久化 + 热加载**：`--radar-config <file>` 读 INI `[radar]` 节（exclude/thresholdMs/intervalMs/cpuThreshold/cpuRuns），优先级 **默认 < 配置文件 < 显式 CLI**（显式位合成）；`RadarScheduler` 每 tick mtime 检查热重载（worker 线程独占读无锁），文件消失/解析失败保持旧配置并告警一次
  - **探针侧风暴抑制**：`ScoutAlarmEmitter` 复用核心库 `AlarmSuppressor`（1s 窗口首条必出 + 懒冲刷 suppressed=N 汇总）接入四探针全部 23 个告警发射点，key=`kind:receiver`——冻结状态配对头不吞，静默条降级 DEBUG（RecordSink 仍全量采集，回放完整性优先）
  - **UnitTests Scout 纯逻辑段**：+10 用例（FreezeTracker 三态全路径/CpuEpisodeTracker 迟滞/RadarConfig 解析）32→35 cases、131→153 checks，四矩阵全绿
- **CI uplink 全链路 e2e**：linux 矩阵 radar e2e 扩展为 aggregator 同机 QLocalServer 收链路——断言落盘 JSON 含 `radar=1` 记录且 `received==pushed==lastSeq`；artifact 补漏 `scout_radar.log`/`basic_demo3.log`/`aggregator.log`/`scout_radar_uplink.json`
- **本地一键 e2e**：`scripts/e2e_scout.ps1` 四场景（cpu/freeze/radar/uplink）断言健康自洽，Windows 本地可重复执行

### 修复
- `ScoutAlarmEmitter`：方法名不可叫 `emit`——Qt 将 `emit` 定义为空宏（signal 关键字），会把函数签名整行撕碎并炸穿下游所有 Qt 头，改名 `emitAlarm`
- `RadarScheduler`：cpuRuns 热更后仅新 episode 生效（既有 episode 的 runsNeeded 为构造期常量）——诚实边界，docs/34 §7 记录

## [v6.5.0] - 2026-10-02

### 新增
- **系统级 ANR 雷达**（V8，docs/33）：`scout --radar` 常驻守护全机所有 GUI 程序的卡顿——
  - **全机发现**：每拍枚举"拥有可见顶层窗口"的进程集（Windows 单趟 EnumWindows + 单次 Toolhelp 快照建名表 / Linux X11 `_NET_WM_PID` 归属），天然覆盖 Qt/Electron/WPF/Win32/GTK，天然过滤无窗口后台进程
  - **单线程轮询**：全机目标共享一个 worker 线程（不随目标数增长线程/fd），per-target 三态状态机逐字段对齐 WindowFreezeProber；增量管理（新目标入册 / 消失目标 freeze lost 收口）；进程名缓存只解析新 pid
  - **零改动渲染**：告警行沿用 `[FreezeWatch]` 前缀 + `receiver=name@pid`（实例级唯一，防同名多开三态配对串扰）+ `radar=1` 标注——aggregator/HTML/仪表盘/导出原样渲染
  - `--radar-exclude <name1,name2>` 进程名子串排除表（大小写不敏感）；`--radar` 与 `--pid/--name` 互斥、可与 `--cdp-port` 并存
  - X11 探活原语抽出共享头 `X11Probe.h`（T1 单目标探针与雷达共用，token 改进程级单例跨 TU 防串扰）；CI 新增 linux 雷达 e2e（Xvfb 内雷达自动发现 busy-loop 靶 → freeze 三态断言）
  - Wayland native / 无 X display 诚实降级（日志明示，CDP 探针不受影响）

### 修复
- `RadarScheduler`：`QHash::unite` 是 Qt5 专属（Qt6 移除）——显式 insert 循环兼容 Qt5/Qt6 双版本
- `RadarScheduler`：目标消亡竞态修复——kill 恰落在探活消息在途时，`SendMessageTimeoutW` 对垂死窗口返回非 0 被误判 recovered，freeze lost 永不可达；recovered 前以内核对象信号态（Windows `WaitForSingleObject` 100ms 宽限）/ zombie 态（Linux `/proc/<pid>/stat` state != Z）真判存活，死进程一律收口 freeze lost；X11 段装 no-op XError handler（雷达长驻下枚举与探活间隙目标窗口随时消亡，BadWindow 默认处理器会终止进程）

## [v6.4.0] - 2026-10-02

### 新增
- **Linux 探针适配**（V7，docs/32 §10）：Scout 三探针 Linux 全可用——
  - **T1 窗口冻结**（X11）：`_NET_WM_PING` 探活对齐 Windows `SendMessageTimeoutW(WM_NULL)` 语义（token 原子计数防迟到 pong 串扰；`_NET_CLIENT_LIST` ∪ root 子窗口枚举 + `_NET_WM_PID` 过滤，Xvfb/无 WM 环境兜底）；X11 为可选依赖（`QEWT_SCOUT_X11`，缺失禁用不阻断）；Wayland native / 无 display 诚实降级（CPU/CDP 探针不受影响）
  - **T1b CPU 启发**：`/proc/<pid>/stat` 差分采样（utime+stime × `sysconf(_SC_CLK_TCK)`），日志行/迟滞口径逐字对齐 Windows 版
  - **TargetResolver**：`/proc` 枚举 + comm 探活 + ppid 建图，`descendantPids` BFS 3 层与 Windows 版语义一致
  - **StackCapture（core）**：glibc `backtrace()` + `dladdr()`（≤32 帧、模块基址缓存、RVA 对齐）；`${CMAKE_DL_LIBS}` 链接（glibc 2.34 前 libdl 分离发行版如麒麟 V10 兼容）
- **CI linux-smoke job**（ubuntu-latest）：Qt 5.15.2/6.5.3 × gcc 双矩阵——apt 一键依赖（libgl1-mesa-dev + xcb 平台插件全家桶含 Qt 6.5 硬依赖 libxcb-cursor0）、xvfb 无头冒烟、**CPU/冻结双 e2e**（basic_demo `--spin` 验证靶：cpuSpin 断言 + freeze 三态断言，靶与探针同 X server）；Configure/Build/Smoke/e2e 四级失败取证全落 GITHUB_STEP_SUMMARY（匿名可读）+ ldd soname 前置检查

### 修复
- `DataExporter.cpp:458`：`int64_t → QVariant` 隐式转换在 Linux GCC 下歧义（LP64 下 int64_t=long，与 QVariant int/long long 构造器打平；MSVC 下 int64_t=long long 精确匹配故从未暴露）——显式 `static_cast<qlonglong>`

## [v6.3.0] - 2026-10-02

### 新增
- **Scout 独立可视化仪表盘 S3**（V7，examples/scout-dashboard，docs/32 §9）：`scout-dashboard --in <aggregator JSON> --out <html>`——读 aggregator `--out` 快照生成单文件自包含 HTML 仪表盘（五章节：会话概览与健康度/冻结时间轴/CDP 长任务 per-url 直方/CPU 与慢事件/明细折叠）；交互版泳道时间轴（缩放/拖拽/双击复位/类型与 pid 筛选/分页）经 qrc 内嵌零依赖零构建链，资源缺失自动降级纯静态
- scout-dashboard CLI 过滤：`--pid` / `--kind <csv 0..3>`，摘要与渲染/内嵌 JSON 共用 filterModel 同一口径
- ScoutDashboardTests：8 用例 89 checks（解析/归类/跨午夜 relMs/容错/转义与 JSON 内嵌安全/filterModel），四矩阵纳入

### 安全
- XSS 三层分工：C++ escapeHtml（静态位）+ embedJsonSafe（`</`→`<\/` 封死 script 提前闭合，内嵌 JSON 唯一注入面）+ JS esc()（DOM 位）；进入 `<script>` 的内容（含 JS 注释）禁止字面闭合标签序列

## [v6.2.0] - 2026-10-01

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
