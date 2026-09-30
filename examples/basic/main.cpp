#include "MainWindow.h"

#include "CusApplication.h"
#include "MetaCallFilter.h"
#include "QssStyleWatcher.h"
#include "StyleTokens.h"
#include "WatchLogMacros.h"
#include "WatchLogger.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <QThread>

#include <memory>

int main(int argc, char* argv[])
{
    qt_event_watcher::CusApplication app(argc, argv);
    QApplication::setApplicationName("QtEventWatcherBasicDemo");

    // 初始化监控日志（目录不存在时自动创建）
    qt_event_watcher::WatchLogger::instance().initialize("./logs", "QtEventWatcher");

    // --autofreeze <ms>：启动 1.5s 后主线程人为冻结（FreezeWatch 自动化实测入口；
    // 不带参数则无任何自动行为，GUI 交互不受影响）
    // --stress <count>：1.5s 后以 50ms 节拍合成慢事件告警（每拍 64 条 DEBUG 级，
    // 文件/控制台 sink 用户级过滤不刷屏，RecordSink debug 全量采集 → 环形缓冲
    // → UplinkClient 上行；V4 D 线断点续推/背压丢弃验收的风暴源），发完自动退出
    // --spin <ms>：1.5s 后主线程 busy-loop 指定时长（CPU 单核满转 + 消息泵停摆，
    // 同帧喂 T1 冻结与 T1b CPU 启发——Scout 外部探针的验证靶，V7 S1）
    int autoFreezeMs = 0;
    int stressCount = 0;
    int spinMs = 0;
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--autofreeze" && i + 1 < argc)
            autoFreezeMs = QByteArray(argv[i + 1]).toInt();
        else if (QByteArray(argv[i]) == "--stress" && i + 1 < argc)
            stressCount = QByteArray(argv[i + 1]).toInt();
        else if (QByteArray(argv[i]) == "--spin" && i + 1 < argc)
            spinMs = QByteArray(argv[i + 1]).toInt();
    }

    // 主题跟随系统（PRD 19 §9）；外部 QSS 文件每次启动按当前令牌重建，
    // 保证"外部文件加载"演示路径与运行时令牌同源
    const QString theme = style_tokens::systemPrefersDark() ? QStringLiteral("dark")
                                                            : QStringLiteral("light");
    QFile::remove(QStringLiteral("./res/style.qss"));
    style_tokens::ensureStyleFile(QStringLiteral("./res/style.qss"), theme);
    {
        auto* watcher = qt_event_watcher::QssStyleWatcher::instance();
        watcher->beginLoadQss("./res/style.qss");
        QFile file("./res/style.qss");
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            app.setStyleSheet(QString::fromUtf8(file.readAll()));
        watcher->endLoadQss();
    }

    qt_event_watcher::MainWindow window;
    window.resize(1024, 720);
    window.show();

    // 顶栏监控状态点：任一监控位开启即显示"监控中"（含 V3 B 线冻结位 bit4）
    window.setWatchStatus(app.watchEnabled(1) || app.watchEnabled(2) ||
                          app.watchEnabled(4) || app.watchEnabled(8) ||
                          app.watchEnabled(16));

    // MetaCallFilter 演示：屏蔽 EphemeralWorker（槽内自毁 sender）的慢 MetaCall 告警，
    // 其余 sender（QTimer / invokeMethod）不受影响
    if (auto* filter = app.metaCallFilter())
        filter->addSuppressedSender("qt_event_watcher::EphemeralWorker");

    if (autoFreezeMs > 0) {
        QTimer::singleShot(1500, &app, [autoFreezeMs]() {
            QThread::msleep(autoFreezeMs);	// 心跳停跳 → FreezeWatch 三态告警
        });
    }

    if (spinMs > 0) {
        QTimer::singleShot(1500, &app, [spinMs]() {
            // 主线程 busy-loop：CPU 单核满转且消息泵停摆（Scout T1/T1b 双验证靶）
            QElapsedTimer clock;
            clock.start();
            volatile double sink = 0.0;	// 防空转被优化掉
            while (clock.elapsed() < spinMs) {
                for (int i = 0; i < 20000; ++i)
                    sink += i * 0.5;
            }
        });
    }

    if (stressCount > 0) {
        auto* stressTimer = new QTimer(&app);
        auto sent = std::make_shared<int>(0);	// lambda 多拍共享计数
        QObject::connect(stressTimer, &QTimer::timeout, &app, [&app, stressTimer, sent, stressCount]() {
            for (int i = 0; i < 64 && *sent < stressCount; ++i, ++*sent) {
                const double cost = 3.0 + (*sent % 20) * 0.37;
                QEW_LOG_DEBUG(
                    "[EventWatcher] slow event "
                    "receiver=StressGen object=stress{} event=Timer type=43 depth=1 "
                    "costMs={:.3f} exclusiveCostMs={:.3f} curThread=0x0 recvThread=0x0 "
                    "match=true thresholdMs=2 stack=",
                    *sent % 7, cost, cost);
            }
            if (*sent >= stressCount) {
                stressTimer->stop();
                // 缓冲 3s：给 UplinkClient 最后几拍 flush +（断线场景）手动拉起
                // aggregator 留出重连推送窗口，避免发送完成即退出截断验证
                QTimer::singleShot(3000, &app, [&app]() { app.quit(); });
            }
        });
        QTimer::singleShot(1500, &app, [stressTimer]() { stressTimer->start(50); });
    }

    return app.exec();
}
