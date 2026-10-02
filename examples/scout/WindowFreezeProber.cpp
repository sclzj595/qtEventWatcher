#include "WindowFreezeProber.h"

#include "ProbeLogic.h"
#include "TargetResolver.h"
#include "WatchLogMacros.h"

#include <QElapsedTimer>
#include <QSet>

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qt_event_watcher {

namespace {

/// EnumWindows 上下文：pid 过滤 + 可见顶层窗口收集
struct EnumCtx
{
	const QSet<qint64> *pids;
	QList<HWND> *windows;
};

BOOL CALLBACK enumTopWindow(HWND hwnd, LPARAM lp)
{
	EnumCtx *ctx = reinterpret_cast<EnumCtx *>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (ctx->pids->contains(qint64(pid)) && IsWindowVisible(hwnd))
		ctx->windows->append(hwnd);
	return TRUE;
}

} // namespace

WindowFreezeProber::WindowFreezeProber(const QString &targetName, qint64 targetPid,
									   int thresholdMs, int intervalMs, QObject *parent)
	: QThread(parent)
	, m_targetName(targetName)
	, m_targetPid(targetPid)
	, m_thresholdMs(thresholdMs)
	, m_intervalMs(intervalMs)
	, m_running(false)
{
}

void WindowFreezeProber::stop()
{
	m_running = false;
}

bool WindowFreezeProber::probeTopWindows(const QList<qint64> &pids, QString &procName) const
{
	const QSet<qint64> pidSet(pids.cbegin(), pids.cend());
	QList<HWND> windows;
	EnumCtx ctx{ &pidSet, &windows };
	EnumWindows(enumTopWindow, reinterpret_cast<LPARAM>(&ctx));

	for (HWND hwnd : windows) {
		DWORD_PTR result = 0;
		// WM_NULL 探活：消息泵停摆（未响应）时超时返回 0——与任务管理器
		// "未响应"同源判定，阈值由本探针自控
		const DWORD_PTR ret = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
												  SMTO_ABORTIFHUNG,
												  DWORD(m_thresholdMs), &result);
		if (ret == 0) {
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			procName = TargetResolver::processNameOf(qint64(pid));
			if (procName.isEmpty())
				procName = QStringLiteral("pid:%1").arg(pid);
			return true;
		}
	}
	return false;
}

void WindowFreezeProber::run()
{
	m_running = true;
	QElapsedTimer clock;
	clock.start();

	// 三态状态机（纯逻辑 ProbeLogic::FreezeTracker）对齐 EventWatchdog：
	// started（首检停滞）→ ongoing*（1s 节流）→ recovered（恢复）；
	// 目标消失收口为 freeze lost（"恢复"语义不诚实）
	ProbeLogic::FreezeTracker tracker(m_thresholdMs);
	QString recvName;

	while (m_running) {
		QElapsedTimer tick;
		tick.start();

		const QList<qint64> pids = TargetResolver::resolvePids(m_targetName, m_targetPid);
		QString proc;
		bool hung = false;
		if (!pids.isEmpty())
			hung = probeTopWindows(pids, proc);

		const qint64 nowMs = clock.elapsed();
		// alive = pid 解析非空（T1 存活代理）；pids 空时 hung 恒 false，
		// freezing 中走 lost 分支——与原分支序逐语义等价
		const ProbeLogic::FreezeEvent ev = tracker.onTick(
			hung, nowMs, [&pids]() { return !pids.isEmpty(); });
		switch (ev.kind) {
		case ProbeLogic::FreezeEvent::Started:
			recvName = proc;
			// stalledMs 保守取阈值下界：外部探针只能保证"至少已停滞 threshold"
			m_alarm.emitAlarmNow("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 m_thresholdMs, ev.stalledMs, recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::Ongoing:
			m_alarm.emitAlarm("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze ongoing elapsedMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 ev.totalMs, recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::Recovered:
			m_alarm.emitAlarmNow("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze recovered totalMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 ev.totalMs, recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::Lost:
			m_alarm.emitAlarmNow("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze lost receiver={:s} type=0",
						 recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::None:
			break;
		}

		// 分片睡眠保证 stop 响应性（hung 拍 tick 本身可能占满 threshold）
		int remain = m_intervalMs - int(tick.elapsed());
		while (remain > 0 && m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
		}
	}
}

} // namespace qt_event_watcher

#elif defined(Q_OS_UNIX)	// Linux：X11 _NET_WM_PING 探活（EWMH 标准，同 WM "未响应"判定源）

#ifdef QEWT_SCOUT_X11

// X11 探活原语共享头（V8 抽出）：pingWindow / collectTargetWindows / token
#include "X11Probe.h"

namespace qt_event_watcher {

WindowFreezeProber::WindowFreezeProber(const QString &targetName, qint64 targetPid,
									   int thresholdMs, int intervalMs, QObject *parent)
	: QThread(parent)
	, m_targetName(targetName)
	, m_targetPid(targetPid)
	, m_thresholdMs(thresholdMs)
	, m_intervalMs(intervalMs)
	, m_running(false)
{
}

void WindowFreezeProber::stop()
{
	m_running = false;
}

void WindowFreezeProber::run()
{
	m_running = true;
	QElapsedTimer clock;	// 局部名 shadow POSIX ::clock()——合法且仅本段作用域
	clock.start();
	Display *dpy = XOpenDisplay(nullptr);
	if (dpy == nullptr) {
		// Wayland native / 无 X display：外部探针无等价协议，诚实降级
		// （CPU/CDP 探针不受影响）；轮询空转维持线程生命周期
		QEW_LOG_WARN("[FreezeWatch] X11 display unavailable, freeze probe "
					 "disabled (cpu/cdp probes unaffected) type=0");
		int remain = m_intervalMs;
		while (m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
			if (remain <= 0)
				remain = m_intervalMs;
		}
		return;
	}

	Window root = DefaultRootWindow(dpy);
	const Atom wmProtocolsAtom = XInternAtom(dpy, "WM_PROTOCOLS", False);
	const Atom pingAtom = XInternAtom(dpy, "_NET_WM_PING", False);
	const Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
	// pong 回流：Qt xcb 收到 ping（须 WM_PROTOCOLS 封装，见 pingWindow 注释）
	// 后整包 echo 到 root（qxcbwindow.cpp handleClientMessageEvent：reply
	// 整包拷贝仅改 window=root，type=WM_PROTOCOLS 与 data 全原样），回发
	// event_mask=StructureNotify|SubstructureRedirect——探针选 root 的
	// StructureNotifyMask 即与回发 mask 有交集必达（注意不是
	// SubstructureNotifyMask：两者是不同掩码位，选错则 pong 永远收不到；
	// SubstructureRedirect 是 WM 独占掩码不可选；WM 在/不在均成立）
	XSelectInput(dpy, root, StructureNotifyMask);

	// 三态状态机（纯逻辑 ProbeLogic::FreezeTracker）对齐 EventWatchdog：
	// started（首检停滞）→ ongoing*（1s 节流）→ recovered（恢复）；
	// 目标消失收口为 freeze lost（"恢复"语义不诚实）
	ProbeLogic::FreezeTracker tracker(m_thresholdMs);
	QString recvName;

	while (m_running) {
		QElapsedTimer tick;
		tick.start();

		const QList<qint64> pids = TargetResolver::resolvePids(m_targetName, m_targetPid);
		QString proc;
		bool hung = false;
		if (!pids.isEmpty()) {
			// 逐窗口探活，首个无 pong 即 hung（早退语义同 Windows 版）
			const QSet<qint64> pidSet(pids.cbegin(), pids.cend());
			const QList<X11Probe::TargetWindow> targets =
				X11Probe::collectTargetWindows(dpy, root, pidSet, pidAtom);
			for (const X11Probe::TargetWindow &t : targets) {
				if (!X11Probe::pingWindow(dpy, wmProtocolsAtom, pingAtom, t.win, m_thresholdMs)) {
					proc = TargetResolver::processNameOf(t.pid);
					if (proc.isEmpty())
						proc = QStringLiteral("pid:%1").arg(t.pid);
					hung = true;
					break;
				}
			}
		}

		const qint64 nowMs = clock.elapsed();
		// alive = pid 解析非空（T1 存活代理）；pids 空时 hung 恒 false，
		// freezing 中走 lost 分支——与原分支序逐语义等价
		const ProbeLogic::FreezeEvent ev = tracker.onTick(
			hung, nowMs, [&pids]() { return !pids.isEmpty(); });
		switch (ev.kind) {
		case ProbeLogic::FreezeEvent::Started:
			recvName = proc;
			// stalledMs 保守取阈值下界：外部探针只能保证"至少已停滞 threshold"
			m_alarm.emitAlarmNow("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 m_thresholdMs, ev.stalledMs, recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::Ongoing:
			m_alarm.emitAlarm("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze ongoing elapsedMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 ev.totalMs, recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::Recovered:
			m_alarm.emitAlarmNow("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze recovered totalMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 ev.totalMs, recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::Lost:
			m_alarm.emitAlarmNow("freeze:" + recvName.toStdString(),
						 "[FreezeWatch] freeze lost receiver={:s} type=0",
						 recvName.toStdString());
			break;
		case ProbeLogic::FreezeEvent::None:
			break;
		}

		// 分片睡眠保证 stop 响应性（hung 拍 tick 本身可能占满 threshold）
		int remain = m_intervalMs - int(tick.elapsed());
		while (remain > 0 && m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
		}
	}

	XCloseDisplay(dpy);
}

} // namespace qt_event_watcher

#else // QEWT_SCOUT_X11 未定义：X11 依赖缺失，冻结探针禁用（CPU/CDP 不受影响）

namespace qt_event_watcher {

WindowFreezeProber::WindowFreezeProber(const QString &, qint64, int, int, QObject *parent)
	: QThread(parent)
{
}
void WindowFreezeProber::stop() {}
void WindowFreezeProber::run() {}

} // namespace qt_event_watcher

#endif // QEWT_SCOUT_X11

#else // 其他平台：探针空实现（Scout 冻结探针未覆盖的平台）

namespace qt_event_watcher {

WindowFreezeProber::WindowFreezeProber(const QString &, qint64, int, int, QObject *parent)
	: QThread(parent)
{
}
void WindowFreezeProber::stop() {}
void WindowFreezeProber::run() {}

} // namespace qt_event_watcher

#endif // Q_OS_WIN / Q_OS_UNIX
