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
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>

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
	case WatchRecordStore::KindFreeze:		return "freeze";
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
/// V3 A1：行尾追加 stack 列（模块!偏移帧串；按表头解析的既有消费方不受影响）
const char* const kCsvColumns[] = {
	"time", "kind", "receiver", "object", "event",
	"sender", "signal", "signalId", "depth",
	"costMs", "exclusiveCostMs", "thresholdMs",
	"curThread", "recvThread", "senderThread", "match",
	"stack",
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
			   << "," << csvCell(cell("stack"))
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
	int freezeEvents = 0;
	QJsonArray recordArray;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind == WatchRecordStore::KindMetaCall)	++metaCalls;
		else if (r.kind == WatchRecordStore::KindFreeze)	++freezeEvents;
		else											++slowEvents;

		QJsonObject entry;
		entry.insert(QStringLiteral("kind"), QString::fromLatin1(kindName(r.kind)));
		entry.insert(QStringLiteral("time"), QString::fromStdString(r.time));
		QJsonObject fields;
		for (const Field& f : r.fields)
			fields.insert(QString::fromStdString(f.first), typedField(f.first, f.second));
		entry.insert(QStringLiteral("fields"), fields);
		// V3 A1：调用栈帧数组（模块!偏移；offset 数值化，符号解析放离线工具）
		QJsonArray frames;
		for (const WatchRecordStore::Frame& fr : r.frames) {
			QJsonObject frame;
			frame.insert(QStringLiteral("module"), QString::fromStdString(fr.module));
			frame.insert(QStringLiteral("offset"), static_cast<double>(fr.offset));
			frames.append(frame);
		}
		entry.insert(QStringLiteral("frames"), frames);
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
	counts.insert(QStringLiteral("freezeEvents"), freezeEvents);
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

/// SQLite 字段值类型化（与 typedField 同规则：耗时→REAL，深度/signalId→INTEGER）
QVariant typedVariant(const std::string& key, const std::string& value)
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

/// SQLite 持久化（V3 C1）：meta/records/frames/statistics 四表，单次落盘。
/// 连接生命周期严格限定在本函数（创建线程 = 调用线程，QSqlDatabase 线程约束）；
/// 事务包裹全部写入（数百条记录 <10ms）；失败即删半成品文件，不留脏库
bool exportSqlite(const QString& filePath, const WatchConfig* config,
				  const EventStatistics* statistics, QString* error)
{
	if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
		if (error)	*error = QStringLiteral("QSQLITE driver not available "
											   "(plugins/sqldrivers missing)");
		return false;
	}

	// 全量重建语义（与 CSV/JSON 一致）：先清掉旧文件，避免残留旧表干扰回放
	QFile::remove(filePath);

	const std::vector<WatchRecordStore::Record> records = WatchRecordStore::instance().snapshot();

	int slowEvents = 0;
	int metaCalls = 0;
	int freezeEvents = 0;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind == WatchRecordStore::KindMetaCall)		++metaCalls;
		else if (r.kind == WatchRecordStore::KindFreeze)	++freezeEvents;
		else												++slowEvents;
	}

	const QString connName = QStringLiteral("QtEventWatcher_Export");
	bool ok = true;		// 作用域覆盖整个导出流程（块外失败清理需访问）
	{
		QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
		db.setDatabaseName(filePath);
		if (!db.open()) {
			if (error)	*error = QStringLiteral("cannot open database: %1")
										.arg(db.lastError().text());
			return false;
		}

		// 四表 schema：meta（KV 元信息）/ records（宽表事件记录）/ frames
		//（调用栈帧，recordId 外键）/ statistics（周期统计行，含分位数）
		const char* const kSchema[] = {
			"CREATE TABLE meta ("
			"key TEXT PRIMARY KEY,"
			"value TEXT NOT NULL)",
			"CREATE TABLE records ("
			"id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"kind TEXT NOT NULL,"
			"time TEXT,"
			"receiver TEXT, object TEXT, event TEXT,"
			"sender TEXT, signal TEXT, signalId INTEGER, depth INTEGER,"
			"costMs REAL, exclusiveCostMs REAL, thresholdMs REAL,"
			"curThread TEXT, recvThread TEXT, senderThread TEXT, match TEXT,"
			"stack TEXT,"
			"raw TEXT)",
			"CREATE TABLE frames ("
			"recordId INTEGER NOT NULL,"
			"idx INTEGER NOT NULL,"
			"module TEXT,"
			"offset INTEGER)",
			"CREATE INDEX IF NOT EXISTS idx_frames_record ON frames(recordId)",
			"CREATE TABLE statistics ("
			"periodIdx INTEGER NOT NULL,"
			"isLive INTEGER NOT NULL,"
			"periodMs INTEGER,"
			"event TEXT NOT NULL,"
			"count INTEGER,"
			"totalCostMs REAL, maxCostMs REAL,"
			"exclusiveTotalMs REAL, exclusiveMaxMs REAL,"
			"p50Ms REAL, p99Ms REAL, p999Ms REAL)",
		};

		ok = db.transaction();
		for (const char* sql : kSchema) {
			QSqlQuery query(db);
			if (!query.exec(QString::fromLatin1(sql))) {
				ok = false;
				if (error)	*error = QStringLiteral("create schema failed: %1")
											.arg(query.lastError().text());
				break;
			}
		}

		if (ok) {
			// meta 段：格式版本 / 导出时刻 / 计数 / 监控配置（JSON 同源字段）
			QSqlQuery metaQuery(db);
			metaQuery.prepare(QStringLiteral(
				"INSERT INTO meta (key, value) VALUES (?, ?)"));
			const auto putMeta = [&metaQuery](const char* key, const QVariant& value) {
				metaQuery.addBindValue(QString::fromLatin1(key));
				metaQuery.addBindValue(value);
				metaQuery.exec();
			};
			putMeta("formatVersion", 1);
			putMeta("exportedAt", QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
			putMeta("counts.slowEvents", slowEvents);
			putMeta("counts.metaCalls", metaCalls);
			putMeta("counts.freezeEvents", freezeEvents);
			putMeta("counts.total", static_cast<int>(records.size()));
			if (config != nullptr) {
				putMeta("monitor.watchFun",
						QString::asprintf("0x%02x", config->watchFun()));
				putMeta("monitor.slowEventThresholdMs", config->slowEventThresholdMs());
				putMeta("monitor.slowMetaCallThresholdMs", config->slowMetaCallThresholdMs());
			}

			// records + frames：缺失字段绑 NULL（QVariant()），字段值类型化
			QSqlQuery recordQuery(db);
			recordQuery.prepare(QStringLiteral(
				"INSERT INTO records (kind, time, receiver, object, event, sender, signal,"
				" signalId, depth, costMs, exclusiveCostMs, thresholdMs,"
				" curThread, recvThread, senderThread, match, stack, raw)"
				" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
			QSqlQuery frameQuery(db);
			frameQuery.prepare(QStringLiteral(
				"INSERT INTO frames (recordId, idx, module, offset) VALUES (?, ?, ?, ?)"));

			for (const WatchRecordStore::Record& r : records) {
				const auto cell = [&r](const char* key) -> QVariant {
					const std::string* v = findField(r.fields, key);
					return v != nullptr ? typedVariant(key, *v) : QVariant();
				};
				recordQuery.addBindValue(QString::fromLatin1(kindName(r.kind)));
				recordQuery.addBindValue(QString::fromStdString(r.time));
				recordQuery.addBindValue(cell("receiver"));
				recordQuery.addBindValue(cell("object"));
				recordQuery.addBindValue(cell("event"));
				recordQuery.addBindValue(cell("sender"));
				recordQuery.addBindValue(cell("signal"));
				recordQuery.addBindValue(cell("signalId"));
				recordQuery.addBindValue(cell("depth"));
				recordQuery.addBindValue(cell("costMs"));
				recordQuery.addBindValue(cell("exclusiveCostMs"));
				recordQuery.addBindValue(cell("thresholdMs"));
				recordQuery.addBindValue(cell("curThread"));
				recordQuery.addBindValue(cell("recvThread"));
				recordQuery.addBindValue(cell("senderThread"));
				recordQuery.addBindValue(cell("match"));
				recordQuery.addBindValue(cell("stack"));
				recordQuery.addBindValue(QString::fromStdString(r.raw));
				if (!recordQuery.exec()) {
					ok = false;
					if (error)	*error = QStringLiteral("insert record failed: %1")
												.arg(recordQuery.lastError().text());
					break;
				}

				const QVariant recordId = recordQuery.lastInsertId();
				for (std::size_t i = 0; i < r.frames.size(); ++i) {
					frameQuery.addBindValue(recordId);
					frameQuery.addBindValue(static_cast<int>(i));
					frameQuery.addBindValue(QString::fromStdString(r.frames[i].module));
					frameQuery.addBindValue(static_cast<qlonglong>(r.frames[i].offset));
					if (!frameQuery.exec()) {
						ok = false;
						if (error)	*error = QStringLiteral("insert frame failed: %1")
													.arg(frameQuery.lastError().text());
						break;
					}
				}
				if (!ok)	break;
			}
		}

		if (ok && statistics != nullptr) {
			// statistics：复用 periodToJson 的排序/分位计算，行化落盘
			constexpr int kTopN = 20;
			const auto dumpPeriod = [&](const EventStatistics::PeriodSnapshot& period,
										int periodIdx, bool isLive) {
				const QJsonArray top = periodToJson(period, kTopN)
										   .value(QStringLiteral("top")).toArray();
				QSqlQuery statQuery(db);
				statQuery.prepare(QStringLiteral(
					"INSERT INTO statistics (periodIdx, isLive, periodMs, event, count,"
					" totalCostMs, maxCostMs, exclusiveTotalMs, exclusiveMaxMs,"
					" p50Ms, p99Ms, p999Ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
				for (const QJsonValue& v : top) {
					const QJsonObject e = v.toObject();
					statQuery.addBindValue(periodIdx);
					statQuery.addBindValue(isLive ? 1 : 0);
					statQuery.addBindValue(static_cast<qlonglong>(period.periodMs));
					statQuery.addBindValue(e.value(QStringLiteral("event")).toString());
					statQuery.addBindValue(e.value(QStringLiteral("count")).toInt());
					statQuery.addBindValue(e.value(QStringLiteral("totalCostMs")).toDouble());
					statQuery.addBindValue(e.value(QStringLiteral("maxCostMs")).toDouble());
					statQuery.addBindValue(e.value(QStringLiteral("exclusiveTotalMs")).toDouble());
					statQuery.addBindValue(e.value(QStringLiteral("exclusiveMaxMs")).toDouble());
					statQuery.addBindValue(e.value(QStringLiteral("p50Ms")).toDouble());
					statQuery.addBindValue(e.value(QStringLiteral("p99Ms")).toDouble());
					statQuery.addBindValue(e.value(QStringLiteral("p999Ms")).toDouble());
					if (!statQuery.exec()) {
						if (error)	*error = QStringLiteral("insert statistic failed: %1")
													.arg(statQuery.lastError().text());
						return false;
					}
				}
				return true;
			};

			const QVector<EventStatistics::PeriodSnapshot> history =
				statistics->statisticsHistory();
			for (int i = 0; i < history.size() && ok; ++i)
				ok = dumpPeriod(history.at(i), i, false);
			if (ok) {
				const EventStatistics::PeriodSnapshot live = statistics->liveSnapshot();
				if (!live.entries.isEmpty())
					ok = dumpPeriod(live, history.size(), true);
			}
		}

		if (ok)
			ok = db.commit();
		else
			db.rollback();

		if (!ok && error != nullptr && error->isEmpty())
			*error = QStringLiteral("sqlite export failed (transaction rolled back)");
	}

	QSqlDatabase::removeDatabase(connName);

	// 失败清理：删除半成品文件，不留脏库误导回放方
	if (!ok && QFile::exists(filePath))
		QFile::remove(filePath);
	return ok;
}

} // namespace

bool DataExporter::exportData(const QString& filePath, const WatchConfig* config,
							  const EventStatistics* statistics, QString* error)
{
	if (filePath.isEmpty()) {
		if (error)	*error = QStringLiteral("empty file path");
		return false;
	}
	// 后缀决定格式（大小写不敏感）：json → JSON；db/sqlite/sqlite3 → SQLite
	//（V3 C1 持久化）；其余 → CSV
	const QString suffix = QFileInfo(filePath).suffix();
	if (suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0)
		return exportJson(filePath, config, statistics, error);
	if (suffix.compare(QStringLiteral("db"), Qt::CaseInsensitive) == 0 ||
		suffix.compare(QStringLiteral("sqlite"), Qt::CaseInsensitive) == 0 ||
		suffix.compare(QStringLiteral("sqlite3"), Qt::CaseInsensitive) == 0) {
		return exportSqlite(filePath, config, statistics, error);
	}
	return exportCsv(filePath, error);
}

} // namespace qt_event_watcher
