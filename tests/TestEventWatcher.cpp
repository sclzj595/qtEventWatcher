// TestEventWatcher - 冒烟测试
// 验证 Runtime Diagnostics 全链路（PRD 16）+ 报告导出（PRD 17 §5）
// TODO: 后续接入 QtTest 框架补充正式单元测试

#include "ReportExporter.h"
#include "RuntimeDiagnostics.h"
#include "WatchConfig.h"
#include "DataExporter.h"
#include "DiagnosticSummarizer.h"
#include "EventStatistics.h"
#include "IpcConfigServer.h"
#include "MetaCallFilter.h"
#include "ThreadRegistry.h"
#include "WatchLogger.h"
#include "WatchLogMacros.h"
#include "WatchRecordStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <memory>

#include <iostream>

using namespace qt_event_watcher;

namespace {

const char* dependencyTag(DependencyStatus status)
{
    switch (status) {
    case DependencyStatus::Loaded:          return "LOADED";
    case DependencyStatus::PathUnexpected:  return "PATH?!";
    case DependencyStatus::VersionMismatch: return "VERSION";
    case DependencyStatus::Missing:         return "MISSING";
    }
    return "?";
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // ---- Overview（轻量路径，PRD 16 §1）----
    const auto overview = RuntimeDiagnostics::collectOverview();
    const RuntimeSnapshot& s = overview.runtime;
    std::cout << "[Overview] app=" << s.applicationName.toStdString()
              << " pid=" << s.applicationPid
              << " config=" << s.buildConfig.toStdString()
              << " compiler=" << s.compilerType.toStdString()
              << " " << s.compilerVersion.toStdString()
              << " arch=" << s.compilerArch.toStdString() << std::endl;
    std::cout << "[Overview] qt compiled=" << s.qtVersionCompiled.toStdString()
              << " runtime=" << s.qtVersionRuntime.toStdString()
              << " match=" << (s.qtVersionMatch() ? "yes" : "NO") << std::endl;
    std::cout << "[Overview] pluginsPath=" << overview.qtEnvironment.pluginsPath.toStdString() << std::endl;
    std::cout << "[Overview] plugins=" << overview.qtEnvironment.plugins.size()
              << " qwindowsMissing=" << (overview.qtEnvironment.qwindowsMissing() ? "yes" : "no")
              << std::endl;

    // ---- Full（主动触发路径：模块枚举 + PE Import 分析）----
    const DiagnosticsReport report = RuntimeDiagnostics::collectFull();
    std::cout << "[Full] modules=" << report.modules.size()
              << " modulesOk=" << (report.modulesOk ? "yes" : "no") << std::endl;
    std::cout << "[Full] dependencies ok=" << (report.dependencies.ok ? "yes" : "no")
              << " total=" << report.dependencies.dependencies.size()
              << " problems=" << report.dependencies.problemCount()
              << " missing=" << report.dependencies.missingCount() << std::endl;

    if (!report.dependencies.ok)
        std::cout << "[Full] failReason=" << report.dependencies.failReason.toStdString() << std::endl;

    // Qt / 运行时关键 DLL 逐条展示
    for (const DependencyInfo& d : report.dependencies.dependencies) {
        const QString& n = d.name;
        if (n.startsWith("Qt5") || n.startsWith("VCRUNTIME", Qt::CaseInsensitive)
            || n.startsWith("MSVCP", Qt::CaseInsensitive)
            || d.status != DependencyStatus::Loaded) {
            std::cout << "  " << d.name.toStdString()
                      << " [" << dependencyTag(d.status) << "] "
                      << d.resolvedPath.toStdString();
            if (!d.hint.isEmpty())
                std::cout << "  hint=" << d.hint.toStdString();
            std::cout << std::endl;
        }
    }

    // 自检断言：exe 自身 PE 解析必须成功；QtCore 必须出现在 import 列表且已加载
    // （Qt5/Qt6 的 DLL 名不同，按编译期版本判定）
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const char* coreDllName = "Qt6Core.dll";
#else
    const char* coreDllName = "Qt5Core.dll";
#endif
    bool qt5CoreLoaded = false;
    for (const DependencyInfo& d : report.dependencies.dependencies) {
        if (d.name.compare(coreDllName, Qt::CaseInsensitive) == 0
            && d.status == DependencyStatus::Loaded)
            qt5CoreLoaded = true;
    }

    int failures = 0;
    if (report.modulesOk && report.modules.isEmpty())          ++failures;
    if (!report.dependencies.ok)                               ++failures;
    if (report.dependencies.dependencies.isEmpty())            ++failures;
    if (!qt5CoreLoaded)                                        ++failures;

    // ---- 报告导出（PRD 17 §5）：TXT + JSON 双格式 ----
    const QString txtPath = QStringLiteral("diag_report_test.txt");
    const QString jsonPath = QStringLiteral("diag_report_test.json");

    QString exportError;
    if (!ReportExporter::exportReport(txtPath, nullptr, &exportError)) {
        std::cout << "  [EXPORT TXT FAIL] " << exportError.toStdString() << std::endl;
        ++failures;
    }
    if (!ReportExporter::exportReport(jsonPath, nullptr, &exportError)) {
        std::cout << "  [EXPORT JSON FAIL] " << exportError.toStdString() << std::endl;
        ++failures;
    }

    // TXT 基本断言：非空 + 含报告头
    QFile txtFile(txtPath);
    QByteArray txt;
    if (txtFile.open(QIODevice::ReadOnly))
        txt = txtFile.readAll();
    if (txt.isEmpty() || !txt.contains("QtEventWatcher Diagnostic Report")) {
        std::cout << "  [TXT CONTENT FAIL] size=" << txt.size() << std::endl;
        ++failures;
    } else {
        std::cout << "  txt report: " << txt.size() << " bytes" << std::endl;
    }

    // JSON 基本断言：可解析 + 关键段齐全
    QFile jsonFile(jsonPath);
    QByteArray jsonBytes;
    if (jsonFile.open(QIODevice::ReadOnly))
        jsonBytes = jsonFile.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (doc.isNull() || !doc.isObject()) {
        std::cout << "  [JSON PARSE FAIL]" << std::endl;
        ++failures;
    } else {
        const QJsonObject root = doc.object();
        const char* sections[] = { "application", "qtEnvironment", "compiler",
                                   "modules", "plugins", "dependencies",
                                   "recentDiagnostics" };
        for (const char* sec : sections) {
            if (!root.contains(QString::fromLatin1(sec))) {
                std::cout << "  [JSON SECTION MISSING] " << sec << std::endl;
                ++failures;
            }
        }
        std::cout << "  json report: modules=" << root.value("modules").toArray().size()
                  << " plugins=" << root.value("plugins").toArray().size()
                  << " dependencies=" << root.value("dependencies").toObject()
                                           .value("items").toArray().size() << std::endl;
    }
    QFile::remove(txtPath);
    QFile::remove(jsonPath);

    // ---- Monitor 段（带真实 config）：Watch_Fun / 各阈值行存在 ----
    {
        WatchConfig config;
        config.load();
        config.setWatchFun(0xF);
        const QString monitorPath = QStringLiteral("diag_report_monitor.txt");
        if (ReportExporter::exportReport(monitorPath, &config, &exportError)) {
            QFile mf(monitorPath);
            QByteArray content;
            if (mf.open(QIODevice::ReadOnly))
                content = mf.readAll();
            const bool hasMask = content.contains("Watch_Fun: 0x0f");
            const bool hasThresholds = content.contains("Slow Event:")
                && content.contains("MetaCall:") && content.contains("QSS:");
            if (!hasMask || !hasThresholds) {
                std::cout << "  [MONITOR SECTION FAIL] mask=" << hasMask
                          << " thresholds=" << hasThresholds << std::endl;
                // 失败时输出 Monitor 段原文定位差异
                const QString text = QString::fromUtf8(content);
                const int idx = text.indexOf(QStringLiteral("Monitor"));
                if (idx >= 0)
                    std::cout << text.mid(idx, 420).toStdString() << std::endl;
                ++failures;
            } else {
                std::cout << "  monitor section: OK (Watch_Fun 0xf + thresholds)"
                          << std::endl;
            }
            mf.close();
            QFile::remove(monitorPath);
        } else {
            std::cout << "  [MONITOR EXPORT FAIL] "
                      << exportError.toStdString() << std::endl;
            ++failures;
        }
    }

    // ---- 批量数据导出（V2 A1）：合成记录 → WatchRecordStore → CSV + JSON ----
    {
        const QString logDir = QStringLiteral("test_record_logs");
        WatchLogger::instance().initialize(logDir.toStdString(), "qew-test",
                                           WatchLogLevel::Warn);

        // 与监控器输出格式逐字段一致（含边缘：空 object 值、空 sender 值）
        QEW_LOG_WARN("[EventWatcher] slow event receiver=QPushButton object=btn event=MouseButtonPress "
                     "type=187 depth=0 costMs=123.500 exclusiveCostMs=100.250 curThread=0x1 "
                     "recvThread=0x1 match=true thresholdMs=30");
        QEW_LOG_WARN("[EventWatcher] slow event receiver=QWidget object= event=Paint "
                     "type=12 depth=1 costMs=55.000 exclusiveCostMs=55.000 curThread=0x1 "
                     "recvThread=0x1 match=true thresholdMs=30");
        QEW_LOG_WARN("[MetaCallWatcher] slow MetaCall sender= signal= signalId=-1 senderThread=0x0 "
                     "receiver=MainWindow object= recvThread=0x1 curThread=0x1 match=- "
                     "costMs=42.000 thresholdMs=1");
        // 非目标前缀（INFO 级 / 前缀不匹配）不得采集
        QEW_LOG_INFO("[EventStatistics] period report count=10");
        QEW_LOG_WARN("[MetaCallWatcher] signal spy callbacks hijacked/uninstalled, "
                     "sender identity degraded to empty fields");

        const std::size_t stored = WatchRecordStore::instance().count();
        if (stored != 3) {
            std::cout << "  [RECORD COUNT FAIL] stored=" << stored << " expected=3" << std::endl;
            ++failures;
        }

        const QString csvPath = QStringLiteral("data_export_test.csv");
        if (!DataExporter::exportData(csvPath, nullptr, nullptr, &exportError)) {
            std::cout << "  [DATA CSV FAIL] " << exportError.toStdString() << std::endl;
            ++failures;
        } else {
            QFile cf(csvPath);
            QByteArray csv;
            if (cf.open(QIODevice::ReadOnly))
                csv = cf.readAll();
            const int rows = csv.count('\n');
            const bool utf8Bom = csv.startsWith("\xEF\xBB\xBF");
            if (rows != 4 || !csv.contains("slowEvent") || !csv.contains("metaCall")
                || !csv.contains("123.500") || !utf8Bom) {
                std::cout << "  [DATA CSV CONTENT FAIL] rows=" << rows
                          << " bom=" << utf8Bom << std::endl;
                ++failures;
            } else {
                std::cout << "  csv data: " << rows - 1 << " rows, utf8+bom" << std::endl;
            }
            cf.close();
            QFile::remove(csvPath);
        }

        const QString dataJsonPath = QStringLiteral("data_export_test.json");
        if (!DataExporter::exportData(dataJsonPath, nullptr, nullptr, &exportError)) {
            std::cout << "  [DATA JSON FAIL] " << exportError.toStdString() << std::endl;
            ++failures;
        } else {
            QFile df(dataJsonPath);
            QByteArray bytes;
            if (df.open(QIODevice::ReadOnly))
                bytes = df.readAll();
            const QJsonDocument dataDoc = QJsonDocument::fromJson(bytes);
            const QJsonObject dataRoot = dataDoc.object();
            const QJsonObject counts = dataRoot.value("counts").toObject();
            const QJsonArray recs = dataRoot.value("records").toArray();
            const QJsonObject meta = recs.at(2).toObject().value("fields").toObject();
            const bool csvOk = counts.value("slowEvents").toInt() == 2
                && counts.value("metaCalls").toInt() == 1;
            const bool fieldsOk = meta.value("signal").toString() == "timeout()"
                || meta.value("signal").toString().isEmpty();	// 空值记录字段可缺省
            const bool typedOk = recs.at(0).toObject().value("fields").toObject()
                                     .value("costMs").toDouble() == 123.5;
            if (dataDoc.isNull() || !csvOk || !fieldsOk || !typedOk) {
                std::cout << "  [DATA JSON CONTENT FAIL] parse=" << !dataDoc.isNull()
                          << " counts=" << csvOk << " fields=" << fieldsOk
                          << " typed=" << typedOk << std::endl;
                ++failures;
            } else {
                std::cout << "  json data: slowEvents=" << counts.value("slowEvents").toInt()
                          << " metaCalls=" << counts.value("metaCalls").toInt()
                          << " typedFields=OK" << std::endl;
            }
            df.close();
            QFile::remove(dataJsonPath);
        }
        QDir(logDir).removeRecursively();
    }

    // ---- 周期统计导出（V2 A2）：直方图 / 分位数 / TOP-N / 归档 ----
    {
        WatchConfig config;
        config.setWatchFun(0x4);		// bit2 = WatchEventStatistics
        config.setEventStatPeriodMs(1);
        EventStatistics stats(&config);

        // 确定性归档：warm-up 先耗尽周期（空转 flush 重置计时器），
        // 保证下方 burst 落入同一完整周期
        QThread::msleep(2);
        stats.record(99, QStringLiteral("Warmup"), 1000);

        // 分布明确：TestPaint = 9×1ms + 1×10ms；TestTimer = 50×0.5ms
        for (int i = 0; i < 9; ++i)
            stats.record(20, QStringLiteral("TestPaint"), 1000000);
        stats.record(20, QStringLiteral("TestPaint"), 10000000);
        for (int i = 0; i < 50; ++i)
            stats.record(30, QStringLiteral("TestTimer"), 500000);

        // 触发归档：periodMs=1，sleep 后再 record → checkPeriod 内 flush
        QThread::msleep(2);
        stats.record(20, QStringLiteral("TestPaint"), 1000000);

        const QVector<EventStatistics::PeriodSnapshot> history = stats.statisticsHistory();
        const EventStatistics::StatEntry* paint = nullptr;
        const EventStatistics::StatEntry* timer = nullptr;
        for (const EventStatistics::PeriodSnapshot& p : history) {
            for (const EventStatistics::StatEntry& e : p.entries) {
                if (e.eventKey == QLatin1String("20:TestPaint") && e.count == 11)
                    paint = &e;		// burst 10 条 + 归档触发那条
                if (e.eventKey == QLatin1String("30:TestTimer"))
                    timer = &e;
            }
        }
        if (history.isEmpty() || paint == nullptr || timer == nullptr) {
            std::cout << "  [STAT ENTRY MISSING] periods=" << history.size()
                      << " paint=" << (paint != nullptr)
                      << " timer=" << (timer != nullptr) << std::endl;
            ++failures;
        } else {
            const bool countsOk = paint->count == 11 && timer->count == 50
                && paint->maxCostNs == 10000000ULL;
            // p50：第 6 小样本（n=11）= 1ms 区间桶（0.4~1.1ms 之间）
            const double p50 = EventStatistics::estimatePercentileMs(*paint, 0.50);
            // p99：第 11 小样本 = 10ms 样本桶，桶上界 16.8ms（误差 ≤2x）
            const double p99 = EventStatistics::estimatePercentileMs(*paint, 0.99);
            const bool pctOk = p50 > 0.4 && p50 < 1.1 && p99 >= 10.0 && p99 < 20.0;
            if (!countsOk || !pctOk) {
                std::cout << "  [STAT VALUE FAIL] counts=" << countsOk
                          << " p50=" << p50 << " p99=" << p99 << std::endl;
                ++failures;
            } else {
                std::cout << "  stat snapshot: paint n=11 max=10ms p50=" << p50
                          << " p99=" << p99 << std::endl;
            }
        }

        // 导出 JSON：statistics 段存在 + TOP-N 排序（TestTimer 25e6 > TestPaint 19e6）
        const QString statJsonPath = QStringLiteral("data_export_stat.json");
        if (!DataExporter::exportData(statJsonPath, nullptr, &stats, &exportError)) {
            std::cout << "  [STAT EXPORT FAIL] " << exportError.toStdString() << std::endl;
            ++failures;
        } else {
            QFile sf(statJsonPath);
            QByteArray bytes;
            if (sf.open(QIODevice::ReadOnly))
                bytes = sf.readAll();
            const QJsonDocument statDoc = QJsonDocument::fromJson(bytes);
            const QJsonArray periods = statDoc.object().value("statistics").toArray();
            bool topOk = false;
            for (const QJsonValue& pv : periods) {	// 定位含 TestTimer 的周期（跳过 Warmup）
                const QJsonArray top = pv.toObject().value("top").toArray();
                if (top.isEmpty()
                    || top.at(0).toObject().value("event").toString()
                        != QLatin1String("30:TestTimer"))
                    continue;
                topOk = top.at(0).toObject().value("count").toDouble() == 50.0
                    && top.at(0).toObject().value("p99Ms").toDouble() > 0.0;
                break;
            }
            if (statDoc.isNull() || periods.isEmpty() || !topOk) {
                std::cout << "  [STAT JSON FAIL] periods=" << periods.size()
                          << " top=" << topOk << std::endl;
                ++failures;
            } else {
                std::cout << "  json statistics: periods=" << periods.size()
                          << " topOk" << std::endl;
            }
            sf.close();
            QFile::remove(statJsonPath);
        }
    }

    // ---- 诊断摘要（V2 A3）：四规则归因 + 导出 ----
    {
        // 注入三类记录（logger 沿用上方初始化；含 A1 段遗留记录，断言按聚合容忍）
        QEW_LOG_WARN("[EventWatcher] slow event receiver=QPushButton object=btn event=Paint "
                     "type=12 depth=0 costMs=600.000 exclusiveCostMs=600.000 curThread=0x1 "
                     "recvThread=0x1 match=true thresholdMs=30");
        QEW_LOG_WARN("[EventWatcher] slow event receiver=QPushButton object=btn event=Paint "
                     "type=12 depth=0 costMs=600.000 exclusiveCostMs=600.000 curThread=0x1 "
                     "recvThread=0x1 match=true thresholdMs=30");
        QEW_LOG_WARN("[MetaCallWatcher] slow MetaCall sender=Worker signal=timeout() signalId=3 "
                     "senderThread=0x2 receiver=MainWindow object= recvThread=0x1 curThread=0x1 "
                     "match=false costMs=5.000 thresholdMs=1");
        QEW_LOG_WARN("[MetaCallWatcher] slow MetaCall sender=Worker signal=timeout() signalId=3 "
                     "senderThread=0x2 receiver=MainWindow object= recvThread=0x1 curThread=0x1 "
                     "match=false costMs=6.000 thresholdMs=1");
        QEW_LOG_WARN("[MetaCallWatcher] slow MetaCall sender=Worker signal=timeout() signalId=3 "
                     "senderThread=0x2 receiver=MainWindow object= recvThread=0x1 curThread=0x1 "
                     "match=false costMs=7.000 thresholdMs=1");
        QEW_LOG_WARN("[QssStyleWatcher] SlowQssLoad | File=theme.qss Size=12KB IO=3.0ms Style=9.0ms Total=12.0ms");
        QEW_LOG_WARN("[QssStyleWatcher] SlowSetStyleSheet | Widget=MainWindow StyleLen=2048 Cost=8.0ms");

        const auto findings = DiagnosticSummarizer::analyze(nullptr, nullptr);

        const DiagnosticSummarizer::Finding* slowTop = nullptr;
        const DiagnosticSummarizer::Finding* hiFreq = nullptr;
        const DiagnosticSummarizer::Finding* qss = nullptr;
        for (const DiagnosticSummarizer::Finding& f : findings) {
            if (f.category == QLatin1String("SlowEventTop"))			slowTop = &f;
            if (f.category == QLatin1String("HighFrequencySignal"))		hiFreq = &f;
            if (f.category == QLatin1String("QssJitter"))				qss = &f;
        }
        const bool slowOk = slowTop != nullptr
            && slowTop->severity == DiagnosticSummarizer::Severity::Critical
            && slowTop->headline.contains(QLatin1String("QPushButton"))
            && slowTop->details.size() >= 1;
        const bool freqOk = hiFreq != nullptr
            && !hiFreq->details.isEmpty()
            && hiFreq->details.first().contains(QLatin1String("timeout()"));
        const bool qssOk = qss != nullptr && qss->details.size() == 2;
        if (!slowOk || !freqOk || !qssOk) {
            std::cout << "  [SUMMARY ANALYZE FAIL] slow=" << slowOk
                      << " freq=" << freqOk << " qss=" << qssOk << std::endl;
            ++failures;
        } else {
            std::cout << "  summary analyze: findings=" << findings.size()
                      << " slowTop=CRITICAL freq/qss=OK" << std::endl;
        }

        // 导出双格式
        const QString sumTxt = QStringLiteral("summary_test.txt");
        const QString sumJson = QStringLiteral("summary_test.json");
        if (!DiagnosticSummarizer::exportSummary(sumTxt, findings, nullptr, &exportError)) {
            std::cout << "  [SUMMARY TXT FAIL] " << exportError.toStdString() << std::endl;
            ++failures;
        } else {
            QFile tf(sumTxt);
            QByteArray bytes;
            if (tf.open(QIODevice::ReadOnly))
                bytes = tf.readAll();
            if (!bytes.contains("Performance Summary") || !bytes.contains("[CRITICAL]")) {
                std::cout << "  [SUMMARY TXT CONTENT FAIL]" << std::endl;
                ++failures;
            } else {
                std::cout << "  summary txt: " << bytes.size() << " bytes" << std::endl;
            }
            tf.close();
            QFile::remove(sumTxt);
        }
        if (!DiagnosticSummarizer::exportSummary(sumJson, findings, nullptr, &exportError)) {
            std::cout << "  [SUMMARY JSON FAIL] " << exportError.toStdString() << std::endl;
            ++failures;
        } else {
            QFile jf(sumJson);
            QByteArray bytes;
            if (jf.open(QIODevice::ReadOnly))
                bytes = jf.readAll();
            const QJsonDocument sd = QJsonDocument::fromJson(bytes);
            const QJsonArray fa = sd.object().value("findings").toArray();
            if (sd.isNull() || fa.size() < 3
                || fa.at(0).toObject().value("severity").toString().isEmpty()) {
                std::cout << "  [SUMMARY JSON CONTENT FAIL] findings=" << fa.size() << std::endl;
                ++failures;
            } else {
                std::cout << "  summary json: findings=" << fa.size() << std::endl;
            }
            jf.close();
            QFile::remove(sumJson);
        }
    }

    // ---- IPC 配置服务（V2 Phase B）：进程内 server + 客户端全协议验证 ----
    {
        WatchConfig ipcConfig;
        ipcConfig.setWatchFun(0);
        MetaCallFilter ipcFilter;
        IpcConfigServer ipcServer(&ipcConfig, &ipcFilter);
        const QString ipcName = QStringLiteral("QewIpcTest.%1")
                                    .arg(QCoreApplication::applicationPid());
        QString ipcError;
        if (!ipcServer.start(ipcName, &ipcError)) {
            std::cout << "  [IPC START FAIL] " << ipcError.toStdString() << std::endl;
            ++failures;
        } else {
            QLocalSocket client;
            client.connectToServer(ipcName);
            if (!client.waitForConnected(2000)) {
                std::cout << "  [IPC CONNECT FAIL] " << client.errorString().toStdString()
                          << std::endl;
                ++failures;
            } else {
                // 请求-响应助手：QEventLoop 等响应行（server 信号驱动，需事件处理）；
                // expectReply=false 用于静默丢弃类请求（服务端不回包）
                const auto request = [&](const QByteArray& req, bool expectReply) -> QJsonObject {
                    QJsonObject result;
                    QByteArray received;
                    QEventLoop loop;
                    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
                    QObject::connect(&client, &QLocalSocket::readyRead, [&]() {
                        received += client.readAll();
                        if (received.contains('\n'))	loop.quit();
                    });
                    client.write(req + '\n');
                    if (!expectReply) {
                        loop.exec();	// 走完整超时：验证期间无崩溃即可
                        return result;
                    }
                    loop.exec();
                    const int newline = received.indexOf('\n');
                    if (newline >= 0)
                        result = QJsonDocument::fromJson(received.left(newline)).object();
                    return result;
                };

                // 1) get：ok + filterAvailable + watchFun 快照
                const QJsonObject r1 = request(R"({"op":"get"})", true);
                const bool getOk = !r1.isEmpty() && r1.value("ok").toBool() == true
                    && r1.value("filterAvailable").toBool() == true
                    && r1.contains(QLatin1String("watchFun"));
                if (!getOk) {
                    std::cout << "  [IPC GET FAIL] empty=" << r1.isEmpty() << std::endl;
                    ++failures;
                }

                // 2) set：watchFun=15 + slowEventThresholdMs=10 → 立即生效
                const QJsonObject r2 = request(
                    R"({"op":"set","watchFun":15,"slowEventThresholdMs":10})", true);
                const bool setOk = r2.value("watchFun").toString() == QLatin1String("0x0f")
                    && ipcConfig.slowEventThresholdMs() == 10;
                if (!setOk) {
                    std::cout << "  [IPC SET FAIL] watchFun="
                              << r2.value("watchFun").toString().toStdString()
                              << " threshold=" << ipcConfig.slowEventThresholdMs() << std::endl;
                    ++failures;
                }

                // 3) 坏 JSON / 未知 op：静默丢弃（无响应、不崩溃）
                const QJsonObject r3 = request("this is not json", false);
                const QJsonObject r4 = request(R"({"op":"nonsense"})", false);
                Q_UNUSED(r3);
                Q_UNUSED(r4);

                // 4) filter.add sender → check() 立即 Suppress
                request(R"({"op":"filter.add","sender":"FooClass"})", true);
                const bool filterOn = ipcFilter.check(QStringLiteral("FooClass"),
                                                      QStringLiteral("sig()"),
                                                      QStringLiteral("Recv"))
                    == MetaCallFilter::Verdict::Suppress;
                request(R"({"op":"filter.add","anonymous":true})", true);
                const bool anonOn = ipcFilter.suppressAnonymousSenders();

                // 5) filter.clear → 规则与匿名开关全部复位
                request(R"({"op":"filter.clear"})", true);
                const bool filterOff = ipcFilter.check(QStringLiteral("FooClass"),
                                                       QStringLiteral("sig()"),
                                                       QStringLiteral("Recv"))
                    == MetaCallFilter::Verdict::Log;
                const bool anonOff = !ipcFilter.suppressAnonymousSenders();

                if (!filterOn || !filterOff || !anonOn || !anonOff) {
                    std::cout << "  [IPC FILTER FAIL] on=" << filterOn << " off=" << filterOff
                              << " anonOn=" << anonOn << " anonOff=" << anonOff << std::endl;
                    ++failures;
                } else {
                    std::cout << "  ipc roundtrip: get/set/bad-json/filter all OK" << std::endl;
                }
                client.disconnectFromServer();
            }
        }
    }

    // ---- ThreadRegistry（V2 C1）：注册 / 回填 / destroyed 注销 ----
    {
        ThreadRegistry registry;
        QThread* mainThreadObj = QThread::currentThread();

        // 当前线程 observe：isCurrentThread → 立即拿到 os id
        registry.observe(mainThreadObj);

        // 跨线程 QThread 对象：observe 时非本线程 → os id = 0（待回填）
        quintptr workerOsId = 12345;
        {
            // 独立作用域：QThread 对象 destroyed → 自动注销（条目计数回到 1）
            QThread workerLocal;
            registry.observe(&workerLocal);
            const bool workerFound = registry.lookup(&workerLocal, workerOsId);
            if (!workerFound || workerOsId != 0) {
                std::cout << "  [THREAD REGISTRY FAIL] workerFound=" << workerFound
                          << " workerOsId=" << workerOsId << std::endl;
                ++failures;
            }
        }
        const int sizeAfter = registry.size();	// mainThread(1)，worker 已随析构注销

        quintptr osId = 0;
        const bool found = registry.lookup(mainThreadObj, osId);
        if (!found || osId == 0 || sizeAfter != 1) {
            std::cout << "  [THREAD REGISTRY FAIL] found=" << found << " osId=" << osId
                      << " size=" << sizeAfter << std::endl;
            ++failures;
        } else {
            std::cout << "  thread registry: register/fill-back/unregister OK" << std::endl;
        }
    }

    // ---- Exclusive Cost 聚合（V2 C2）：record 传 exclusive → 周期快照聚合 ----
    {
        WatchConfig c2Config;
        c2Config.setWatchFun(0x4);
        c2Config.setEventStatPeriodMs(1);
        EventStatistics c2Stats(&c2Config);

        // 确定性归档（同 A2 段 warm-up 模式）：ExclTest 两条落入同一完整周期
        QThread::msleep(2);
        c2Stats.record(99, QStringLiteral("Warmup"), 1000);
        c2Stats.record(20, QStringLiteral("ExclTest"), 1000000, 600000);	// 1ms / 0.6ms
        c2Stats.record(20, QStringLiteral("ExclTest"), 2000000, 1500000);	// 2ms / 1.5ms
        QThread::msleep(2);
        c2Stats.record(99, QStringLiteral("Warmup"), 1000);					// 触发归档

        const EventStatistics::StatEntry* excl = nullptr;
        for (const EventStatistics::PeriodSnapshot& p : c2Stats.statisticsHistory()) {
            for (const EventStatistics::StatEntry& e : p.entries) {
                if (e.eventKey == QLatin1String("20:ExclTest"))	excl = &e;
            }
        }
        const bool exclOk = excl != nullptr
            && excl->totalCostNs == 3000000ULL
            && excl->exclusiveTotalNs == 2100000ULL
            && excl->exclusiveMaxNs == 1500000ULL;
        if (!exclOk) {
            std::cout << "  [EXCLUSIVE AGG FAIL] found=" << (excl != nullptr) << std::endl;
            ++failures;
        } else {
            std::cout << "  exclusive agg: total=3.001ms exclTotal=2.1005ms exclMax=1.5ms"
                      << std::endl;
        }
    }

    std::cout << (failures == 0 ? "[PASS] Runtime Diagnostics smoke test"
                                : "[FAIL] failures=") ;
    if (failures != 0)
        std::cout << failures;
    std::cout << std::endl;
    return failures == 0 ? 0 : 1;
}
