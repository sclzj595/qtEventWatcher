// consume-test —— V4 C2 双模型验收载体（独立 CMake 工程，不进主构建树）
//
// 模式 1 find_package：
//   cmake -S . -B build -DCMAKE_PREFIX_PATH="<install-prefix>;<qt-root>"
// 模式 2 add_subdirectory：
//   cmake -S . -B build -DQEW_ROOT=<repo-root> -DCMAKE_PREFIX_PATH=<qt-root>
//
// 验收内容：CusApplication 接入 + WatchLogger 打点 + 事件循环退出

#include "CusApplication.h"
#include "WatchLogger.h"
#include "WatchLogMacros.h"

#include <QCoreApplication>
#include <QTimer>
#include <iostream>

int main(int argc, char* argv[])
{
    qt_event_watcher::CusApplication app(argc, argv);
    qt_event_watcher::WatchLogger::instance().initialize(".", "consume_test");

    QEW_LOG_INFO("consume-test alive");
    QTimer::singleShot(50, &app, [] { QCoreApplication::exit(0); });

    const int code = app.exec();
    std::cout << (code == 0 ? "[PASS] consume-test" : "[FAIL] consume-test")
              << std::endl;
    return code;
}
