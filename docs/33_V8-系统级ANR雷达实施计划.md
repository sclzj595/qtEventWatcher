# 33. V8 实施计划：系统级 ANR 雷达（Scout --radar）

> V7 Scout 三探针（T1 冻结 / T1b CPU / T2 CDP）交付后，用户提出产品扩展方向：
> "不只是 Qt 程序，所有桌面端软件的卡顿都要能检测"。V8 把 scout 从"点对点"
> 探针升级为"面"级守护：一条命令监护全机所有 GUI 程序的消息泵响应性。

## 1. 背景与产品定位

V7 scout 是点对点外部探针：一次盯一个目标（`--pid`/`--name`）或一组 CDP 页面。
真实桌面环境里用户往往不知道"哪个进程"会卡——等卡了再找 pid 已然被动。

V8 雷达模式（`scout --radar`）常驻守护：

- 每拍全量发现"拥有可见顶层窗口"的进程集（天然覆盖 Qt/Electron/WPF/Win32/GTK，
  天然过滤系统服务与无窗口后台进程）
- 对每个目标复用 T1 探活原语（Windows `SendMessageTimeoutW(WM_NULL)` /
  Linux X11 `_NET_WM_PING`），三态状态机逐字段对齐 WindowFreezeProber
- 告警行走既有 `[FreezeWatch]` 前缀协议 → RecordSink → WatchRecordStore →
  UplinkClient → aggregator/HTML/仪表盘，**消费侧零改动渲染**

## 2. 顶层设计

```
RadarDiscover（发现：窗口归属 GUI 进程全集，每拍全量枚举）
        │  pid→进程名 / pid→顶层窗口句柄
        ▼
RadarScheduler（单线程常驻轮询 worker）
   ├─ 增量管理：新目标入册 / 消失目标 freeze lost 收口
   ├─ 逐目标顺序探活：复用 T1 原语（不随目标数增长线程/fd）
   └─ 三态状态机 per-target：started → ongoing*(1s 节流) → recovered
        │  [FreezeWatch] freeze ... receiver=name@pid radar=1
        ▼
WatchLogger → RecordSink → WatchRecordStore → UplinkClient → aggregator/HTML/仪表盘
```

## 3. 关键设计决策

1. **单线程多目标轮询**：全机目标共享一个 worker 线程（沿 EventWatchdog/
   WindowFreezeProber 线程先例），逐目标顺序探活。hung 目标最长阻塞 threshold，
   拍耗 ≈ N×ε + k_hung×threshold；`stop()` 在探活循环内逐目标检查，退出延迟
   ≈ 单窗口探活上界。
2. **receiver=name@pid 实例级唯一**：freeze 三态配对按 receiver 键合——雷达下
   同名多开（如多个 basic_demo.exe）若共享 receiver 会交叉串扰，实例级唯一
   串（name@pid）保证配对正确且展示自带 pid 归因。
3. **GUI 进程判定 = 拥有可见顶层窗口**：Windows 单趟 EnumWindows +
   `IsWindowVisible`；Linux X11 顶层窗口枚举 + `_NET_WM_PID` 归属（Qt/GTK
   建窗时均写该属性）。语义上"会卡出未响应的进程"正是"有窗口的进程"。
4. **风暴边界（v1 接受）**：per-target ongoing 1s 节流；系统级全面冻结的最坏
   上界 = N 行/s——真实系统级事件值得"吵"，radar=1 字段供消费侧过滤聚合。
5. **CPU 探针不纳入 v1**：全机 cpuSpin 噪声大（编译/索引都会触发），freeze 才
   是 ANR 本义；二期再评估 per-target CpuSampler 复用。
6. **日志行协议零改动**：RecordSink `parseFields` 通用 key=value 分词，
   `radar=1`/`pid=N` 等 token 对既有消费方（HTML 明细/CSV/SQLite/仪表盘）纯增量。
7. **进程名缓存**：nameCache 只对新出现 pid 解析（Windows 单次 Toolhelp 快照
   建全表），消失项随拍清理，避免每拍全量快照开销。

## 4. 阶段划分（R1→R5）与验证门

| 阶段 | 内容 | 验证门 |
|------|------|--------|
| R1 | RadarDiscover（Windows EnumWindows+Toolhelp 单快照 / Linux 复用 X11Probe）+ X11Probe.h 从 WindowFreezeProber.cpp 抽出共享 | 四矩阵编译 |
| R2 | RadarScheduler（单线程轮询 + per-target 三态状态机 + 增量管理 + freeze lost 收口） | 四矩阵编译 + 回归 |
| R3 | scout CLI 接线（--radar / --radar-exclude）+ CMake + 用法文本 | static-check 0 findings |
| R4 | 本地 Windows e2e（双 basic_demo 靶：发现/started/ongoing/recovered/lost 全链）+ CI（linux radar e2e + 四矩阵回归） | e2e 断言全绿 |
| R5 | 发布收口：docs 交付记录 + CHANGELOG v6.5.0 + README + tag 双端推送 + GitHub Release + 工作台 | 双端绿 + 门面闭环 |

## 5. CLI 规格

```
scout --radar [--threshold ms=2000] [--interval ms=250]
          [--radar-exclude <name1,name2>] [--uplink <server>] [--flush-ms ms] [--duration ms]
```

- `--radar` 与 `--pid`/`--name` 互斥（发现自动进行）；可与 `--cdp-port` 并存
  （全机冻结 + 页面级长任务互补）
- `--radar-exclude`：进程名子串排除表，逗号分隔，大小写不敏感（如排除
  explorer.exe 等桌面进程的噪声）

## 6. 风险评估

1. **拍耗退化**：hung 目标每拍各占满 threshold——k 个 hung 目标时有效探测周期
   退化为 k×threshold，诚实语义（卡死的目标本来就该占满探测预算），不丢告警
   只降采样率。
2. **pid 复用致进程名陈旧**：nameCache 只增不刷新，pid 被系统复用给不同 exe 时
   receiver 名可能陈旧——监控会话内概率极低，v1 接受（已知边界）。
3. **X11 无 WM 环境**：_NET_CLIENT_LIST 缺失走 root 子窗口兜底（T1 已验证）；
   无 _NET_WM_PID 的窗口收不进（Qt/GTK 应用均写该属性，e2e 覆盖）。
4. **Wayland native**：同 T1 诚实降级——雷达冻结探测禁用（日志明示），CPU/CDP
   不受影响。
5. **Windows 多桌面/虚屏幕**：EnumWindows 只见调用桌面的顶层窗口，远程会话/
   多用户场景各自跑 scout 实例。

## 7. 已知边界（v1）

- CPU/CDP 探针不在雷达模式（单目标语义，见决策 5）
- 雷达发现不区分虚拟桌面（可见顶层窗口即收）
- exclude 按进程名子串，不支持 pid 排除（自身 pid 代码内强制排除）

## 8. 交付记录（2026-10-02，v6.5.0）

### 8.1 代码落地（R1-R3）

| 文件 | 内容 |
|------|------|
| `examples/scout/X11Probe.h`（新） | X11 探活原语从 WindowFreezeProber.cpp 抽出共享：pingWindow / collectTargetWindows / pingToken；token 改函数局部静态单例（跨 TU 防串扰）；collectTargetWindows 契约扩展：空 pid 集 = 全收（带 _NET_WM_PID 的窗口），T1 调用方不受影响 |
| `examples/scout/RadarDiscover.h`（新） | 全机发现：Windows 单趟 EnumWindows 可见顶层窗口 + 单次 Toolhelp 快照建 pid→name 全表；Linux 复用 X11Probe 空 pid 集全收 + comm 取名 |
| `examples/scout/RadarScheduler.h/.cpp`（新） | 单线程常驻轮询：每拍发现→增量管理（消失目标 freeze lost 收口 + 名字缓存清理）→逐目标顺序探活→per-target 三态状态机；告警行 `[FreezeWatch]` 前缀 + `receiver=name@pid` 实例级唯一 + `radar=1` 标注；Q_OS_WIN / Q_OS_UNIX+QEWT_SCOUT_X11 / stub 三段式 |
| `examples/scout/main.cpp` | `--radar` / `--radar-exclude` 接线；与 --pid/--name 互斥校验；[Scout] 日志行加 radar/radarExclude 字段；aboutToQuit 优雅停机 |
| `examples/scout/CMakeLists.txt` | 新增四文件入 SCOUT_SOURCES |

### 8.2 质量门禁

- 四矩阵回归 16/16 PASS（qt5152/qt653 × msvc/mingw：build+smoke+unit 96 checks+benchmark）
- static-check 0 findings（cppcheck 2.22.0）
- CI：linux-smoke 双矩阵新增 "Scout e2e - radar mode" 步骤（Xvfb 内雷达自动发现 busy-loop 靶 → freeze started/recovered + radar=1 断言）

### 8.3 本地 e2e 实证（Windows，qt5152-msvc）

- **场景 A（自然恢复）**：basic_demo --spin 6000 + 常驻 demo → 雷达自动发现 → `freeze started`（stalledMs=1500 阈值下界）→ ongoing 1s 节流 → `freeze recovered totalMs=4353`（spin 末自然恢复）
- **场景 B（目标消亡）**：basic_demo --spin 20000，freezing 中 kill → `freeze lost` 收口
- **意外红利**：雷达在开发机上持续捕获到真实系统级 ANR（RtkUWP.exe 长期 hung，逐拍 ongoing 上报）——产品本职工作的现场自证

### 8.4 排障实录：freeze lost 不可达的三层竞态（R4 实测发现）

kill 恰落在探活消息在途时，假 recovered 吞掉 freeze lost——三层递进定位：

1. **表层**：`SendMessageTimeoutW` 对垂死窗口（kill 与消息派发竞态）返回非 0 → 被判"响应了"→ recovered
2. **第二层**：以 `TargetResolver::resolvePids` 验存活无效——OpenProcess 对"已终止但句柄未释放"的进程对象仍成功
3. **根因层**：TerminateProcess 发起后内核对象置信号态有毫秒级滞后，SendMessage 返回后微秒级检查撞上拆除中窗口——`WaitForSingleObject(h, 0)` 返回 WAIT_TIMEOUT 假活

**修复**：Windows 改 `OpenProcess(SYNCHRONIZE|...)` + `WaitForSingleObject(h, 100)` 宽限判信号态；Linux 读 `/proc/<pid>/stat` state 排除 Z（zombie）态。修复后场景 B 稳定产出 freeze lost，场景 A 自然恢复不受影响（活进程 100ms 等待仅发生在 recovered 判定路径）。

**顺带加固**：雷达长驻、目标进出频繁——X11 枚举与探活间隙目标窗口随时消亡，BadWindow 触发 Xlib 默认错误处理器会**直接终止 scout 进程**；run() 装 no-op XSetErrorHandler，出错调用返回 0 走"不可探活"分支。

### 8.5 已知边界补充

- freeze lost 在"kill 恰落在探活在途"时经 aliveOf 宽限判定收口，仍有理论残余窗口（kill 落在 aliveOf 检查之后、下一拍发现之前的 <100ms）——语义诚实（目标确实消亡，下一拍 erase 兜底不补发）
- RtkUWP 类常驻 hung 进程每拍占满 threshold（拍耗退化语义，见 §6.1）——`--radar-exclude` 是当前答案

