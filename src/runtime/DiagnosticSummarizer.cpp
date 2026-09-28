#include "DiagnosticSummarizer.h"

#include "EventStatistics.h"
#include "WatchConfig.h"
#include "WatchRecordStore.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>

namespace qt_event_watcher
{

namespace
{

using Field = WatchRecordStore::Field;
using Finding = DiagnosticSummarizer::Finding;

const std::string* findField(const std::vector<Field>& fields, const char* key)
{
	for (const Field& f : fields) {
		if (f.first == key)		return &f.second;
	}
	return nullptr;
}

double fieldAsMs(const std::vector<Field>& fields, const char* key)
{
	const std::string* v = findField(fields, key);
	return v != nullptr ? QString::fromStdString(*v).toDouble() : 0.0;
}

/// 慢事件聚合：receiver+event → {count, totalMs, maxMs}
struct EventAgg
{
	QString receiver;
	QString event;
	int count = 0;
	double totalMs = 0.0;
	double maxMs = 0.0;
};

/// 慢 MetaCall 聚合：sender+signal → {count, totalMs, maxMs}
struct MetaAgg
{
	QString sender;
	QString signalName;
	int count = 0;
	double totalMs = 0.0;
	double maxMs = 0.0;
};

QVector<Finding> analyzeSlowEventTop()
{
	const std::vector<WatchRecordStore::Record> records = WatchRecordStore::instance().snapshot();

	QHash<QString, EventAgg> aggs;
	std::size_t slowEventSamples = 0;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind != WatchRecordStore::KindSlowEvent)	continue;
		++slowEventSamples;
		const std::string* recv = findField(r.fields, "receiver");
		const std::string* ev = findField(r.fields, "event");
		if (recv == nullptr || ev == nullptr)	continue;

		const QString key = QString::fromStdString(*recv) + QLatin1Char('|')
							+ QString::fromStdString(*ev);
		EventAgg& agg = aggs[key];
		agg.receiver = QString::fromStdString(*recv);
		agg.event = QString::fromStdString(*ev);
		++agg.count;
		const double costMs = fieldAsMs(r.fields, "costMs");
		agg.totalMs += costMs;
		agg.maxMs = std::max(agg.maxMs, costMs);
	}
	if (aggs.isEmpty())		return {};

	QVector<EventAgg> list;
	for (auto it = aggs.cbegin(); it != aggs.cend(); ++it)
		list.append(it.value());
	std::sort(list.begin(), list.end(),
			  [](const EventAgg& a, const EventAgg& b) { return a.totalMs > b.totalMs; });

	Finding f;
	f.category = QStringLiteral("SlowEventTop");
	const EventAgg& top = list.first();
	f.severity = top.totalMs >= 1000.0 ? DiagnosticSummarizer::Severity::Critical
									   : DiagnosticSummarizer::Severity::Warning;
	f.headline = QStringLiteral("慢事件耗时 TOP：%1 累计 %2ms（共 %3 组聚合，样本 %4 条）")
					 .arg(top.receiver)
					 .arg(top.totalMs, 0, 'f', 1)
					 .arg(aggs.size())
					 .arg(static_cast<qulonglong>(slowEventSamples));
	for (int i = 0; i < list.size() && i < 5; ++i) {
		const EventAgg& a = list.at(i);
		f.details << QStringLiteral("receiver=%1 event=%2 count=%3 totalMs=%4 maxMs=%5")
					 .arg(a.receiver, a.event).arg(a.count)
					 .arg(a.totalMs, 0, 'f', 1).arg(a.maxMs, 0, 'f', 1);
	}
	return { f };
}

QVector<Finding> analyzeHighFrequencySignal()
{
	const std::vector<WatchRecordStore::Record> records = WatchRecordStore::instance().snapshot();

	QHash<QString, MetaAgg> aggs;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind != WatchRecordStore::KindMetaCall)	continue;
		const std::string* sender = findField(r.fields, "sender");
		const std::string* sig = findField(r.fields, "signal");
		const std::string* recv = findField(r.fields, "receiver");
		if (sig == nullptr)	continue;

		// sender 为空（invokeMethod/降级）时以 receiver 定位信号来源
		QString senderName = sender != nullptr ? QString::fromStdString(*sender)
											   : QString();
		if (senderName.isEmpty() && recv != nullptr)
			senderName = QStringLiteral("(via %1)").arg(QString::fromStdString(*recv));
		const QString sigName = QString::fromStdString(*sig);

		const QString key = senderName + QLatin1Char('|') + sigName;
		MetaAgg& agg = aggs[key];
		agg.sender = senderName;
		agg.signalName = sigName;
		++agg.count;
		const double costMs = fieldAsMs(r.fields, "costMs");
		agg.totalMs += costMs;
		agg.maxMs = std::max(agg.maxMs, costMs);
	}
	if (aggs.isEmpty())		return {};

	QVector<MetaAgg> list;
	for (auto it = aggs.cbegin(); it != aggs.cend(); ++it)
		list.append(it.value());
	std::sort(list.begin(), list.end(),
			  [](const MetaAgg& a, const MetaAgg& b) { return a.count > b.count; });

	Finding f;
	f.category = QStringLiteral("HighFrequencySignal");
	f.severity = list.first().count >= 10 ? DiagnosticSummarizer::Severity::Warning
										  : DiagnosticSummarizer::Severity::Info;
	f.headline = QStringLiteral("高频慢信号：共 %1 组").arg(list.size());
	for (int i = 0; i < list.size() && i < 5; ++i) {
		const MetaAgg& a = list.at(i);
		f.details << QStringLiteral("sender=%1 signal=%2 count=%3 avgMs=%4 maxMs=%5")
					 .arg(a.sender, a.signalName).arg(a.count)
					 .arg(a.count > 0 ? a.totalMs / a.count : 0.0, 0, 'f', 1)
					 .arg(a.maxMs, 0, 'f', 1);
	}
	return { f };
}

QVector<Finding> analyzeQssJitter()
{
	const std::vector<WatchRecordStore::Record> records = WatchRecordStore::instance().snapshot();

	QHash<QString, int> opCounts;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind != WatchRecordStore::KindQss)	continue;
		const std::string* op = findField(r.fields, "op");
		++opCounts[op != nullptr ? QString::fromStdString(*op) : QStringLiteral("Unknown")];
	}
	if (opCounts.isEmpty())		return {};

	int total = 0;
	for (auto it = opCounts.cbegin(); it != opCounts.cend(); ++it)
		total += it.value();

	Finding f;
	f.category = QStringLiteral("QssJitter");
	f.severity = total >= 10 ? DiagnosticSummarizer::Severity::Warning
							 : DiagnosticSummarizer::Severity::Info;
	f.headline = QStringLiteral("QSS 抖动嫌疑：慢样式操作共 %1 次").arg(total);
	for (auto it = opCounts.cbegin(); it != opCounts.cend(); ++it)
		f.details << QStringLiteral("op=%1 count=%2").arg(it.key()).arg(it.value());
	return { f };
}

/// 周期统计跨周期合并：eventKey → {count, totalNs}，TOP-3
QVector<Finding> analyzeStatTop(const EventStatistics* statistics)
{
	if (statistics == nullptr)		return {};

	QVector<EventStatistics::PeriodSnapshot> periods = statistics->statisticsHistory();
	const EventStatistics::PeriodSnapshot live = statistics->liveSnapshot();
	if (!live.entries.isEmpty())
		periods.append(live);

	QHash<QString, EventStatistics::StatEntry> merged;
	for (const EventStatistics::PeriodSnapshot& p : periods) {
		for (const EventStatistics::StatEntry& e : p.entries) {
			EventStatistics::StatEntry& m = merged[e.eventKey];
			m.eventKey = e.eventKey;
			m.count += e.count;
			m.totalCostNs += e.totalCostNs;
			m.maxCostNs = std::max(m.maxCostNs, e.maxCostNs);
			m.exclusiveTotalNs += e.exclusiveTotalNs;
			m.exclusiveMaxNs = std::max(m.exclusiveMaxNs, e.exclusiveMaxNs);
		}
	}
	if (merged.isEmpty())		return {};

	QVector<EventStatistics::StatEntry> list;
	for (auto it = merged.cbegin(); it != merged.cend(); ++it)
		list.append(it.value());
	std::sort(list.begin(), list.end(),
			  [](const EventStatistics::StatEntry& a, const EventStatistics::StatEntry& b) {
				  return a.totalCostNs > b.totalCostNs;
			  });

	Finding f;
	f.category = QStringLiteral("StatTop");
	f.severity = DiagnosticSummarizer::Severity::Info;
	f.headline = QStringLiteral("周期统计 TOP（跨 %1 个周期，含进行中）").arg(periods.size());
	for (int i = 0; i < list.size() && i < 3; ++i) {
		const EventStatistics::StatEntry& e = list.at(i);
		f.details << QStringLiteral("event=%1 count=%2 totalMs=%3 exclusiveTotalMs=%4 maxMs=%5")
					 .arg(e.eventKey)
					 .arg(static_cast<qulonglong>(e.count))
					 .arg(static_cast<double>(e.totalCostNs) / 1000000.0, 0, 'f', 1)
					 .arg(static_cast<double>(e.exclusiveTotalNs) / 1000000.0, 0, 'f', 1)
					 .arg(static_cast<double>(e.maxCostNs) / 1000000.0, 0, 'f', 1);
	}
	return { f };
}

const char* severityTag(DiagnosticSummarizer::Severity s)
{
	switch (s) {
	case DiagnosticSummarizer::Severity::Critical:	return "CRITICAL";
	case DiagnosticSummarizer::Severity::Warning:	return "WARNING";
	default:										return "INFO";
	}
}

} // namespace

QVector<Finding> DiagnosticSummarizer::analyze(const WatchConfig* config,
											   const EventStatistics* statistics)
{
	Q_UNUSED(config);	// 预留：规则阈值后续可入 WatchConfig

	QVector<Finding> findings;
	findings += analyzeSlowEventTop();
	findings += analyzeHighFrequencySignal();
	findings += analyzeQssJitter();
	findings += analyzeStatTop(statistics);
	return findings;
}

bool DiagnosticSummarizer::exportSummary(const QString& filePath,
										 const QVector<Finding>& findings,
										 const WatchConfig* config, QString* error)
{
	if (filePath.isEmpty()) {
		if (error)	*error = QStringLiteral("empty file path");
		return false;
	}
	const QString suffix = QFileInfo(filePath).suffix();
	const bool json = suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open file: %1").arg(file.errorString());
		return false;
	}
	QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif

	if (json) {
		QJsonArray arr;
		for (const Finding& f : findings) {
			QJsonObject obj;
			obj.insert(QStringLiteral("severity"), QString::fromLatin1(severityTag(f.severity)));
			obj.insert(QStringLiteral("category"), f.category);
			obj.insert(QStringLiteral("headline"), f.headline);
			obj.insert(QStringLiteral("details"), QJsonArray::fromStringList(f.details));
			arr.append(obj);
		}
		QJsonObject root;
		root.insert(QStringLiteral("formatVersion"), 1);
		root.insert(QStringLiteral("generatedAt"),
					QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
		if (config != nullptr)
			root.insert(QStringLiteral("watchFun"),
						QString::asprintf("0x%02x", config->watchFun()));
		root.insert(QStringLiteral("findings"), arr);
		stream << QJsonDocument(root).toJson(QJsonDocument::Indented);
	} else {
		stream << QStringLiteral("QtEventWatcher Performance Summary\n");
		stream << QStringLiteral("================================\n\n");
		stream << QStringLiteral("Generated: %1\n")
					  .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
		if (config != nullptr)
			stream << QStringLiteral("Watch_Fun: 0x%1\n\n")
						  .arg(static_cast<uint>(config->watchFun()), 2, 16, QLatin1Char('0'));
		if (findings.isEmpty())
			stream << QStringLiteral("No findings. All quiet.\n");
		for (const Finding& f : findings) {
			stream << QStringLiteral("[%1] %2\n").arg(QLatin1String(severityTag(f.severity)),
													  f.category);
			stream << QStringLiteral("  %1\n").arg(f.headline);
			for (const QString& d : f.details)
				stream << QStringLiteral("    %1\n").arg(d);
			stream << QLatin1Char('\n');
		}
	}
	return true;
}

} // namespace qt_event_watcher
