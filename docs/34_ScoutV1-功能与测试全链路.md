# 34 - Scout V1 功能与测试全链路实施计划（仓库 v7.0.0）

> 版本定位：**Scout 卡顿检测产品线第一完整版**（用户 2026-10-02 定）——仓库版本号 v7.0.0，
> 产品语义 = Windows 桌面端（Qt / Electron / Tauri 等常用桌面软件）卡顿检测的能力补全 +
> 单元测试全链路补链。macOS 暂缓不排期（同日决策批注）。

## 1. 背景与问题定义

V7 S1~S3 + V8 雷达交付后（v6.0.0~v6.5.0），Scout 探针能力矩阵已成型（T1 冻结 / T1b CPU /
T2 CDP / 全机雷达），但两类欠账使产品尚未达到"第一完整版"标准：

1. **单元测试全链路断链**：
   - `tests/unit/UnitTests.cpp`（25 用例 / 96 checks）只覆盖 src 核心库（解析 / 环形缓冲 /
     AlarmSuppressor / WatchConfig）；`ScoutDashboardTests`（8 用例 / 89 checks）只覆盖
     消费侧仪表盘模型。
   - **Scout 探针侧单测为零**：三态冻结状态机内嵌在 `RadarScheduler::run()`（双平台重复
     两份）与 `WindowFreezeProber::run()`（双平台重复两份）；CPU 迟滞内嵌在
     `CpuSampler::sample()`（双平台重复两份）；`receiverOf` / `excluded` 是匿名命名空间
     函数不可测。全靠 e2e 脚本断言兜底，纯逻辑回归无防护网。
2. **功能缺口**（docs/33 §7 已知边界遗留）：
   - 雷达无 per-target CPU 探针（`CpuSampler` 单目标 episode 状态未多目标化）
   - 雷达参数仅命令行，无配置文件持久化（长驻守护场景不友好）
   - 探针侧无告警风暴抑制（核心库 `AlarmSuppressor` 未接入 scout；全机雷达在系统级
     风暴时可产生告警洪峰）
   - uplink → aggregator 全链路仅在本地手动验证过，CI 无断言；CI e2e artifact 漏
     `scout_radar.log` 等

## 2. 现状审计（重构前基线）

### 2.1 状态机重复矩阵（R1 抽离对象）

| 逻辑 | 现存位置 | 重复份数 |
|---|---|---|
| 冻结三态 + 1s ongoing 节流 | RadarScheduler.cpp（Win / X11）、WindowFreezeProber.cpp（Win / X11） | 4 |
| CPU episode 迟滞（runs 迟滞 + 4 拍收口） | CpuSampler.cpp（Win / proc） | 2 |
| receiverOf（name@pid / pid:N） | RadarScheduler.cpp ×2 | 2 |
| excluded（子串大小写不敏感） | RadarScheduler.cpp ×2 | 2 |

三态语义在各处一致：`started`（首检停滞，stalledMs=阈值下界）→ `ongoing*`（1s 节流）
→ `recovered`（恢复）/ `lost`（目标消失，"恢复"语义不诚实）。V8 差异点：
recovered 前须真判存活（T1 = pid 解析非空；雷达 = 内核对象信号态 / zombie 判定）——
竞态语义统一进抽离层。

### 2.2 消费链路现状（零改动区）

```
scout 探针 ──[FreezeWatch]/[EventWatcher] 日志行──▶ RecordSink（kindOf 前缀门控）
  ──▶ WatchRecordStore（环形）──▶ UplinkClient ──▶ aggregator ──▶ HTML/仪表盘/导出
```

parseFields 通用 key=value 分词（附加 token 纯增量安全）；**行格式逐字节不变**是
R1 重构的硬约束，由既有四矩阵回归 + 本地 e2e 断言守护。

## 3. 顶层设计

### 3.1 ProbeLogic.h 抽离（R1）

新增 `examples/scout/ProbeLogic.h`（header-only，零平台依赖，仅 Qt Core 容器）：

```cpp
namespace qt_event_watcher { namespace ProbeLogic {

QString receiverOf(const QString &name, qint64 pid);   // name@pid / pid:N
bool excluded(const QString &name, const QStringList &patterns);

struct FreezeEvent { enum Kind { None, Started, Ongoing, Recovered, Lost };
                      Kind kind; qint64 totalMs; qint64 stalledMs; };

struct FreezeTracker {          // 冻结三态（per-target / 单目标通用）
    explicit FreezeTracker(int thresholdMs, int ongoingIntervalMs = 1000);
    // alive 用 std::function 惰性传入：仅 freezing && !hung 时才调用
    // （雷达 aliveOf 每次 OpenProcess+100ms，不能每拍每目标无谓付出）
    FreezeEvent onTick(bool hung, qint64 nowMs, const std::function<bool()> &alive);
    FreezeEvent onTargetGone(); // 在册集消失收口
};

struct CpuEpisodeTracker {      // CPU 迟滞（runs 超阈值一条 + 4 拍收口）
    explicit CpuEpisodeTracker(int thresholdPct, int runsNeeded);
    void reset();               // 目标消失观测断点
    CpuEvent onTick(double cpuPct);   // Spin 每 episode 至多一条
}; }}
```

平台层（EnumWindows / SendMessageTimeoutW / X11 / proc）保持薄壳不动。
日志格式化（QEW_LOG_WARN 调用点）留在平台壳内——抽离层只产事件，不产 I/O。

### 3.2 雷达 per-target CPU（R3a）

RadarScheduler 每 tick 对非排除 alive pid 增加 CPU 采样（与冻结探活同循环复用
确定性轮询序）：

- Windows：`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetProcessTimes`
  差分（同 CpuSampler 口径，100ns→ms）；Linux：`/proc/<pid>/stat` utime+stime 差分
  （锚定解析同 TargetResolver）。采样全为非阻塞 API，O(N) 不破坏单线程轮询模型。
- episode 状态 = `QHash<pid, ProbeLogic::CpuEpisodeTracker>`（消失随拍清理 reset）。
- 告警行对齐 CpuSampler 格式 + 雷达标注（receiver 实例级唯一防同名多开）：
  `[EventWatcher] slow event receiver={name@pid} object={name} event=cpuSpin type=99
  depth=0 costMs={cpu%:.1f} ... match=true thresholdMs={cpu阈值} source=scout radar=1`

### 3.3 雷达配置文件（R3b）

```ini
[radar]
exclude=RtkUWP,SearchHost      ; 逗号分隔子串排除表
thresholdMs=2000               ; 探活超时 = 冻结判定下界
intervalMs=500                 ; 轮询间隔
cpuThreshold=95                ; per-target CPU 阈值（单核口径）
cpuRuns=3                      ; CPU 迟滞拍数
```

- CLI：`--radar-config <file>`；优先级 **默认 < 配置文件 < 显式 CLI**（main.cpp 记录
  显式传入位，显式 CLI 参数压过文件）
- 热加载：RadarScheduler 每 tick `QFileInfo::lastModified()` 检查，变更即重载
  exclude/threshold/interval/cpu 参数（worker 线程独占读取，无锁）；mtime 读取失败
  保持旧配置并告警一次

### 3.4 探针侧风暴抑制（R3c）

核心库 `src/event/AlarmSuppressor.h`（1s 窗口首条必出 + 懒冲刷 suppressed=N 汇总，
key 隔离）接入 scout 告警发射点：key = `kind:receiver`。冻结 started/ongoing/
recovered 是状态配对语义——抑制器 1s 窗口内首条必出保证配对头不被吞，仅压制洪峰
重复（如系统级卡死时雷达全机 started 爆发、CDP longtask 连发）。

### 3.5 uplink 全链路 CI 化 + 本地 e2e 固化（R4）

- CI（linux 矩阵）：radar e2e 步骤扩展为 aggregator + scout `--uplink` 同跑，
  断言落盘 JSON 含 radar=1 记录且 received==pushed==lastSeq
- artifact 补漏：`scout_radar.log` / `basic_demo*.log` / aggregator JSON 入 upload
- 本地：`scripts/e2e_scout.ps1` 一键四探针（freeze / cpu / radar [+ cdp 可选]）+
  uplink 断言，Windows 本地可重复执行

## 4. 阶段计划与验证门

| 阶段 | 内容 | 验证门 |
|---|---|---|
| R1 | ProbeLogic.h 抽离 + 三消费方重构（行为零变化） | 四矩阵 scout 构建 + 本地 e2e 行格式逐字节比对 |
| R2 | UnitTests 增补探针纯逻辑段（目标 +30 checks） | 四矩阵 unit 全绿，README 徽章更新 |
| R3 | R3a per-target CPU + R3b 配置文件 + R3c 风暴抑制 | 本地 e2e：雷达 cpuSpin 告警 + 配置热加载 + 风暴抑制行 |
| R4 | CI uplink 全链路 + artifact 补漏 + 本地 e2e 脚本 | CI linux e2e 全绿 + 本地脚本 PASS |
| R5 | 门禁收口 + CHANGELOG v7.0.0 + tag/Release/工作台 | 四矩阵回归 16/16 + static-check 0 + CI badge |

## 5. CLI 规格（V1 全量）

```
scout --pid <pid> | --name <substr>          T1/T1b 单目标
scout --cdp-port <port> [--cdp-target ...]   T2 CDP 长任务（免 pid/name）
scout --radar [--radar-exclude a,b]          V8 全机雷达（冻结）
       [--radar-config <file>]               V1 新增：配置文件（热加载）
       [--threshold --interval --cpu-threshold --cpu-runs]   通用参数（雷达复用）
       [--uplink <name> --flush-ms --duration]
```

## 6. 风险评估

| 风险 | 缓解 |
|---|---|
| R1 重构日志行漂移 | 抽离层只产事件不产 I/O；本地 e2e 逐字节比对 + 四矩阵回归守护 |
| aliveOf 每拍误付 100ms | std::function 惰性求值，仅 freezing && !hung 才调（语义同现网） |
| per-target CPU 采样权限失败（系统进程） | OpenProcess 失败静默跳过（同 CpuSampler），差分缺失自然无告警 |
| 风暴抑制破坏 freeze 三态配对 | 1s 窗口首条必出（抑制器语义），配对头必达；e2e 断言三态完整 |
| 配置热加载竞态 | worker 线程独占读取（QFileInfo + 值拷贝），无跨线程共享可变状态 |
| QSettings 拖入 GUI 依赖 | QSettings 属 QtCore，无风险；解析失败回退默认值 |

## 7. 已知边界（V1 不做）

- CDP 附加已运行进程（需目标冷启动带 `--remote-debugging-port`；Tauri 走环境变量
  注入已覆盖）——通用运行中进程附加涉及注入，单列后续版本
- macOS（用户决策暂缓）；Wayland native 冻结探针（X11 诚实降级不变）
- 雷达上行 TLS/鉴权（uplink 本地管道语义不变）

## 8. 交付记录（R5 回填，2026-10-02）

### 8.1 代码落地表

| 阶段 | 落地物 |
|---|---|
| R1 ProbeLogic 抽离 | `examples/scout/ProbeLogic.h`（header-only：`FreezeTracker` 三态 / `CpuEpisodeTracker` 迟滞 / `receiverOf` / `excluded`）；WindowFreezeProber / RadarScheduler / CpuSampler 三消费方重构，日志行格式逐字节不变 |
| R2 单测补链 | `tests/unit/UnitTests.cpp` +10 用例（FreezeTracker 三态全路径 / CpuEpisodeTracker 迟滞 / receiverOf/excluded），25→35 cases、96→131 checks |
| R3a 雷达 per-target CPU | RadarScheduler 双平台每 tick 冻结探活后同循环 CPU 差分采样（Windows `GetProcessTimes` / Linux `TargetResolver::cpuTimeMsOf`），episode `QHash` 随目标消失清理；cpuSpin 行 + `radar=1`，receiver=`name@pid` 实例级唯一 |
| R3b 配置文件 + 热加载 | `examples/scout/RadarConfig.h`（手写逐行 INI 解析）+ main.cpp 显式位合成（优先级 默认<文件<显式 CLI）+ RadarScheduler 每 tick mtime 热重载（失败保持旧配置告警一次） |
| R3c 风暴抑制 | `examples/scout/ScoutAlarmEmitter.h`（复用 AlarmSuppressor，方法名 `emitAlarm` 避开 Qt `emit` 空宏）；四探针 23 处发射点接入，key=`kind:receiver`，静默条降 DEBUG |
| R4 CI + 本地 e2e | regression.yml linux radar e2e 扩 aggregator uplink 断言（received==pushed==lastSeq + radar=1）+ artifact 补漏；`scripts/e2e_scout.ps1` 四场景一键 e2e |

### 8.2 质量门禁

- UnitTests **35 cases / 153 checks** × 四矩阵全绿（qt5152-msvc / qt653-msvc / qt5152-mingw / qt653-mingw）
- benchmark 全 5 模式四矩阵回归无 DRIFT（±25% 容差）
- static-check（cppcheck 2.22.0）**0 findings**
- Qt 5.15.2 / 6.5.3 × MSVC / MinGW 双编译器编译通过

### 8.3 e2e 实证（本地 scripts/e2e_scout.ps1 四场景 ALL PASS）

- **cpu**：`event=cpuSpin` + `source=scout` 断言通过
- **freeze**：三态 started/recovered 完整（totalMs≈3729 对齐 S1 基线 3731）
- **radar**：`radar=1` 标注 + 三态完整（全机发现，RtkUWP 排除表生效）
- **uplink**：aggregator 落盘 JSON `received==pushed==lastSeq` 且含 `radar=1` 记录，链路健康自洽
- CI 侧：linux 矩阵同链路断言随 workflow 提交（badge 全绿为最终凭证）

### 8.4 修复记录

- `ScoutAlarmEmitter` 方法名 `emit` 撞 Qt 空宏（signal 关键字），签名整行被撕碎炸穿下游 Qt 头 → 改名 `emitAlarm` 全量替换 23 处
- `RadarScheduler` cpuRuns 热更仅新 episode 生效（既有 episode runsNeeded 为构造期常量）——诚实边界保留
