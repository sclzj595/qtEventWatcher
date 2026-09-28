# Qt 卡顿排查辅助器 PRD 08：QSS 资源加载监控

## 1. 目标
定位外部 QSS 文件读取和 `setStyleSheet()` 造成的启动或运行时卡顿。

## 2. 埋点接口
提供：

```cpp
QssStyleWatcher::instance()->beginLoadQss(filePath);
// 业务读取文件 + setStyleSheet()
QssStyleWatcher::instance()->endLoadQss();
```

## 3. 采集
- 文件路径。
- 文件大小。
- IO 耗时。
- setStyleSheet / 样式处理耗时。
- 总耗时。

## 4. 说明
Qt 上层无法稳定、跨版本地直接获得“QSS 内部解析器每一步”的公开耗时，因此产品将“setStyleSheet 调用耗时”作为可观测代理指标，而不是声称能够精确拆解 Qt 私有样式引擎内部每个阶段。

## 5. 告警
超过 QSS 加载阈值时输出 WARN。
