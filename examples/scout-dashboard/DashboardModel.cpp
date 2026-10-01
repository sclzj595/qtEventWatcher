#include "DashboardModel.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonParseError>
#include <QTime>

namespace qt_event_watcher {
namespace dashboard {

namespace {

/// 冻结行状态推导（字段签名，对齐 AggregationReporter 冻结时间线口径）：
/// started 带 stalledMs、ongoing 带 elapsedMs、recovered 带 totalMs、lost 全无
/// （WindowFreezeProber 三态行与 EventWatchdog 告警行同签名）
QString freezeStateOf(const QJsonObject &rec)
{
	if (!recordField(rec, QStringLiteral("stalledMs")).isEmpty())
		return QStringLiteral("started");
	if (!recordField(rec, QStringLiteral("elapsedMs")).isEmpty())
		return QStringLiteral("ongoing");
	if (!recordField(rec, QStringLiteral("totalMs")).isEmpty())
		return QStringLiteral("recovered");
	return QStringLiteral("lost");
}

/// "HH:mm:ss.zzz" → 当日毫秒（无日期口径；不可解析返回 false）
int msecsOfDay(const QString &time, bool *ok)
{
	const QTime t = QTime::fromString(time, QStringLiteral("HH:mm:ss.zzz"));
	*ok = t.isValid();
	return t.msecsSinceStartOfDay();
}

/// freeze started→recovered/lost 配对（会话内 JSON 顺序即时间序）：
/// started 开段（防御：前段未闭合按 lost 收）/ ongoing 忽略（1s 节流心跳）/
/// recovered 以 totalMs 闭段 / lost 以 -1 闭段 / 收尾开放段=ongoing（导出
/// 时刻仍在冻结，时间轴条画到末端）。同一 receiver 多段冻结天然成多个 span
/// （scout 状态机 freezing bool 保证不并发）。
void buildFreezeSpans(SessionModel &session)
{
	FreezeSpan open;
	bool hasOpen = false;
	for (const EventRecord &ev : session.events) {
		if (ev.cls != EventClass::Freeze)
			continue;
		if (ev.state == QLatin1String("started")) {
			if (hasOpen) {						// 防御：状态机异常跳变，前段按 lost 收
				open.endState = QStringLiteral("lost");
				open.durationMs = -1;
				session.freezeSpans.append(open);
			}
			open = FreezeSpan{};
			open.receiver = ev.receiver;
			open.startTime = ev.time;
			open.startRelMs = ev.relMs;
			hasOpen = true;
		} else if (ev.state == QLatin1String("recovered")) {
			if (hasOpen) {
				open.durationMs = qint64(ev.totalMs);
				open.endState = QStringLiteral("recovered");
				session.freezeSpans.append(open);
				hasOpen = false;
			}
		} else if (ev.state == QLatin1String("lost")) {
			if (hasOpen) {
				open.durationMs = -1;
				open.endState = QStringLiteral("lost");
				session.freezeSpans.append(open);
				hasOpen = false;
			}
		}
	}
	if (hasOpen) {
		open.durationMs = -1;
		open.endState = QStringLiteral("ongoing");
		session.freezeSpans.append(open);
	}
}

} // namespace

QString recordField(const QJsonObject &rec, const QString &key)
{
	const QJsonArray fields = rec.value(QStringLiteral("fields")).toArray();
	for (const QJsonValue &v : fields) {
		const QJsonArray pair = v.toArray();
		if (pair.size() == 2 && pair.at(0).toString() == key)
			return pair.at(1).toString();
	}
	return QString();
}

EventClass classifyRecord(int kind, const QString &eventField,
						  const QString &sourceField, const QString &typeField)
{
	if (kind == 3)
		return EventClass::Freeze;
	if (typeField == QLatin1String("98")
		|| eventField == QLatin1String("cdpLongTask")
		|| sourceField == QLatin1String("scout-cdp"))
		return EventClass::CdpLongTask;
	if (typeField == QLatin1String("99")
		|| eventField == QLatin1String("cpuSpin")
		|| sourceField == QLatin1String("scout"))
		return EventClass::CpuSpin;
	if (kind == 0)
		return EventClass::SlowEvent;
	if (kind == 1)
		return EventClass::MetaCall;
	if (kind == 2)
		return EventClass::Qss;
	return EventClass::Other;
}

QString escapeHtml(const QString &s)
{
	QString out = s;
	out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
	out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
	out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
	out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
	return out;
}

QString embedJsonSafe(const QJsonDocument &doc)
{
	QString s = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
	s.replace(QStringLiteral("</"), QStringLiteral("<\\/"));
	return s;
}

bool loadModel(const QString &jsonPath, DashboardModel &out, QString *error)
{
	out = DashboardModel{};

	QFile file(jsonPath);
	if (!file.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("cannot open %1: %2")
						 .arg(jsonPath, file.errorString());
		return false;
	}
	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
	if (!doc.isObject()) {
		if (error)
			*error = QStringLiteral("invalid JSON: %1 (offset %2)")
						 .arg(parseError.errorString())
						 .arg(parseError.offset);
		return false;
	}
	const QJsonObject root = doc.object();
	out.generatedAt = root.value(QStringLiteral("generatedAt")).toString();
	out.server = root.value(QStringLiteral("server")).toString();

	const QJsonArray sessionArray = root.value(QStringLiteral("sessions")).toArray();
	for (const QJsonValue &sv : sessionArray) {
		if (!sv.isObject())
			continue;						// 会话级损坏：静默跳过（协议容错）
		const QJsonObject so = sv.toObject();
		SessionModel session;
		session.pid = static_cast<qint64>(
			so.value(QStringLiteral("host_pid")).toDouble());
		session.received = static_cast<quint64>(
			so.value(QStringLiteral("received")).toDouble(0));
		session.dropped = static_cast<quint64>(
			so.value(QStringLiteral("dropped")).toDouble(0));
		const QJsonValue hv = so.value(QStringLiteral("health"));
		if (hv.isObject()) {
			session.health = hv.toObject();
			session.hasHealth = true;
		}

		// 第一遍：record → EventRecord + 当日毫秒（t0 取首条可解析记录——
		// JSON 顺序即时间序，与 freeze 配对同假设；min 口径在跨午夜时会被
		// 次日小值击穿）
		QVector<QPair<EventRecord, int>> parsed;	// <事件, 当日毫秒(-1=不可解析)>
		qint64 t0 = -1;
		const QJsonArray recs = so.value(QStringLiteral("records")).toArray();
		parsed.reserve(recs.size());
		for (const QJsonValue &rv : recs) {
			if (!rv.isObject())
				continue;
			const QJsonObject ro = rv.toObject();
			EventRecord er;
			er.kind = ro.value(QStringLiteral("kind")).toInt(0);
			er.time = ro.value(QStringLiteral("time")).toString();
			er.raw = ro.value(QStringLiteral("raw")).toString();
			er.event = recordField(ro, QStringLiteral("event"));
			er.source = recordField(ro, QStringLiteral("source"));
			er.cls = classifyRecord(er.kind, er.event, er.source,
									recordField(ro, QStringLiteral("type")));
			// 防御：fields 全空 = hydrate 失败/损坏记录，无结构化数据可渲染，
			// 不按 kind 落业务泳道（Other 兜底；kind=3 Freeze 保留 kind 判定）
			if (ro.value(QStringLiteral("fields")).toArray().isEmpty()
				&& (er.cls == EventClass::SlowEvent
					|| er.cls == EventClass::MetaCall
					|| er.cls == EventClass::Qss))
				er.cls = EventClass::Other;
			if (er.cls == EventClass::Freeze)
				er.state = freezeStateOf(ro);
			er.url = recordField(ro, QStringLiteral("url"));
			er.receiver = recordField(ro, QStringLiteral("receiver"));
			er.costMs = recordField(ro, QStringLiteral("costMs")).toDouble();
			er.thresholdMs = recordField(ro, QStringLiteral("thresholdMs")).toDouble();
			er.totalMs = recordField(ro, QStringLiteral("totalMs")).toDouble();
			bool ok = false;
			const int m = msecsOfDay(er.time, &ok);
			parsed.append(qMakePair(er, ok ? m : -1));
			if (ok && t0 < 0)
				t0 = m;
			if (er.kind >= 0 && er.kind <= 3)
				out.kindCounts[er.kind]++;
			if (er.cls == EventClass::CdpLongTask)
				out.cdpCount++;
			else if (er.cls == EventClass::CpuSpin)
				out.cpuCount++;
			if (er.source == QLatin1String("scout")
				|| er.source == QLatin1String("scout-cdp"))
				session.isScoutSession = true;
		}

		// 第二遍：relMs（跨午夜防御：差值为负 = 23:59→00:xx 跨日，+86400000）
		session.t0Ms = t0 < 0 ? 0 : t0;
		for (const auto &pr : parsed) {
			EventRecord er = pr.first;
			if (pr.second >= 0) {
				qint64 rel = qint64(pr.second) - session.t0Ms;
				if (rel < 0)
					rel += 86400000;
				er.relMs = rel;
			}
			session.events.append(er);
		}
		buildFreezeSpans(session);
		out.sessions.append(session);
		out.totalRecords += session.events.size();
	}
	return true;
}

DashboardModel filterModel(const DashboardModel &in, const QVector<int> &kinds,
						   qint64 pidFilter)
{
	if (pidFilter <= 0 && kinds.isEmpty())
		return in;							// 全量透传（值拷贝，调用方可改）

	DashboardModel out;
	out.generatedAt = in.generatedAt;
	out.server = in.server;
	for (const SessionModel &s : in.sessions) {
		if (pidFilter > 0 && s.pid != pidFilter)
			continue;
		SessionModel fs = s;
		fs.events.clear();
		fs.freezeSpans.clear();
		for (const EventRecord &ev : s.events) {
			if (!kinds.isEmpty() && !kinds.contains(ev.kind))
				continue;
			fs.events.append(ev);
			if (ev.kind >= 0 && ev.kind <= 3)
				out.kindCounts[ev.kind]++;
			if (ev.cls == EventClass::CdpLongTask)
				out.cdpCount++;
			else if (ev.cls == EventClass::CpuSpin)
				out.cpuCount++;
		}
		// freezeSpans 是 kind=3 的聚合视图：kinds 含 3（或全量）才保留
		if (kinds.isEmpty() || kinds.contains(3))
			fs.freezeSpans = s.freezeSpans;
		out.sessions.append(fs);
		out.totalRecords += fs.events.size();
	}
	return out;
}

} // namespace dashboard
} // namespace qt_event_watcher
