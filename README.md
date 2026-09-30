# QtEventWatcher — Qt 卡顿排查辅助器

<p align="right"><b>中文</b> | <a href="README.en.md">English</a></p>

[![CI](https://github.com/sclzj595/qtEventWatcher/actions/workflows/regression.yml/badge.svg)](https://github.com/sclzj595/qtEventWatcher/actions/workflows/regression.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Qt](https://img.shields.io/badge/Qt-5.15%20%7C%206.5-41cd52.svg)](https://www.qt.io/)
[![platform](https://img.shields.io/badge/platform-Windows%20%7C%20MSVC%20%7C%20MinGW-lightgrey.svg)](docs/02_需求范围与版本矩阵.md)
[![standard](https://img.shields.io/badge/C%2B%2B-17-00599c.svg)](CMakeLists.txt)
[![tests](https://img.shields.io/badge/tests-25%20cases%20%2F%2096%20checks-2ea44f.svg)](tests/unit/UnitTests.cpp)
[![static analysis](https://img.shields.io/badge/cppcheck%20%2F%20W4-0%20findings-2ea44f.svg)](scripts/static-check.ps1)
[![sanitizer](https://img.shields.io/badge/ASan-0%20reports-2ea44f.svg)](docs/21_版本规划与交付物.md)

> 仓库描述（复制到 Gitee/GitHub 仓库设置 → 基本信息 → 仓库描述，推荐 Topics：`qt` `cpp` `performance` `profiler` `anr` `desktop`）：

```text
Qt Widgets 桌面程序的轻量级 ANR 看门狗与事件级性能剖析组件：拦截 notify() 计时全部事件，慢事件/慢信号/冻结告警 + 函数级调用栈归因，自身开销 ~140ns/事件。
```


面向 Qt Widgets 桌面程序的轻量级 ANR 看门狗与事件级性能剖析组件：拦截 `QApplication::notify()` 计时全部事件，识别慢事件 / 慢 MetaCall（跨线程信号）/ QSS 加载抖动 / 主线程冻结（ANR），周期统计聚合，慢事件自动采集调用栈到函数级，日志落盘、报告导出、外部进程实时调控。

> **自身开销已量化到 ns 级**：全关常驻 ≈140ns/事件（帧预算的 0.0008%），开启监控后 <1µs/事件，告警路径仅超阈值事件付费——监控器自身不成为卡顿来源。完整四矩阵（Qt 5.15/6.5 × MSVC/MinGW）基准数据见 [docs/22_性能基准报告.md](docs/22_性能基准报告.md)。

<p align="center">
  <img src="docs/img/basic-demo.gif" alt="Basic Demo 动态演示：慢事件/MetaCall/高频事件统计卡片与最近异常实时流" width="700"><br>
  <em>examples/basic 七页诊断 UI —— 概览页实时演示（动图）</em>
</p>

<p align="center">
  <img src="docs/img/aggregation-report.png" alt="多进程聚合 HTML 报告：概览/进程健康度/TOP 接收者/冻结时间线/明细" width="700"><br>
  <em>aggregator 自包含聚合报告（零 JS）—— 多进程按 host_pid 分组</em>
</p>


## 核心能力

- **全局事件监控**：`notify()` 覆写计时全部事件，慢事件/慢 MetaCall/QSS 抖动分类告警，阈值可配、INI/IPC 运行时热更新
- **函数级归因**：慢事件自动采集调用栈（≤32 帧 + 模块归属），告警风暴抑制（1s 窗口）避免日志刷屏
- **ANR 看门狗**：主线程心跳 + 常驻线程轮询，冻结 started/ongoing/recovered 三态告警
- **多进程聚合**：监控进程经 Named Pipe 上行 NDJSON，aggregator 中心收集器按 pid 聚合，自包含 HTML 聚合报告（零 JS）
- **数据出口**：CSV/JSON/SQLite 全量回放、九段诊断报告、规则化归因摘要、单进程/聚合 HTML 报告
- **安全降级**：解析失败安全降级、日志异常不崩溃、监控失败不影响业务（PRD 14）

## 快速接入（3 步）

### 1. 引入构建

将本仓库作为子目录加入宿主工程：

```cmake
add_subdirectory(QtEventWatcher)            # 自动构建 QtEventWatcherCore（静态库）
target_link_libraries(YourApp PRIVATE QtEventWatcherCore)
```

或 `find_package(QtEventWatcher)`（`cmake --install . --prefix <dir>` 后指向该前缀，spdlog 经源码子构建自动提供）。

### 2. 替换 Application 并初始化日志

```cpp
#include "CusApplication.h"
#include "WatchLogger.h"

int main(int argc, char* argv[])
{
    qt_event_watcher::CusApplication app(argc, argv);   // 替换 QApplication
    qt_event_watcher::WatchLogger::instance().initialize("./logs");
    return app.exec();
}
```

`CusApplication` 构造即完成监控链装配（配置加载、四条监控链、INI 热更新轮询、IPC 服务）。日志未初始化时所有 `QEW_LOG_*` 自动静默，无硬依赖。

### 3. 开启监控

**环境变量**（进程启动前设置）：

```text
QT_EVENT_WATCHER_WATCH_FUN=15        # 位掩码：1 慢事件 / 2 MetaCall / 4 周期统计 / 8 QSS / 15 全部
QT_EVENT_WATCHER_SLOW_EVENT_THRESHOLD_MS=30
```

**或 INI**（默认 `./QtEventWatcher.ini`，运行中修改自动热更新，无需重启）：

```ini
[QtEventWatcher]
Watch_Fun=15
SlowEventThresholdMs=30
SlowMetaCallThresholdMs=30
EventStatPeriodMs=1000
```

配置优先级：默认值 → 环境变量 → INI → 运行时 setter。全部环境变量与阈值含义见 [docs/13_配置项与阈值.md](docs/13_配置项与阈值.md)。

## 可选能力

| 能力 | 接入方式 | 说明 |
|---|---|---|
| QSS 加载监控 | `QssStyleWatcher::instance()->beginLoadQss(path)` / `endLoadQss()` 包住加载与 `setStyleSheet` | 记录 IO 与样式应用耗时 |
| IPC 实时调控 | 无需代码，默认开启（命名 `QtEventWatcher.<pid>`） | 外部进程经 QLocalSocket JSON 行协议改配置，见 `examples/ipc-console` |
| 多进程聚合 | 监控进程设 `QT_EVENT_WATCHER_UPLINK_NAME=<server>` + 运行 `aggregator` | `--html` 出自包含聚合报告，`--out` 出 JSON，见 `examples/aggregator` |
| 数据导出 | `ReportExporter`（九段报告）/ `DataExporter`（CSV/JSON/SQLite）/ `HtmlReporter`（单进程 HTML）/ `DiagnosticSummarizer`（归因摘要） | 入口见 `src/runtime/` |
| 运行环境诊断 | `RuntimeDiagnostics` | Qt 环境/模块枚举/依赖树，不占 Watch_Fun 位 |

## 完整示例

- [examples/basic](examples/basic/) — 七页诊断 UI（概览/明细/运行环境/测试场景/导出），含 QSS 埋点、主题令牌、导出按钮
- [examples/aggregator](examples/aggregator/) — 多进程中心收集器（tail 实时流 / JSON / HTML 聚合报告）
- [examples/ipc-console](examples/ipc-console/) — 外部控制台，实时调控另一进程的监控行为

## 构建与测试

**CMake Presets（推荐）**——Qt 路径经环境变量注入，无需改任何文件：

```powershell
$env:QT_EVENT_WATCHER_QT_DIR = "<Qt>/5.15.2/msvc2019_64"   # 或 6.5.3 等任一 Qt 前缀
cmake --preset windows-msvc          # 另有 windows-msvc2022 / windows-mingw / linux-gcc
cmake --build --preset windows-msvc --config Release
ctest --preset windows-msvc -C Release
```

MinGW 需另设 `QT_EVENT_WATCHER_MINGW_BIN` 指向工具链 bin 目录。需 CMake ≥ 3.21。

> **VS2022 (17.13+) + Qt 6.5.x 编译注意**：新版 MSVC STL 已移除 `stdext`，而 Qt 6.5.x 的
> `qcompilerdetection.h` 仍引用它（QTBUG-111580，Qt 6.6+ 已修复），会在 `qvarlengtharray.h`
> 报 `C2065: 'stdext'`。解法任选：升级 Qt ≥ 6.6；或参照
> [regression.yml 的 Install Qt 步](.github/workflows/regression.yml)对 Qt 安装树打两行直通补丁
> （checked iterator 纯调试安全包装，语义零差异）。本仓库 CI 即按此补丁在 windows-latest 上常绿。

**传统方式**：

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64   # 或 -G Ninja + MinGW 工具链
cmake --build build --config Release
ctest -C Release                                        # 冒烟测试（在 build 目录内执行）
```

一键回归（构建 + 冒烟 + 基准漂移检测，四矩阵）：`scripts/run_regression.ps1`，参数见脚本头注。

## 文档索引

- 接入与适配：[docs/23_集成与适配说明.md](docs/23_集成与适配说明.md)（spdlog / Qt 版本兼容 / MetaCall ABI）
- 开销证据：[docs/22_性能基准报告.md](docs/22_性能基准报告.md)
- 事件模型：[docs/05_全局事件耗时监控.md](docs/05_全局事件耗时监控.md)、[docs/06_MetaCall跨线程信号监控.md](docs/06_MetaCall跨线程信号监控.md)
- 配置参考：[docs/13_配置项与阈值.md](docs/13_配置项与阈值.md)

完整 PRD 全集（01~29，含架构/协议/性能/版本规划/交付总结）见 [docs/README.md](docs/README.md) 索引。

## Roadmap

- [x] **V6 质量证明线**：纯逻辑单元测试 + 静态分析门禁 + ASan 实证（[docs/31](docs/31_V6实施计划.md)）
- [ ] **Scout 外部探针（产品线 B）**：进程外检测任意桌面程序（Electron/WPF/Win32）的卡顿，统一汇入 aggregator 报告——**S1 已交付**：T1 窗口冻结 + T1b CPU 启发（[examples/scout](examples/scout/)，[docs/32](docs/32_V7-Scout实施计划.md)）；**S2 已交付**：T2 CDP 长任务——`scout --cdp-port 9222` 直连 Electron/Chromium 页面，PerformanceObserver 精确到渲染长任务（目标带 `--remote-debugging-port` 冷启动）
- [ ] Linux 验证（可移植性已在代码层预留，等环境）
- [ ] QML/Qt Quick 事件路径支持（当前面向 Widgets）
- [ ] 火焰图导出（调用栈数据已具备，缺渲染端）
- [ ] vcpkg / Conan 包管理接入

## 变更历史

见 [CHANGELOG.md](CHANGELOG.md)。

## License

[MIT](LICENSE)
