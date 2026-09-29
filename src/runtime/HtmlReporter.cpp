#include "HtmlReporter.h"

#include "DiagnosticSummarizer.h"
#include "EventStatistics.h"
#include "WatchConfig.h"
#include "WatchRecordStore.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <map>
#include <vector>

namespace qt_event_watcher
{

namespace
{

/// HTML 转义（所有动态文本必经；防御 objectName 等自由文本注入标签）
QString escapeHtml(const QString& s)
{
	QString out = s;
	out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
	out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
	out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
	out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
	return out;
}

const char* kindName(int kind)
{
	switch (kind) {
	case WatchRecordStore::KindMetaCall:	return "metaCall";
	case WatchRecordStore::KindQss:			return "qss";
	case WatchRecordStore::KindFreeze:		return "freeze";
	default:								return "slowEvent";
	}
}

const char* kindBadgeClass(int kind)
{
	switch (kind) {
	case WatchRecordStore::KindMetaCall:	return "badge-metaCall";
	case WatchRecordStore::KindQss:			return "badge-qss";
	case WatchRecordStore::KindFreeze:		return "badge-freeze";
	default:								return "badge-slowEvent";
	}
}

const char* severityTag(int severity)
{
	switch (severity) {
	case 2:		return "CRITICAL";
	case 1:		return "WARNING";
	default:	return "INFO";
	}
}

const char* severityBadgeClass(int severity)
{
	switch (severity) {
	case 2:		return "badge-sev-critical";
	case 1:		return "badge-sev-warning";
	default:	return "badge-sev-info";
	}
}

const std::string* findField(const std::vector<WatchRecordStore::Field>& fields,
							 const char* key)
{
	for (const WatchRecordStore::Field& f : fields) {
		if (f.first == key)		return &f.second;
	}
	return nullptr;
}

QString fieldValue(const WatchRecordStore::Record& r, const char* key)
{
	const std::string* v = findField(r.fields, key);
	return v != nullptr ? QString::fromStdString(*v) : QString();
}

/// 跨周期合并统计行（累计耗时口径；percentile 需直方图，跨周期不可合并故不展示）
struct MergedStat
{
	QString event;
	qint64 count = 0;
	double totalCostMs = 0.0;
	double maxCostMs = 0.0;
	double exclusiveTotalMs = 0.0;
};

QString buildStatsSection(const EventStatistics* statistics)
{
	if (statistics == nullptr) {
		return QStringLiteral("<section>\n<h2>统计 TOP</h2>\n"
							  "<p class=\"muted\">未提供周期统计（statistics=nullptr）。</p>\n"
							  "</section>\n");
	}

	std::map<QString, MergedStat> merged;
	const auto mergePeriod = [&merged](const EventStatistics::PeriodSnapshot& p) {
		for (const EventStatistics::StatEntry& e : p.entries) {
			MergedStat& m = merged[e.eventKey];
			m.event = e.eventKey;
			m.count += e.count;
			m.totalCostMs += static_cast<double>(e.totalCostNs) / 1000000.0;
			m.maxCostMs = std::max(m.maxCostMs,
								   static_cast<double>(e.maxCostNs) / 1000000.0);
			m.exclusiveTotalMs += static_cast<double>(e.exclusiveTotalNs) / 1000000.0;
		}
	};
	const QVector<EventStatistics::PeriodSnapshot> history = statistics->statisticsHistory();
	for (const EventStatistics::PeriodSnapshot& p : history)
		mergePeriod(p);
	mergePeriod(statistics->liveSnapshot());

	QVector<MergedStat> rows;
	rows.reserve(static_cast<int>(merged.size()));
	for (const auto& kv : merged)
		rows.append(kv.second);
	std::sort(rows.begin(), rows.end(),
			  [](const MergedStat& a, const MergedStat& b) {
				  return a.totalCostMs > b.totalCostMs;
			  });

	QString html = QStringLiteral(
		"<section>\n<h2>统计 TOP（跨周期合并，按累计耗时）</h2>\n"
		"<table><thead><tr><th>事件</th><th>次数</th><th>累计耗时 ms</th>"
		"<th>最大耗时 ms</th><th>Exclusive 累计 ms</th></tr></thead><tbody>\n");
	if (rows.isEmpty())
		html += QStringLiteral("<tr><td colspan=\"5\" class=\"muted\">暂无周期统计数据</td></tr>\n");
	const int topN = std::min<int>(rows.size(), 10);
	for (int i = 0; i < topN; ++i) {
		const MergedStat& m = rows.at(i);
		html += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"num\">%2</td>"
					"<td class=\"num\">%3</td><td class=\"num\">%4</td>"
					"<td class=\"num\">%5</td></tr>\n")
					.arg(escapeHtml(m.event))
					.arg(m.count)
					.arg(m.totalCostMs, 0, 'f', 1)
					.arg(m.maxCostMs, 0, 'f', 1)
					.arg(m.exclusiveTotalMs, 0, 'f', 1);
	}
	html += QStringLiteral(
				"</tbody></table>\n<p class=\"muted\">归档周期 %1 个 + 进行中周期；"
				"Exclusive Cost 已扣除嵌套子事件（V2 C2 口径）。</p>\n</section>\n")
			.arg(history.size());
	return html;
}

QString buildFindingsSection(const WatchConfig* config, const EventStatistics* statistics)
{
	const QVector<DiagnosticSummarizer::Finding> findings =
		DiagnosticSummarizer::analyze(config, statistics);

	static const char* tags[] = { "INFO", "WARNING", "CRITICAL" };
	static const char* badges[] = { "badge-sev-info", "badge-sev-warning", "badge-sev-critical" };

	QString html = QStringLiteral("<section>\n<h2>诊断结论</h2>\n");
	if (findings.isEmpty()) {
		html += QStringLiteral(
			"<p class=\"muted\">无归因结论：暂无慢事件 / MetaCall / QSS / 冻结告警记录。</p>\n");
	}
	for (const DiagnosticSummarizer::Finding& f : findings) {
		const int sev = static_cast<int>(f.severity);
		html += QStringLiteral(
					"<div class=\"finding\"><span class=\"badge %1\">%2</span> "
					"<strong>%3</strong> — %4\n<ul>\n")
					.arg(QLatin1String(badges[sev]), QLatin1String(tags[sev]),
						 escapeHtml(f.category), escapeHtml(f.headline));
		for (const QString& d : f.details)
			html += QStringLiteral("<li class=\"mono\">%1</li>\n").arg(escapeHtml(d));
		html += QStringLiteral("</ul>\n</div>\n");
	}
	html += QStringLiteral("</section>\n");
	return html;
}

/// 冻结时间线：FreezeWatch 三态告警行（started → ongoing* → recovered）
QString buildFreezeSection(const std::vector<WatchRecordStore::Record>& records)
{
	QString rows;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind != WatchRecordStore::KindFreeze)
			continue;
		QString detail;
		const std::string* stalled = findField(r.fields, "stalledMs");
		const std::string* elapsed = findField(r.fields, "elapsedMs");
		const std::string* total = findField(r.fields, "totalMs");
		if (stalled != nullptr)
			detail = QStringLiteral("开始：停滞 %1ms").arg(QString::fromStdString(*stalled));
		else if (elapsed != nullptr)
			detail = QStringLiteral("进行中：已持续 %1ms").arg(QString::fromStdString(*elapsed));
		else if (total != nullptr)
			detail = QStringLiteral("恢复：总时长 %1ms").arg(QString::fromStdString(*total));
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td>%2</td>"
					"<td class=\"mono\">%3</td><td class=\"mono\">%4</td></tr>\n")
					.arg(escapeHtml(QString::fromStdString(r.time)),
						 escapeHtml(detail.isEmpty() ? QStringLiteral("-") : detail),
						 escapeHtml(fieldValue(r, "receiver").isEmpty()
										? QStringLiteral("-") : fieldValue(r, "receiver")),
						 escapeHtml(fieldValue(r, "type").isEmpty()
										? QStringLiteral("-") : fieldValue(r, "type")));
	}
	if (rows.isEmpty()) {
		return QStringLiteral(
			"<section>\n<h2>冻结时间线</h2>\n"
			"<p class=\"muted\">无冻结告警记录（未触发 FreezeWatch 或监控位关闭）。</p>\n"
			"</section>\n");
	}
	return QStringLiteral(
				"<section>\n<h2>冻结时间线</h2>\n"
				"<table><thead><tr><th>时间</th><th>状态</th>"
				"<th>receiver</th><th>事件类型</th></tr></thead><tbody>\n"
				"%1</tbody></table>\n</section>\n").arg(rows);
}

/// 明细记录（最近 maxCount 条，<details> 折叠；时间倒序）
QString buildDetailsSection(const std::vector<WatchRecordStore::Record>& records, int maxCount)
{
	const int total = static_cast<int>(records.size());
	const int begin = std::max(0, total - maxCount);

	QString rows;
	for (int i = total - 1; i >= begin; --i) {
		const WatchRecordStore::Record& r = records.at(static_cast<std::size_t>(i));
		const auto cell = [&r](const char* key) -> QString {
			const QString v = fieldValue(r, key);
			return v.isEmpty() ? QStringLiteral("-") : v;
		};
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td>"
					"<td><span class=\"badge %2\">%3</span></td>"
					"<td class=\"mono\">%4</td><td>%5</td><td class=\"mono\">%6</td>"
					"<td class=\"mono\">%7</td><td class=\"num\">%8</td>"
					"<td class=\"num\">%9</td></tr>\n")
					.arg(escapeHtml(QString::fromStdString(r.time)))
					.arg(QLatin1String(kindBadgeClass(r.kind)))
					.arg(QLatin1String(kindName(r.kind)))
					.arg(escapeHtml(cell("receiver")))
					.arg(escapeHtml(cell("event")))
					.arg(escapeHtml(cell("sender")))
					.arg(escapeHtml(cell("signal")))
					.arg(escapeHtml(cell("costMs")))
					.arg(escapeHtml(cell("thresholdMs")));
	}

	QString html = QStringLiteral(
		"<details>\n<summary>明细记录（最近 %1 条 / 共 %2 条）</summary>\n"
		"<table><thead><tr><th>时间</th><th>类型</th><th>receiver</th><th>event</th>"
		"<th>sender</th><th>signal</th><th>costMs</th><th>thresholdMs</th>"
		"</tr></thead><tbody>\n").arg(maxCount).arg(total);
	if (rows.isEmpty())
		html += QStringLiteral("<tr><td colspan=\"8\" class=\"muted\">无记录</td></tr>\n");
	html += rows;
	html += QStringLiteral("</tbody></table>\n</details>\n");
	return html;
}

} // namespace

bool HtmlReporter::exportHtml(const QString& filePath, const WatchConfig* config,
							  const EventStatistics* statistics, QString* error)
{
	if (filePath.isEmpty()) {
		if (error)	*error = QStringLiteral("empty file path");
		return false;
	}

	const std::vector<WatchRecordStore::Record> records =
		WatchRecordStore::instance().snapshot();

	// 概览计数（与 DataExporter JSON counts 同口径：QSS 归入 slowEvents 计数）
	int slowEvents = 0;
	int metaCalls = 0;
	int freezeEvents = 0;
	for (const WatchRecordStore::Record& r : records) {
		if (r.kind == WatchRecordStore::KindMetaCall)		++metaCalls;
		else if (r.kind == WatchRecordStore::KindFreeze)	++freezeEvents;
		else												++slowEvents;
	}

	QString configHtml;
	if (config != nullptr) {
		configHtml = QStringLiteral(
			"<tr><td>监控位 watchFun</td><td class=\"mono\">0x%1</td></tr>\n"
			"<tr><td>慢事件阈值</td><td class=\"mono\">%2 ms</td></tr>\n"
			"<tr><td>慢 MetaCall 阈值</td><td class=\"mono\">%3 ms</td></tr>\n"
			"<tr><td>冻结判定阈值</td><td class=\"mono\">%4 ms（心跳 %5 ms）</td></tr>\n")
			.arg(config->watchFun(), 2, 16, QChar('0'))
			.arg(config->slowEventThresholdMs())
			.arg(config->slowMetaCallThresholdMs())
			.arg(config->freezeThresholdMs())
			.arg(config->heartbeatIntervalMs());
	} else {
		configHtml = QStringLiteral(
			"<tr><td colspan=\"2\" class=\"muted\">未提供配置</td></tr>\n");
	}

	QString html = QStringLiteral(
		"<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
		"<title>QtEventWatcher 性能报告</title>\n"
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
		".badge-sev-info{background:#eef0f5;color:#4a5160}\n"
		".badge-sev-warning{background:#fdf0d9;color:#a86807}\n"
		".badge-sev-critical{background:#fdecea;color:#c0392b}\n"
		".finding{background:#fff;border:1px solid #d8dce6;border-radius:8px;"
		"padding:10px 14px;margin:10px 0}\n"
		".finding ul{margin:6px 0 0 18px;padding:0}\n"
		"summary{cursor:pointer;font-weight:600;margin:16px 0 8px}\n"
		"</style>\n</head>\n<body>\n"
		"<h1>QtEventWatcher 性能报告</h1>\n"
		"<p class=\"muted\">导出时间 %1 · Qt 运行时 %2（编译期 %3） · 记录 %4 条</p>\n"
		"\n<section>\n<h2>概览</h2>\n"
		"<table><tbody>\n%5</tbody></table>\n</section>\n")
		.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
			 QString::fromLatin1(qVersion()),
			 QString::fromLatin1(QT_VERSION_STR))
		.arg(static_cast<int>(records.size()))
		.arg(configHtml);

	html += buildFindingsSection(config, statistics);
	html += buildStatsSection(statistics);
	html += buildFreezeSection(records);
	html += buildDetailsSection(records, 200);
	html += QStringLiteral("</body>\n</html>\n");

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open file: %1").arg(file.errorString());
		return false;
	}
	// HTML 严格 UTF-8 无 BOM（与 DataExporter JSON 同规则）
	QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif
	stream.setGenerateByteOrderMark(false);
	stream << html;
	return true;
}

} // namespace qt_event_watcher
