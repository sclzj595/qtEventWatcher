#include "MainWindow.h"

#include "CusApplication.h"
#include "MetaCallFilter.h"
#include "QssStyleWatcher.h"
#include "StyleTokens.h"
#include "WatchLogger.h"

#include <QFile>

int main(int argc, char* argv[])
{
    qt_event_watcher::CusApplication app(argc, argv);
    QApplication::setApplicationName("QtEventWatcherBasicDemo");

    // 初始化监控日志（目录不存在时自动创建）
    qt_event_watcher::WatchLogger::instance().initialize("./logs", "QtEventWatcher");

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

    // 顶栏监控状态点：任一监控位开启即显示"监控中"
    window.setWatchStatus(app.watchEnabled(1) || app.watchEnabled(2) ||
                          app.watchEnabled(4) || app.watchEnabled(8));

    // MetaCallFilter 演示：屏蔽 EphemeralWorker（槽内自毁 sender）的慢 MetaCall 告警，
    // 其余 sender（QTimer / invokeMethod）不受影响
    if (auto* filter = app.metaCallFilter())
        filter->addSuppressedSender("qt_event_watcher::EphemeralWorker");

    return app.exec();
}
