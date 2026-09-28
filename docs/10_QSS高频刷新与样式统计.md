# Qt 卡顿排查辅助器 PRD 10：QSS 高频刷新与样式统计

## 1. 高频刷新
默认统计周期 1000ms。

记录：
- Widget 类名。
- objectName。
- 调用次数。
- 最大单次耗时。
- 样式文本最大长度。

## 2. TOP 排行
周期结束输出高频 Widget TOP N。

## 3. 文件资源统计
累计：
- QSS 文件数量。
- 最大文件大小。
- 最大 IO 耗时。
- 最大 setStyleSheet 耗时。
- 总 QSS 加载次数。

## 4. 典型告警
```text
FrequentStyleUpdate | Widget: StatusBar | Count: 28 | Window: 1000ms
SlowSetStyleSheet | Widget: MainWindow | StyleLen: 48620 | Cost: 96ms
```
