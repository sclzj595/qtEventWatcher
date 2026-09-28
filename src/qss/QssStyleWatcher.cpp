#include "QssStyleWatcher.h"

#include "WatchConfig.h"
#include "WatchLogMacros.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QWidget>

#include <algorithm>
#include <vector>

namespace qt_event_watcher
{

namespace
{
constexpr int kTopN = 5;

QString widgetKey(const QWidget *widget)
{
	const QString className = QString::fromLatin1(widget->metaObject()->className());
	const QString objectName = widget->objectName();
	return objectName.isEmpty() ? className : className + QLatin1Char('/') + objectName;
}
} // namespace

QssStyleWatcher::QssStyleWatcher()
	: m_loadTimer(new QElapsedTimer),
	  m_periodTimer(new QElapsedTimer)
{
	m_periodTimer->start();
}

QssStyleWatcher::~QssStyleWatcher()
{
	delete m_loadTimer;
	delete m_periodTimer;
}

QssStyleWatcher *QssStyleWatcher::instance()
{
	static QssStyleWatcher s_instance;
	return &s_instance;
}

void QssStyleWatcher::setup(WatchConfig *config)
{
	m_config = config;
}

bool QssStyleWatcher::isEnabled() const
{
	return m_config != nullptr &&
	       m_config->isWatchEnabled(WatchConfig::WatchQssMonitor);
}

void QssStyleWatcher::beginLoadQss(const QString &filePath)
{
	if (!isEnabled())	return;

	QFileInfo info(filePath);
	m_loadFilePath = filePath;
	m_loadFileSizeBytes = info.exists() ? info.size() : -1;
	m_styleCostInLoadMs = 0.0;
	m_loadTimer->start();
	m_loadActive = true;
}

void QssStyleWatcher::endLoadQss()
{
	if (!isEnabled() || !m_loadActive)	return;
	m_loadActive = false;

	const double totalMs = static_cast<double>(m_loadTimer->nsecsElapsed()) / 1000000.0;
	// 代理拆分（PRD 08 §4）：样式部分 = 窗口内包装调用实测耗时，IO ≈ Total - Style
	const double styleMs = m_styleCostInLoadMs;
	const double ioMs = std::max(0.0, totalMs - styleMs);

	++m_totalLoadCount;
	++m_totalFileCount;
	m_maxFileSizeBytes = std::max(m_maxFileSizeBytes, m_loadFileSizeBytes);
	m_maxIoCostMs = std::max(m_maxIoCostMs, ioMs);
	m_maxApplyCostMs = std::max(m_maxApplyCostMs, styleMs);

	if (totalMs >= static_cast<double>(m_config->qssLoadThresholdMs())) {
		QEW_LOG_WARN(
		    "[QssStyleWatcher] SlowQssLoad | File={} Size={}KB IO={:.1f}ms Style={:.1f}ms Total={:.1f}ms",
		    m_loadFilePath.toStdString(),
		    m_loadFileSizeBytes < 0 ? -1 : static_cast<int>(m_loadFileSizeBytes / 1024),
		    ioMs, styleMs, totalMs);
	}
}

bool QssStyleWatcher::setStyleSheet(QWidget *widget, const QString &style)
{
	if (widget == nullptr)	return false;

	if (!isEnabled()) {
		// Bit3 关闭：直通，零统计成本（PRD 14）
		widget->setStyleSheet(style);
		return true;
	}

	maybeFlushPeriod();

	const double startMs = static_cast<double>(m_periodTimer->nsecsElapsed()) / 1000000.0;
	widget->setStyleSheet(style);
	const double costMs = static_cast<double>(m_periodTimer->nsecsElapsed()) / 1000000.0 - startMs;

	// PRD 09 §4：总调用次数 / 单控件次数 / 单次最大耗时 / 文本长度
	const QString key = widgetKey(widget);
	WidgetStat &stat = m_periodStats[key];
	stat.className = QString::fromLatin1(widget->metaObject()->className());
	stat.objectName = widget->objectName();
	++stat.count;
	stat.maxCostMs = std::max(stat.maxCostMs, costMs);
	stat.maxStyleLen = std::max(stat.maxStyleLen, static_cast<int>(style.size()));	// Qt6 size()=qsizetype，Qt5=int，统一收窄到字段类型

	m_maxApplyCostMs = std::max(m_maxApplyCostMs, costMs);

	if (m_loadActive)
		m_styleCostInLoadMs += costMs;

	if (costMs >= static_cast<double>(m_config->setStyleSheetThresholdMs())) {
		QEW_LOG_WARN(
		    "[QssStyleWatcher] SlowSetStyleSheet | Widget={} StyleLen={} Cost={:.1f}ms",
		    key.toStdString(), style.size(), costMs);
	}

	return true;
}

void QssStyleWatcher::flushStatistics()
{
	if (!isEnabled())	return;

	// 高频 Widget TOP N（PRD 10 §2）
	std::vector<const WidgetStat *> ranked;
	ranked.reserve(m_periodStats.size());
	for (const WidgetStat &stat : m_periodStats)
		ranked.push_back(&stat);
	std::sort(ranked.begin(), ranked.end(),
	    [](const WidgetStat *a, const WidgetStat *b) { return a->count > b->count; });

	const int frequentThreshold = m_config->qssFrequentCountThreshold();
	const qint64 windowMs = m_config->eventStatPeriodMs();
	int warned = 0;
	for (const WidgetStat *stat : ranked) {
		if (warned >= kTopN)	break;
		if (stat->count < frequentThreshold)	break;
		const QString name = stat->objectName.isEmpty()
		    ? stat->className : stat->className + QLatin1Char('/') + stat->objectName;
		QEW_LOG_WARN(
		    "[QssStyleWatcher] FrequentStyleUpdate | Widget={} Count={} Window={}ms MaxCost={:.1f}ms MaxLen={}",
		    name.toStdString(), stat->count, windowMs, stat->maxCostMs, stat->maxStyleLen);
		++warned;
	}

	// 周期汇总 + 资源累计（PRD 10 §3）
	QEW_LOG_INFO(
	    "[QssStyleWatcher] QssStat({}ms) | periodCalls={} frequentWarned={} "
	    "totalLoads={} files={} maxFile={}KB maxApply={:.1f}ms",
	    windowMs,
	    m_periodStats.size(), warned,
	    m_totalLoadCount, m_totalFileCount,
	    static_cast<int>(m_maxFileSizeBytes / 1024), m_maxApplyCostMs);

	m_periodStats.clear();
	m_periodTimer->start();
}

void QssStyleWatcher::maybeFlushPeriod()
{
	if (static_cast<qint64>(m_periodTimer->elapsed()) >= m_config->eventStatPeriodMs())
		flushStatistics();
}

} // namespace qt_event_watcher
