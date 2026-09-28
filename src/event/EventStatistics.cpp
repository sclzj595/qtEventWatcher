#include "EventStatistics.h"
#include "WatchConfig.h"
#include "WatchLogMacros.h"

#include <QMutexLocker>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

/// floor(log2(v)) 的桶下标（v>0）；v==0 归入桶 0。
/// MSVC 用 _BitScanReverse64，GCC/Clang 用 __builtin_clzll（C++17 无 <bit>）
inline std::size_t log2Bucket(std::uint64_t v)
{
	if (v == 0)	return 0;
#if defined(_MSC_VER)
	unsigned long index = 0;
	_BitScanReverse64(&index, v);
	return static_cast<std::size_t>(index);
#else
	return static_cast<std::size_t>(63 - __builtin_clzll(v));
#endif
}

} // namespace

namespace qt_event_watcher
{

EventStatistics::EventStatistics(WatchConfig *config)
	: m_config(config)
{
	m_periodTimer.start();
}

void EventStatistics::record(int eventType, const QString &eventName, std::int64_t elapsedNs,
							 std::int64_t exclusiveElapsedNs)
{
	if (m_config == nullptr)	return;
    if (!m_config->isWatchEnabled(WatchConfig::WatchEventStatistics))	return;
    if (elapsedNs < 0)	return;

	{
		QMutexLocker locker(&m_mutex);
		const QString key = makeEventKey(eventType, eventName);
		EventStat& stat = m_statistics[key];

		++stat.count;
		stat.totalCostNs += static_cast<std::uint64_t>(elapsedNs);
		if (static_cast<std::uint64_t>(elapsedNs) > stat.maxCostNs)
			stat.maxCostNs = static_cast<std::uint64_t>(elapsedNs);
		if (exclusiveElapsedNs > 0) {
			stat.exclusiveTotalNs += static_cast<std::uint64_t>(exclusiveElapsedNs);
			if (static_cast<std::uint64_t>(exclusiveElapsedNs) > stat.exclusiveMaxNs)
				stat.exclusiveMaxNs = static_cast<std::uint64_t>(exclusiveElapsedNs);
		}
		// log2 bucket：热路径一次位扫描 + 递增（PRD 14 低开销）
		++stat.hist[log2Bucket(static_cast<std::uint64_t>(elapsedNs))];
	}
	checkPeriod();
}

void EventStatistics::reset()
{
	QMutexLocker locker(&m_mutex);
	m_statistics.clear();
	m_history.clear();
	m_periodTimer.restart();
}

void EventStatistics::checkPeriod()
{
	if (m_config == nullptr)	return;
    if (!m_config->isWatchEnabled(WatchConfig::WatchEventStatistics))	return;

    const qint64 periodMs = m_config->eventStatPeriodMs();

    if (periodMs <= 0)	return;
    if (m_periodTimer.elapsed() < periodMs)	return;

    flush();
}

void EventStatistics::flush()
{
	std::int64_t periodMs = 0;
	QHash<QString, EventStat> statistics;
	{
		QMutexLocker locker(&m_mutex);
		if (m_statistics.isEmpty()) {
			m_periodTimer.restart();
			return;
		}
		if (m_periodTimer.elapsed() < m_config->eventStatPeriodMs())	return;
		periodMs = m_periodTimer.elapsed();
        statistics.swap(m_statistics);
        m_periodTimer.restart();
	}

	const int countThreshold = m_config->eventCountThreshold();
	const std::int64_t totalCostThresholdNs = static_cast<std::int64_t>(m_config->eventTotalCostThresholdMs()) * 1000000LL;

	// 注意：遍历的是 swap 出来的局部快照，m_statistics 已被清空
	for (auto it = statistics.cbegin(); it != statistics.cend(); ++it) {
		const EventStat& stat = it.value();
		const bool highFrequency = static_cast<std::int64_t>(stat.count) >= countThreshold;
        const bool highTotalCost = stat.totalCostNs >= static_cast<std::uint64_t>(totalCostThresholdNs);

		if (!highFrequency && !highTotalCost)  continue;

		const double totalCostMs = static_cast<double>(stat.totalCostNs) / 1000000.0;
        if (highFrequency && highTotalCost) {
            QEW_LOG_WARN(
                "[EventStatistics] high frequency and high cost "
                "event={} count={} totalCostMs={:.3f}",
                it.key().toStdString(),
                stat.count,
                totalCostMs);
        } else if (highFrequency) {
            QEW_LOG_WARN(
                "[EventStatistics] high frequency "
                "event={} count={} threshold={}",
                it.key().toStdString(),
                stat.count,
                countThreshold);
        } else {
            QEW_LOG_WARN(
                "[EventStatistics] high total cost "
                "event={} totalCostMs={:.3f} thresholdMs={}",
                it.key().toStdString(),
                totalCostMs,
                m_config->eventTotalCostThresholdMs());
        }
	}

	// 归档到导出历史（V2 A2）：告警之后写入，日志语义不变；
	// 超容量裁剪最旧周期，防长期运行内存膨胀
	{
		PeriodSnapshot snap;
		snap.periodMs = periodMs;
		snap.entries.reserve(statistics.size());
		for (auto it = statistics.cbegin(); it != statistics.cend(); ++it) {
			StatEntry entry;
			entry.eventKey = it.key();
			entry.count = it.value().count;
			entry.totalCostNs = it.value().totalCostNs;
			entry.maxCostNs = it.value().maxCostNs;
			entry.exclusiveTotalNs = it.value().exclusiveTotalNs;
			entry.exclusiveMaxNs = it.value().exclusiveMaxNs;
			entry.hist = it.value().hist;
			snap.entries.append(entry);
		}
		QMutexLocker locker(&m_mutex);
		m_history.append(snap);
		while (m_history.size() > kMaxHistoryPeriods)
			m_history.removeFirst();
	}
}

QVector<EventStatistics::PeriodSnapshot> EventStatistics::statisticsHistory(int maxPeriods) const
{
	QVector<PeriodSnapshot> out;
	QMutexLocker locker(&m_mutex);
	const int start = maxPeriods > 0 && m_history.size() > maxPeriods
						? m_history.size() - maxPeriods : 0;
	for (int i = start; i < m_history.size(); ++i)
		out.append(m_history.at(i));
	return out;
}

EventStatistics::PeriodSnapshot EventStatistics::liveSnapshot() const
{
	PeriodSnapshot snap;
	QMutexLocker locker(&m_mutex);
	if (m_statistics.isEmpty())	return snap;
	snap.periodMs = m_periodTimer.elapsed();
	snap.entries.reserve(m_statistics.size());
	for (auto it = m_statistics.cbegin(); it != m_statistics.cend(); ++it) {
		StatEntry entry;
		entry.eventKey = it.key();
		entry.count = it.value().count;
		entry.totalCostNs = it.value().totalCostNs;
		entry.maxCostNs = it.value().maxCostNs;
		entry.exclusiveTotalNs = it.value().exclusiveTotalNs;
		entry.exclusiveMaxNs = it.value().exclusiveMaxNs;
		entry.hist = it.value().hist;
		snap.entries.append(entry);
	}
	return snap;
}

double EventStatistics::estimatePercentileMs(const StatEntry& entry, double q)
{
	if (entry.count == 0 || q <= 0.0)	return 0.0;
	if (q > 1.0)	q = 1.0;

	// 目标秩（1-based）：q=count 的样本
	const std::uint64_t rank = static_cast<std::uint64_t>(q * static_cast<double>(entry.count) + 0.5);
	std::uint64_t accumulated = 0;
	for (int b = 0; b < kHistBins; ++b) {
		const std::uint64_t bucketCount = entry.hist[static_cast<std::size_t>(b)];
		if (bucketCount == 0)	continue;
		accumulated += bucketCount;
		if (accumulated < rank)	continue;

		// 命中桶 b：范围 [2^b, 2^(b+1)) ns，桶内按秩比例线性插值
		const std::uint64_t accumulatedBefore = accumulated - bucketCount;
		const double frac = rank > accumulatedBefore
			? static_cast<double>(rank - accumulatedBefore) / static_cast<double>(bucketCount)
			: 0.0;
		const double lowNs = static_cast<double>(std::uint64_t(1) << b);
		const double highNs = b + 1 < kHistBins ? static_cast<double>(std::uint64_t(1) << (b + 1)) : lowNs * 2.0;
		return (lowNs + (highNs - lowNs) * frac) / 1000000.0;
	}
	// rank 超出（浮点边界）：返回最大样本值
	return static_cast<double>(entry.maxCostNs) / 1000000.0;
}

QString EventStatistics::makeEventKey(int eventType, const QString &eventName)
{
	return QStringLiteral("%1:%2").arg(eventType).arg(eventName);
}

} // namespace qt_event_watcher
