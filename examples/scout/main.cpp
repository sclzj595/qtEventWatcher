// scout - 跨栈程序卡顿外部探针（产品线 B / V7 + V8 雷达）
// 进程外检测任意桌面程序的卡顿：
//   T1  窗口冻结——SendMessageTimeout 轮询消息泵停摆（未响应），三态告警
//       行对齐 EventWatchdog 冻结语义，aggregator/HTML 冻结时间线零改动渲染
//   T1b CPU 启发——目标进程+子进程单核满转检测（Electron renderer 即子进程）
//   T2  CDP 长任务——Chromium 系（Electron/Edge/Chrome）页面渲染长任务精确
//       检测（目标须以 --remote-debugging-port=<port> 冷启动）
//   T3  全机雷达（V8）——--radar 常驻发现全部"窗口归属 GUI 进程"逐个探活，
//       系统级 ANR 雷达（docs/33）
// 观测记录走与自监控完全相同的日志行协议：
//   WatchLogger → RecordSink → WatchRecordStore → UplinkClient → aggregator
// 用法：
//   scout --pid <pid> | --name <proc.exe> [--threshold ms] [--interval ms]
//         [--cpu-threshold pct] [--cpu-runs n] [--uplink <server>]
//         [--flush-ms ms] [--duration ms]
//   scout --cdp-port <port> [--cdp-target <substr>] [--cdp-threshold ms]
//         [--uplink <server>] [--duration ms]     # T2 纯 CDP 模式（免 pid/name）
//   scout --radar [--radar-exclude <name1,name2>] [--threshold ms] [--interval ms]
//         [--uplink <server>] [--duration ms]     # T3 全机雷达（免 pid/name）

#include "CpuSampler.h"
#include "RadarScheduler.h"
#include "UplinkClient.h"
#include "WatchConfig.h"
#include "WatchLogger.h"
#include "WatchLogMacros.h"
#include "WindowFreezeProber.h"
#ifdef QEWT_SCOUT_CDP
#include "CdpLongTaskProber.h"
#endif

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
		"scout - cross-stack jank probe (QtEventWatcher V7/V8)\n"
		"usage: scout (--pid <pid> | --name <proc.exe>)\n"
		"          [--threshold ms=2000] [--interval ms=250]\n"
		"          [--cpu-threshold pct=95] [--cpu-runs n=3]\n"
		"          [--uplink <server>] [--flush-ms ms=200] [--duration ms=0(stay)]\n"
		"       scout --cdp-port <port> [--cdp-target <substr>] [--cdp-threshold ms=50]\n"
		"          [--uplink <server>] [--duration ms]\n"
		"       scout --radar [--radar-exclude <name1,name2>] [--threshold ms=2000]\n"
		"          [--interval ms=250] [--uplink <server>] [--duration ms]\n"
		"examples:\n"
		"  scout --name basic_demo.exe --threshold 2000 --duration 15000\n"
		"  scout --pid 12345 --uplink QtEventWatcherAggregator\n"
		"  scout --cdp-port 9222 --uplink QtEventWatcherAggregator   # Electron/Chromium\n"
		"            (target must be started with --remote-debugging-port=9222)\n"
		"  scout --radar --uplink QtEventWatcherAggregator          # all GUI apps\n");
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
	int cdpPort = 0;				// 0 = CDP 探针关闭
	int cdpThresholdMs = 50;
	QString cdpTarget;				// url/title 子串过滤（空 = 首个 page）
	bool radarMode = false;			// V8 T3：全机雷达（免 pid/name）
	QString radarExclude;			// 逗号分隔进程名子串排除表

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
		} else if (arg == "--cdp-port" && !next().isEmpty()) {
			cdpPort = next().toInt();			++i; consumed = true;
		} else if (arg == "--cdp-threshold" && !next().isEmpty()) {
			cdpThresholdMs = next().toInt();	++i; consumed = true;
		} else if (arg == "--cdp-target" && !next().isEmpty()) {
			cdpTarget = QString::fromLocal8Bit(next()); ++i; consumed = true;
		} else if (arg == "--radar") {
			radarMode = true;					consumed = true;
		} else if (arg == "--radar-exclude" && !next().isEmpty()) {
			radarExclude = QString::fromLocal8Bit(next()); ++i; consumed = true;
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

	// T2 纯 CDP / T3 雷达模式：免 pid/name；雷达与显式目标互斥（发现自动进行）
	if (!radarMode && pid <= 0 && name.isEmpty() && cdpPort <= 0) {
		printUsage();
		return 2;
	}
	if (radarMode && (pid > 0 || !name.isEmpty())) {
		std::printf("--radar discovers targets automatically; --pid/--name not allowed\n");
		return 2;
	}
	if ((pid > 0 || !name.isEmpty() || radarMode)
		&& (thresholdMs <= 0 || intervalMs <= 0 || cpuThresholdPct <= 0)) {
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
	// 仅 pid/name 目标模式启用——纯 CDP 模式由 T2 覆盖；雷达模式由 T3 接管
	const bool hasTarget = (pid > 0 || !name.isEmpty());
	std::unique_ptr<WindowFreezeProber> freezeProber;
	std::unique_ptr<CpuSampler> cpuSampler;
	std::unique_ptr<RadarScheduler> radarScheduler;		// V8 T3：全机雷达
	if (radarMode) {
		radarScheduler = std::make_unique<RadarScheduler>(
			thresholdMs, intervalMs, QCoreApplication::applicationPid(),
			radarExclude.split(QLatin1Char(','), Qt::SkipEmptyParts));
		radarScheduler->start(QThread::LowPriority);
	} else if (hasTarget) {
		freezeProber = std::make_unique<WindowFreezeProber>(
			name, pid, thresholdMs, intervalMs);
		freezeProber->start(QThread::LowPriority);
		cpuSampler = std::make_unique<CpuSampler>(
			name, pid, cpuThresholdPct, cpuRuns, intervalMs);
		cpuSampler->start();
	}

#ifdef QEWT_SCOUT_CDP
	// T2：CDP 长任务探针（cdpPort>0 启用；纯异步，主线程事件循环驱动）
	std::unique_ptr<CdpLongTaskProber> cdpProber;
	if (cdpPort > 0) {
		cdpProber = std::make_unique<CdpLongTaskProber>(
			cdpPort, cdpTarget, cdpThresholdMs);
		cdpProber->start();
	}
#else
	if (cdpPort > 0)
		std::printf("warning: built without Qt WebSockets, --cdp-port ignored\n");
#endif

	const std::string targetDesc = radarMode
		? std::string("radar(all GUI windows)")
		: (name.isEmpty() ? QStringLiteral("pid:%1").arg(pid).toStdString()
						  : name.toStdString());
	QEW_LOG_INFO("[Scout] watching target={:s} pid={} thresholdMs={} intervalMs={} "
				 "cpuThresholdPct={} cpuRuns={} cdpPort={} radar={} radarExclude={:s} "
				 "uplink={:s}",
				 targetDesc, pid, thresholdMs, intervalMs, cpuThresholdPct, cpuRuns,
				 cdpPort, radarMode ? 1 : 0, radarExclude.toStdString(),
				 uplink.toStdString());

	// duration > 0：到点自动退出（e2e/巡检模式）；0 = 常驻
	if (durationMs > 0)
		QTimer::singleShot(durationMs, &app, &QCoreApplication::quit);

	QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
#ifdef QEWT_SCOUT_CDP
		if (cdpProber)
			cdpProber->stop();
#endif
		if (cpuSampler)
			cpuSampler->stop();
		if (freezeProber) {
			freezeProber->stop();
			freezeProber->wait(2000);
		}
		if (radarScheduler) {
			radarScheduler->stop();
			radarScheduler->wait(2000);
		}
	});

	return app.exec();
}
