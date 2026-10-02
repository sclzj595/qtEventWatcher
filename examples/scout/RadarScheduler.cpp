#include "RadarScheduler.h"

#include "ProbeLogic.h"
#include "RadarConfig.h"
#include "RadarDiscover.h"
#include "WatchLogMacros.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qt_event_watcher {

namespace {

/// 雷达在册目标：冻结状态机（纯逻辑见 ProbeLogic.h）+ 实例级 receiver 名
struct RadarTarget
{
	ProbeLogic::FreezeTracker tracker{ 0 };
	QString recvName;
};

/// 进程真实存活判定：OpenProcess 对"已终止但句柄未释放"的进程对象仍成功
/// （探活"成功"撞 kill 竞态时正是这种状态），须等内核对象判信号态——
/// 100ms 宽限等 TerminateProcess 完成对象置位（kill 恰落在探活在途时，
/// SendMessage 返回后微秒级检查会撞上拆除中窗口的 WAIT_TIMEOUT 假活）
bool aliveOf(qint64 pid)
{
	const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
								 FALSE, DWORD(pid));
	if (h == nullptr)
		return false;
	const DWORD w = WaitForSingleObject(h, 100);
	CloseHandle(h);
	return w == WAIT_TIMEOUT;
}

} // namespace

RadarScheduler::RadarScheduler(int thresholdMs, int intervalMs, qint64 selfPid,
							   const QStringList &excludeNames,
							   int cpuThresholdPct, int cpuRuns, QObject *parent)
	: QThread(parent)
	, m_thresholdMs(thresholdMs)
	, m_intervalMs(intervalMs)
	, m_selfPid(selfPid)
	, m_excludeNames(excludeNames)
	, m_cpuThresholdPct(cpuThresholdPct)
	, m_cpuRuns(cpuRuns)
	, m_running(false)
{
}

void RadarScheduler::stop()
{
	m_running = false;
}

void RadarScheduler::run()
{
	m_running = true;
	QElapsedTimer clock;
	clock.start();

	// 进程名缓存：只对新出现 pid 解析（nameMapOf 单次快照建全表），消失项随拍清理
	QHash<qint64, QString> nameCache;

	// 三态状态机 per-target（纯逻辑 ProbeLogic::FreezeTracker）；
	// 目标消失收口 freeze lost（"恢复"语义不诚实）
	QHash<qint64, RadarTarget> targets;

	// per-target CPU 启发状态（V1 R3a）：episode 迟滞 + 采样基线
	QHash<qint64, ProbeLogic::CpuEpisodeTracker> cpuEpisodes;
	QHash<qint64, qint64> lastTotal100ns;

	// 配置热加载状态（V1 R3b）：mtime 变更即重载；kNoConfigMtime = 未加载哨兵
	static constexpr qint64 kNoConfigMtime = std::numeric_limits<qint64>::min();
	qint64 lastConfigMtime = kNoConfigMtime;
	bool configWarned = false;

	while (m_running) {
		QElapsedTimer tick;
		tick.start();

		// 配置热加载（V1 R3b）：mtime 变更即重载（worker 线程独占读，无锁）；
		// 文件消失/解析失败保持旧配置并告警一次，成功重载后复位
		if (!m_configFile.isEmpty()) {
			const QFileInfo cf(m_configFile);
			const qint64 cm
				= cf.exists() ? cf.lastModified().toMSecsSinceEpoch() : -1;
			if (cm != lastConfigMtime) {
				lastConfigMtime = cm;
				RadarConfig cfg;
				if (cm >= 0 && loadRadarConfig(m_configFile, cfg)
					&& cfg.hasAny()) {
					configWarned = false;
					if (cfg.thresholdMs > 0)
						m_thresholdMs = cfg.thresholdMs;
					if (cfg.intervalMs > 0)
						m_intervalMs = cfg.intervalMs;
					if (cfg.cpuThreshold > 0)
						m_cpuThresholdPct = cfg.cpuThreshold;
					if (cfg.cpuRuns > 0)
						m_cpuRuns = cfg.cpuRuns;
					if (cfg.excludeSet)
						m_excludeNames = cfg.exclude;
					QEW_LOG_INFO("[Scout] radar config reloaded thresholdMs={} "
								 "intervalMs={} cpuThreshold={} cpuRuns={} exclude={:s}",
								 m_thresholdMs, m_intervalMs, m_cpuThresholdPct,
								 m_cpuRuns,
								 m_excludeNames.join(QLatin1Char(',')).toStdString());
				} else if (!configWarned) {
					configWarned = true;
					QEW_LOG_WARN("[Scout] radar config unavailable/invalid, "
								 "keeping last: {:s}",
								 m_configFile.toStdString());
				}
			}
		}

		// 发现：可见顶层窗口 → pid 集 + 句柄归组（跳过自身 pid 双保险）
		const QList<RadarDiscover::RadarWindow> wins = RadarDiscover::sweepWindows();
		QSet<qint64> alive;
		QHash<qint64, QList<qint64>> byPid;
		for (const RadarDiscover::RadarWindow &w : wins) {
			if (w.pid <= 0 || w.pid == m_selfPid)
				continue;
			alive.insert(w.pid);
			byPid[w.pid].append(w.handle);
		}
		QList<qint64> alivePids = alive.values();
		std::sort(alivePids.begin(), alivePids.end());	// 确定性轮询序

		// 增量管理：消失目标收口 + 名字缓存与 CPU 状态清理
		for (auto it = targets.begin(); it != targets.end();) {
			if (!alive.contains(it.key())) {
				if (it.value().tracker.onTargetGone().kind
					== ProbeLogic::FreezeEvent::Lost) {
					m_alarm.emitAlarm("freeze:" + it.value().recvName.toStdString(),
								 "[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
								 it.value().recvName.toStdString());
				}
				it = targets.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = cpuEpisodes.begin(); it != cpuEpisodes.end();) {
			if (!alive.contains(it.key()))
				it = cpuEpisodes.erase(it);
			else
				++it;
		}
		for (auto it = lastTotal100ns.begin(); it != lastTotal100ns.end();) {
			if (!alive.contains(it.key()))
				it = lastTotal100ns.erase(it);
			else
				++it;
		}
		for (auto it = nameCache.begin(); it != nameCache.end();) {
			if (!alive.contains(it.key()))
				it = nameCache.erase(it);
			else
				++it;
		}
		QList<qint64> unknownPids;
		for (qint64 pid : alivePids) {
			if (!nameCache.contains(pid))
				unknownPids.append(pid);
		}
		if (!unknownPids.isEmpty()) {
			// QHash::unite 是 Qt5 专属（Qt6 移除），显式 insert 兼容双版本
			const QHash<qint64, QString> resolved = RadarDiscover::nameMapOf(unknownPids);
			for (auto it = resolved.cbegin(); it != resolved.cend(); ++it)
				nameCache.insert(it.key(), it.value());
		}

		// 逐目标顺序探活（stop 在循环内检查，退出延迟 ≈ 单窗口探活上界）
		for (qint64 pid : alivePids) {
			if (!m_running)
				break;
			const QString name = nameCache.value(pid);
			if (ProbeLogic::excluded(name, m_excludeNames))
				continue;
			bool hung = false;
			for (qint64 h : byPid.value(pid)) {
				DWORD_PTR result = 0;
				// WM_NULL 探活：同 WindowFreezeProber（任务管理器"未响应"同源）
				const DWORD_PTR ret = SendMessageTimeoutW(
					reinterpret_cast<HWND>(static_cast<quintptr>(h)), WM_NULL, 0, 0,
					SMTO_ABORTIFHUNG, DWORD(m_thresholdMs), &result);
				if (ret == 0) {
					hung = true;
					break;
				}
				if (!m_running)
					break;
			}

			RadarTarget &rt = targets[pid];
			rt.tracker.thresholdMs = m_thresholdMs;	// 阈值跟随成员（热加载铺路）
			const qint64 nowMs = clock.elapsed();
			// alive 惰性求值：仅 !hung 拍才付 OpenProcess+100ms（探活"成功"可能
			// 撞目标消亡竞态——垂死窗口返回非 0，死进程一律收口 freeze lost）
			const ProbeLogic::FreezeEvent ev = rt.tracker.onTick(
				hung, nowMs, [&pid]() { return aliveOf(pid); });
			switch (ev.kind) {
			case ProbeLogic::FreezeEvent::Started:
				rt.recvName = ProbeLogic::receiverOf(name, pid);
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 m_thresholdMs, ev.stalledMs, rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::Ongoing:
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze ongoing elapsedMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 ev.totalMs, rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::Recovered:
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze recovered totalMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 ev.totalMs, rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::Lost:
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
							 rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::None:
				break;
			}
		}

		// per-target CPU 启发（V1 R3a）：差分采样对齐 CpuSampler 单核口径
		// （kernel+user 生命周期累计 100ns→ms / 拍间隔）；新 pid 首拍仅建基线；
		// OpenProcess/GetProcessTimes 全非阻塞，O(N) 不破坏单线程轮询模型
		const qint64 cpuElapsedMs = tick.elapsed();
		for (qint64 pid : alivePids) {
			if (!m_running)
				break;
			const QString name = nameCache.value(pid);
			if (ProbeLogic::excluded(name, m_excludeNames))
				continue;
			qint64 total = -1;
			const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
										 FALSE, DWORD(pid));
			if (h != nullptr) {
				FILETIME ftCreate = {}, ftExit = {}, ftKernel = {}, ftUser = {};
				if (GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
					ULARGE_INTEGER k, u;
					k.LowPart = ftKernel.dwLowDateTime;
					k.HighPart = ftKernel.dwHighDateTime;
					u.LowPart = ftUser.dwLowDateTime;
					u.HighPart = ftUser.dwHighDateTime;
					total = qint64(k.QuadPart + u.QuadPart);
				}
				CloseHandle(h);
			}
			if (total < 0)
				continue;	// 打开/读取失败（受保护进程等）：静默跳过
			const auto base = lastTotal100ns.constFind(pid);
			if (base == lastTotal100ns.constEnd()) {
				lastTotal100ns.insert(pid, total);
				continue;	// 首拍仅建基线，无差分
			}
			const double cpuPct = cpuElapsedMs > 0
				? 100.0 * (double(total - base.value()) / 10000.0)
					/ double(cpuElapsedMs)
				: 0.0;
			lastTotal100ns.insert(pid, total);

			auto ep = cpuEpisodes.find(pid);
			if (ep == cpuEpisodes.end())
				ep = cpuEpisodes.insert(pid, ProbeLogic::CpuEpisodeTracker(
												 m_cpuThresholdPct, m_cpuRuns));
			ep.value().thresholdPct = m_cpuThresholdPct;	// 热加载铺路
			const ProbeLogic::CpuEvent ev = ep.value().onTick(cpuPct);
			if (ev.kind == ProbeLogic::CpuEvent::Spin) {
				// 行格式对齐 CpuSampler cpuSpin + radar=1 标注；
				// receiver=name@pid 实例级唯一（同名多开各自成 episode）
				m_alarm.emitAlarm("cpu:" + ProbeLogic::receiverOf(name, pid).toStdString(),
							 "[EventWatcher] slow event receiver={:s} object={:s} "
							 "event=cpuSpin type=99 depth=0 costMs={:.1f} "
							 "exclusiveCostMs=0.000 curThread=0x0 recvThread=0x0 "
							 "match=true thresholdMs={} source=scout radar=1",
							 ProbeLogic::receiverOf(name, pid).toStdString(),
							 name.toStdString(), ev.cpuPct, m_cpuThresholdPct);
			}
		}

		// 分片睡眠保证 stop 响应性（hung 拍 tick 本身可能占满 threshold×k）
		int remain = m_intervalMs - int(tick.elapsed());
		while (remain > 0 && m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
		}
	}
}

} // namespace qt_event_watcher

#elif defined(Q_OS_UNIX)	// Linux：X11 _NET_WM_PING 全机雷达（同 T1 探活原语）

#ifdef QEWT_SCOUT_X11

#include "TargetResolver.h"
#include "X11Probe.h"

namespace qt_event_watcher {

namespace {

/// 雷达在册目标：冻结状态机（纯逻辑见 ProbeLogic.h）+ 实例级 receiver 名
struct RadarTarget
{
	ProbeLogic::FreezeTracker tracker{ 0 };
	QString recvName;
};

/// 进程真实存活判定：/proc/<pid> 对 zombie（已终止待收尸）进程仍存在，
/// 须读 stat 的 state 字段排除 Z 态（解析锚定同 TargetResolver::statPpidOf）
bool aliveOf(qint64 pid)
{
	QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QString data = QString::fromUtf8(f.readAll());
	const int close = data.lastIndexOf(QLatin1Char(')'));
	if (close < 0 || close + 2 >= data.size())
		return false;
	const QStringList fields = data.mid(close + 2).split(QLatin1Char(' '));
	if (fields.isEmpty())
		return false;
	return fields.first() != QStringLiteral("Z");
}

} // namespace

RadarScheduler::RadarScheduler(int thresholdMs, int intervalMs, qint64 selfPid,
							   const QStringList &excludeNames,
							   int cpuThresholdPct, int cpuRuns, QObject *parent)
	: QThread(parent)
	, m_thresholdMs(thresholdMs)
	, m_intervalMs(intervalMs)
	, m_selfPid(selfPid)
	, m_excludeNames(excludeNames)
	, m_cpuThresholdPct(cpuThresholdPct)
	, m_cpuRuns(cpuRuns)
	, m_running(false)
{
}

void RadarScheduler::stop()
{
	m_running = false;
}

void RadarScheduler::run()
{
	m_running = true;
	QElapsedTimer clock;
	clock.start();
	Display *dpy = XOpenDisplay(nullptr);
	if (dpy == nullptr) {
		// Wayland native / 无 X display：外部探针无等价协议，诚实降级
		// （CDP 探针不受影响）；轮询空转维持线程生命周期
		QEW_LOG_WARN("[FreezeWatch] X11 display unavailable, radar freeze probe "
					 "disabled type=0 radar=1");
		int remain = m_intervalMs;
		while (m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
			if (remain <= 0)
				remain = m_intervalMs;
		}
		return;
	}

	// 雷达长驻、目标进出频繁：枚举与探活间隙目标窗口随时可能消亡，
	// BadWindow 会触发 Xlib 默认错误处理器直接终止进程——装 no-op handler，
	// 出错调用返回 0 由调用方按"发送失败/不可探活"分支处理
	XSetErrorHandler([](Display *, XErrorEvent *) -> int { return 0; });

	Window root = DefaultRootWindow(dpy);
	const Atom wmProtocolsAtom = XInternAtom(dpy, "WM_PROTOCOLS", False);
	const Atom pingAtom = XInternAtom(dpy, "_NET_WM_PING", False);
	const Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);

	// pong 回流：同 T1——应用 echo 到 root，回发 mask 含 StructureNotify，
	// 探针选 root 的 StructureNotifyMask 必达（掩码位辨析见 T1 注释）
	XSelectInput(dpy, root, StructureNotifyMask);

	QHash<qint64, QString> nameCache;
		QHash<qint64, RadarTarget> targets;

		// per-target CPU 启发状态（V1 R3a）：episode 迟滞 + 采样基线（ms）
		QHash<qint64, ProbeLogic::CpuEpisodeTracker> cpuEpisodes;
		QHash<qint64, double> lastCpuMs;

		while (m_running) {
			QElapsedTimer tick;
			tick.start();

			// 配置热加载（V1 R3b）：mtime 变更即重载（worker 线程独占读，无锁）；
			// 文件消失/解析失败保持旧配置并告警一次，成功重载后复位
			if (!m_configFile.isEmpty()) {
				const QFileInfo cf(m_configFile);
				const qint64 cm
					= cf.exists() ? cf.lastModified().toMSecsSinceEpoch() : -1;
				if (cm != lastConfigMtime) {
					lastConfigMtime = cm;
					RadarConfig cfg;
					if (cm >= 0 && loadRadarConfig(m_configFile, cfg)
						&& cfg.hasAny()) {
						configWarned = false;
						if (cfg.thresholdMs > 0)
							m_thresholdMs = cfg.thresholdMs;
						if (cfg.intervalMs > 0)
							m_intervalMs = cfg.intervalMs;
						if (cfg.cpuThreshold > 0)
							m_cpuThresholdPct = cfg.cpuThreshold;
						if (cfg.cpuRuns > 0)
							m_cpuRuns = cfg.cpuRuns;
						if (cfg.excludeSet)
							m_excludeNames = cfg.exclude;
						QEW_LOG_INFO("[Scout] radar config reloaded thresholdMs={} "
									 "intervalMs={} cpuThreshold={} cpuRuns={} exclude={:s}",
									 m_thresholdMs, m_intervalMs, m_cpuThresholdPct,
									 m_cpuRuns,
									 m_excludeNames.join(QLatin1Char(',')).toStdString());
					} else if (!configWarned) {
						configWarned = true;
						QEW_LOG_WARN("[Scout] radar config unavailable/invalid, "
									 "keeping last: {:s}",
									 m_configFile.toStdString());
					}
				}
			}

			const QList<RadarDiscover::RadarWindow> wins
				= RadarDiscover::sweepWindows(dpy, root, pidAtom);
			QSet<qint64> alive;
			QHash<qint64, QList<qint64>> byPid;
			for (const RadarDiscover::RadarWindow &w : wins) {
				if (w.pid <= 0 || w.pid == m_selfPid)
					continue;
				alive.insert(w.pid);
				byPid[w.pid].append(w.handle);
			}
			QList<qint64> alivePids = alive.values();
			std::sort(alivePids.begin(), alivePids.end());

			for (auto it = targets.begin(); it != targets.end();) {
				if (!alive.contains(it.key())) {
					if (it.value().tracker.onTargetGone().kind
						== ProbeLogic::FreezeEvent::Lost) {
						m_alarm.emitAlarm("freeze:" + it.value().recvName.toStdString(),
									 "[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
									 it.value().recvName.toStdString());
					}
					it = targets.erase(it);
				} else {
					++it;
				}
			}
			for (auto it = cpuEpisodes.begin(); it != cpuEpisodes.end();) {
				if (!alive.contains(it.key()))
					it = cpuEpisodes.erase(it);
				else
					++it;
			}
			for (auto it = lastCpuMs.begin(); it != lastCpuMs.end();) {
				if (!alive.contains(it.key()))
					it = lastCpuMs.erase(it);
				else
					++it;
			}
		for (auto it = nameCache.begin(); it != nameCache.end();) {
			if (!alive.contains(it.key()))
				it = nameCache.erase(it);
			else
				++it;
		}
		QList<qint64> unknownPids;
		for (qint64 pid : alivePids) {
			if (!nameCache.contains(pid))
				unknownPids.append(pid);
		}
		if (!unknownPids.isEmpty()) {
			// QHash::unite 是 Qt5 专属（Qt6 移除），显式 insert 兼容双版本
			const QHash<qint64, QString> resolved = RadarDiscover::nameMapOf(unknownPids);
			for (auto it = resolved.cbegin(); it != resolved.cend(); ++it)
				nameCache.insert(it.key(), it.value());
		}

		for (qint64 pid : alivePids) {
			if (!m_running)
				break;
			const QString name = nameCache.value(pid);
			if (ProbeLogic::excluded(name, m_excludeNames))
				continue;
			// 逐窗口探活，首个无 pong 即 hung（早退语义同 T1 Linux 版）
			bool hung = false;
			for (qint64 h : byPid.value(pid)) {
				if (!X11Probe::pingWindow(dpy, wmProtocolsAtom, pingAtom,
										  Window(h), m_thresholdMs)) {
					hung = true;
					break;
				}
				if (!m_running)
					break;
			}

			RadarTarget &rt = targets[pid];
			rt.tracker.thresholdMs = m_thresholdMs;	// 阈值跟随成员（热加载铺路）
			const qint64 nowMs = clock.elapsed();
			// alive 惰性求值：仅 !hung 拍才读 /proc（探活"成功"撞目标消亡竞态
			// 同 Windows 版——XSendEvent 对垂死窗口返回 0 → pingWindow 按
			// "不可探活"放行非 hung；死进程一律收口 freeze lost）
			const ProbeLogic::FreezeEvent ev = rt.tracker.onTick(
				hung, nowMs, [&pid]() { return aliveOf(pid); });
			switch (ev.kind) {
			case ProbeLogic::FreezeEvent::Started:
				rt.recvName = ProbeLogic::receiverOf(name, pid);
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 m_thresholdMs, ev.stalledMs, rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::Ongoing:
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze ongoing elapsedMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 ev.totalMs, rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::Recovered:
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze recovered totalMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 ev.totalMs, rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::Lost:
				m_alarm.emitAlarm("freeze:" + rt.recvName.toStdString(),
							 "[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
							 rt.recvName.toStdString());
				break;
			case ProbeLogic::FreezeEvent::None:
				break;
			}
		}

		// per-target CPU 启发（V1 R3a）：/proc stat utime+stime 差分（ms）/
		// 拍间隔，口径对齐 CpuSampler Linux 版；新 pid 首拍仅建基线；
		// /proc 读全非阻塞，O(N) 不破坏单线程轮询模型
		const qint64 cpuElapsedMs = tick.elapsed();
		for (qint64 pid : alivePids) {
			if (!m_running)
				break;
			const QString name = nameCache.value(pid);
			if (ProbeLogic::excluded(name, m_excludeNames))
				continue;
			double cpuMs = 0.0;
			if (!TargetResolver::cpuTimeMsOf(pid, &cpuMs))
				continue;	// 读取失败：静默跳过
			const auto base = lastCpuMs.constFind(pid);
			if (base == lastCpuMs.constEnd()) {
				lastCpuMs.insert(pid, cpuMs);
				continue;	// 首拍仅建基线，无差分
			}
			const double cpuPct = cpuElapsedMs > 0
				? 100.0 * (cpuMs - base.value()) / double(cpuElapsedMs) : 0.0;
			lastCpuMs.insert(pid, cpuMs);

			auto ep = cpuEpisodes.find(pid);
			if (ep == cpuEpisodes.end())
				ep = cpuEpisodes.insert(pid, ProbeLogic::CpuEpisodeTracker(
												 m_cpuThresholdPct, m_cpuRuns));
			ep.value().thresholdPct = m_cpuThresholdPct;	// 热加载铺路
			const ProbeLogic::CpuEvent ev = ep.value().onTick(cpuPct);
			if (ev.kind == ProbeLogic::CpuEvent::Spin) {
				m_alarm.emitAlarm("cpu:" + ProbeLogic::receiverOf(name, pid).toStdString(),
							 "[EventWatcher] slow event receiver={:s} object={:s} "
							 "event=cpuSpin type=99 depth=0 costMs={:.1f} "
							 "exclusiveCostMs=0.000 curThread=0x0 recvThread=0x0 "
							 "match=true thresholdMs={} source=scout radar=1",
							 ProbeLogic::receiverOf(name, pid).toStdString(),
							 name.toStdString(), ev.cpuPct, m_cpuThresholdPct);
			}
		}

		int remain = m_intervalMs - int(tick.elapsed());
		while (remain > 0 && m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
		}
	}

	XCloseDisplay(dpy);
}

} // namespace qt_event_watcher

#else // QEWT_SCOUT_X11 未定义：X11 依赖缺失，雷达禁用（CDP 不受影响）

namespace qt_event_watcher {

RadarScheduler::RadarScheduler(int, int, qint64, const QStringList &, int, int,
							   QObject *parent)
	: QThread(parent)
{
}
void RadarScheduler::stop() {}
void RadarScheduler::run() {}

} // namespace qt_event_watcher

#endif // QEWT_SCOUT_X11

#else // 其他平台：雷达空实现

namespace qt_event_watcher {

RadarScheduler::RadarScheduler(int, int, qint64, const QStringList &, int, int,
							   QObject *parent)
	: QThread(parent)
{
}
void RadarScheduler::stop() {}
void RadarScheduler::run() {}

} // namespace qt_event_watcher

#endif // Q_OS_WIN / Q_OS_UNIX
