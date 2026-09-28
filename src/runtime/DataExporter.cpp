#include "DataExporter.h"

#include "EventStatistics.h"
#include "WatchConfig.h"
#include "WatchRecordStore.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
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

const char* kindName(int kind)
{
	switch (kind) {
	case WatchRecordStore::KindMetaCall:	return "metaCall";
	case WatchRecordStore::KindQss:			return "qss";
	default:								return "slowEvent";
	}
}

/// 线性查找字段值（记录字段数 ~14，导出路径非热区，无需索引）
const std::string* findField(const std::vector<Field>& fields, const char* key)
{
	for (const Field& f : fields) {
		if (f.first == key)		return &f.second;
	}
	return nullptr;
}

/// CSV 单元格：全量加引号 + " → ""（防御 objectName 等自由文本含逗号/引号）
QString csvCell(const std::string& value)
{
	QString s = QString::fromStdString(value);
	s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
	return QStringLiteral("\"%1\"").arg(s);
}

QString csvCell(const QString& value)
{
	QString s = value;
	s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
	return QStringLiteral("\"%1\"").arg(s);
}

/// 宽表列序（两类记录共用，N/A 留空；CSV 无嵌套能力，signal/receiver 并列展开）
const char* const kCsvColumns[] = {
	"time", "kind", "receiver", "object", "event",
	"sender", "signal", "signalId", "depth",
	"costMs", "exclusiveCostMs", "thresholdMs",
	"curThread", "recvThread", "senderThread", "match",
};
constexpr int kCsvColumnCount = sizeof(kCsvColumns) / sizeof(kCsvColumns[0]);

/// CSV 供 Excel 直接打开：UTF-8 + BOM；JSON 严格 UTF-8 无 BOM。
/// Qt5 QTextStream 默认 locale 编码（中文 Windows = GBK），必须显式指定；
/// Qt6 默认已是 UTF-8 且 setCodec 已移除
void setUtf8(QTextStream& stream, bool withBom)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif
	stream.setGenerateByteOrderMark(withBom);
}

bool exportCsv(const QString& filePath, QString* error)
{
	const std::vector<WatchRecordStore::Record> records = WatchRecordStore::instance().snapshot();

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open file: %1").arg(file.errorString());
		return false;
	}
	QTextStream stream(&file);
	setUtf8(stream, true);

	for (int i = 0; i < kCsvColumnCount; ++i)
		stream << csvCell(QString::fromLatin1(kCsvColumns[i]))
			   << (i + 1 < kCsvColumnCount ? "," : "\n");

	for (const WatchRecordStore::Record& r : records) {
		const auto cell = [&r](const char* key) -> QString {
			const std::string* v = findField(r.fields, key);
			return v != nullptr ? QString::fromStdString(*v) : QString();
		};
		stream << csvCell(r.time)
			   << "," << csvCell(QString::fromLatin1(kindName(r.kind)))
			   << "," << csvCell(cell("receiver"))
			   << "," << csvCell(cell("object"))
			   << "," << csvCell(cell("event"))
			   << "," << csvCell(cell("sender"))
			   << "," << csvCell(cell("signal"))
			   << "," << csvCell(cell("signalId"))
			   << "," << csvCell(cell("depth"))
			   << "," << csvCell(cell("costMs"))
			   << "," << csvCell(cell("exclusiveCostMs"))
			   << "," << csvCell(cell("thresholdMs"))
			   << "," << csvCell(cell("curThread"))
			   << "," << csvCell(cell("recvThread"))
			   << "," << csvCell(cell("senderThread"))
			   << "," << csvCell(cell("match"))
			   << "\n";
	}
	return true;
}

/// JSON 字段值类型化：耗时/阈值 → double，深度/signalId → int，其余保字符串（回放分析友好）
QJsonValue typedField(const std::string& key, const std::string& value)
{
	const QString k = QString::fromStdString(key);
	const QString v = QString::fromStdString(value);
	bool ok = false;
	if (k == QLatin1String("costMs") || k == QLatin1String("exclusiveCostMs") ||
		k == QLatin1String("thresholdMs")) {
		const double d = v.toDouble(&ok);
		if (ok)		return d;
	} else if (k == QLatin1String("depth") || k == QLatin1String("signalId")) {
		const int i = v.toInt(&ok);
		if (ok)		return i;
	}
	return v;
}

/// 统计周期 → JSON 对象：TOP-N by totalCost + 分位数估计（V2 A2）
QJsonObject periodToJson(const EventStatistics::PeriodSnapshot& period, int topN)
{
	QJsonArray top;
	QVector<EventStatistics::StatEntry> entries = period.entries;
	std::sort(entries.begin(), entries.end(),
			  [](const EventStatistics::StatEntry& a, const EventStatistics::StatEntry& b) {
				  return a.totalCostNs > b.totalCostNs;
			  });
	for (int i = 0; i < entries.size() && i < topN; ++i) {
		const auto& e = entries.at(i);
		QJsonObject entry;
		entry.insert(QStringLiteral("event"), e.eventKey);
		entry.insert(QStringLiteral("count"), static_cast<double>(e.count));
		entry.insert(QStringLiteral("totalCostMs"),
					 static_cast<double>(e.totalCostNs) / 1000000.0);
		entry.insert(QStringLiteral("maxCostMs"),
					 static_cast<double>(e.maxCostNs) / 1000000.0);
		entry.insert(QStringLiteral("exclusiveTotalMs"),
					 static_cast<double>(e.exclusiveTotalNs) / 1000000.0);
		entry.insert(QStringLiteral("exclusiveMaxMs"),
					 static_cast<double>(e.exclusiveMaxNs) / 1000000.0);
		entry.insert(QStringLiteral("p50Ms"),
					 EventStatistics::estimatePercentileMs(e, 0.50));
		entry.insert(QStringLiteral("p99Ms"),
					 EventStatistics::estimatePercentileMs(e, 0.99));
		entry.insert(QStringLiteral("p999Ms"),
					 EventStatistics::estimatePercentileMs(e, 0.999));
		top.append(entry);
	}

	QJsonObject obj;
	obj.insert(QStringLiteral("periodMs"), static_cast<double>(period.periodMs));
	obj.insert(QStringLiteral("eventKinds"), static_cast<double>(entries.size()));
	obj.insert(QStringLiteral("top"), top);
	return obj;
}

bool exportJson(const QString& filePath, const WatchConfig* config,
				const EventStatistics* statistics, QString* error)
{
	const std::vector<WatchRecordStore::Record> records = WatchRecordStore::instance().snapshot();

	int slowEvents = 0;
	int metaCalls = 0;
	QJsonArray recordArray;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind == WatchRecordStore::KindMetaCall)	++metaCalls;
		else											++slowEvents;

		QJsonObject entry;
		entry.insert(QStringLiteral("kind"), QString::fromLatin1(kindName(r.kind)));
		entry.insert(QStringLiteral("time"), QString::fromStdString(r.time));
		QJsonObject fields;
		for (const Field& f : r.fields)
			fields.insert(QString::fromStdString(f.first), typedField(f.first, f.second));
		entry.insert(QStringLiteral("fields"), fields);
		entry.insert(QStringLiteral("raw"), QString::fromStdString(r.raw));
		recordArray.append(entry);
	}

	QJsonObject root;
	root.insert(QStringLiteral("formatVersion"), 1);
	root.insert(QStringLiteral("exportedAt"),
				QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
	QJsonObject counts;
	counts.insert(QStringLiteral("slowEvents"), slowEvents);
	counts.insert(QStringLiteral("metaCalls"), metaCalls);
	root.insert(QStringLiteral("counts"), counts);

	if (config != nullptr) {
		QJsonObject monitor;
		monitor.insert(QStringLiteral("watchFun"),
					   QString::asprintf("0x%02x", config->watchFun()));
		monitor.insert(QStringLiteral("slowEventThresholdMs"), config->slowEventThresholdMs());
		monitor.insert(QStringLiteral("slowMetaCallThresholdMs"), config->slowMetaCallThresholdMs());
		root.insert(QStringLiteral("monitorConfig"), monitor);
	}

	// 周期统计段（V2 A2）：归档历史 + 当前进行中周期；TOP-20 by totalCost
	if (statistics != nullptr) {
		constexpr int kTopN = 20;
		QJsonArray periods;
		const QVector<EventStatistics::PeriodSnapshot> history =
			statistics->statisticsHistory();
		for (const EventStatistics::PeriodSnapshot& p : history)
			periods.append(periodToJson(p, kTopN));
		const EventStatistics::PeriodSnapshot live = statistics->liveSnapshot();
		if (!live.entries.isEmpty())
			periods.append(periodToJson(live, kTopN));
		root.insert(QStringLiteral("statistics"), periods);
	}

	root.insert(QStringLiteral("records"), recordArray);

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open file: %1").arg(file.errorString());
		return false;
	}
	QTextStream stream(&file);
	setUtf8(stream, false);
	stream << QJsonDocument(root).toJson(QJsonDocument::Indented);
	return true;
}

} // namespace

bool DataExporter::exportData(const QString& filePath, const WatchConfig* config,
							  const EventStatistics* statistics, QString* error)
{
	if (filePath.isEmpty()) {
		if (error)	*error = QStringLiteral("empty file path");
		return false;
	}
	// 后缀决定格式（大小写不敏感）；非 json 后缀一律 CSV
	const QString suffix = QFileInfo(filePath).suffix();
	const bool json = suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
	return json ? exportJson(filePath, config, statistics, error)
				: exportCsv(filePath, error);
}

} // namespace qt_event_watcher
