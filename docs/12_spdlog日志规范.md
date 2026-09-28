# Qt 卡顿排查辅助器 PRD 12：spdlog 日志规范

## 1. 原则
所有工具日志统一通过 spdlog 输出，不使用 qDebug 作为正式日志通道。

## 2. 日志级别
- INFO：周期统计、启动配置。
- WARN：慢事件、慢 MetaCall、慢 QSS。
- ERROR：监控初始化失败、日志初始化失败。
- DEBUG：开发调试信息，默认关闭。

## 3. 示例
```text
[WARN] SlowEvent | Type: 43 (MouseButtonPress) | Cost: 126ms | Receiver: MainWidget
[WARN] SlowMetaCall | Cost: 105ms | SignalId: 8 | Sender: DataManager | Receiver: UIPanel
[INFO] EventStat(1000ms) | Type: 10 (Timer) | Count: 128 | TotalCost: 86ms
[WARN] SlowQssLoad | File: ./res/style.qss | Size: 124KB | IO: 8ms | Style: 142ms | Total: 150ms
```

## 4. 日志策略
- 支持控制台。
- 支持文件。
- 支持 rotating file sink。
- 支持日志级别控制。
- 监控组件不能因为日志写入异常影响业务。
