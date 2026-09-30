// scout - 跨栈程序卡顿外部探针（产品线 B / V7 S1）
// 进程外检测任意 Windows 桌面程序（Qt/Electron/WPF/Win32）的卡顿：
//   T1  窗口冻结——SendMessageTimeout 轮询消息泵停摆（未响应），三态告警
//       行对齐 EventWatchdog 冻结语义，aggregator/HTML 冻结时间线零改动渲染
//   T1b CPU 启发——目标进程+子进程单核满转检测（Electron renderer 即子进程）
// 观测记录走与自监控完全相同的日志行协议：
//   WatchLogger → RecordSink → WatchRecordStore → UplinkClient → aggregator
// 用法：
//   scout --pid <pid> | --name <proc.exe> [--threshold ms] [--interval ms]
//         [--cpu-threshold pct] [--cpu-runs n] [--uplink <server>]
//         [--flush-ms ms] [--duration ms]

#include "CpuSampler.h"
#include "UplinkClient.h"
#include "WatchConfig.h"
#include "WatchLogger.h"
#include "WatchLogMacros.h"
#include "WindowFreezeProber.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <memory>

using namespace qt_event_watcher;

namespace {

void printUsage()
{
	std::printf(
		"scout - cross-stack jank probe (QtEventWatcher V7 S1)\n"
		"usage: scout (--pid <pid> | --name <proc.exe>)\n"
		"          [--threshold ms=2000] [--interval ms=250]\n"
		"          [--cpu-threshold pct=95] [--cpu-runs n=3]\n"
		"          [--uplink <server>] [--flush-ms ms=200] [--duration ms=0(stay)]\n"
		"examples:\n"
		"  scout --name basic_demo.exe --threshold 2000 --duration 15000\n"
		"  scout --pid 12345 --uplink QtEventWatcherAggregator\n");
}

} // namespace

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName("QtEventWatcherScout");

	// CLI 解析（对齐 examples/basic 的 QByteArray 参数模式）
	QString name;
	qint64 pid = 0;
	int thresholdMs = 2000;
	int intervalMs = 250;
	int cpuThresholdPct = 95;
	int cpuRuns = 3;
	QString uplink;
	int flushMs = 200;
	qint64 durationMs = 0;

	for (int i = 1; i < argc; ++i) {
		const QByteArray arg(argv[i]);
		const auto next = [&]() -> QByteArray {
			return i + 1 < argc ? QByteArray(argv[i + 1]) : QByteArray();
		};
		bool consumed = false;
		if (arg == "--pid" && !next().isEmpty()) {
			pid = next().toLongLong();			++i; consumed = true;
		} else if (arg == "--name" && !next().isEmpty()) {
			name = QString::fromLocal8Bit(next());	++i; consumed = true;
		} else if (arg == "--threshold" && !next().isEmpty()) {
			thresholdMs = next().toInt();		++i; consumed = true;
		} else if (arg == "--interval" && !next().isEmpty()) {
			intervalMs = next().toInt();		++i; consumed = true;
		} else if (arg == "--cpu-threshold" && !next().isEmpty()) {
			cpuThresholdPct = next().toInt();	++i; consumed = true;
		} else if (arg == "--cpu-runs" && !next().isEmpty()) {
			cpuRuns = next().toInt();			++i; consumed = true;
		} else if (arg == "--uplink" && !next().isEmpty()) {
			uplink = QString::fromLocal8Bit(next()); ++i; consumed = true;
		} else if (arg == "--flush-ms" && !next().isEmpty()) {
			flushMs = next().toInt();			++i; consumed = true;
		} else if (arg == "--duration" && !next().isEmpty()) {
			durationMs = next().toLongLong();	++i; consumed = true;
		}
		if (!consumed) {
			std::printf("unknown or incomplete argument: %s\n", arg.constData());
			printUsage();
			return 2;
		}
	}

	if (pid <= 0 && name.isEmpty()) {
		printUsage();
		return 2;
	}
	if (thresholdMs <= 0 || intervalMs <= 0 || cpuThresholdPct <= 0) {
		std::printf("invalid threshold/interval/cpu-threshold\n");
		return 2;
	}

	// 监控日志初始化：console+file sink（用户级 WARN 输出）+ RecordSink
	// （debug 全量采集进环形缓冲）——与自监控完全同一条采集链
	WatchLogger::instance().initialize("./logs", "QtEventWatcherScout");

	// 上行链路：uplink 非空即启用（配置面复用 WatchConfig setter，热更通道
	// 对 scout 无意义但协议行为一致）
	WatchConfig config;
	if (!uplink.isEmpty()) {
		config.setUplinkName(uplink);
		if (flushMs > 0)
			config.setUplinkFlushMs(flushMs);
	}
	std::unique_ptr<UplinkClient> uplinkClient =
		uplink.isEmpty() ? nullptr : std::make_unique<UplinkClient>(&config);

	// T1：窗口冻结探针（常驻 worker 线程；hung 拍最坏占满 threshold）
	WindowFreezeProber freezeProber(name, pid, thresholdMs, intervalMs);
	freezeProber.start(QThread::LowPriority);

	// T1b：CPU 启发探针（主线程轻量采样）
	CpuSampler cpuSampler(name, pid, cpuThresholdPct, cpuRuns, intervalMs);
	cpuSampler.start();

	const std::string targetDesc = name.isEmpty()
		? QStringLiteral("pid:%1").arg(pid).toStdString()
		: name.toStdString();
	QEW_LOG_INFO("[Scout] watching target={:s} pid={} thresholdMs={} intervalMs={} "
				 "cpuThresholdPct={} cpuRuns={} uplink={:s}",
				 targetDesc, pid, thresholdMs, intervalMs, cpuThresholdPct, cpuRuns,
				 uplink.toStdString());

	// duration > 0：到点自动退出（e2e/巡检模式）；0 = 常驻
	if (durationMs > 0)
		QTimer::singleShot(durationMs, &app, &QCoreApplication::quit);

	QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
		cpuSampler.stop();
		freezeProber.stop();
		freezeProber.wait(2000);
	});

	return app.exec();
}
