# Qt 卡顿排查辅助器 PRD 15：工程结构与 API

## 1. 推荐目录

```text
QtEventWatcher/
├── include/
│   └── qt_event_watcher/
│       ├── cusapplication.h
│       ├── qssstylewatcher.h
│       ├── watchconfig.h
│       └── watchlogger.h
├── src/
│   ├── cusapplication.cpp
│   ├── eventwatcher.cpp
│   ├── metacallwatcher.cpp
│   ├── eventstatistics.cpp
│   ├── qssstylewatcher.cpp
│   ├── watchconfig.cpp
│   └── watchlogger.cpp
├── cmake/
├── tests/
├── docs/
└── CMakeLists.txt
```

## 2. 核心 API
```cpp
class CusApplication : public QGuiApplication
{
public:
    using QGuiApplication::QGuiApplication;
    bool notify(QObject* receiver, QEvent* event) override;
};
```

```cpp
class QssStyleWatcher
{
public:
    static QssStyleWatcher* instance();

    void beginLoadQss(const QString& filePath);
    void endLoadQss();

    bool setStyleSheet(QWidget* widget, const QString& style);
};
```

## 3. 模块原则
公开 API 少而稳定，内部实现可以随着 Qt 版本适配变化。
