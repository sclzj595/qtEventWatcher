// ScoutDashboardTests - V7 S3 仪表盘数据层单元测试（QEWT 断言，只链 Qt Core）
//
// 覆盖：loadModel 解析/容错 / classifyRecord 归类（含自监控 event 字段防陷阱
// 回归）/ freeze 三态配对 / relMs 跨午夜防御 / escapeHtml / embedJsonSafe /
// recordField。合成 JSON 直接以 QJsonDocument 构造落盘（形状与 aggregator
// --out 导出端逐字段对齐）。

#include "DashboardModel.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "QEWT.h"

using namespace qt_event_watcher::dashboard;

namespace {

using FieldList = QVector<QPair<QString, QString>>;

/// 构造一条 aggregator 导出形状的 record（fields=[[k,v]..]）
QJsonObject makeRecord(int kind, const QString &time, const QString &raw,
					   const FieldList &fields)
{
	QJsonObject rec;
	rec.insert(QStringLiteral("kind"), kind);
	rec.insert(QStringLiteral("seq"), 1);
	rec.insert(QStringLiteral("time"), time);
	rec.insert(QStringLiteral("raw"), raw);
	QJsonArray arr;
	for (const auto &f : fields) {
		QJsonArray pair;
		pair.append(f.first);
		pair.append(f.second);
		arr.append(pair);
	}
	rec.insert(QStringLiteral("fields"), arr);
	return rec;
}

/// 文档落盘到临时目录，返回文件路径（dir 需存活到 loadModel 之后）
QString writeJson(const QJsonDocument &doc, const QTemporaryDir &dir,
				  const QString &name)
{
	const QString path = dir.filePath(name);
	QFile f(path);
	f.open(QIODevice::WriteOnly | QIODevice::Truncate);
	f.write(doc.toJson(QJsonDocument::Indented));
	return path;
}

QJsonObject freezeStarted(const QString &time, const QString &receiver,
						  const QString &stalledMs)
{
	return makeRecord(3, time,
					  QStringLiteral("[FreezeWatch] freeze started receiver=%1")
						  .arg(receiver),
					  { { QStringLiteral("receiver"), receiver },
						{ QStringLiteral("type"), QStringLiteral("0") },
						{ QStringLiteral("inProgress"), QStringLiteral("false") },
						{ QStringLiteral("stalledMs"), stalledMs },
						{ QStringLiteral("thresholdMs"), QStringLiteral("2000") } });
}

QJsonObject freezeRecovered(const QString &time, const QString &receiver,
							const QString &totalMs)
{
	return makeRecord(3, time,
					  QStringLiteral("[FreezeWatch] freeze recovered receiver=%1")
						  .arg(receiver),
					  { { QStringLiteral("receiver"), receiver },
						{ QStringLiteral("type"), QStringLiteral("0") },
						{ QStringLiteral("inProgress"), QStringLiteral("false") },
						{ QStringLiteral("totalMs"), totalMs } });
}

QJsonObject freezeLost(const QString &time, const QString &receiver)
{
	return makeRecord(3, time,
					  QStringLiteral("[FreezeWatch] freeze lost receiver=%1")
						  .arg(receiver),
					  { { QStringLiteral("receiver"), receiver },
						{ QStringLiteral("type"), QStringLiteral("0") } });
}

/// 自监控普通慢事件（带 event 字段——QEvent 枚举名，非 scout 事件）
QJsonObject plainSlowEvent(const QString &time, const QString &costMs)
{
	return makeRecord(0, time,
					  QStringLiteral("[EventWatcher] slow event receiver=demo "
									 "event=Timeout costMs=%1").arg(costMs),
					  { { QStringLiteral("receiver"), QStringLiteral("demo") },
						{ QStringLiteral("object"), QStringLiteral("QTimer") },
						{ QStringLiteral("event"), QStringLiteral("Timeout") },
						{ QStringLiteral("costMs"), costMs },
						{ QStringLiteral("thresholdMs"), QStringLiteral("100") },
						{ QStringLiteral("match"), QStringLiteral("true") } });
}

// ---- parseAggJson：最小会话（freeze started+recovered + 慢事件）----

void testParseAggJson()
{
	QTemporaryDir dir;
	QJsonArray records;
	records.append(freezeStarted(QStringLiteral("10:00:00.000"),
								 QStringLiteral("basic_demo.exe"),
								 QStringLiteral("2100")));
	records.append(freezeRecovered(QStringLiteral("10:00:02.500"),
								   QStringLiteral("basic_demo.exe"),
								   QStringLiteral("2500")));
	records.append(plainSlowEvent(QStringLiteral("10:00:03.000"),
								  QStringLiteral("42.5")));

	QJsonObject health;
	health.insert(QStringLiteral("op"), QStringLiteral("health"));
	health.insert(QStringLiteral("pushed"), 3);
	health.insert(QStringLiteral("dropped"), 0);
	health.insert(QStringLiteral("reconnects"), 0);
	health.insert(QStringLiteral("lastSeq"), 3);

	QJsonObject session;
	session.insert(QStringLiteral("host_pid"), 1234);
	session.insert(QStringLiteral("received"), 3);
	session.insert(QStringLiteral("dropped"), 0);
	session.insert(QStringLiteral("health"), health);
	session.insert(QStringLiteral("records"), records);

	QJsonObject root;
	root.insert(QStringLiteral("generatedAt"),
				QStringLiteral("2026-10-02T01:00:00.000"));
	root.insert(QStringLiteral("server"), QStringLiteral("QtEventWatcherAggregator"));
	root.insert(QStringLiteral("sessions"), QJsonArray{ session });

	DashboardModel model;
	QString error;
	QEWT_CHECK(loadModel(writeJson(QJsonDocument(root), dir,
									   QStringLiteral("agg.json")),
						 model, &error));
	QEWT_CHECK_EQ(error.toStdString(), std::string());
	QEWT_CHECK_EQ(model.sessions.size(), 1);
	const SessionModel &s = model.sessions.first();
	QEWT_CHECK_EQ(s.pid, qint64(1234));
	QEWT_CHECK_EQ(s.received, quint64(3));
	QEWT_CHECK_EQ(s.dropped, quint64(0));
	QEWT_CHECK(s.hasHealth);
	QEWT_CHECK_EQ(s.health.value(QStringLiteral("pushed")).toInt(), 3);
	QEWT_CHECK(!s.isScoutSession);
	QEWT_CHECK_EQ(model.totalRecords, 3);
	QEWT_CHECK_EQ(model.kindCounts[3], 2);
	QEWT_CHECK_EQ(model.kindCounts[0], 1);
	QEWT_CHECK_EQ(model.cdpCount, 0);
	QEWT_CHECK_EQ(model.cpuCount, 0);

	// t0 = 10:00:00.000 的当日毫秒；relMs 依次 0 / 2500 / 3000
	QEWT_CHECK_EQ(s.t0Ms, qint64(10 * 3600) * 1000);
	QEWT_CHECK_EQ(s.events.size(), 3);
	QEWT_CHECK_EQ(int(s.events[0].cls), int(EventClass::Freeze));
	QEWT_CHECK_EQ(s.events[0].state.toStdString(), std::string("started"));
	QEWT_CHECK_EQ(s.events[0].relMs, qint64(0));
	QEWT_CHECK_EQ(int(s.events[1].cls), int(EventClass::Freeze));
	QEWT_CHECK_EQ(s.events[1].state.toStdString(), std::string("recovered"));
	QEWT_CHECK_EQ(s.events[1].relMs, qint64(2500));
	QEWT_CHECK_EQ(int(s.events[2].cls), int(EventClass::SlowEvent));
	QEWT_CHECK_EQ(s.events[2].relMs, qint64(3000));
	QEWT_CHECK_EQ(s.events[2].event.toStdString(), std::string("Timeout"));
	QEWT_CHECK(s.events[2].costMs > 42.4 && s.events[2].costMs < 42.6);

	// freeze 配对：started@0 → recovered(totalMs=2500)
	QEWT_CHECK_EQ(s.freezeSpans.size(), 1);
	QEWT_CHECK_EQ(s.freezeSpans[0].receiver.toStdString(),
				  std::string("basic_demo.exe"));
	QEWT_CHECK_EQ(s.freezeSpans[0].startRelMs, qint64(0));
	QEWT_CHECK_EQ(s.freezeSpans[0].durationMs, qint64(2500));
	QEWT_CHECK_EQ(s.freezeSpans[0].endState.toStdString(),
				  std::string("recovered"));
}

// ---- classifyRecord：三信号冗余判定 + 自监控 event 字段防陷阱 ----

void testClassify()
{
	QEWT_CHECK_EQ(int(classifyRecord(3, QString(), QString(), QString())),
				  int(EventClass::Freeze));
	QEWT_CHECK_EQ(int(classifyRecord(0, QStringLiteral("cdpLongTask"),
									 QStringLiteral("scout-cdp"),
									 QStringLiteral("98"))),
				  int(EventClass::CdpLongTask));
	// 单信号命中（容错）：type / event / source 任一即可
	QEWT_CHECK_EQ(int(classifyRecord(0, QString(), QStringLiteral("scout-cdp"),
									 QString())),
				  int(EventClass::CdpLongTask));
	QEWT_CHECK_EQ(int(classifyRecord(0, QStringLiteral("cpuSpin"),
									 QStringLiteral("scout"),
									 QStringLiteral("99"))),
				  int(EventClass::CpuSpin));
	QEWT_CHECK_EQ(int(classifyRecord(0, QString(), QStringLiteral("scout"),
									 QString())),
				  int(EventClass::CpuSpin));
	// 防陷阱回归：普通慢事件行也有 event 字段（QEvent 名），非 cpuSpin
	// 精确匹配必须回落 SlowEvent，不得误判 scout 事件
	QEWT_CHECK_EQ(int(classifyRecord(0, QStringLiteral("Timeout"), QString(),
									 QString())),
				  int(EventClass::SlowEvent));
	QEWT_CHECK_EQ(int(classifyRecord(0, QStringLiteral("Timer"), QString(),
									 QString())),
				  int(EventClass::SlowEvent));
	QEWT_CHECK_EQ(int(classifyRecord(1, QString(), QString(), QString())),
				  int(EventClass::MetaCall));
	QEWT_CHECK_EQ(int(classifyRecord(2, QString(), QString(), QString())),
				  int(EventClass::Qss));
	QEWT_CHECK_EQ(int(classifyRecord(7, QString(), QString(), QString())),
				  int(EventClass::Other));
}

// ---- freezeSpans：recovered/lost/ongoing + 多段多 receiver ----

void testFreezeSpans()
{
	QTemporaryDir dir;
	QJsonArray records;
	records.append(freezeStarted(QStringLiteral("10:00:00.000"),
								 QStringLiteral("appA.exe"),
								 QStringLiteral("1100")));
	records.append(freezeRecovered(QStringLiteral("10:00:01.000"),
								   QStringLiteral("appA.exe"),
								   QStringLiteral("1000")));
	records.append(freezeStarted(QStringLiteral("10:00:02.000"),
								 QStringLiteral("appB.exe"),
								 QStringLiteral("2200")));
	records.append(freezeLost(QStringLiteral("10:00:03.000"),
							  QStringLiteral("appB.exe")));
	records.append(freezeStarted(QStringLiteral("10:00:04.000"),
								 QStringLiteral("appC.exe"),
								 QStringLiteral("3300")));

	QJsonObject session;
	session.insert(QStringLiteral("host_pid"), 100);
	session.insert(QStringLiteral("received"), 5);
	session.insert(QStringLiteral("dropped"), 0);
	session.insert(QStringLiteral("records"), records);
	QJsonObject root;
	root.insert(QStringLiteral("sessions"), QJsonArray{ session });

	DashboardModel model;
	QString error;
	QEWT_CHECK(loadModel(writeJson(QJsonDocument(root), dir,
								   QStringLiteral("agg.json")),
						 model, &error));
	QEWT_CHECK_EQ(model.sessions.size(), 1);
	const QVector<FreezeSpan> &spans = model.sessions.first().freezeSpans;
	QEWT_CHECK_EQ(spans.size(), 3);

	QEWT_CHECK_EQ(spans[0].receiver.toStdString(), std::string("appA.exe"));
	QEWT_CHECK_EQ(spans[0].durationMs, qint64(1000));
	QEWT_CHECK_EQ(spans[0].endState.toStdString(), std::string("recovered"));

	QEWT_CHECK_EQ(spans[1].receiver.toStdString(), std::string("appB.exe"));
	QEWT_CHECK_EQ(spans[1].durationMs, qint64(-1));
	QEWT_CHECK_EQ(spans[1].endState.toStdString(), std::string("lost"));

	// 收尾开放段（导出时刻仍在冻结）→ ongoing
	QEWT_CHECK_EQ(spans[2].receiver.toStdString(), std::string("appC.exe"));
	QEWT_CHECK_EQ(spans[2].durationMs, qint64(-1));
	QEWT_CHECK_EQ(spans[2].endState.toStdString(), std::string("ongoing"));
}

// ---- relMs：跨午夜防御（23:59:59 → 00:00:01 差值 +86400000）----

void testRelMsCrossMidnight()
{
	QTemporaryDir dir;
	QJsonArray records;
	records.append(freezeStarted(QStringLiteral("23:59:59.000"),
								 QStringLiteral("app.exe"),
								 QStringLiteral("2000")));
	records.append(freezeRecovered(QStringLiteral("00:00:01.000"),
								   QStringLiteral("app.exe"),
								   QStringLiteral("2000")));

	QJsonObject session;
	session.insert(QStringLiteral("host_pid"), 7);
	session.insert(QStringLiteral("records"), records);
	QJsonObject root;
	root.insert(QStringLiteral("sessions"), QJsonArray{ session });

	DashboardModel model;
	QString error;
	QEWT_CHECK(loadModel(writeJson(QJsonDocument(root), dir,
								   QStringLiteral("agg.json")),
						 model, &error));
	QEWT_CHECK_EQ(model.sessions.size(), 1);
	const SessionModel &s = model.sessions.first();
	QEWT_CHECK_EQ(s.t0Ms, qint64(86399) * 1000);
	QEWT_CHECK_EQ(s.events[0].relMs, qint64(0));
	QEWT_CHECK_EQ(s.events[1].relMs, qint64(2000));		// 1000-86399000<0 → +86400000
}

// ---- parseAggJsonTolerant：record 级损坏静默跳过，顶层错误显式报 ----

void testParseTolerant()
{
	QTemporaryDir dir;

	// 顶层非 JSON → false + error
	{
		QFile f(dir.filePath(QStringLiteral("bad.json")));
		f.open(QIODevice::WriteOnly);
		f.write("not json at all");
	}
	DashboardModel model;
	QString error;
	QEWT_CHECK(!loadModel(dir.filePath(QStringLiteral("bad.json")), model, &error));
	QEWT_CHECK(!error.isEmpty());

	// 文件不存在 → false + error
	QEWT_CHECK(!loadModel(dir.filePath(QStringLiteral("nope.json")), model, &error));
	QEWT_CHECK(!error.isEmpty());

	// record 级损坏：字符串元素跳过、无 fields 的对象保留（fields 空）
	QJsonArray records;
	records.append(QJsonValue(QStringLiteral("garbage string")));
	QJsonObject emptyRec;		// kind/time/raw 全缺省
	records.append(emptyRec);
	records.append(plainSlowEvent(QStringLiteral("10:00:00.000"),
								  QStringLiteral("42.5")));
	QJsonObject session;
	session.insert(QStringLiteral("host_pid"), 5);
	session.insert(QStringLiteral("records"), records);
	QJsonObject root;
	root.insert(QStringLiteral("sessions"), QJsonArray{ session });
	QEWT_CHECK(loadModel(writeJson(QJsonDocument(root), dir,
								   QStringLiteral("tol.json")),
						 model, &error));
	QEWT_CHECK_EQ(model.sessions.size(), 1);
	QEWT_CHECK_EQ(model.sessions.first().events.size(), 2);
	QEWT_CHECK_EQ(int(model.sessions.first().events[0].cls),
				  int(EventClass::Other));
	QEWT_CHECK_EQ(int(model.sessions.first().events[1].cls),
				  int(EventClass::SlowEvent));
}

// ---- escapeHtml / embedJsonSafe / recordField ----

void testEscapeAndEmbed()
{
	QEWT_CHECK_EQ(escapeHtml(QStringLiteral("a<b>&\"c")).toStdString(),
				  std::string("a&lt;b&gt;&amp;&quot;c"));

	// 内嵌 JSON 唯一注入面 = </script> 提前闭合 → "</" 必须被打掉
	QJsonObject payload;
	payload.insert(QStringLiteral("url"),
				   QStringLiteral("</script><img src=x onerror=alert(1)>"));
	const QString embedded = embedJsonSafe(QJsonDocument(payload));
	QEWT_CHECK(!embedded.contains(QStringLiteral("</")));
	QEWT_CHECK(embedded.contains(QStringLiteral("<\\/script>")));

	// \/ 为 JSON 合法转义：回读语义不变
	const QJsonDocument back = QJsonDocument::fromJson(embedded.toUtf8());
	QEWT_CHECK(back.isObject());
	QEWT_CHECK_EQ(back.object().value(QStringLiteral("url")).toString().toStdString(),
				  payload.value(QStringLiteral("url")).toString().toStdString());
}

void testRecordField()
{
	QJsonObject rec = makeRecord(0, QStringLiteral("10:00:00.000"),
								 QStringLiteral("raw text"),
								 { { QStringLiteral("a"), QStringLiteral("1") },
								   { QStringLiteral("b"), QStringLiteral("x y") } });
	QEWT_CHECK_EQ(recordField(rec, QStringLiteral("a")).toStdString(),
				  std::string("1"));
	QEWT_CHECK_EQ(recordField(rec, QStringLiteral("b")).toStdString(),
				  std::string("x y"));
	QEWT_CHECK_EQ(recordField(rec, QStringLiteral("missing")).toStdString(),
				  std::string());
	QJsonObject noFields;
	QEWT_CHECK_EQ(recordField(noFields, QStringLiteral("a")).toStdString(),
				  std::string());
}

// ---- filterModel：kinds 白名单 + pid 会话过滤（摘要/渲染/内嵌共用口径）----

void testFilterModel()
{
	QTemporaryDir dir;
	QJsonArray recordsA;
	recordsA.append(freezeStarted(QStringLiteral("10:00:00.000"),
								  QStringLiteral("appA.exe"),
								  QStringLiteral("2100")));
	recordsA.append(plainSlowEvent(QStringLiteral("10:00:01.000"),
								   QStringLiteral("42.5")));
	QJsonArray recordsB;
	recordsB.append(plainSlowEvent(QStringLiteral("10:00:02.000"),
								   QStringLiteral("12.0")));
	QJsonObject sa;
	sa.insert(QStringLiteral("host_pid"), 11);
	sa.insert(QStringLiteral("records"), recordsA);
	QJsonObject sb;
	sb.insert(QStringLiteral("host_pid"), 22);
	sb.insert(QStringLiteral("records"), recordsB);
	QJsonObject root;
	root.insert(QStringLiteral("sessions"), QJsonArray{ sa, sb });

	DashboardModel model;
	QString error;
	QEWT_CHECK(loadModel(writeJson(QJsonDocument(root), dir,
								   QStringLiteral("agg.json")),
						 model, &error));

	// 全量透传（kinds 空 + pid 0）
	const DashboardModel full = filterModel(model, QVector<int>(), 0);
	QEWT_CHECK_EQ(full.sessions.size(), 2);
	QEWT_CHECK_EQ(full.totalRecords, 3);

	// kinds={3}：只留 freeze 事件，spans 保留，计数重算
	const DashboardModel fz = filterModel(model, QVector<int>{ 3 }, 0);
	QEWT_CHECK_EQ(fz.sessions.size(), 2);
	QEWT_CHECK_EQ(fz.totalRecords, 1);
	QEWT_CHECK_EQ(fz.kindCounts[3], 1);
	QEWT_CHECK_EQ(fz.kindCounts[0], 0);
	QEWT_CHECK_EQ(fz.sessions[0].freezeSpans.size(), 1);

	// kinds={0}：spans 是 kind=3 聚合视图 → 清空
	const DashboardModel slow = filterModel(model, QVector<int>{ 0 }, 0);
	QEWT_CHECK_EQ(slow.totalRecords, 2);
	QEWT_CHECK_EQ(slow.sessions[0].freezeSpans.size(), 0);

	// pid 会话过滤
	const DashboardModel pid = filterModel(model, QVector<int>(), 22);
	QEWT_CHECK_EQ(pid.sessions.size(), 1);
	QEWT_CHECK_EQ(pid.sessions[0].pid, qint64(22));
	QEWT_CHECK_EQ(pid.totalRecords, 1);
}

} // namespace

int main()
{
	const qewt::Case cases[] = {
		{ "parseAggJson", testParseAggJson },
		{ "classify", testClassify },
		{ "freezeSpans", testFreezeSpans },
		{ "relMsCrossMidnight", testRelMsCrossMidnight },
		{ "parseTolerant", testParseTolerant },
		{ "escapeAndEmbed", testEscapeAndEmbed },
		{ "recordField", testRecordField },
		{ "filterModel", testFilterModel },
	};
	return qewt::runAll(cases, int(sizeof(cases) / sizeof(cases[0])));
}
