# QtEventWatcher — Qt 卡顿排查辅助器

面向 Qt Widgets 桌面程序的轻量级卡顿诊断组件：拦截 `QApplication::notify()` 计时全部事件，识别慢事件 / 慢 MetaCall（跨线程信号）/ QSS 加载抖动，周期统计聚合，日志落盘、报告导出、外部进程实时调控。

- 支持 Qt 5.15.2 / 6.5.3 × MSVC / MinGW（Qt ≥ 5.14 编译期保证，5.14.2 运行时验证待环境）
- 自身开销已量化：全关常驻 ≈140ns/事件，开启后 <1µs/事件（见 [docs/22_性能基准报告.md](docs/22_性能基准报告.md)）
- 监控失败不影响业务：解析失败安全降级，日志异常不崩溃（PRD 14）

## 快速接入（3 步）

### 1. 引入构建

将本仓库作为子目录加入宿主工程：

```cmake
add_subdirectory(QtEventWatcher)            # 自动构建 QtEventWatcherCore（静态库）
target_link_libraries(YourApp PRIVATE QtEventWatcherCore)
```

或使用安装产物：`cmake --install . --prefix <dir>` 后在宿主 CMake 中指向该前缀。spdlog 经源码子构建自动提供（`spdlog::spdlog` 由 Core PUBLIC 传播，宿主无需重复引入）。

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
| 数据导出 | `ReportExporter`（九段报告）/ `DataExporter`（CSV/JSON 全量回放）/ `DiagnosticSummarizer`（规则化归因摘要） | 入口见 `src/runtime/` |
| 运行环境诊断 | `RuntimeDiagnostics` | Qt 环境/模块枚举/依赖树，不占 Watch_Fun 位 |

## 完整示例

- [examples/basic](examples/basic/) — 七页诊断 UI（概览/明细/运行环境/测试场景/导出），含 QSS 埋点、主题令牌、导出按钮
- [examples/ipc-console](examples/ipc-console/) — 外部控制台，实时调控另一进程的监控行为

## 构建与测试

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64   # 或 -G Ninja + MinGW 工具链
cmake --build build --config Release
ctest -C Release                                        # 冒烟测试（在 build 目录内执行）
```

## 文档索引

PRD 全集见 [docs/](docs/)（01~23），重点：

- 接入与适配：[23_集成与适配说明.md](docs/23_集成与适配说明.md)（spdlog / Qt 版本兼容 / MetaCall ABI）
- 开销证据：[22_性能基准报告.md](docs/22_性能基准报告.md)
- 事件模型：[05_全局事件耗时监控.md](docs/05_全局事件耗时监控.md)、[06_MetaCall跨线程信号监控.md](docs/06_MetaCall跨线程信号监控.md)
- 版本规划：[21_版本规划与交付物.md](docs/21_版本规划与交付物.md)
