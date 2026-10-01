// scout-dashboard - Scout 独立可视化仪表盘（V7 S3 线，console 冷路径工具）
//
// 读取 aggregator --out 导出的 JSON 快照文件，输出单文件自包含 HTML
// （零外部依赖零 JS 依赖，双击即开断网可用）。纯文件消费方：数据契约 =
// aggregator JSON 形状，不链接 QtEventWatcherCore。
//
// 用法：
//   scout-dashboard --in <agg.json> [--out <dash.html>] [--title <text>]
//                   [--pid <n>] [--kind <csv>]   # kind ∈ 0,1,2,3
//
// kind：0=SlowEvent 1=MetaCall 2=Qss 3=Freeze（scout cpuSpin/cdpLongTask
// 行 kind=0，由数据层 EventClass 归类进专属章节）
//
// 退出码：0 成功 / 2 用法错误 / 1 运行失败（文件不可读/写出错）

#include "DashboardHtml.h"
#include "DashboardModel.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>

#include <cstdio>

using namespace qt_event_watcher::dashboard;

namespace {

void printUsage()
{
	std::printf(
		"scout-dashboard - Scout standalone visual dashboard (QtEventWatcher V7)\n"
		"usage: scout-dashboard --in <agg.json>\n"
		"          [--out <dash.html>=<in>.html] [--title <text>]\n"
		"          [--pid <n>] [--kind <csv>]   # kind 0=SlowEvent 1=MetaCall 2=Qss 3=Freeze\n"
		"examples:\n"
		"  scout-dashboard --in agg.json\n"
		"  scout-dashboard --in agg.json --pid 1234 --kind 3\n");
}

/// 摘要统计（与 filterModel 同口径：pid/kinds 过滤后的视图数字，
/// 保证 stdout 摘要与产出 HTML 内容一致）
void countFiltered(const DashboardModel &m, const QVector<int> &kinds,
				   qint64 pidFilter, int *sessions, int *records,
				   int *freezeSpans)
{
	*sessions = 0;
	*records = 0;
	*freezeSpans = 0;
	for (const SessionModel &s : m.sessions) {
		if (pidFilter > 0 && s.pid != pidFilter)
			continue;
		++*sessions;
		for (const EventRecord &ev : s.events) {
			if (!kinds.isEmpty() && !kinds.contains(ev.kind))
				continue;
			++*records;
		}
		if (kinds.isEmpty() || kinds.contains(3))
			*freezeSpans += static_cast<int>(s.freezeSpans.size());
	}
}

} // namespace

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName("QtEventWatcherScoutDashboard");

	QString inPath;
	QString outPath;
	HtmlOptions options;
	for (int i = 1; i < argc; ++i) {
		const QByteArray arg(argv[i]);
		const auto next = [&]() -> QByteArray {
			return i + 1 < argc ? QByteArray(argv[i + 1]) : QByteArray();
		};
		bool consumed = false;
		if (arg == "--in" && !next().isEmpty()) {
			inPath = QString::fromLocal8Bit(next());		++i; consumed = true;
		} else if (arg == "--out" && !next().isEmpty()) {
			outPath = QString::fromLocal8Bit(next());		++i; consumed = true;
		} else if (arg == "--title" && !next().isEmpty()) {
			options.title = QString::fromLocal8Bit(next());	++i; consumed = true;
		} else if (arg == "--pid" && !next().isEmpty()) {
			options.pidFilter = next().toLongLong();		++i; consumed = true;
		} else if (arg == "--kind" && !next().isEmpty()) {
			const QStringList parts =
				QString::fromLocal8Bit(next()).split(QLatin1Char(','));
			for (const QString &p : parts) {
				bool ok = false;
				const int k = p.trimmed().toInt(&ok);
				if (!ok || k < 0 || k > 3) {
					std::printf("invalid --kind value: %s\n",
								p.trimmed().toStdString().c_str());
					return 2;
				}
				if (!options.kinds.contains(k))
					options.kinds.append(k);
			}
			++i; consumed = true;
		}
		if (!consumed) {
			std::printf("unknown or incomplete argument: %s\n", arg.constData());
			printUsage();
			return 2;
		}
	}
	if (inPath.isEmpty()) {
		printUsage();
		return 2;
	}
	if (outPath.isEmpty())
		outPath = QFileInfo(inPath).completeBaseName() + QStringLiteral(".html");

	DashboardModel model;
	QString error;
	if (!loadModel(inPath, model, &error)) {
		std::fprintf(stderr, "[FAIL] scout-dashboard: %s\n",
					 error.toStdString().c_str());
		return 1;
	}

	int sessions = 0;
	int records = 0;
	int freezeSpans = 0;
	countFiltered(model, options.kinds, options.pidFilter,
				  &sessions, &records, &freezeSpans);

	if (!exportDashboard(model, outPath, options, &error)) {
		std::fprintf(stderr, "[FAIL] scout-dashboard: %s\n",
					 error.toStdString().c_str());
		return 1;
	}

	// 摘要补 cdp/cpu 计数（过滤口径：kind 过滤时 cdp/cpu 行 kind=0，仅当
	// kinds 含 0（或全量）才计入；pid 过滤由会话过滤自然生效）
	int cdp = 0;
	int cpu = 0;
	const bool kind0Visible =
		options.kinds.isEmpty() || options.kinds.contains(0);
	if (kind0Visible) {
		for (const SessionModel &s : model.sessions) {
			if (options.pidFilter > 0 && s.pid != options.pidFilter)
				continue;
			for (const EventRecord &ev : s.events) {
				if (ev.cls == EventClass::CdpLongTask)
					++cdp;
				else if (ev.cls == EventClass::CpuSpin)
					++cpu;
			}
		}
	}
	std::printf("[DASH] sessions=%d records=%d freeze_spans=%d cdp=%d cpu=%d -> %s\n",
				sessions, records, freezeSpans, cdp, cpu,
				outPath.toStdString().c_str());
	return 0;
}
