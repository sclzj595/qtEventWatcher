# Qt 卡顿排查辅助器 PRD 11：动态配置 Watch_Fun

## 1. 位定义

```cpp
enum WatchBitFlag
{
    WatchFun_None       = 0,
    WatchFun_OneEveDeal = 1 << 0,
    WatchFun_MetaCall   = 1 << 1,
    WatchFun_EventStat  = 1 << 2,
    WatchFun_QssMonitor = 1 << 3
};
```

## 2. 示例
- `Watch_Fun=1`：慢事件。
- `Watch_Fun=2`：MetaCall。
- `Watch_Fun=4`：周期统计。
- `Watch_Fun=8`：QSS。
- `Watch_Fun=15`：全部功能。

## 3. 动态刷新
默认每 60 秒轮询一次环境变量。

## 4. 现实约束
Windows 环境变量通常由进程启动环境继承，运行中直接修改系统环境变量不一定能被当前进程看到。因此“无需重启即可修改”必须采用组件自己的运行时配置源，或者明确限定为可读取的进程级环境变量机制。

建议 PRD 将 Watch_Fun 定义为统一配置键，并预留配置文件 / IPC / setter 作为动态更新后端。

## 5. INI 配置热更新（V1 定稿）
### 5.1 机制
采用 **INI 文件轮询 + 配置变更检测**，暂不引入 IPC：

```text
WatchConfig
    ├── 当前配置
    └── 每 ConfigPollIntervalMs 检查文件修改时间
            └── 变化 → 重新读取 INI → sanitize → 原子替换 Values → 新配置生效
```

运行中修改 `Watch_Fun=1` → `Watch_Fun=3`、`SlowEventThresholdMs=30 → 10` 无需重启程序或重建 CusApplication。

### 5.2 边界与优先级
- **环境变量不参与热更新**：环境变量属进程启动配置，INI 属运行期可变配置；热更新只 reload INI。
- 优先级保持：Runtime Setter > INI > Environment > Default。
- V1 **不做 Runtime Override 层**：setter 继续作为当前运行时直接修改 API；INI reload 重算 Default → Environment → INI 后整体覆盖。已知的“setter 后被 60 秒 reload 覆盖”行为 V1 接受，将来确有需要再加 Override 层。

### 5.3 线程安全
- 沿用 `QReadWriteLock`：getter 走 QReadLocker，reload / setter 走 QWriteLocker。
- reload 一次性构造新 Values 并校验，完成后**整体替换**（`m_values = newValues`），禁止逐字段修改让其他线程看到半更新状态。

### 5.4 容错与 UI
- 单个配置项非法只回退该项默认值（如 `SlowEventThresholdMs=-10 → 30ms` 并提示），不得因单个错误导致整个监控系统失效。
- Runtime / Settings 页展示：配置文件路径、加载状态、最后修改时间、最后检查时间、[立即重新加载]；非法项列出“输入值 → 已回退值”。
