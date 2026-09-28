#include "MainWindow.h"

#include "CusApplication.h"
#include "QssStyleWatcher.h"
#include "ReportExporter.h"
#include "DataExporter.h"
#include "DiagnosticSummarizer.h"
#include "RuntimeDiagnostics.h"
#include "StyleTokens.h"
#include "WatchConfig.h"
#include "WatchLogCapture.h"

#include <QApplication>
#include <QAction>
#include <QButtonGroup>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QKeySequence>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QColor>
#include <QMap>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {

using qt_event_watcher::WatchLogCapture;
using qt_event_watcher::RuntimeDiagnostics;
using qt_event_watcher::DiagnosticsReport;
using qt_event_watcher::RuntimeSnapshot;
using qt_event_watcher::QtEnvironmentSnapshot;
using qt_event_watcher::QtPluginInfo;
using qt_event_watcher::DependencyStatus;
using qt_event_watcher::DependencyInfo;
using qt_event_watcher::ModuleInfo;

/*
 * 确定性嵌套场景（PRD 05 §6）：先投递一个 40ms queued MetaCall，
 * 父事件阻塞 100ms 后 processEvents() 将其作为嵌套事件派发。
 */
void triggerNestedEvent()
{
    QMetaObject::invokeMethod(qApp, []() {
        QThread::msleep(40);
    }, Qt::QueuedConnection);
    QThread::msleep(100);
    QCoreApplication::processEvents();
}

// ============ 日志解析（明细表数据源，PRD 18 §4） ============

/// WatchLogCapture 分类 → 日志前缀（与 WatchLogCapture::Impl::categoryOf 一致）
const char* categoryPrefix(int category)
{
    switch (category) {
    case WatchLogCapture::CatSlowEvent: return "[EventWatcher]";
    case WatchLogCapture::CatMetaCall:  return "[MetaCallWatcher]";
    case WatchLogCapture::CatEventStat: return "[EventStatistics]";
    case WatchLogCapture::CatQss:       return "[QssStyleWatcher]";
    default:                            return "";
    }
}

struct ParsedLog
{
    QString op;                     ///< 前缀后的短语（如 "slow event"、"SlowQssLoad"）
    QMap<QString, QString> kv;      ///< key=value 键值（各 watcher 日志格式保证无空格值）
};

ParsedLog parseLogBody(int category, const std::string& raw)
{
    ParsedLog p;
    QString text = QString::fromStdString(raw);
    const QString prefix = QString::fromLatin1(categoryPrefix(category));
    if (text.startsWith(prefix))
        text.remove(0, prefix.size());

    const QStringList tokens = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList opParts;
    for (const QString& t : tokens) {
        const int eq = t.indexOf(QLatin1Char('='));
        if (eq > 0) {
            p.kv.insert(t.left(eq), t.mid(eq + 1));
        } else if (t != QStringLiteral("|")) {
            opParts.append(t);
        }
    }
    p.op = opParts.join(QLatin1Char(' '));
    return p;
}

/// 明细列取值：keys 支持 "a|b|c" 依次回退；"" = 采集时间；"$op" = 前缀短语
QString detailCell(const WatchLogCapture::Entry& e, const QString& keys,
                   const ParsedLog& parsed)
{
    if (keys.isEmpty())
        return QString::fromStdString(e.time);
    if (keys == QStringLiteral("$op"))
        return parsed.op;
    const QStringList alternatives = keys.split(QLatin1Char('|'));
    for (const QString& k : alternatives)
        if (parsed.kv.contains(k))
            return parsed.kv.value(k);
    return QStringLiteral("-");
}

/// Cost 类列值告警色（PRD 19 §4：强调色仅少量用于数值）
bool isCostKey(const QString& keys)
{
    return keys.contains(QStringLiteral("ost"));	// costMs/totalCostMs/Cost/MaxCost
}

// ============ Runtime Diagnostics 文本报告（PRD 16 §3~§7 分组） ============

QString formatRuntimeOverview(const RuntimeDiagnostics::Overview& o)
{
    const RuntimeSnapshot& s = o.runtime;
    QString text;
    text += QStringLiteral("=== Runtime Overview ===\n");
    text += QStringLiteral("Application : %1  (PID %2)\n")
                .arg(s.applicationName).arg(s.applicationPid);
    text += QStringLiteral("Path        : %1\n").arg(s.applicationFilePath);
    text += QStringLiteral("Build       : %1  ABI: %2\n").arg(s.buildConfig, s.buildAbi);
    text += QStringLiteral("Compiler    : %1 %2  Target: %3\n")
                .arg(s.compilerType, s.compilerVersion, s.compilerArch);
    text += QStringLiteral("Qt compiled : %1\n").arg(s.qtVersionCompiled);
    text += QStringLiteral("Qt runtime  : %1  %2\n")
                .arg(s.qtVersionRuntime,
                     s.qtVersionMatch() ? QStringLiteral("(match)")
                                        : QStringLiteral("(MISMATCH!)"));
    text += QStringLiteral("Platform    : %1  OS: %2  Kernel: %3\n")
                .arg(s.platformName, s.osProductName, s.osKernelVersion);
    text += QStringLiteral("CPU         : build %1 / current %2\n")
                .arg(o.qtEnvironment.buildCpuArchitecture,
                     o.qtEnvironment.currentCpuArchitecture);
    text += QStringLiteral("PluginsPath : %1\n").arg(o.qtEnvironment.pluginsPath);

    if (o.qtEnvironment.qwindowsMissing())
        text += QStringLiteral("!! WARNING  : qwindows.dll missing -> "
                               "\"no Qt platform plugin could be initialized\"\n");

    text += QStringLiteral("\nPlugins (category / name / state):\n");
    for (const QtPluginInfo& p : o.qtEnvironment.plugins) {
        text += QStringLiteral("  %1  %2  %3\n")
                    .arg(p.category.leftJustified(13),
                         p.name.leftJustified(28),
                         p.loaded ? QStringLiteral("[Loaded]")
                                  : QStringLiteral("[OnDisk]"));
    }
    return text;
}

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

QString formatFullReport(const DiagnosticsReport& r)
{
    QString text = formatRuntimeOverview({ r.runtime, r.qtEnvironment });

    text += QStringLiteral("\n=== Modules (%1) ===\n")
                .arg(r.modulesOk ? QString::number(r.modules.size())
                                 : QStringLiteral("enumerate failed"));
    for (const ModuleInfo& m : r.modules) {
        text += QStringLiteral("  %1  0x%2  %3 MB  %4\n")
                    .arg(m.name.leftJustified(26),
                         QString::number(m.baseAddress, 16).rightJustified(12, QLatin1Char('0')),
                         QString::number(m.size / (1024.0 * 1024.0), 'f', 1).leftJustified(7),
                         m.fullPath);
    }

    text += QStringLiteral("\n=== Dependencies (PE Imports) ===\n");
    if (!r.dependencies.ok) {
        text += QStringLiteral("  %1\n").arg(r.dependencies.failReason);
    } else {
        for (const DependencyInfo& d : r.dependencies.dependencies) {
            text += QStringLiteral("  %1  %2  %3\n")
                        .arg(d.name.leftJustified(28),
                             QString::fromLatin1(dependencyTag(d.status)).leftJustified(9),
                             d.resolvedPath);
            if (!d.hint.isEmpty())
                text += QStringLiteral("      hint: %1\n").arg(d.hint);
        }
        text += QStringLiteral("  summary: total=%1 problems=%2 missing=%3\n")
                    .arg(r.dependencies.dependencies.size())
                    .arg(r.dependencies.problemCount())
                    .arg(r.dependencies.missingCount());
    }
    return text;
}

} // namespace

namespace qt_event_watcher {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 主题跟随系统（PRD 19 §9），accent 默认 Blue
    m_theme = style_tokens::systemPrefersDark() ? QStringLiteral("dark")
                                                : QStringLiteral("light");
    m_accent = QString::fromLatin1(style_tokens::AccentBlue);

    setWindowTitle(tr("QtEventWatcher Basic Demo"));

    // 慢事件演示源：周期 200ms，槽内阻塞 40ms（> SlowEventThresholdMs 30ms）
    auto* slowTimer = new QTimer(this);
    connect(slowTimer, &QTimer::timeout, this, []() {
        QThread::msleep(40);
    });
    slowTimer->start(200);

    // 高频 Timer 演示源：10ms 周期、槽内轻负载（约 100 次/秒，> EventCountThreshold 10）
    auto* fastTimer = new QTimer(this);
    connect(fastTimer, &QTimer::timeout, this, []() {
        volatile int sink = 0;
        for (int i = 0; i < 100; ++i) {
            sink += i;
        }
    });
    fastTimer->start(10);

    // 高频 MouseMove 演示源：向自身投递合成鼠标移动事件（约 200 次/秒）
    auto* moveTimer = new QTimer(this);
    connect(moveTimer, &QTimer::timeout, this, [this]() {
        QMouseEvent move(QEvent::MouseMove,
                         QPointF(10.0, 10.0),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(this, &move);
    });
    moveTimer->start(5);

    // MetaCall 演示源 1：queued invokeMethod（functor 形态，slotObj 路径）
    auto* metaCallTimer = new QTimer(this);
    connect(metaCallTimer, &QTimer::timeout, this, [this]() {
        QMetaObject::invokeMethod(this, []() {
            QThread::msleep(5);
        }, Qt::QueuedConnection);
    });
    metaCallTimer->start(50);

    // MetaCall 演示源 2：queued signal → lambda（真 signalId 路径）
    auto* queuedSignalTimer = new QTimer(this);
    connect(queuedSignalTimer, &QTimer::timeout, this, []() {
        QThread::msleep(2);
    }, Qt::QueuedConnection);
    queuedSignalTimer->start(100);

    // 3B sender 生命周期压力源：queued 槽内直接 delete sender
    auto* stressTimer = new QTimer(this);
    connect(stressTimer, &QTimer::timeout, this, [this]() {
        auto* worker = new EphemeralWorker(this);
        connect(worker, &EphemeralWorker::workDone, this, [worker]() {
            QThread::msleep(2);
            delete worker;	// sender 在槽内自毁 → 之后记录日志时 sender 已悬空
        }, Qt::QueuedConnection);
        worker->run();
    });
    stressTimer->start(100);

    // 跨线程 MetaCall 演示源（PRD 06 §5）：worker 线程 emit → GUI 线程执行
    auto* crossThreadTimer = new QTimer(nullptr);	// 无 parent 才可 moveToThread
    crossThreadTimer->setInterval(300);
    auto* crossThread = new QThread(this);
    crossThreadTimer->moveToThread(crossThread);
    connect(crossThread, &QThread::started, crossThreadTimer, [crossThreadTimer]() {
        crossThreadTimer->start();
    });
    connect(crossThreadTimer, &QTimer::timeout, this, []() {
        QThread::msleep(2);	// queued 执行于 GUI 线程；发射发生在 worker 线程
    }, Qt::QueuedConnection);
    connect(qApp, &QCoreApplication::aboutToQuit, crossThread, &QThread::quit);
    connect(crossThread, &QThread::finished, crossThreadTimer, &QObject::deleteLater);
    crossThread->start();

    buildUi();

    // UI 数据轮询：KPI / 最近异常 / 明细表（WatchLogCapture 快照，纯内存）
    auto* panelTimer = new QTimer(this);
    connect(panelTimer, &QTimer::timeout, this, &MainWindow::refreshMonitorPanel);
    panelTimer->start(500);

    // 自动演示序列：切 accent → 外部加载 → 高频刷新 3 秒（无交互验证 Bit3 全路径）
    QTimer::singleShot(800, this, [this]() {
        applyAccent(QString::fromLatin1(style_tokens::AccentTeal));
        applyAccent(QString::fromLatin1(style_tokens::AccentAmber));
        loadExternalQss();
        if (m_styleSpamTimer == nullptr) {
            m_styleSpamTimer = new QTimer(this);
            connect(m_styleSpamTimer, &QTimer::timeout, this, [this]() {
                if (m_statusLabel == nullptr)	return;
                const char* colors[] = {style_tokens::AccentBlue, style_tokens::AccentTeal};
                m_spamFlip ^= 1;
                QssStyleWatcher::instance()->setStyleSheet(m_statusLabel,
                    QStringLiteral("color:%1; font-weight:600;").arg(colors[m_spamFlip]));
            });
        }
        m_styleSpamTimer->start(50);
        QTimer::singleShot(3000, this, [this]() {
            if (m_styleSpamTimer != nullptr)	m_styleSpamTimer->stop();
            if (m_statusLabel != nullptr)
                m_statusLabel->setText(tr("自动演示完成 — 详见 logs/QtEventWatcher.log"));
        });
        // 自动演示尾段：触发一次确定性嵌套事件（验证 depth/exclusive，PRD 05 §6）
        QTimer::singleShot(4000, this, []() {	triggerNestedEvent();	});
    });
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    central->setObjectName("CentralWidget");
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ============ 顶栏（chrome bar）============
    auto* header = new QFrame(central);
    header->setObjectName("HeaderBar");
    header->setFixedHeight(style_tokens::HeaderH);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, 0, 20, 0);
    headerLayout->setSpacing(10);

    auto* title = new QLabel(tr("QtEventWatcher"), header);
    title->setObjectName("HeaderTitle");
    auto* subtitle = new QLabel(tr("卡顿排查辅助器"), header);
    subtitle->setObjectName("SubLabel");
    m_watchStatus = new QLabel(header);

    headerLayout->addWidget(title);
    headerLayout->addWidget(subtitle);
    headerLayout->addStretch(1);

    // 主题切换（Dark/Light，PRD 19 §9；accent 三个按钮演示运行时令牌重建）
    auto* themeGroup = new QButtonGroup(this);
    themeGroup->setExclusive(true);
    const struct { const char* id; const char* label; } themes[] = {
        {"dark", "Dark"}, {"light", "Light"},
    };
    for (const auto& t : themes) {
        auto* b = new QPushButton(QString::fromLatin1(t.label), header);
        b->setCheckable(true);
        b->setChecked(t.id == m_theme);
        connect(b, &QPushButton::clicked, this, [this, t]() {
            m_theme = QString::fromLatin1(t.id);
            applyTheme();
        });
        themeGroup->addButton(b);
        headerLayout->addWidget(b);
    }

    const struct { const char* key; const char* label; } accents[] = {
        {style_tokens::AccentBlue, "Blue"},
        {style_tokens::AccentTeal, "Teal"},
        {style_tokens::AccentAmber, "Amber"},
    };
    for (const auto& a : accents) {
        auto* b = new QPushButton(QString::fromLatin1(a.label), header);
        b->setProperty("accent", true);
        connect(b, &QPushButton::clicked, this, [this, a]() {
            applyAccent(QString::fromLatin1(a.key));
        });
        headerLayout->addWidget(b);
    }
    headerLayout->addWidget(m_watchStatus);

    root->addWidget(header);

    // ============ 主体：侧边导航 + 七页内容（PRD 18 §2）============
    auto* body = new QHBoxLayout();
    body->setSpacing(0);

    auto* side = new QFrame(central);
    side->setObjectName("SideNav");
    side->setFixedWidth(style_tokens::SideNavW);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 16, 12, 16);
    sideLayout->setSpacing(6);

    const struct { int id; const char* label; } navs[] = {
        {0, "概览"}, {1, "慢事件"}, {2, "MetaCall"}, {3, "高频事件"},
        {4, "QSS"}, {5, "运行环境"}, {6, "测试场景"},
    };
    auto* navGroup = new QButtonGroup(this);
    navGroup->setExclusive(true);
    for (const auto& n : navs) {
        auto* b = new QPushButton(tr(n.label), side);
        b->setProperty("nav", true);
        b->setCheckable(true);
        b->setChecked(n.id == 0);
        navGroup->addButton(b, n.id);
        sideLayout->addWidget(b);
    }
    sideLayout->addStretch(1);

    m_pages = new QStackedWidget(central);
    m_pages->addWidget(buildOverviewPage());

    // 四类明细页（PRD 18 §4，WatchLogCapture 前缀过滤，Core 零改动）
    const QVector<DetailPage> specs = {
        { nullptr, nullptr, WatchLogCapture::CatSlowEvent,
          { {"Time", ""}, {"Kind", "$op"}, {"Event", "event"}, {"Type", "type"},
            {"Receiver", "receiver"}, {"Object", "object"},
            {"Cost (ms)", "costMs"}, {"Threshold (ms)", "thresholdMs"} } },
        { nullptr, nullptr, WatchLogCapture::CatMetaCall,
          { {"Time", ""}, {"Kind", "$op"}, {"Sender", "sender"}, {"Signal", "signal"},
            {"SignalId", "signalId"}, {"Receiver", "receiver"}, {"Object", "object"},
            {"Cost (ms)", "costMs"}, {"Threshold (ms)", "thresholdMs"} } },
        { nullptr, nullptr, WatchLogCapture::CatEventStat,
          { {"Time", ""}, {"Kind", "$op"}, {"Event", "event"}, {"Count", "count"},
            {"Total Cost (ms)", "totalCostMs"}, {"Threshold", "threshold|thresholdMs"} } },
        { nullptr, nullptr, WatchLogCapture::CatQss,
          { {"Time", ""}, {"Operation", "$op"}, {"File / Object", "File|Widget"},
            {"Count", "Count|StyleLen"}, {"Cost (ms)", "Cost|Total|MaxCost"},
            {"Extra", "Size|Window"} } },
    };
    m_detailPages = specs;
    for (int i = 0; i < m_detailPages.size(); ++i)
        m_pages->addWidget(buildDetailPage(m_detailPages[i], tr(navs[i + 1].label)));

    m_pages->addWidget(buildRuntimePage());
    m_pages->addWidget(buildTestPage());

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    // Qt 5.15+：idClicked 为标准信号（buttonClicked 自 5.15 弃用且有重载歧义）
    connect(navGroup, QOverload<int>::of(&QButtonGroup::idClicked), this,
#else
    // Qt 5.14：无 idClicked 信号（官方 5.14 文档核实），回退 buttonClicked(int) 重载
    connect(navGroup, QOverload<int>::of(&QButtonGroup::buttonClicked), this,
#endif
            [this](int id) {
                m_pages->setCurrentIndex(id);
                // 打开运行环境页时轻量刷新；完整扫描仅在按钮触发（PRD 16 §1）
                if (id == 5)	refreshRuntimePage(false);
            });

    body->addWidget(side);
    body->addWidget(m_pages, 1);
    root->addLayout(body, 1);

    // 快捷键（PRD 19 §7）：Ctrl+E 导出报告；Ctrl+Shift+E 导出批量数据（V2 A1）
    auto* exportAction = new QAction(this);
    exportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportReport);
    addAction(exportAction);

    auto* exportDataAction = new QAction(this);
    exportDataAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(exportDataAction, &QAction::triggered, this, &MainWindow::exportData);
    addAction(exportDataAction);

    setCentralWidget(central);
    resize(1024, 720);
}

QWidget* MainWindow::buildOverviewPage()
{
    auto* page = new QWidget(m_pages);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(style_tokens::PadLg, style_tokens::PadLg,
                               style_tokens::PadLg, style_tokens::PadLg);
    layout->setSpacing(style_tokens::PadMd);

    // ---- KPI 计数卡行 ----
    auto* kpiRow = new QHBoxLayout();
    kpiRow->setSpacing(12);
    kpiRow->addWidget(makeKpiCard(tr("慢事件"), page), 1);
    kpiRow->addWidget(makeKpiCard(tr("慢 MetaCall"), page), 1);
    kpiRow->addWidget(makeKpiCard(tr("高频事件"), page), 1);
    kpiRow->addWidget(makeKpiCard(tr("QSS 告警"), page), 1);
    layout->addLayout(kpiRow);

    // ---- 最近异常（PRD 18 §3 / §7：无异常显示 ✓ 暂无异常）----
    auto* sectionTitle = new QLabel(tr("最近异常"), page);
    sectionTitle->setObjectName("HeaderTitle");
    sectionTitle->setStyleSheet(
        QStringLiteral("font-size:%1px; font-weight:600;").arg(style_tokens::FontBody));
    layout->addWidget(sectionTitle);

    m_overviewEmpty = new QLabel(tr("✓ 暂无异常"), page);
    m_overviewEmpty->setObjectName("EmptyState");
    layout->addWidget(m_overviewEmpty);

    m_warnList = new QListWidget(page);
    layout->addWidget(m_warnList, 1);

    return page;
}

QWidget* MainWindow::buildDetailPage(DetailPage& page, const QString& heading)
{
    auto* widget = new QWidget(m_pages);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(style_tokens::PadLg, style_tokens::PadLg,
                               style_tokens::PadLg, style_tokens::PadLg);
    layout->setSpacing(style_tokens::PadMd);

    auto* title = new QLabel(heading, widget);
    title->setStyleSheet(
        QStringLiteral("font-size:%1px; font-weight:600; background:transparent;")
            .arg(style_tokens::FontTitle));
    layout->addWidget(title);

    page.hint = new QLabel(tr("暂无诊断数据"), widget);
    page.hint->setObjectName("PageHint");
    layout->addWidget(page.hint);

    page.table = new QTableWidget(widget);
    page.table->setColumnCount(page.columns.size());
    QStringList headers;
    for (const DetailColumn& c : page.columns)
        headers << QString::fromLatin1(c.title);
    page.table->setHorizontalHeaderLabels(headers);
    page.table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    page.table->horizontalHeader()->setStretchLastSection(true);
    page.table->verticalHeader()->setVisible(false);
    page.table->verticalHeader()->setDefaultSectionSize(26);	// 行高统一（PRD 19 §2）
    page.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    page.table->setSelectionBehavior(QAbstractItemView::SelectRows);
    page.table->setShowGrid(false);	// 少边框，层级靠背景/间距
    layout->addWidget(page.table, 1);

    return widget;
}

QWidget* MainWindow::buildRuntimePage()
{
    auto* page = new QWidget(m_pages);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(style_tokens::PadLg, style_tokens::PadLg,
                               style_tokens::PadLg, style_tokens::PadLg);
    layout->setSpacing(12);

    auto* card = new QGroupBox(tr("运行环境诊断（Runtime Diagnostics）"), page);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(8);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);

    auto* fullButton = new QPushButton(tr("完整诊断（模块 + PE 依赖分析）"), card);
    fullButton->setToolTip(tr("枚举进程模块并解析 PE Import Directory；仅在主动点击时执行"));
    connect(fullButton, &QPushButton::clicked, this, [this]() {
        refreshRuntimePage(true);
    });

    // 导出报告（PRD 17 §5，PRD 19 §6 Primary 层级）：.txt / .json 由后缀决定
    auto* exportButton = new QPushButton(tr("导出报告"), card);
    exportButton->setProperty("accent", true);
    exportButton->setToolTip(tr("导出诊断报告 .txt / .json（Ctrl+E）"));
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportReport);

    // 导出批量数据（V2 A1）：慢事件 / MetaCall 全量记录，.csv / .json 由后缀决定
    auto* exportDataButton = new QPushButton(tr("导出数据"), card);
    exportDataButton->setToolTip(tr("导出慢事件与 MetaCall 全量记录 .csv / .json（Ctrl+Shift+E）"));
    connect(exportDataButton, &QPushButton::clicked, this, &MainWindow::exportData);

    // 诊断摘要（V2 A3）：规则化归因结果直接显示在下方文本区
    auto* summaryButton = new QPushButton(tr("诊断摘要"), card);
    summaryButton->setToolTip(tr("基于已捕获数据生成性能诊断摘要（主线程阻塞 TOP / 高频信号 / QSS 抖动）"));
    connect(summaryButton, &QPushButton::clicked, this, &MainWindow::generateSummary);

    buttonRow->addWidget(fullButton);
    buttonRow->addWidget(exportButton);
    buttonRow->addWidget(exportDataButton);
    buttonRow->addWidget(summaryButton);
    buttonRow->addStretch(1);
    cardLayout->addLayout(buttonRow);
    m_diagHint = new QLabel(tr("打开本页已刷新概览；完整扫描请点击按钮"), card);
    m_diagHint->setObjectName("PageHint");
    cardLayout->addWidget(m_diagHint);

    m_diagView = new QPlainTextEdit(card);
    m_diagView->setReadOnly(true);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::TypeWriter);
    m_diagView->setFont(mono);
    cardLayout->addWidget(m_diagView, 1);

    layout->addWidget(card);
    return page;
}

QWidget* MainWindow::buildTestPage()
{
    auto* page = new QWidget(m_pages);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(style_tokens::PadLg, style_tokens::PadLg,
                               style_tokens::PadLg, style_tokens::PadLg);
    layout->setSpacing(12);

    // DEMO/TEST 徽标（PRD 18 §5：与正式诊断视觉隔离）
    auto* badgeRow = new QHBoxLayout();
    auto* badge = new QLabel(tr("DEMO / TEST"), page);
    badge->setObjectName("DemoBadge");
    badgeRow->addWidget(badge);
    badgeRow->addWidget(new QLabel(tr("以下触发器仅用于功能验证与演示，不产生真实业务负载"), page));
    badgeRow->addStretch(1);
    layout->addLayout(badgeRow);

    // ---- 卡顿触发器卡 ----
    auto* triggerCard = new QGroupBox(tr("卡顿触发器"), page);
    auto* triggerLayout = new QHBoxLayout(triggerCard);
    triggerLayout->setSpacing(8);

    auto* slowEventButton = new QPushButton(tr("制造 100ms 慢事件"), triggerCard);
    connect(slowEventButton, &QPushButton::clicked, this, []() {
        QThread::msleep(100);	// Timer 事件超 30ms 阈值 → Bit0 WARN
    });

    auto* slowMetaCallButton = new QPushButton(tr("制造慢 MetaCall"), triggerCard);
    connect(slowMetaCallButton, &QPushButton::clicked, this, [this]() {
        QMetaObject::invokeMethod(this, []() {
            QThread::msleep(80);	// queued MetaCall 超 30ms 阈值 → Bit1 WARN
        }, Qt::QueuedConnection);
    });

    auto* nestedButton = new QPushButton(tr("制造嵌套事件"), triggerCard);
    connect(nestedButton, &QPushButton::clicked, this, &triggerNestedEvent);

    triggerLayout->addWidget(slowEventButton);
    triggerLayout->addWidget(slowMetaCallButton);
    triggerLayout->addWidget(nestedButton);
    triggerLayout->addStretch(1);
    layout->addWidget(triggerCard);

    // ---- QSS 监控演示卡（Bit3 埋点流程）----
    auto* styleCard = new QGroupBox(tr("QSS 监控演示（Watch_Fun bit3）"), page);
    auto* cardLayout = new QVBoxLayout(styleCard);
    cardLayout->setSpacing(10);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);

    auto* loadButton = new QPushButton(tr("载入外部 QSS"), styleCard);
    loadButton->setToolTip(tr("beginLoadQss → 读取 res/style.qss → 包装 setStyleSheet → endLoadQss"));
    connect(loadButton, &QPushButton::clicked, this, &MainWindow::loadExternalQss);

    auto* spamButton = new QPushButton(tr("高频样式刷新：关"), styleCard);
    connect(spamButton, &QPushButton::clicked, this, [this, spamButton]() {
        if (m_styleSpamTimer == nullptr) {
            // 约 20 次/秒：超 QssFrequentCountThreshold(10) → 周期 flush 输出（PRD 10）
            m_styleSpamTimer = new QTimer(this);
            connect(m_styleSpamTimer, &QTimer::timeout, this, [this]() {
                if (m_statusLabel == nullptr)	return;
                const char* colors[] = {style_tokens::AccentBlue, style_tokens::AccentTeal};
                m_spamFlip ^= 1;
                QssStyleWatcher::instance()->setStyleSheet(m_statusLabel,
                    QStringLiteral("color:%1; font-weight:600;").arg(colors[m_spamFlip]));
            });
        }
        if (m_styleSpamTimer->isActive()) {
            m_styleSpamTimer->stop();
            spamButton->setText(tr("高频样式刷新：关"));
        } else {
            m_styleSpamTimer->start(50);
            spamButton->setText(tr("高频样式刷新：开"));
        }
    });

    auto* bigStyleButton = new QPushButton(tr("制造大样式更新"), styleCard);
    connect(bigStyleButton, &QPushButton::clicked, this, [this]() {
        const QString big = QStringLiteral("color:#ffffff;padding:%1px;").arg(style_tokens::PadMd) +
                            QStringLiteral("/*pad*/").repeated(2600);
        QssStyleWatcher::instance()->setStyleSheet(this, big);
        if (m_statusLabel != nullptr)
            m_statusLabel->setText(tr("已应用 %1 字符大样式").arg(big.size()));
    });

    buttonRow->addWidget(loadButton);
    buttonRow->addWidget(spamButton);
    buttonRow->addWidget(bigStyleButton);
    buttonRow->addStretch(1);
    cardLayout->addLayout(buttonRow);

    m_statusLabel = new QLabel(tr("监控链路待命 — 打开 Watch_Fun=8 观察本卡片的 QSS 统计"), styleCard);
    m_statusLabel->setObjectName("StatusLabel");
    cardLayout->addWidget(m_statusLabel);

    layout->addWidget(styleCard);
    layout->addStretch(1);
    return page;
}

void MainWindow::applyTheme()
{
    // 双主题切换：令牌源整体重建（PRD 19 §9），经包装接口被 Bit3 观测
    QssStyleWatcher::instance()->setStyleSheet(this,
        style_tokens::buildAppStyle(m_theme, m_accent));
}

void MainWindow::applyAccent(const QString& accent)
{
    // 运行时令牌路径：单一令牌源重新组装 → 包装接口（PRD 09）
    m_accent = accent;
    QssStyleWatcher::instance()->setStyleSheet(this, style_tokens::buildAppStyle(m_theme, accent));
    if (m_statusLabel != nullptr)
        m_statusLabel->setText(tr("主题已切换 accent=%1").arg(accent));
}

void MainWindow::refreshRuntimePage(bool full)
{
    if (m_diagView == nullptr)
        return;

    if (!full) {
        // 轻量概览：无模块枚举、无 PE 解析（PRD 16 §1）
        const auto overview = RuntimeDiagnostics::collectOverview();
        m_diagView->setPlainText(formatRuntimeOverview(overview));
        if (m_diagHint != nullptr)
            m_diagHint->setText(tr("概览已刷新（轻量）— 完整扫描请点击按钮"));
        return;
    }

    const DiagnosticsReport report = RuntimeDiagnostics::collectFull();
    m_diagView->setPlainText(formatFullReport(report));
    if (m_diagHint != nullptr) {
        QString summary = tr("完整诊断已刷新 %1").arg(report.generatedAt);
        if (!report.dependencies.ok)
            summary += tr(" | ") + report.dependencies.failReason;
        else
            summary += tr(" | problems=%1 missing=%2")
                           .arg(report.dependencies.problemCount())
                           .arg(report.dependencies.missingCount());
        m_diagHint->setText(summary);
    }
}

void MainWindow::exportReport()
{
    // PRD 17 §5：路径由用户选择，格式由后缀决定（.json → JSON，其余 → TXT）
    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("导出诊断报告"), QStringLiteral("QtEventWatcherReport"),
        tr("诊断报告 (*.txt *.json)"));
    if (filePath.isEmpty())
        return;

    // Monitor 段数据源：CusApplication 上的全局配置（只读传递）。
    // app 类型由 main.cpp 保证为 CusApplication，无需 Q_OBJECT 反射
    const auto* app = static_cast<const CusApplication*>(QCoreApplication::instance());
    QString error;
    const bool ok = ReportExporter::exportReport(filePath,
                                                 app ? app->watchConfig() : nullptr,
                                                 &error);

    // 轻量结果反馈（PRD 19 §7：Toast 语义，不弹窗打断）
    if (m_diagHint != nullptr) {
        m_diagHint->setText(ok ? tr("✓ 报告已导出 %1").arg(filePath)
                               : tr("✗ 导出失败：%1").arg(error));
    }
}

void MainWindow::exportData()
{
    // V2 A1：慢事件 / MetaCall 全量记录，格式由后缀决定（.json → JSON，其余 → CSV）
    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("导出监控数据"), QStringLiteral("QtEventWatcherData"),
        tr("监控数据 (*.csv *.json)"));
    if (filePath.isEmpty())
        return;

    const auto* app = static_cast<const CusApplication*>(QCoreApplication::instance());
    QString error;
    const bool ok = DataExporter::exportData(filePath,
                                             app ? app->watchConfig() : nullptr,
                                             app ? app->eventStatistics() : nullptr,
                                             &error);

    if (m_diagHint != nullptr) {
        m_diagHint->setText(ok ? tr("✓ 数据已导出 %1").arg(filePath)
                               : tr("✗ 导出失败：%1").arg(error));
    }
}

void MainWindow::generateSummary()
{
    // V2 A3：规则化归因 → 文本摘要显示在本页文本区（不落盘，导出走报告/数据按钮）
    const auto* app = static_cast<const CusApplication*>(QCoreApplication::instance());
    const auto findings = DiagnosticSummarizer::analyze(
        app ? app->watchConfig() : nullptr,
        app ? app->eventStatistics() : nullptr);

    if (m_diagHint != nullptr)
        m_diagHint->setText(tr("✓ 摘要已生成：共 %1 条归因结论").arg(findings.size()));

    if (m_diagView == nullptr)	return;

    if (findings.isEmpty()) {
        m_diagView->setPlainText(tr("诊断摘要\n========\n\n无归因结论：暂无慢事件 / MetaCall / QSS 告警记录。\n"));
        return;
    }

    static const char* tags[] = { "INFO", "WARNING", "CRITICAL" };
    QString text = tr("诊断摘要\n========\n");
    for (const auto& f : findings) {
        text += tr("[%1] %2\n  %3\n")
                    .arg(QLatin1String(tags[static_cast<int>(f.severity)]), f.category, f.headline);
        for (const QString& d : f.details)
            text += tr("    %1\n").arg(d);
        text += QLatin1Char('\n');
    }
    m_diagView->setPlainText(text);
}

void MainWindow::refreshMonitorPanel()
{
    auto& capture = WatchLogCapture::instance();

    // ---- KPI ----
    if (m_kpiSlowEvent != nullptr)
        m_kpiSlowEvent->setText(QString::number(capture.categoryCount(WatchLogCapture::CatSlowEvent)));
    if (m_kpiMetaCall != nullptr)
        m_kpiMetaCall->setText(QString::number(capture.categoryCount(WatchLogCapture::CatMetaCall)));
    if (m_kpiEventStat != nullptr)
        m_kpiEventStat->setText(QString::number(capture.categoryCount(WatchLogCapture::CatEventStat)));
    if (m_kpiQss != nullptr)
        m_kpiQss->setText(QString::number(capture.categoryCount(WatchLogCapture::CatQss)));

    // ---- 概览最近异常（新→旧，最多 12 条；空态切换 PRD 19 §7）----
    const auto entries = capture.recent(12);
    if (m_overviewEmpty != nullptr)
        m_overviewEmpty->setVisible(entries.empty());
    if (m_warnList != nullptr) {
        m_warnList->clear();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            m_warnList->addItem(QString::fromStdString(it->time + "  " + it->text));
    }

    // ---- 四类明细表（前缀过滤 + key=value 解析）----
    for (const DetailPage& page : m_detailPages) {
        if (page.table == nullptr)
            continue;

        const auto all = capture.recent(32);
        QVector<const WatchLogCapture::Entry*> filtered;
        for (const auto& e : all) {
            if (e.text.rfind(categoryPrefix(page.category), 0) == 0)
                filtered.append(&e);
        }

        // 空态提示
        if (page.hint != nullptr)
            page.hint->setText(filtered.isEmpty() ? tr("暂无诊断数据")
                                : tr("共 %1 条（最近 32 条告警内）").arg(filtered.size()));

        page.table->setRowCount(filtered.size());
        for (int r = 0; r < filtered.size(); ++r) {
            const WatchLogCapture::Entry* e = filtered.at(r);
            const ParsedLog parsed = parseLogBody(page.category, e->text);
            for (int c = 0; c < page.columns.size(); ++c) {
                const DetailColumn& col = page.columns.at(c);
                auto* item = new QTableWidgetItem(detailCell(*e, QString::fromLatin1(col.key), parsed));
                if (isCostKey(QString::fromLatin1(col.key))) {
                    // 耗时数值突出（PRD 19 §7），由 QSS 选择色覆盖选中态
                    item->setForeground(QColor(style_tokens::themeByName(m_theme).warn));
                }
                page.table->setItem(r, c, item);
            }
        }
    }
}

QFrame* MainWindow::makeKpiCard(const QString& title, QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName("KpiCard");
    card->setFixedHeight(style_tokens::KpiCardH);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 10, 14, 10);
    cardLayout->setSpacing(2);

    auto* value = new QLabel(QStringLiteral("0"), card);
    value->setObjectName("KpiValue");
    auto* caption = new QLabel(title, card);
    caption->setObjectName("KpiTitle");
    cardLayout->addWidget(value);
    cardLayout->addWidget(caption);
    cardLayout->addStretch(1);

    // 值标签按 m_kpi* 成员顺序绑定
    if (m_kpiSlowEvent == nullptr)			m_kpiSlowEvent = value;
    else if (m_kpiMetaCall == nullptr)		m_kpiMetaCall = value;
    else if (m_kpiEventStat == nullptr)		m_kpiEventStat = value;
    else									m_kpiQss = value;
    return card;
}

void MainWindow::setWatchStatus(bool active)
{
    if (m_watchStatus == nullptr)	return;
    // 状态不只靠颜色（PRD 19 §9）：文字 + 状态点
    m_watchStatus->setText(active ? tr("● 监控中") : tr("○ 已停止"));
    const style_tokens::ThemeTokens& themeT = style_tokens::themeByName(m_theme);
    const QString& color = active ? themeT.ok : themeT.textDim;
    m_watchStatus->setStyleSheet(QStringLiteral(
        "color:%1; padding:%2px 10px; border-radius:%3px;"
        " background:%4; border:1px solid %5; font-weight:600;")
        .arg(color)
        .arg(style_tokens::PadSm)
        .arg(style_tokens::RadiusBadge)
        .arg(themeT.surface)
        .arg(themeT.border));
}

void MainWindow::loadExternalQss()
{
    // PRD 08 埋点流程：begin → 业务读取文件 + setStyleSheet → end
    QssStyleWatcher* watcher = QssStyleWatcher::instance();
    watcher->beginLoadQss("./res/style.qss");

    QFile file("./res/style.qss");
    QString content;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        content = QString::fromUtf8(file.readAll());
    watcher->setStyleSheet(this, content);

    watcher->endLoadQss();
    if (m_statusLabel != nullptr)
        m_statusLabel->setText(tr("外部 QSS 已载入（res/style.qss，%1 字符）").arg(content.size()));
}

}  // namespace qt_event_watcher
