# Qt 卡顿排查辅助器 PRD 09：setStyleSheet 监控

## 1. 目标
发现运行时频繁调用 `setStyleSheet()` 导致的样式重算和 UI 更新压力。

## 2. 关键技术约束
`QWidget::setStyleSheet()` 不是可通过 Qt 全局事件过滤器直接捕获的事件，因此不能承诺通过 eventFilter 无侵入拦截任意调用。

本产品采用两级方案：
1. 官方 API 可观测范围内的事件监控。
2. 对业务可控代码提供轻量埋点包装接口。

## 3. 推荐接口
```cpp
QssStyleWatcher::setStyleSheet(widget, style);
```

内部负责：
- 记录开始时间。
- 调用 QWidget::setStyleSheet。
- 记录结束时间。
- 统计对象、文本长度和耗时。

## 4. 统计
- 总调用次数。
- 单控件调用次数。
- 单次最大耗时。
- 周期调用次数。

## 5. 设计原则
不使用修改 Qt 源码、二进制 Hook 等高风险方案。
