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

## 7. S2 交付记录（2026-09-30）

**T2 CDP 长任务探针已交付**：examples/scout/ 新增 CdpLongTaskProber（.h/.cpp），
scout 新增 `--cdp-port` / `--cdp-target` / `--cdp-threshold` 三参数，支持
**纯 CDP 模式**（免 pid/name，CDP 端口即目标标识）。Qt WebSockets 为可选依赖
（CMake QUIET 探测，缺失时禁用探针不阻断构建，main.cpp 经 `QEWT_SCOUT_CDP` 条件接线）。

### 7.1 T2 前置验证（真 Electron 靶）

npm(npmmirror) 安装 Electron 152 为可控靶（CDP 9224），node22 内置 WebSocket
零依赖探针四轮迭代，踩实四个语义边界：

1. **Runtime.evaluate 内忙转不产生 longtask 条目**——debugger 通道任务不算页面
   任务；探针只消费页面真实任务，反而零自身噪声
2. **data: URL 页面 inline script 不执行**（Electron 152）——测试靶必须 file://
3. **页面 hidden 时 observer 投递停滞**——条目延迟不丢失；联调靶设
   backgroundThrottling:false 规避
4. **已运行 Electron 全局单例吞 --remote-debugging-port**——目标必须带参冷启动

msedge（Edg/154）旁证先行 PASS——Chromium 系全覆盖。

### 7.2 S2 实现（CdpLongTaskProber）

- 发现：QNetworkAccessManager GET `/json/list` → 选 type=page target
  （`--cdp-target` 按 url/title 子串过滤，空取首个）；不可达 2s 静默重试
  （仅首次告警）
- 注入：QWebSocket 连 webSocketDebuggerUrl → Runtime.evaluate 安装
  PerformanceObserver(longtask, **buffered:true**——注入即回放历史条目）
- 回读：500ms 周期 `JSON.stringify(__qewt_lt.splice(0))` 增量清窗；命中
  `need-inject` 哨兵（页面导航重置 window）自动重注入
- 断连：disconnected 信号统一驱动重发现（errorOccurred 为 Qt6.5+ 专属信号，
  跨版本不可用——Qt5.15 编译期实证）
- 行格式：复用 `[EventWatcher] slow event` 前缀（KindSlowEvent，HTML/聚合/
  导出零改动），`event=cdpLongTask type=98 source=scout-cdp url=<page url>`

### 7.3 E2E 实测证据（electron-lab 靶，每 2s 忙转 120ms）

- **检测精度**：costMs=120/121 与靶忙转时长精确吻合（29 条 min=120 max=121）
- **断连重发现**：kill@10s → `page disconnected, rediscovering in 2000ms` →
  重启后自动重连+重注入（间隔 4.5s）→ 记录恢复，两端行为级实证
- **上行账目**：aggregator `received=29 == pushed=29 == lastSeq=29, dropped=0`，
  seq 零缺口，non-cdp=0，health 自洽——**零改动闭环 S2 版成立**
- **buffered 红利**：注入首拍即回放历史长任务（19 条突发），页面启动即有上下文

### 7.4 质量门禁

- 四矩阵构建全绿（Qt WebSockets 四矩阵 Qt 官方二进制均自带）
- 回归 16/16 + static-check 0 findings（见实施当轮记录）

### 7.5 已知边界

- 探针侧无风暴抑制：持续高频长任务页面逐条发行，依赖 RecordSink 环形 4096
  吸收；可放宽 `--cdp-threshold` 降噪
- hidden 页面条目延迟投递（Chromium 节流语义，非探针缺陷）
- CDP 需目标配合（带参冷启动）；无 CDP 的任意程序回落 S1 的 T1/T1b

## 8. Tauri 支持实证（2026-10-01，v6.2.0）

用户诉求：卡顿检测不能只覆盖 Electron，**Tauri 也要能用**。架构差异与验证结论：

### 8.1 Tauri 架构与探针覆盖面

Tauri = Rust 主进程（tao 事件循环）+ **系统 WebView2**（不打包 Chromium），
进程树为 `app.exe → msedgewebview2.exe（浏览器进程）→ renderer/GPU（孙进程）`：

| 探针 | Tauri 覆盖 | 机制 |
|---|---|---|
| T1 窗口冻结 | ✅ 直接适用 | tao 主线程即 Win32 消息泵，阻塞即"未响应" |
| T1b CPU 启发 | ✅ **需孙进程枚举**（本轮修复） | renderer 忙转在孙进程；`childPids`（一层）→ `descendantPids`（BFS 3 层） |
| T2 CDP 长任务 | ✅ **WebView2 原生支持** | WebView2 全兼容 CDP；调试端口经环境变量注入（见下） |

**CDP 端口注入**（Tauri 无命令行透传，WebView2 专用机制）：

```powershell
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=9224"
.\app.exe
```

页面 target URL 形如 `http://tauri.localhost/`（tauri 2.12.1 / wry 0.57 / WebView2 154 实测）。

### 8.2 验证靶与 E2E 实测

验证靶 `%TEMP%\qewt-tauri-lab`（tauri 2 最小工程，rsproxy 镜像构建）：
页面 JS 每 2s 忙转 120ms（长任务流）；t=4s 主线程忙转 2.5s（冻结窗）；
t=6.9s 双 worker 线程忙转 5s（CPU 窗）——确定性时间线。

`scout --name qewt-tauri-lab.exe --threshold 2000 --cpu-threshold 95 --cdp-port 9224 --uplink ...` 一体三探针：

- **T2**：`page connected url=http://tauri.localhost/` → longtask 行
  `costMs=120` × 11 条精确吻合
- **T1**：`freeze started stalledMs=2000` + `recovered totalMs=363`
  （scout 语义：started 行 stalledMs=SendMessageTimeout 阈值即下界；
  recovered 行 totalMs=确认恢复差值，真冻结 2.5s）
- **T1b**：`cpuSpin costMs=98.8`（主线程忙转段）。两段忙转间隙仅 0.5s
  （`run_on_main_thread` 异步派发，双 burst 间隔=2 迟滞拍 < 4 拍收口线）
  → **同一 busy episode 合并为单条告警**，产品语义正确
- **上行**：aggregator `received=14 == pushed=14 == lastSeq=14, dropped=0`
  （Slow=12 + Freeze=2），Tauri 记录与自监控/Qt 程序走完全相同链路

### 8.3 本轮代码变更

- `TargetResolver::childPids` → **`descendantPids`**（BFS 3 层后代枚举，
  单快照建图 + 去重防 pid 环）——修 T1b 对 Tauri 漏采孙进程 renderer 的真缺口；
  Electron（直接子进程）语义不变
- 四矩阵 scout 构建全绿 + static-check 0 findings

## 9. S3 交付记录：独立可视化仪表盘（2026-10-02）

`examples/scout-dashboard/`（console 工具，冷路径）：读 aggregator `--out` JSON
快照 → 单文件自包含 HTML 仪表盘。纯文件消费方：不链 QtEventWatcherCore，
仅 Qt Core 公共 API（QJsonDocument/QJsonArray/QTime 在 Qt5.15/6.5 零适配）。

### 9.1 分层与交付物

- **数据层** `DashboardModel.{h,cpp}`：loadModel（JSON → 模型）、classifyRecord
  （kind 枚举 + scout 事件细分，type/event/source 三信号冗余判定）、freeze
  started→recovered/lost 配对、relMs 跨午夜防御（t0 = **首条可解析记录**口径，
  非 min(msecs)——min 口径跨午夜被次日小值击穿）、fields 全空 → Other 兜底
  （损坏记录不落业务泳道）、filterModel（kinds 白名单 + pid 过滤，freezeSpans
  仅当 kinds 含 3 保留）、escapeHtml / embedJsonSafe
- **渲染层** `DashboardHtml.{h,cpp}`：exportDashboard 五章节静态 HTML（会话概览
  与健康度 / 冻结时间轴 / CDP 长任务 per-url 汇总+CSS 条形直方 / CPU 与慢事件
  / 明细折叠）+ 双形态共存：QEWT_DASHBOARD_EMBED 定义时追加 buildEmbedJson
  （`<script type="application/json" id="dashboard-data">`）+ buildInteractiveShell
  （style.css/dashboard.js 经 AUTORCC 内嵌）；资源缺失 stderr 警告降级纯静态
- **交互层** `dashboard.js`（~300 行 vanilla IIFE）/ `style.css`：泳道时间轴
  （freeze span 状态条：recovered 实心蓝 / lost 灰 / ongoing 橙条纹）、wheel
  光标中心缩放 0.2x–50x、拖拽平移、双击复位、类型筛选（含 clsOff[0] 联动清空
  时间轴）、pid 会话筛选、明细分页 100 条/页、悬停 tip——零依赖零构建链
- **CLI** `main.cpp`：`--in`（必填）`--out`（默认 in 去后缀 .html）`--title`
  `--pid` `--kind <csv 0..3>`；stdout 摘要与渲染/内嵌 JSON 三处共用 filterModel
  同一口径

### 9.2 安全设计（XSS 三层分工 + script 注入铁律）

- C++ 静态位：escapeHtml（& \< \> " 四字符，对齐既有口径）
- JSON 内嵌位：embedJsonSafe（Compact 序列化后 `</` → `<\/`，封死
  `</script>` 提前闭合；`\/` 为 JSON 合法转义，fromJson 回读语义不变）
- JS DOM 位：esc()
- **HTML script 块注入铁律**：进入 `<script>` 的任何内容（含 JS 注释）禁止出现
  字面 `</script` 序列——HTML parser 不管 JS 语义（本轮实测：dashboard.js 头
  注释含字面闭合标签导致脚本块被提前截断，剩余 JS 源码成 DOM 文本）

### 9.3 E2E 实测证据（混合会话，qt5152-msvc）

采集链：aggregator（--name TestAggE2E --duration 50 落盘）← 双会话上行：
basic_demo（WATCH_FUN=31 + --autofreeze 3000，自监控）+ scout（--name
qewt-tauri-lab.exe --cdp-port 9226 --uplink TestAggE2E --duration 25000，
外部探针；tauri-lab 经 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS 开 CDP）。

- CLI 摘要：`sessions=2 records=197 freeze_spans=1 cdp=14 cpu=1`
- 自监控会话（pid 42288）：182 条（kind0×175 + kind1×1 + kind2×3 + kind3×3），
  health 快照齐全，freeze 配对 1 span（--autofreeze 3000）
- scout 会话（pid 56584）：15 条 kind0 = cdpLongTask×14（costMs=120 精确，
  url=http://tauri.localhost/）+ cpuSpin×1（costMs=187.5 = CPU% 口径）
- 概览表「外部探针/自监控」双徽章各 1 次互不串；内嵌 JSON 复核：scout 源记录
  全部归属 pid 56584，basic_demo 会话零 scout 源
- file:// 断网可用：grep 确认零外部资源引用（无 script src/link/img/url(http)）
- CDP 自动化手测 8 项（headless Edge + Runtime.evaluate）：渲染 lanes/span、
  悬停 tip、光标中心缩放、拖拽、双击复位、类型筛选联动、pid 筛选、分页态

### 9.4 质量门禁

- ScoutDashboardTests（qewt::Case 自建框架，8 用例 89 checks）：四矩阵全绿
  （数据层直编入测试 target，避免 LNK2019）
- 四矩阵 scout-dashboard 构建 + E2E JSON 复放出图摘要一致（qt5.15.2/6.5.3 ×
  MSVC/MinGW）
- static-check 0 findings（cppcheck 2.22.0）
- 四矩阵回归（scripts/run_regression.ps1 快速档）PASS

### 9.5 已知边界与实现注记

- CMake `if (EXISTS assets.qrc)` 在 configure 期评估——新建 qrc 后旧 build 目录
  必须 `cmake -S -B` 重新配置才感知（实测 vcxproj 无 EMBED 宏即此因）
- cpuSpin 行槽位复用 slow event 行格式：costMs 实填 CPU 占用百分比、thresholdMs
  实填阈值百分比——仪表盘列头诚实标注
- QJsonArray 无 reserve()（Qt 6.5）；QTextStream Qt5 须 setCodec("UTF-8")
- E2E 场景 B（electron-lab 纯 CDP）为可选项，其 CDP 数据通路与混合会话完全
  一致（S2 §7.3 已单独验证），本轮裁剪跳过

## 10. Linux 探针适配交付记录（2026-10-02，v6.4.0）

里程碑 #8：Scout 三探针 Linux 全可用 + CI ubuntu 双矩阵门禁。四阶段推进
（L1 core 调用栈 → L2 /proc 进程/CPU → L3 X11 冻结 → L4 收口），每阶段
可编译可验证、双端推送。

### 10.1 L1：StackCapture Linux 化（src/event/StackCapture.cpp）

- `#elif defined(__linux__)`：glibc `backtrace()`（execinfo.h，≤32 帧同 kMaxFrames）
  + `dladdr()` 解析 Dl_info.dli_fbase/dli_fname
- moduleName(void* base) 缓存语义对齐 Windows 版 HMODULE 缓存（mutex +
  unordered_map，告警风暴 32 帧×N 条/s 降本）；basename 取 '/' 后段，
  空格替换 '_'（日志 key=value 单 token 完整），UTF-8 原样保留
- offset -= fbase：RVA 口径与 Windows 版一致（addr2line 可直接归因）
- src/CMakeLists.txt：UNIX 下链接 `${CMAKE_DL_LIBS}`——glibc 2.34 前 dl* 符号
  在独立 libdl（麒麟 V10 glibc 2.28 等国产环境），该变量在无需显式 dl 的平台为空，跨发行版安全
- 其余平台保持空 stub（`#else` 兜底）

### 10.2 L2：TargetResolver / CpuSampler /proc 化（examples/scout/）

- TargetResolver.h `#elif defined(Q_OS_UNIX)`：QDir("/proc") 数字目录枚举；
  探活 = `/proc/<pid>` 存在性；comm 读 `/proc/<pid>/comm`（注明内核 15 字符
  截断边界——按 comm 匹配目标时截断段不影响前缀匹配场景）；
  `/proc/<pid>/stat` 解析铁律 = comm 字段可含空格与 `)`，必须
  `lastIndexOf(')')` 锚定后再切字段（`')'` 后下标 11/12 = utime/stime）
- descendantPids：单轮 ppid 建图 + BFS 3 层去重，与 Windows 版语义一致
- CpuSampler：`cpuTimeMsOf` = (utime+stime) tick × `1000.0 / sysconf(_SC_CLK_TCK)`；
  差分/迟滞（连续 4 拍收口）/episode/`cpuSpin type=99 source=scout` 日志行
  逐字对齐 Windows 版；成员按平台分支（m_lastCpuMs double vs m_lastTotal100ns qint64）

### 10.3 L3：WindowFreezeProber X11 化（examples/scout/WindowFreezeProber.cpp）

- 探活协议：`_NET_WM_PING` ClientMessage 直接发目标窗口（propagate=False、
  event_mask=0 → 送达创建该窗口的 client，与 WM 探测同路径）；Qt/GTK 应用
  逐字段 echo 回 root（Qt 源码 `xev = *event; xev.window = root;` 数据不重写），
  探针 `XSelectInput(root, StructureNotifyMask)` 收副本——有无 WM 均成立。
  掩码位取证实录：Qt xcb 回发 event_mask=StructureNotify|SubstructureRedirect
  （qxcbwindow.cpp handleClientMessageEvent，整包 echo 仅改 window=root），
  探针须选 StructureNotifyMask（1L<<17）才有交集——误选 SubstructureNotifyMask
  （1L<<18）则 pong 永远收不到，恢复态判定失效（Run 20 实证：started 可判
  而 ongoing 无限拉长）；SubstructureRedirect 为 WM 独占掩码不可选
- token 防串扰：`g_pingToken` 原子计数（getpid() 作高位基座），逐拍 +1 防
  上一拍迟到 pong 污染本拍判定；匹配 message_type+format==32+token 三条件
- 窗口枚举：`_NET_CLIENT_LIST`（EWMH，有 WM 时权威）∪ XQueryTree root 直接
  子窗口（Xvfb 等无 WM 兜底），`_NET_WM_PID` 过滤（fmt=32 → long 数组，
  LP64 每元素 8 字节——size_t 混淆即读错）
- 三态状态机逐字复制 Windows 版（started/ongoing 1s 节流/recovered/lost +
  stalledMs 阈值下界口径）；XOpenDisplay 失败（Wayland native/无头）WARN 降级
  空转，CPU/CDP 探针不受影响
- CMake 接线：UNIX 下 `find_package(X11 QUIET)`，TARGET 存在才定义
  `QEWT_SCOUT_X11=1` 并链接 X11::X11，否则 message(STATUS) 禁用不阻断

### 10.4 CI linux-smoke job（.github/workflows/regression.yml）

- 双矩阵：qt5152-gcc（5.15.2/gcc_64）+ qt653-gcc（6.5.3/gcc_64）
- apt 依赖一次配齐：libgl1-mesa-dev（Qt5Gui 配置期 gl.h 硬依赖）+
  xcb 平台插件运行依赖全家桶（xkbcommon-x11/icccm/image/keysyms/rand/
  render-util/shape/xinerama/xkb/x11-xcb）+ **libxcb-cursor0（Qt 6.5 xcb
  插件硬依赖，QTBUG-110726）**
- e2e：basic_demo `--spin` 验证靶——cpu 步（`--cpu-threshold 95` 断言
  cpuSpin 行）；freeze 步（`--threshold 2000` 断言 started+recovered）。
  **关键结构约束：靶与 scout 必须同一 X server**——分开的 xvfb-run 是两个
  独立 display，_NET_WM_PING 跨 server 不可达；用单个 xvfb-run 包裹双进程
- 运行期：`LD_LIBRARY_PATH` 指向 aqt 包内 lib（随包 libicu* 等 soname 缺代
  兜底）；Smoke 前置 ldd 'not found' 检查
- 取证闭环（对齐 Windows job 先例）：configure.log/build.log/smoke.log/scout.log
  失败时 tail 落 GITHUB_STEP_SUMMARY + ::error:: 注解（匿名可读，不依赖登录
  拉日志）；Report configure errors 在 build.log 已存在时跳过（失败发生在
  configure 之后，避免误归因）

### 10.5 取证排障实录（CI 取证装置首轮实战）

- Run 11/12（L1/L2）：注解仅 "Process completed with exit code 1"，
  #step:7 "build.log missing" 曾被误读为 Smoke 失败——实为 Report build
  errors 步的从属注解；真失败点 = **Configure exit 1**（gl.h 缺失高嫌疑）
- Run 12+（68e7404 取证补全后）：注解给出完整链——Build 步
  `DataExporter.cpp:458:71: conversion from 'const int64_t' to 'const QVariant'
  is ambiguous` + gmake Error 链
- 根因：Linux LP64 下 int64_t = long，QVariant 的 int/qlonglong 两个整数
  构造器打平；MSVC 下 int64_t = long long 精确匹配，故四矩阵本地回归从未
  暴露——**跨平台首次 CI 编译即抓到 Windows 隐蔽缺陷**，门禁价值实证
- 修复：`static_cast<qlonglong>(period.periodMs)`（对齐同文件 frames 段先例；
  全文件唯一歧义点，其余绑定值均 int/QVariant）；MSVC + MinGW 8.1（GCC 同族）
  双编译器本地验证

### 10.6 质量门禁

- CI：linux-smoke 双矩阵（build + xvfb 冒烟 + cpuSpin e2e + freeze 三态 e2e）
  与 windows smoke 双矩阵全绿
- 本地四矩阵回归（scripts/run_regression.ps1 快速档）16/16 PASS
  （build/smoke/unit 96 checks/benchmark × qt5.15.2/6.5.3 × MSVC/MinGW）
- static-check 0 findings（cppcheck 2.22.0）
- Windows 零扰动：L1/L2/L3 各阶段后 MSVC 构建全绿，探针行为无变化

### 10.7 已知边界

- Wayland native 应用：X11 协议不可达，冻结探针诚实降级（CPU/CDP 不受影响）；
  XWayland 兼容层下与 X11 一致
- comm 15 字符截断：长进程名按 comm 精确匹配可能 miss（`--pid` 模式不受影响）
- 探针侧无风暴抑制（环形 4096 吸收 + 阈值放宽，同 S2 §7 边界）
- CDP 长任务探针（T2）Linux 同样适用（QNetworkAccessManager + QWebSocket
  均跨平台），本轮未单独新增 Linux e2e 步（Chromium 靶依赖较重，后续按需补）
