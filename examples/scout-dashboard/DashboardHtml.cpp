#include "DashboardHtml.h"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace qt_event_watcher {
namespace dashboard {

namespace {

/// EventClass → 展示名 / 徽章样式类
const char *clsName(EventClass cls)
{
	switch (cls) {
	case EventClass::Freeze:		return "Freeze";
	case EventClass::CdpLongTask:	return "CdpLongTask";
	case EventClass::CpuSpin:		return "CpuSpin";
	case EventClass::SlowEvent:		return "SlowEvent";
	case EventClass::MetaCall:		return "MetaCall";
	case EventClass::Qss:			return "Qss";
	default:						return "Other";
	}
}

const char *clsBadge(EventClass cls)
{
	switch (cls) {
	case EventClass::Freeze:		return "badge-freeze";
	case EventClass::CdpLongTask:	return "badge-cdp";
	case EventClass::CpuSpin:		return "badge-cpu";
	case EventClass::SlowEvent:		return "badge-slowEvent";
	case EventClass::MetaCall:		return "badge-metaCall";
	case EventClass::Qss:			return "badge-qss";
	default:						return "badge-other";
	}
}

/// freeze span 终态 → 展示名 / 徽章样式类
const char *spanStateName(const QString &s)
{
	if (s == QLatin1String("recovered"))	return "已恢复";
	if (s == QLatin1String("lost"))			return "目标丢失";
	return "进行中";
}

const char *spanStateBadge(const QString &s)
{
	if (s == QLatin1String("recovered"))	return "badge-span-ok";
	if (s == QLatin1String("lost"))			return "badge-span-lost";
	return "badge-span-ongoing";
}

/// 带所属 pid 的事件引用（跨会话合并遍历的公共载体）
struct PidEvent
{
	qint64 pid = 0;
	const EventRecord *ev = nullptr;
};

std::vector<PidEvent> collectAll(const DashboardModel &m)
{
	std::vector<PidEvent> all;
	for (const SessionModel &s : m.sessions) {
		for (const EventRecord &ev : s.events)
			all.push_back({ s.pid, &ev });
	}
	return all;
}

/// 会话目标 receiver 推断：freeze/cpuSpin 行 receiver 频次 top（scout
/// 会话语义 = 被观测进程名）；自监控会话无此语义，返回空（展示 "-"）
QString inferTargetReceiver(const SessionModel &s)
{
	QHash<QString, int> freq;
	for (const EventRecord &ev : s.events) {
		if ((ev.cls == EventClass::Freeze || ev.cls == EventClass::CpuSpin)
			&& !ev.receiver.isEmpty())
			++freq[ev.receiver];
	}
	QString top;
	int best = 0;
	for (auto it = freq.constBegin(); it != freq.constEnd(); ++it) {
		if (it.value() > best) {
			best = it.value();
			top = it.key();
		}
	}
	return top;
}

/// 章节①：会话概览与健康度（per-pid 表）
QString buildOverview(const DashboardModel &m)
{
	QString rows;
	for (const SessionModel &s : m.sessions) {
		const QString target = inferTargetReceiver(s);
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
				"<td colspan=\"4\" class=\"muted\">未上行（health 随拍 5s 节拍）</td>");
		}
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td>"
					"<td><span class=\"badge %2\">%3</span></td>"
					"<td class=\"mono\">%4</td><td class=\"num\">%5</td>"
					"<td class=\"num\">%6</td><td class=\"num\">%7</td>%8</tr>\n")
					.arg(s.pid)
					.arg(s.isScoutSession ? QLatin1String("badge-cpu")
										  : QLatin1String("badge-metaCall"))
					.arg(s.isScoutSession ? QStringLiteral("外部探针")
										  : QStringLiteral("自监控"))
					.arg(escapeHtml(target.isEmpty() ? QStringLiteral("-") : target))
					.arg(static_cast<int>(s.events.size()))
					.arg(QString::number(static_cast<qulonglong>(s.received)))
					.arg(QString::number(static_cast<qulonglong>(s.dropped)))
					.arg(healthCells);
	}
	return QStringLiteral(
		"<section>\n<h2>会话概览与健康度</h2>\n"
		"<table><thead><tr><th>host_pid</th><th>来源</th><th>目标 receiver（推断）</th>"
		"<th>记录现存</th><th>received</th><th>dropped</th>"
		"<th>pushed</th><th>health dropped</th><th>reconnects</th><th>lastSeq</th>"
		"</tr></thead><tbody>\n%1</tbody></table>\n"
		"<p class=\"muted\">目标 receiver 推断口径：会话内 freeze/cpuSpin 行 "
		"receiver 频次 top（scout 会话 = 被观测进程名）；自监控会话不适用。</p>\n"
		"</section>\n")
		.arg(rows.isEmpty()
				  ? QStringLiteral("<tr><td colspan=\"10\" class=\"muted\">无会话"
								  "（过滤条件过严或快照为空）</td></tr>\n")
				  : rows);
}

/// 章节②：冻结时间轴（静态表格；S3c 起同位置为 JS 泳道，此表保留为无 JS 降级）
QString buildFreezeSection(const DashboardModel &m)
{
	QString rows;
	for (const SessionModel &s : m.sessions) {
		for (const FreezeSpan &sp : s.freezeSpans) {
			rows += QStringLiteral(
						"<tr><td class=\"mono\">%1</td><td class=\"mono\">%2</td>"
						"<td class=\"mono\">%3</td><td class=\"mono\">%4</td>"
						"<td class=\"num\">%5</td>"
						"<td><span class=\"badge %6\">%7</span></td></tr>\n")
						.arg(escapeHtml(sp.receiver))
						.arg(s.pid)
						.arg(escapeHtml(sp.startTime))
						.arg(sp.startRelMs)
						.arg(sp.durationMs < 0
								 ? QStringLiteral("未闭合")
								 : QString::number(sp.durationMs))
						.arg(QLatin1String(spanStateBadge(sp.endState)))
						.arg(QLatin1String(spanStateName(sp.endState)));
		}
	}
	if (rows.isEmpty()) {
		return QStringLiteral(
			"<section>\n<h2>冻结时间轴</h2>\n"
			"<p class=\"muted\">无冻结时段（未触发 FreezeWatch / 监控位关闭 / "
			"被 kind 过滤）。</p>\n</section>\n");
	}
	return QStringLiteral(
		"<section>\n<h2>冻结时间轴</h2>\n"
		"<table><thead><tr><th>receiver</th><th>host_pid</th><th>开始时间</th>"
		"<th>relMs</th><th>时长 ms</th><th>终态</th></tr></thead><tbody>\n"
		"%1</tbody></table>\n"
		"<p class=\"muted\">口径：started→recovered/lost 配对（会话内 JSON 顺序"
		"即时间序）；进行中 = 导出时刻仍在冻结；目标丢失 = 会话结束前未观测到恢复。</p>\n"
		"</section>\n")
		.arg(rows);
}

/// 章节③：CDP 长任务（per-url 汇总 + CSS 条形直方零 JS + 明细折叠）
QString buildCdpSection(const DashboardModel &m)
{
	const std::vector<PidEvent> all = collectAll(m);

	struct UrlAgg
	{
		int count = 0;
		double totalCostMs = 0.0;
		double maxCostMs = 0.0;
	};
	QHash<QString, UrlAgg> byUrl;
	QString body;
	for (const PidEvent &pe : all) {
		if (pe.ev->cls != EventClass::CdpLongTask)
			continue;
		const QString url = pe.ev->url.isEmpty()
								? QStringLiteral("(unknown)")
								: pe.ev->url;
		UrlAgg &a = byUrl[url];
		++a.count;
		a.totalCostMs += pe.ev->costMs;
		a.maxCostMs = std::max(a.maxCostMs, pe.ev->costMs);
		body += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"mono\">%2</td>"
					"<td class=\"mono\">%3</td><td class=\"num\">%4</td>"
					"<td class=\"num\">%5</td></tr>\n")
					.arg(escapeHtml(pe.ev->time))
					.arg(pe.pid)
					.arg(escapeHtml(url))
					.arg(pe.ev->costMs, 0, 'f', 1)
					.arg(pe.ev->thresholdMs, 0, 'f', 0);
	}
	if (body.isEmpty()) {
		return QStringLiteral(
			"<section>\n<h2>CDP 长任务</h2>\n"
			"<p class=\"muted\">无 CDP 长任务记录（未挂 --cdp-port / 目标非 "
			"Chromium 系 / 被 kind 过滤）。</p>\n</section>\n");
	}

	// per-url 汇总 + 条形直方（宽度按 count/maxCount 归一，纯 CSS 零 JS）
	int maxCount = 1;
	for (auto it = byUrl.constBegin(); it != byUrl.constEnd(); ++it)
		maxCount = std::max(maxCount, it.value().count);
	QString summary;
	for (auto it = byUrl.constBegin(); it != byUrl.constEnd(); ++it) {
		const int width = it.value().count * 100 / maxCount;
		summary += QStringLiteral(
					   "<tr><td class=\"mono\">%1</td><td class=\"num\">%2</td>"
					   "<td class=\"num\">%3</td><td class=\"num\">%4</td>"
					   "<td><div class=\"bar\"><div class=\"bar-fill\" "
					   "style=\"width:%5%\"></div></div></td></tr>\n")
					   .arg(escapeHtml(it.key()))
					   .arg(it.value().count)
					   .arg(it.value().totalCostMs, 0, 'f', 1)
					   .arg(it.value().maxCostMs, 0, 'f', 1)
					   .arg(width);
	}
	return QStringLiteral(
		"<section>\n<h2>CDP 长任务（scout T2 探针）</h2>\n"
		"<table><thead><tr><th>页面 url</th><th>次数</th><th>累计 costMs</th>"
		"<th>最大 costMs</th><th>分布</th></tr></thead><tbody>\n%1</tbody></table>\n"
		"<details>\n<summary>明细（%2 条）</summary>\n"
		"<table><thead><tr><th>时间</th><th>host_pid</th><th>url</th>"
		"<th>costMs</th><th>thresholdMs</th></tr></thead><tbody>\n%3"
		"</tbody></table>\n</details>\n</section>\n")
		.arg(summary)
		.arg(static_cast<int>(byUrl.isEmpty() ? 0 : [&byUrl] {
				 int n = 0;
				 for (auto it = byUrl.constBegin(); it != byUrl.constEnd(); ++it)
					 n += it.value().count;
				 return n;
			 }()))
		.arg(body);
}

/// 章节④：CPU 与慢事件（cpuSpin 表 + TOP receiver 对齐既有口径）
QString buildCpuSlowSection(const DashboardModel &m)
{
	const std::vector<PidEvent> all = collectAll(m);

	QString cpuRows;
	for (const PidEvent &pe : all) {
		if (pe.ev->cls != EventClass::CpuSpin)
			continue;
		// 口径标注：cpuSpin 行复用 slow event 行格式，costMs 槽位实填
		// CPU 占用百分比（CpuSampler 单核口径），thresholdMs 实填阈值百分比
		cpuRows += QStringLiteral(
					   "<tr><td class=\"mono\">%1</td><td class=\"mono\">%2</td>"
					   "<td class=\"mono\">%3</td><td class=\"num\">%4</td>"
					   "<td class=\"num\">%5</td></tr>\n")
					   .arg(escapeHtml(pe.ev->time))
					   .arg(pe.pid)
					   .arg(escapeHtml(pe.ev->receiver))
					   .arg(pe.ev->costMs, 0, 'f', 1)
					   .arg(pe.ev->thresholdMs, 0, 'f', 0);
	}

	// TOP receiver：仅 SlowEvent 类（scout 事件有专属章节，不混入既有口径）
	struct Agg
	{
		int count = 0;
		double totalCostMs = 0.0;
		double maxCostMs = 0.0;
	};
	QHash<QString, Agg> merged;
	for (const PidEvent &pe : all) {
		if (pe.ev->cls != EventClass::SlowEvent || pe.ev->receiver.isEmpty())
			continue;
		Agg &a = merged[pe.ev->receiver];
		++a.count;
		a.totalCostMs += pe.ev->costMs;
		a.maxCostMs = std::max(a.maxCostMs, pe.ev->costMs);
	}
	std::vector<std::pair<QString, Agg>> rows;
	for (auto it = merged.constBegin(); it != merged.constEnd(); ++it)
		rows.emplace_back(it.key(), it.value());
	std::sort(rows.begin(), rows.end(),
			  [](const std::pair<QString, Agg> &a,
				 const std::pair<QString, Agg> &b) {
				  return a.second.totalCostMs > b.second.totalCostMs;
			  });
	QString topRows;
	const int topN = std::min<int>(static_cast<int>(rows.size()), 10);
	for (int i = 0; i < topN; ++i) {
		topRows += QStringLiteral(
					   "<tr><td class=\"mono\">%1</td><td class=\"num\">%2</td>"
					   "<td class=\"num\">%3</td><td class=\"num\">%4</td></tr>\n")
					   .arg(escapeHtml(rows.at(static_cast<std::size_t>(i)).first))
					   .arg(rows.at(static_cast<std::size_t>(i)).second.count)
					   .arg(rows.at(static_cast<std::size_t>(i)).second.totalCostMs,
							0, 'f', 1)
					   .arg(rows.at(static_cast<std::size_t>(i)).second.maxCostMs,
							0, 'f', 1);
	}

	return QStringLiteral(
		"<section>\n<h2>CPU 与慢事件</h2>\n"
		"<h3>cpuSpin（scout T1b 单核满转启发）</h3>\n"
		"<table><thead><tr><th>时间</th><th>host_pid</th><th>目标</th>"
		"<th>cpu%</th><th>阈值%</th></tr></thead><tbody>\n%1</tbody></table>\n"
		"<p class=\"muted\">口径：cpuSpin 行复用 slow event 行格式，costMs 槽位"
		"实填 CPU 占用百分比（单核口径，含直接子进程）。</p>\n"
		"<h3>TOP 接收者（SlowEvent，按累计 costMs）</h3>\n"
		"<table><thead><tr><th>receiver</th><th>次数</th><th>累计 costMs</th>"
		"<th>最大 costMs</th></tr></thead><tbody>\n%2</tbody></table>\n"
		"</section>\n")
		.arg(cpuRows.isEmpty()
				  ? QStringLiteral("<tr><td colspan=\"5\" class=\"muted\">无 "
								  "cpuSpin 记录</td></tr>\n")
				  : cpuRows)
		.arg(topRows.isEmpty()
				  ? QStringLiteral("<tr><td colspan=\"4\" class=\"muted\">无 "
								  "SlowEvent 记录</td></tr>\n")
				  : topRows);
}

/// 章节⑤：原始明细（跨会话合并倒序，<details> 折叠，raw 悬停兜底）
QString buildDetailsSection(const DashboardModel &m)
{
	std::vector<PidEvent> all = collectAll(m);
	std::stable_sort(all.begin(), all.end(),
					 [](const PidEvent &a, const PidEvent &b) {
						 return a.ev->time > b.ev->time;
					 });

	constexpr int kMaxCount = 200;
	const int total = static_cast<int>(all.size());
	const int begin = std::max(0, total - kMaxCount);

	QString rows;
	for (int i = total - 1; i >= begin; --i) {
		const EventRecord &ev = *all.at(static_cast<std::size_t>(i)).ev;
		const QString rawTitle = escapeHtml(ev.raw);
		rows += QStringLiteral(
					"<tr><td class=\"mono\">%1</td><td class=\"mono\">%2</td>"
					"<td><span class=\"badge %3\">%4</span></td>"
					"<td class=\"mono\">%5</td><td>%6</td><td>%7</td>"
					"<td class=\"num\">%8</td><td class=\"num\">%9</td>"
					"<td class=\"mono\" title=\"%10\">%10</td></tr>\n")
					.arg(escapeHtml(ev.time))
					.arg(all.at(static_cast<std::size_t>(i)).pid)
					.arg(QLatin1String(clsBadge(ev.cls)))
					.arg(QLatin1String(clsName(ev.cls)))
					.arg(escapeHtml(ev.receiver.isEmpty()
										? QStringLiteral("-") : ev.receiver))
					.arg(escapeHtml(ev.event.isEmpty()
										? QStringLiteral("-") : ev.event))
					.arg(escapeHtml(ev.source.isEmpty()
										? QStringLiteral("-") : ev.source))
					.arg(ev.costMs, 0, 'f', 1)
					.arg(ev.thresholdMs, 0, 'f', 0)
					.arg(rawTitle.left(80));
	}

	return QStringLiteral(
		"<details>\n<summary>原始明细（最近 %1 条 / 共 %2 条，跨会话合并）</summary>\n"
		"<table><thead><tr><th>时间</th><th>host_pid</th><th>类型</th>"
		"<th>receiver</th><th>event</th><th>source</th><th>costMs</th>"
		"<th>thresholdMs</th><th>raw（悬停看全文）</th></tr></thead><tbody>\n%3"
		"</tbody></table>\n</details>\n")
		.arg(std::min(total, kMaxCount))
		.arg(total)
		.arg(rows.isEmpty()
				  ? QStringLiteral("<tr><td colspan=\"9\" class=\"muted\">无记录"
								  "</td></tr>\n")
				  : rows);
}

/// S3c 内嵌 JSON（dashboard.js 消费）：过滤后模型 + 字段名压缩（1-4 字符）+
/// 事件压平成数组位（索引约定见 dashboard.js E/S 常量）。embedJsonSafe 封死
/// </script> 注入面——JSON 位不做 escapeHtml（三层分工：DOM 位由 JS esc 负责）。
QString buildEmbedJson(const DashboardModel &m)
{
	QJsonObject root;
	root.insert(QStringLiteral("g"), m.generatedAt);
	root.insert(QStringLiteral("sv"), m.server);
	QJsonArray ss;
	for (const SessionModel &s : m.sessions) {
		QJsonObject so;
		so.insert(QStringLiteral("p"), s.pid);
		so.insert(QStringLiteral("sc"), s.isScoutSession);
		so.insert(QStringLiteral("t0"), s.t0Ms);
		if (s.hasHealth)
			so.insert(QStringLiteral("h"), s.health);
		QJsonArray evs;
		for (const EventRecord &ev : s.events) {
			QJsonArray e;
			e.append(static_cast<int>(ev.cls));
			e.append(ev.relMs);
			e.append(ev.costMs);
			e.append(ev.thresholdMs);
			e.append(ev.state);
			e.append(ev.receiver);
			e.append(ev.event);
			e.append(ev.url);
			e.append(ev.source);
			e.append(ev.time);
			evs.append(e);
		}
		so.insert(QStringLiteral("ev"), evs);
		QJsonArray sps;
		for (const FreezeSpan &sp : s.freezeSpans) {
			QJsonArray spj;
			spj.append(sp.receiver);
			spj.append(sp.startRelMs);
			spj.append(sp.durationMs);
			spj.append(sp.endState);
			sps.append(spj);
		}
		so.insert(QStringLiteral("sp"), sps);
		ss.append(so);
	}
	root.insert(QStringLiteral("ss"), ss);
	return embedJsonSafe(QJsonDocument(root));
}

/// S3c 交互区 HTML 壳（静态章节保留为无 JS 降级；JS 未注入时这两个
/// section 为空壳，观感无破坏——filters/canvas/pagebody 均为空内容）
QString buildInteractiveShell()
{
	return QStringLiteral(
		"<section id=\"dash-interactive\">\n"
		"<h2>冻结时间轴（交互）</h2>\n"
		"<div id=\"dash-filters\"></div>\n"
		"<div id=\"ft-view\"><div id=\"ft-canvas\"></div></div>\n"
		"<div id=\"ft-tip\" hidden></div>\n"
		"<p class=\"muted\">滚轮缩放（0.2x–50x，以光标为中心）· 拖拽平移 · "
		"双击复位 · 悬停查看时段详情</p>\n"
		"</section>\n"
		"<section id=\"dash-details-ix\">\n"
		"<h2>事件明细（交互分页）</h2>\n"
		"<table><thead><tr><th>时间</th><th>host_pid</th><th>类型</th>"
		"<th>receiver</th><th>event</th><th>source</th><th>costMs</th>"
		"<th>url</th></tr></thead><tbody id=\"dash-pagebody\"></tbody></table>\n"
		"<div id=\"dash-pagebar\"></div>\n"
		"</section>\n");
}

} // namespace

bool exportDashboard(const DashboardModel &model, const QString &outPath,
					 const HtmlOptions &options, QString *error)
{
	if (outPath.isEmpty()) {
		if (error)
			*error = QStringLiteral("empty output path");
		return false;
	}

	const DashboardModel m = filterModel(model, options.kinds, options.pidFilter);

	QString html = QStringLiteral(
		"<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
		"<title>%1</title>\n"
		"<style>\n"
		"body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;margin:24px;"
		"color:#1f2430;background:#f7f8fa;line-height:1.5}\n"
		"h1{font-size:22px;margin-bottom:4px}\n"
		"h2{font-size:16px;border-bottom:1px solid #d8dce6;padding-bottom:6px;margin-top:28px}\n"
		"h3{font-size:14px;margin:18px 0 8px}\n"
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
		".badge-cdp{background:#e2f7ef;color:#1c8a5f}\n"
		".badge-cpu{background:#fdf1dc;color:#b06f14}\n"
		".badge-other{background:#eceef2;color:#5a6072}\n"
		".badge-span-ok{background:#e2f7ef;color:#1c8a5f}\n"
		".badge-span-lost{background:#eceef2;color:#5a6072}\n"
		".badge-span-ongoing{background:#fdecea;color:#b3402a}\n"
		".bar{background:#eef0f5;height:14px;border-radius:3px;overflow:hidden;"
		"min-width:120px}\n"
		".bar-fill{background:#4a90d9;height:100%}\n"
		"summary{cursor:pointer;font-weight:600;margin:16px 0 8px}\n"
		"</style>\n</head>\n<body>\n"
		"<h1>%1</h1>\n"
		"<p class=\"muted\">快照生成 %2 · 中心收集器 %3 · 会话 %4 个 · 记录 %5 条"
		"（冻结时段 %6 / CDP 长任务 %7 / cpuSpin %8）</p>\n")
		.arg(escapeHtml(options.title),
			 escapeHtml(m.generatedAt.isEmpty()
							? QDateTime::currentDateTime().toString(
								  QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
							: m.generatedAt),
			 escapeHtml(m.server.isEmpty()
							? QStringLiteral("-") : m.server))
		.arg(static_cast<int>(m.sessions.size()))
		.arg(m.totalRecords)
		.arg([](const DashboardModel &mm) {
			int n = 0;
			for (const SessionModel &s : mm.sessions)
				n += static_cast<int>(s.freezeSpans.size());
			return n;
		}(m))
		.arg(m.cdpCount)
		.arg(m.cpuCount);

	html += buildOverview(m);
	html += buildFreezeSection(m);
	html += buildCdpSection(m);
	html += buildCpuSlowSection(m);
#ifdef QEWT_DASHBOARD_EMBED
	QFile cssRes(QStringLiteral(":/dashboard/style.css"));
	QFile jsRes(QStringLiteral(":/dashboard/dashboard.js"));
	if (cssRes.open(QIODevice::ReadOnly) && jsRes.open(QIODevice::ReadOnly)) {
		// 交互区壳（位于 CPU/慢事件章节与静态明细之间；JS 未运行时为空壳）
		html += buildInteractiveShell();
		html += QStringLiteral("<style>\n%1</style>\n")
					.arg(QString::fromUtf8(cssRes.readAll()));
		// 内嵌 JSON：script[type=application/json] 不被执行，仅作 JS 数据源
		html += QStringLiteral(
					"<script type=\"application/json\" id=\"dashboard-data\">"
					"%1</script>\n").arg(buildEmbedJson(m));
		html += QStringLiteral("<script>\n%1</script>\n")
					.arg(QString::fromUtf8(jsRes.readAll()));
	} else {
		std::fprintf(stderr,
					 "[WARN] scout-dashboard: embedded resources missing, "
					 "falling back to static-only HTML\n");
	}
#endif
	html += buildDetailsSection(m);
	html += QStringLiteral(
		"<p class=\"muted\">QtEventWatcher scout-dashboard（V7 S3）· "
		"单文件自包含 HTML，断网可用</p>\n"
		"</body>\n</html>\n");

	QFile file(outPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)
			*error = QStringLiteral("cannot open file: %1").arg(file.errorString());
		return false;
	}
	// HTML 严格 UTF-8 无 BOM（与 AggregationReporter/DataExporter 同规则；
	// Qt5 QTextStream 默认 locale 编码，必须显式指定，Qt6 默认已 UTF-8）
	QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif
	stream.setGenerateByteOrderMark(false);
	stream << html;
	return true;
}

} // namespace dashboard
} // namespace qt_event_watcher
