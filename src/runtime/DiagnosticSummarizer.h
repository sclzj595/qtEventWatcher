#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace qt_event_watcher
{

class WatchConfig;
class EventStatistics;

/**
 * @brief 性能诊断摘要（V2 Phase A3，PRD 21 §5）
 *
 * 基于 A1/A2 已捕获数据（WatchRecordStore 结构化记录 + EventStatistics
 * 周期归档）做规则化归因，不触碰监控热路径：
 * - SlowEventTop：慢事件按 receiver+event 聚合，累计耗时 TOP-5
 * - HighFrequencySignal：慢 MetaCall 按 sender+signal 聚合，频次 TOP-5
 * - QssJitter：QSS 慢操作按 op 聚合计数
 * - StatTop：周期统计跨周期合并，累计耗时 TOP-3
 *
 * 输出经 exportSummary 落盘：.json → 结构化 findings；其余 → TXT。
 */
class DiagnosticSummarizer
{
public:
	enum class Severity { Info = 0, Warning = 1, Critical = 2 };

	struct Finding
	{
		Severity severity = Severity::Info;
		QString category;		///< "SlowEventTop" / "HighFrequencySignal" / "QssJitter" / "StatTop"
		QString headline;		///< 一行结论
		QStringList details;	///< 证据条目
	};

	static QVector<Finding> analyze(const WatchConfig* config,
									const EventStatistics* statistics);

	static bool exportSummary(const QString& filePath, const QVector<Finding>& findings,
							  const WatchConfig* config, QString* error = nullptr);

	DiagnosticSummarizer() = delete;
};

} // namespace qt_event_watcher
