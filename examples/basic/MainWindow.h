#pragma once

#include <QMainWindow>
#include <QVector>

class QFrame;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace qt_event_watcher {

/**
 * @brief 短生命周期信号发送者：由定时器创建、queued 槽执行时自毁，
 * 用于复现"日志时刻 sender 已悬空"的真实竞态场景。
 */
class EphemeralWorker : public QObject
{
    Q_OBJECT

public:
    explicit EphemeralWorker(QObject* parent = nullptr) : QObject(parent) {}
    void run() { emit workDone(); }

Q_SIGNALS:
    void workDone();
};

/**
 * @brief 监控演示主窗口（PRD 18 七页结构 + PRD 19 视觉规范）
 *
 * 顶栏（品牌 + 监控状态 + 主题切换 + accent）
 *   └ 左侧导航 + 内容页（QStackedWidget）
 *       ├ 概览（默认首页）：KPI 卡行 + 最近异常（含空态）
 *       ├ 慢事件 / MetaCall / 高频事件 / QSS：四类明细表（WatchLogCapture 前缀过滤）
 *       ├ 运行环境：Runtime Diagnostics（PRD 16）
 *       └ 测试场景：DEMO/TEST 徽标视觉隔离的触发器与 QSS 演示
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    /// 顶栏监控状态点（由 main.cpp 依据 Watch_Fun 配置调用）
    void setWatchStatus(bool active);

private:
    /// 明细页描述：列标题 + 日志键（"" = 采集时间，"$op" = 前缀短语）
    struct DetailColumn
    {
        const char* title;
        const char* key;
    };
    struct DetailPage
    {
        QTableWidget* table = nullptr;
        QLabel* hint = nullptr;
        int category = 0;                   ///< WatchLogCapture::Category
        QVector<DetailColumn> columns;
    };

    /// 主题/accent 切换：重建 QSS（经 QssStyleWatcher，被 Bit3 观测）
    void applyTheme();
    void applyAccent(const QString& accent);
    /// Bit3 演示：外部 QSS 文件加载（PRD 08 埋点流程）
    void loadExternalQss();
    /// 布局组装：顶栏 + 七页导航
    void buildUi();
    QWidget* buildOverviewPage();
    QWidget* buildDetailPage(DetailPage& page, const QString& heading);
    QWidget* buildRuntimePage();
    QWidget* buildTestPage();
    /// 运行环境页刷新：full=false 打开页面时轻量概览（PRD 16 §1）；
    /// full=true 用户主动触发完整诊断（模块枚举 + PE Import 分析）
    void refreshRuntimePage(bool full);
    /// 诊断报告导出（PRD 17 §5）：文件对话框选路径 → ReportExporter，Ctrl+E
    void exportReport();
    void exportData();
    /// HTML 报告导出（V3 C2）：自包含静态报告，Ctrl+Shift+H
    void exportHtmlReport();
    /// 诊断摘要（V2 A3）：DiagnosticSummarizer.analyze → 显示在本页文本区
    void generateSummary();
    /// 500ms 轮询：KPI + 最近异常 + 明细表 + 空态切换
    void refreshMonitorPanel();
    /// KPI 卡片工厂（卡内值标签按调用顺序绑定 m_kpi*）
    QFrame* makeKpiCard(const QString& title, QWidget* parent);

    // 主题状态
    QString m_theme = QStringLiteral("dark");
    QString m_accent;

    // 测试场景页状态
    QLabel* m_statusLabel = nullptr;
    QTimer* m_styleSpamTimer = nullptr;
    int m_spamFlip = 0;

    // 顶栏
    QLabel* m_watchStatus = nullptr;

    // 概览页
    QLabel* m_kpiSlowEvent = nullptr;
    QLabel* m_kpiMetaCall = nullptr;
    QLabel* m_kpiEventStat = nullptr;
    QLabel* m_kpiQss = nullptr;
    QListWidget* m_warnList = nullptr;
    QLabel* m_overviewEmpty = nullptr;
    QStackedWidget* m_pages = nullptr;

    // 四类明细页
    QVector<DetailPage> m_detailPages;

    // 运行环境页
    QPlainTextEdit* m_diagView = nullptr;
    QLabel* m_diagHint = nullptr;
};

}  // namespace qt_event_watcher
