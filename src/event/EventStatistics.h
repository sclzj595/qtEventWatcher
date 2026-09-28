#pragma once

#include <array>
#include <cstdint>

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

namespace qt_event_watcher
{

class WatchConfig;

/**
 * @brief 周期统计器
 * 统计：
 * - Event 次数
 * - Event 总耗时
 * - 单次最大耗时
 * - 耗时分布（log2 直方图，分位数估计用）
 * - 周期内高频 Event
 * - 周期内高累计耗时 Event
 */
class EventStatistics
{
public:
	/// 直方图槽数：bucket i 计 [2^i, 2^(i+1)) ns（1ns ~ 500 年全覆盖，实际用低 40 槽）
	static constexpr int kHistBins = 64;

	/// 单事件周期聚合值快照（值语义，可跨线程传递，PRD 工程约定）
	struct StatEntry {
		QString eventKey;
		std::uint64_t count = 0;
		std::uint64_t totalCostNs = 0;
		std::uint64_t maxCostNs = 0;
		std::uint64_t exclusiveTotalNs = 0;		///< C2：线程内自耗时累计（扣直接子事件）
		std::uint64_t exclusiveMaxNs = 0;
		std::array<std::uint64_t, kHistBins> hist{};
	};

	/// 一个统计周期的快照（已归档周期或进行中周期）
	struct PeriodSnapshot {
		std::int64_t periodMs = 0;		///< 周期时长
		QVector<StatEntry> entries;		///< 无序（TOP-N 排序由消费方决定）
	};

	explicit EventStatistics(WatchConfig* config);

	EventStatistics(const EventStatistics&) = delete;
	EventStatistics& operator=(const EventStatistics&) = delete;

	void record(int eventType, const QString& eventName, std::int64_t elapsedNs,
				std::int64_t exclusiveElapsedNs = 0);
	void reset();

	/// 最近已归档周期快照（新的在后，最多 maxPeriods 个；只读）
	QVector<PeriodSnapshot> statisticsHistory(int maxPeriods = 8) const;

	/// 当前进行中周期快照（只读拷贝，不清空、不触发告警）
	PeriodSnapshot liveSnapshot() const;

	/// log2 直方图分位数估计（q ∈ [0,1]；桶内线性插值，误差至多 2 倍——诊断用途足够）
	static double estimatePercentileMs(const StatEntry& entry, double q);

private:
	struct EventStat {
		std::uint64_t count = 0;
		std::uint64_t totalCostNs = 0;
		std::uint64_t maxCostNs = 0;
		std::uint64_t exclusiveTotalNs = 0;		///< C2
		std::uint64_t exclusiveMaxNs = 0;		///< C2
		std::array<std::uint64_t, kHistBins> hist{};
	};

private:
	void checkPeriod();
	void flush();
	static QString makeEventKey(int eventType, const QString& eventName);

private:
	WatchConfig* m_config = nullptr;
	QElapsedTimer m_periodTimer;
	QHash<QString, EventStat> m_statistics;

	mutable QMutex m_mutex;		// mutable：const 快照接口（statisticsHistory/liveSnapshot）也需加锁

	/// flush 归档历史（告警后写入；容量上限由 flush 内裁剪，防内存膨胀）
	static constexpr int kMaxHistoryPeriods = 8;
	QVector<PeriodSnapshot> m_history;
};

} // namespace qt_event_watcher
