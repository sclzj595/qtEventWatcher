#include "AggregationReporter.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QTextStream>

#include <algorithm>
#include <map>
#include <vector>

namespace {

/// HTML 转义（所有动态文本必经；与 HtmlReporter 同规则）
QString escapeHtml(const QString& s)
{
	QString out = s;
	out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
	out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
	out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
	out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
	return out;
}

/// kind 枚举 → 展示名（对齐 main.cpp kindName；与 WatchRecordStore::Kind 同枚举）
const char* kindName(int kind)
{
	switch (kind) {
	case 1:		return "MetaCall";
	case 2:		return "Qss";
	case 3:		return "Freeze";
	default:	return "SlowEvent";
	}
}

const char* kindBadgeClass(int kind)
{
	switch (kind) {
	case 1:		return "badge-metaCall";
	case 2:		return "badge-qss";
	case 3:		return "badge-freeze";
	default:	return "badge-slowEvent";
	}
}

/// fields=[[k,v]..] 数组取值（形状由 main.cpp rebuildStructured 生成，同仓同发）
QString recordField(const QJsonObject& rec, const QString& key)
{
	const QJsonArray fields = rec.value(QStringLiteral("fields")).toArray();
	for (const QJsonValue& v : fields) {
		const QJsonArray pair = v.toArray();
		if (pair.size() == 2 && pair.at(0).toString() == key)
			return pair.at(1).toString();
	}
	return QString();
}

/// 带所属 pid 的记录引用（跨进程合并遍历的公共载体）
struct PidRecord
{
	qint64 pid = 0;
	const QJsonObject* rec = nullptr;
};

std::vector<PidRecord> collectAll(const std::map<qint64, Session>& sessions)
{
	std::vector<PidRecord> all;
	for (const auto& kv : sessions) {
		for (const QJsonObject& r : kv.second.records)
			all.push_back({ kv.first, &r });
	}
	return all;
}

QString buildOverview(const std::map<qint64, Session>& sessions)
{
	std::uint64_t received = 0;
	std::uint64_t dropped = 0;
	std::size_t stored = 0;
	int kinds[4] = { 0, 0, 0, 0 };
	for (const auto& kv : sessions) {
		received += kv.second.received;
		dropped += kv.second.dropped;
		stored += kv.second.records.size();
		for (const QJsonObject& r : kv.second.records) {
			const int kind = r.value(QStringLiteral("kind")).toInt();
			if (kind >= 0 && kind < 4)
				++kinds[kind];
		}
	}

	const auto kindBadge = [&kinds](int kind) {
		return QStringLiteral("<span class=\"badge %1\">%2 × %3</span>")
			.arg(QLatin1String(kindBadgeClass(kind)),
				 QLatin1String(kindName(kind)))
			.arg(kinds[kind]);
	};

	QString html = QStringLiteral(
		"<section>\n<h2>概览</h2>\n"
		"<table><tbody>\n"
		"<tr><td>聚合进程（sessions）</td><td class=\"num\">%1</td></tr>\n"
		"<tr><td>received 合计</td><td class=\"num\">%2</td></tr>\n"
		"<tr><td>dropped 合计（对端 skipped + 本端环形溢出）</td><td class=\"num\">%3</td></tr>\n"
		"<tr><td>记录现存合计（环形上限 4096×N）</td><td class=\"num\">%4</td></tr>\n"
		"<tr><td>分 kind 计数</td><td>%5 %6 %7 %8</td></tr>\n"
		"</tbody></table>\n</section>\n")
		.arg(static_cast<int>(sessions.size()))
		.arg(QString::number(static_cast<qulonglong>(received)))
		.arg(QString::number(static_cast<qulonglong>(dropped)))
		.arg(QString::number(static_cast<qulonglong>(stored)))
		.arg(kindBadge(0), kindBadge(1), kindBadge(2), kindBadge(3));
	return html;
}

/// V5 B health 快照的可视化脸面（V5.1 C 线主目标之一）
QString buildHealthSection(const std::map<qint64, Session>& sessions)
{
	QString rows;
	for (const auto& kv : sessions) {
		const Session& s = kv.second;
		QString healthCells;
		if (s.hasHealth) {
			healthCells = QStringLiteral(
								"<td class=\"num\">%1</td><td class=\"num\">%2</td>"
								"<td class=\"num\">%3</td><td class=\"num\">%4</td>")
							  .arg(s.health.value(QStringLiteral("pushed")).toInt())
							  .arg(s.health.value(QStringLiteral("dropped")).toInt())
							  .arg(s.health.value(QStringLiteral("reconnects")).toInt())
							  .arg(s.health.value(QStringLiteral("lastSeq")).toInt());
		} else {
			healthCells = QStringLiteral(
				"<td colspan=\"4\" class=\"muted\">未上行（health 为 V5 B 随拍 5s 节拍，断线期不发）</td>");
		}
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"num\">%2</td>"
					"<td class=\"num\">%3</td><td class=\"num\">%4</td>%5</tr>\n")
					.arg(kv.first)
					.arg(static_cast<int>(s.records.size()))
					.arg(QString::number(static_cast<qulonglong>(s.received)))
					.arg(QString::number(static_cast<qulonglong>(s.dropped)))
					.arg(healthCells);
	}
	return QStringLiteral(
		"<section>\n<h2>进程健康度（per-pid）</h2>\n"
		"<table><thead><tr><th>host_pid</th><th>记录现存</th><th>received</th>"
		"<th>dropped</th><th>pushed</th><th>health dropped</th><th>reconnects</th>"
		"<th>lastSeq</th></tr></thead><tbody>\n%1</tbody></table>\n</section>\n")
		.arg(rows);
}

/// TOP 接收者：跨进程合并 SlowEvent 告警记录，按累计 costMs（周期统计
/// EventStatistics 跨进程不可合并，此口径为告警实测值，无 Exclusive/percentile）
QString buildTopReceivers(const std::vector<PidRecord>& all)
{
	struct Agg
	{
		qint64 count = 0;
		double totalCostMs = 0.0;
		double maxCostMs = 0.0;
	};
	std::map<QString, Agg> merged;
	for (const PidRecord& pr : all) {
		if (pr.rec->value(QStringLiteral("kind")).toInt() != 0)
			continue;
		const QString receiver = recordField(*pr.rec, QStringLiteral("receiver"));
		if (receiver.isEmpty())
			continue;
		bool ok = false;
		const double costMs = recordField(*pr.rec, QStringLiteral("costMs")).toDouble(&ok);
		Agg& a = merged[receiver];
		++a.count;
		if (ok) {
			a.totalCostMs += costMs;
			a.maxCostMs = std::max(a.maxCostMs, costMs);
		}
	}

	std::vector<std::pair<QString, Agg>> rows;
	rows.reserve(merged.size());
	for (const auto& kv : merged)
		rows.push_back(kv);
	std::sort(rows.begin(), rows.end(),
			  [](const std::pair<QString, Agg>& a, const std::pair<QString, Agg>& b) {
				  return a.second.totalCostMs > b.second.totalCostMs;
			  });

	QString body;
	const int topN = std::min<int>(static_cast<int>(rows.size()), 10);
	for (int i = 0; i < topN; ++i) {
		const auto& row = rows.at(i);
		body += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"num\">%2</td>"
					"<td class=\"num\">%3</td><td class=\"num\">%4</td></tr>\n")
					.arg(escapeHtml(row.first))
					.arg(row.second.count)
					.arg(row.second.totalCostMs, 0, 'f', 1)
					.arg(row.second.maxCostMs, 0, 'f', 1);
	}
	return QStringLiteral(
		"<section>\n<h2>TOP 接收者（跨进程合并，按累计 costMs）</h2>\n"
		"<table><thead><tr><th>receiver</th><th>告警次数</th><th>累计 costMs</th>"
		"<th>最大 costMs</th></tr></thead><tbody>\n%1</tbody></table>\n"
		"<p class=\"muted\">口径 = 聚合告警记录实测值；跨进程无 EventStatistics "
		"周期统计语义，无 Exclusive Cost / percentile 合并。</p>\n</section>\n")
		.arg(body.isEmpty()
				  ? QStringLiteral("<tr><td colspan=\"4\" class=\"muted\">无 SlowEvent 告警记录</td></tr>\n")
				  : body);
}

/// 冻结时间线：Freeze 记录跨进程合并（pid 列新增）；三态语义与 HtmlReporter 一致
QString buildFreezeSection(const std::vector<PidRecord>& all)
{
	QString rows;
	for (const PidRecord& pr : all) {
		const QJsonObject& r = *pr.rec;
		if (r.value(QStringLiteral("kind")).toInt() != 3)
			continue;
		const auto fieldOf = [&r](const char* key) {
			return recordField(r, QLatin1String(key));
		};
		QString detail;
		const QString stalled = fieldOf("stalledMs");
		const QString elapsed = fieldOf("elapsedMs");
		const QString total = fieldOf("totalMs");
		if (!stalled.isEmpty())
			detail = QStringLiteral("开始：停滞 %1ms").arg(stalled);
		else if (!elapsed.isEmpty())
			detail = QStringLiteral("进行中：已持续 %1ms").arg(elapsed);
		else if (!total.isEmpty())
			detail = QStringLiteral("恢复：总时长 %1ms").arg(total);

		const auto cell = [&fieldOf](const char* key) {
			const QString v = fieldOf(key);
			return v.isEmpty() ? QStringLiteral("-") : v;
		};
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"mono\">%2</td><td>%3</td>"
					"<td class=\"mono\">%4</td><td class=\"mono\">%5</td></tr>\n")
					.arg(escapeHtml(r.value(QStringLiteral("time")).toString()))
					.arg(pr.pid)
					.arg(escapeHtml(detail.isEmpty() ? QStringLiteral("-") : detail))
					.arg(escapeHtml(cell("receiver")))
					.arg(escapeHtml(cell("type")));
	}
	if (rows.isEmpty()) {
		return QStringLiteral(
			"<section>\n<h2>冻结时间线</h2>\n"
			"<p class=\"muted\">无冻结告警记录（各进程未触发 FreezeWatch 或监控位关闭）。</p>\n"
			"</section>\n");
	}
	return QStringLiteral(
		"<section>\n<h2>冻结时间线（跨进程）</h2>\n"
		"<table><thead><tr><th>时间</th><th>host_pid</th><th>状态</th>"
		"<th>receiver</th><th>事件类型</th></tr></thead><tbody>\n"
		"%1</tbody></table>\n</section>\n").arg(rows);
}

/// 明细记录：跨进程合并最近 200 条（时间倒序；跨日排序不做，与导出时点同日假设）
QString buildDetailsSection(std::vector<PidRecord> all)
{
	std::stable_sort(all.begin(), all.end(),
					 [](const PidRecord& a, const PidRecord& b) {
						 return a.rec->value(QStringLiteral("time")).toString()
							 > b.rec->value(QStringLiteral("time")).toString();
					 });

	constexpr int kMaxCount = 200;
	const int total = static_cast<int>(all.size());
	const int begin = std::max(0, total - kMaxCount);

	QString rows;
	for (int i = total - 1; i >= begin; --i) {
		const QJsonObject& r = *all.at(static_cast<std::size_t>(i)).rec;
		const int kind = r.value(QStringLiteral("kind")).toInt();
		const auto cell = [&r](const char* key) {
			const QString v = recordField(r, QLatin1String(key));
			return v.isEmpty() ? QStringLiteral("-") : v;
		};
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"mono\">%2</td>"
					"<td><span class=\"badge %3\">%4</span></td>"
					"<td class=\"mono\">%5</td><td>%6</td><td class=\"mono\">%7</td>"
					"<td class=\"mono\">%8</td><td class=\"num\">%9</td>"
					"<td class=\"num\">%10</td></tr>\n")
					.arg(escapeHtml(r.value(QStringLiteral("time")).toString()))
					.arg(all.at(static_cast<std::size_t>(i)).pid)
					.arg(QLatin1String(kindBadgeClass(kind)))
					.arg(QLatin1String(kindName(kind)))
					.arg(escapeHtml(cell("receiver")))
					.arg(escapeHtml(cell("event")))
					.arg(escapeHtml(cell("sender")))
					.arg(escapeHtml(cell("signal")))
					.arg(escapeHtml(cell("costMs")))
					.arg(escapeHtml(cell("thresholdMs")));
	}

	QString html = QStringLiteral(
		"<details>\n<summary>明细记录（最近 %1 条 / 共 %2 条，跨进程合并）</summary>\n"
		"<table><thead><tr><th>时间</th><th>host_pid</th><th>类型</th><th>receiver</th>"
		"<th>event</th><th>sender</th><th>signal</th><th>costMs</th><th>thresholdMs</th>"
		"</tr></thead><tbody>\n").arg(kMaxCount).arg(total);
	if (rows.isEmpty())
		html += QStringLiteral("<tr><td colspan=\"9\" class=\"muted\">无记录</td></tr>\n");
	html += rows;
	html += QStringLiteral("</tbody></table>\n</details>\n");
	return html;
}

} // namespace

bool AggregationReporter::exportHtml(const QString& filePath,
									 const std::map<qint64, Session>& sessions,
									 const QString& serverName, QString* error)
{
	if (filePath.isEmpty()) {
		if (error)	*error = QStringLiteral("empty file path");
		return false;
	}

	const std::vector<PidRecord> all = collectAll(sessions);

	QString html = QStringLiteral(
		"<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
		"<title>QtEventWatcher 聚合报告</title>\n"
		"<style>\n"
		"body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;margin:24px;"
		"color:#1f2430;background:#f7f8fa;line-height:1.5}\n"
		"h1{font-size:22px;margin-bottom:4px}\n"
		"h2{font-size:16px;border-bottom:1px solid #d8dce6;padding-bottom:6px;margin-top:28px}\n"
		"table{border-collapse:collapse;width:100%;background:#fff;font-size:13px}\n"
		"th,td{border:1px solid #d8dce6;padding:5px 9px;text-align:left;vertical-align:top}\n"
		"th{background:#eef0f5}\n"
		".num{text-align:right;font-variant-numeric:tabular-nums}\n"
		".mono{font-family:Consolas,monospace;font-size:12px}\n"
		".muted{color:#8a90a0}\n"
		".badge{display:inline-block;padding:1px 8px;border-radius:10px;font-size:11px;"
		"font-weight:600}\n"
		".badge-slowEvent{background:#fdecea;color:#b3402a}\n"
		".badge-metaCall{background:#e3ecfb;color:#2b5cb8}\n"
		".badge-qss{background:#efe3fb;color:#7d3cb8}\n"
		".badge-freeze{background:#e3f5fb;color:#1a7ca0}\n"
		"summary{cursor:pointer;font-weight:600;margin:16px 0 8px}\n"
		"</style>\n</head>\n<body>\n"
		"<h1>QtEventWatcher 聚合报告</h1>\n"
		"<p class=\"muted\">导出时间 %1 · 中心收集器 %2 · 会话 %3 个（多进程按 host_pid 分组）</p>\n")
		.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
			 escapeHtml(serverName))
		.arg(static_cast<int>(sessions.size()));

	html += buildOverview(sessions);
	html += buildHealthSection(sessions);
	html += buildTopReceivers(all);
	html += buildFreezeSection(all);
	html += buildDetailsSection(all);
	html += QStringLiteral("</body>\n</html>\n");

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open file: %1").arg(file.errorString());
		return false;
	}
	// HTML 严格 UTF-8 无 BOM（与 HtmlReporter / DataExporter 同规则）
	QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif
	stream.setGenerateByteOrderMark(false);
	stream << html;
	return true;
}
