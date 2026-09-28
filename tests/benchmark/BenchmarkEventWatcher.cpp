/*
 * BenchmarkEventWatcher —— 监控自身开销量化（V2 Phase D，PRD 14/20）
 *
 * 量化四条开销线（每条以"同一接收者、基线 vs 监控"同进程对照）：
 *   1. notify 覆写固定开销：Watch_Fun=0 时 override 入口
 *      （thread_local 嵌套管理 + EventGuard 计时 + isEnabled 短路）
 *   2. 开启后正常路径：事件快照（metaObject/objectName）+ 统计 record
 *   3. 告警路径：慢事件完整告警（含 spdlog 文件 sink + 捕获 sink + 记录环形缓冲）
 *   4. 信号发射 spy 回调：MetaCallSenderRegistry 安装与否的每次 emit 增量
 *
 * 模式（argv[1]，配置经 qputenv 在 CusApplication 构造前注入，分进程运行）：
 *   off          Watch_Fun=0     基线 vs 覆写（普通事件）
 *   event        WATCH_FUN=5     基线 vs 监控（普通事件，低于阈值，正常路径）
 *                                + record() 微基准
 *   slow         WATCH_FUN=5     普通事件（低于阈值）对照 +
 *                阈值=1ms        忙等事件（1.2ms，告警路径，含日志落盘）对照
 *   metacall_on  WATCH_FUN=2     直连信号发射（spy 回调已安装）
 *   metacall_off Watch_Fun=0     直连信号发射（无 spy 回调，基线）
 *
 * 输出：每场景一行 `key=value` 文本（ns/op），人工汇总至 docs/22_性能基准报告.md。
 * 注意：不进 CTest（耗时且数值受负载影响），仅手动运行。
 */

#include "CusApplication.h"
#include "EventStatistics.h"
#include "WatchLogger.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QObject>
#include <QtGlobal>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

/*
 * 忙等接收者：event() 自旋 spinNs 纳秒，模拟一个真实超过阈值的慢事件处理。
 * volatile 写防止编译器把自旋循环优化掉。
 */
class BusyReceiver : public QObject
{
public:
	using QObject::QObject;
	std::int64_t spinNs = 1200000;	// 1.2ms > 1ms 告警阈值

protected:
	bool event(QEvent *e) override
	{
		if (e->type() == QEvent::User) {
			QElapsedTimer timer;
			timer.start();
			while (timer.nsecsElapsed() < spinNs) {
				volatile int sink = 0;
				(void)sink;
			}
			return true;
		}
		return QObject::event(e);
	}
};

class Emitter : public QObject
{
	Q_OBJECT
signals:
	void ping();
};

class SlotHost : public QObject
{
	Q_OBJECT
public slots:
	void onPing() {}
};

struct BenchResult
{
	double minNsPerOp = 0.0;
	double avgNsPerOp = 0.0;
};

/** iters 次操作为一轮，测 runs 轮（先预热 1 轮），报告 ns/op 的 min 与 avg */
template <typename Fn>
BenchResult bench(int iters, int runs, Fn &&fn)
{
	fn();	// 预热（代码路径 / 分支预测 / 文件句柄）

	BenchResult result;
	double totalNs = 0.0;
	for (int run = 0; run < runs; ++run) {
		QElapsedTimer timer;
		timer.start();
		for (int i = 0; i < iters; ++i)
			fn();
		const double nsPerOp = static_cast<double>(timer.nsecsElapsed()) / iters;
		result.minNsPerOp = run == 0 ? nsPerOp : std::min(result.minNsPerOp, nsPerOp);
		totalNs += nsPerOp;
	}
	result.avgNsPerOp = totalNs / runs;
	return result;
}

void printPair(const char *scenario, const BenchResult &base, const BenchResult &monitored,
               int iters, int runs)
{
	const double delta = monitored.avgNsPerOp - base.avgNsPerOp;
	std::fprintf(stdout,
	             "scenario=%s base_avg=%.1f base_min=%.1f mon_avg=%.1f mon_min=%.1f "
	             "delta_avg=%.1f ns/op (iters=%d runs=%d)\n",
	             scenario, base.avgNsPerOp, base.minNsPerOp,
	             monitored.avgNsPerOp, monitored.minNsPerOp, delta, iters, runs);
	std::fflush(stdout);
}

/** 对照测一轮"基线（绕过监控覆写）vs 监控覆写"的 notify 直调 */
template <typename ReceiverT>
void benchNotifyPair(const char *scenario, QCoreApplication &app, ReceiverT *receiver,
                     QEvent &event, int iters, int runs)
{
	const BenchResult base = bench(iters, runs, [&]() {
		// 限定调用基类实现：等价于未安装监控器时的 Qt 原生分发
		app.QCoreApplication::notify(receiver, &event);
	});
	const BenchResult monitored = bench(iters, runs, [&]() {
		app.notify(receiver, &event);
	});
	printPair(scenario, base, monitored, iters, runs);
}

} // namespace

int main(int argc, char *argv[])
{
	const std::string mode = argc > 1 ? argv[1] : "off";

	// 配置注入必须发生在 CusApplication 构造（load()）之前
	if (mode == "event") {
		qputenv("QT_EVENT_WATCHER_WATCH_FUN", "5");	// Event | EventStatistics
	} else if (mode == "slow") {
		qputenv("QT_EVENT_WATCHER_WATCH_FUN", "5");
		qputenv("QT_EVENT_WATCHER_SLOW_EVENT_THRESHOLD_MS", "1");
	} else if (mode == "metacall_on") {
		qputenv("QT_EVENT_WATCHER_WATCH_FUN", "2");	// MetaCall（构造时装 spy 回调）
	}
	// off / metacall_off：不设环境变量，Watch_Fun=0 默认全关

	qt_event_watcher::CusApplication app(argc, argv);
	QCoreApplication::setApplicationName("QtEventWatcherBenchmark");

	// 与 Demo 一致的日志初始化：告警路径含真实文件 sink + flush_on(warn)
	qt_event_watcher::WatchLogger::instance().initialize("./logs", "QtEventWatcher");

	if (mode == "off" || mode == "event" || mode == "slow") {
		// ---- 场景 1/2：普通事件（极快处理）----
		QObject plainReceiver;
		QEvent userEvent(QEvent::User);
		benchNotifyPair("plain-user-event", app, &plainReceiver, userEvent,
		                mode == "off" ? 50000 : 50000, 7);

		if (mode == "event") {
			// ---- record() 微基准：单次统计入表成本（含互斥锁，不含事件快照）----
			const QString eventName = QStringLiteral("Timer");
			auto *statistics = app.eventStatistics();
			if (statistics != nullptr) {
				const BenchResult r = bench(200000, 7, [&]() {
					statistics->record(QEvent::Timer, eventName, 1500000, 1500000);
				});
				std::fprintf(stdout,
				             "scenario=statistics-record avg=%.1f min=%.1f ns/op "
				             "(iters=200000 runs=7)\n",
				             r.avgNsPerOp, r.minNsPerOp);
				std::fflush(stdout);
			}
		}

		if (mode == "slow") {
			// ---- 场景 3：告警路径（忙等 1.2ms 事件，每次都触发慢事件告警落盘）----
			BusyReceiver busyReceiver;
			QEvent busyEvent(QEvent::User);
			benchNotifyPair("busy-user-event-alert", app, &busyReceiver, busyEvent, 300, 3);
		}
	} else if (mode == "metacall_on" || mode == "metacall_off") {
		// ---- 场景 4：直连信号发射（隔离 spy 回调增量，不经过事件队列）----
		Emitter emitter;
		SlotHost slotHost;
		QObject::connect(&emitter, &Emitter::ping, &slotHost, &SlotHost::onPing);

		const BenchResult r = bench(200000, 7, [&emitter]() {
			emit emitter.ping();
		});
		std::fprintf(stdout,
		             "scenario=direct-emit mode=%s avg=%.1f min=%.1f ns/emit "
		             "(iters=200000 runs=7)\n",
		             mode.c_str(), r.avgNsPerOp, r.minNsPerOp);
		std::fflush(stdout);
	} else {
		std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
		return 2;
	}

	qt_event_watcher::WatchLogger::instance().shutdown();
	return 0;
}

#include "BenchmarkEventWatcher.moc"
