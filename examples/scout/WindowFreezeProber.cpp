#include "WindowFreezeProber.h"

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

	// 三态状态机对齐 EventWatchdog：started（首检停滞）→ ongoing*（1s 节流）
	// → recovered（恢复）；目标消失收口为 freeze lost（"恢复"语义不诚实）
	bool freezing = false;
	qint64 startMs = 0;
	qint64 lastOngoingMs = 0;
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
		if (freezing) {
			if (pids.isEmpty()) {
				freezing = false;
				QEW_LOG_WARN("[FreezeWatch] freeze lost receiver={:s} type=0",
							 recvName.toStdString());
			} else if (!hung) {
				freezing = false;
				QEW_LOG_WARN("[FreezeWatch] freeze recovered totalMs={} "
							 "receiver={:s} type=0 inProgress=false",
							 nowMs - startMs, recvName.toStdString());
			} else if (nowMs - lastOngoingMs >= 1000) {
				lastOngoingMs = nowMs;
				QEW_LOG_WARN("[FreezeWatch] freeze ongoing elapsedMs={} "
							 "receiver={:s} type=0 inProgress=false",
							 nowMs - startMs, recvName.toStdString());
			}
		} else if (hung) {
			freezing = true;
			startMs = nowMs;
			lastOngoingMs = nowMs;
			recvName = proc;
			// stalledMs 保守取阈值下界：外部探针只能保证"至少已停滞 threshold"
			QEW_LOG_WARN("[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 m_thresholdMs, m_thresholdMs, recvName.toStdString());
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

#include <poll.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <atomic>
#include <cstring>

namespace qt_event_watcher {

namespace {

/// 单调 ping token：本进程 pid 作高位基座（WM 真实 ping 用 X 服务器时间戳，
/// 值域远低于 pid<<32），逐次 +1 防上一拍迟到 pong 污染本拍判定
std::atomic<unsigned long> g_pingToken{ unsigned long(::getpid()) };

/// 目标顶层窗口（窗口 id + 所属 pid，procName 归因用）
struct TargetWindow
{
	Window win = None;
	qint64 pid = 0;
};

/// 根窗口 client 顶层窗口枚举：_NET_CLIENT_LIST（EWMH，有 WM 时权威）
/// ∪ 根窗口直接子窗口（Xvfb 等无 WM 环境兜底——应用顶层窗口即 root 子窗口），
/// 按 _NET_WM_PID 过滤目标 pid 集（Qt/GTK 创建窗口时均写入该属性）。
QList<TargetWindow> collectTargetWindows(Display *dpy, Window root,
										 const QSet<qint64> &pids, Atom pidAtom)
{
	// 先收集候选窗口（去重）
	QList<Window> candidates;
	QSet<Window> seen;

	Atom type = None;
	int fmt = 0;
	unsigned long n = 0;
	unsigned long remain = 0;
	unsigned char *data = nullptr;
	if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_NET_CLIENT_LIST", False),
						   0, 4096, False, XA_WINDOW, &type, &fmt, &n,
						   &remain, &data) == Success
		&& type == XA_WINDOW && data != nullptr) {
		const Window *wins = reinterpret_cast<const Window *>(data);
		for (unsigned long i = 0; i < n; ++i) {
			if (!seen.contains(wins[i])) {
				seen.insert(wins[i]);
				candidates.append(wins[i]);
			}
		}
	}
	if (data != nullptr)
		XFree(data);

	Window rootRet = None;
	Window parentRet = None;
	Window *children = nullptr;
	unsigned int nChildren = 0;
	if (XQueryTree(dpy, root, &rootRet, &parentRet, &children, &nChildren) != 0
		&& children != nullptr) {
		for (unsigned int i = 0; i < nChildren; ++i) {
			if (!seen.contains(children[i])) {
				seen.insert(children[i]);
				candidates.append(children[i]);
			}
		}
	}
	if (children != nullptr)
		XFree(children);

	// _NET_WM_PID 过滤（fmt=32 → long 数组，LP64 下每元素 8 字节）
	QList<TargetWindow> matched;
	for (Window w : candidates) {
		unsigned long pidVal = 0;
		unsigned char *pidData = nullptr;
		if (XGetWindowProperty(dpy, w, pidAtom, 0, 1, False, XA_CARDINAL,
							   &type, &fmt, &n, &remain, &pidData) == Success
			&& type == XA_CARDINAL && fmt == 32 && n >= 1 && pidData != nullptr) {
			pidVal = *reinterpret_cast<const unsigned long *>(pidData);
			XFree(pidData);
		}
		if (pids.contains(qint64(pidVal)))
			matched.append({ w, qint64(pidVal) });
	}
	return matched;
}

/// 单窗口探活：发 _NET_WM_PING（event_mask=0 → 送达创建该窗口的 client，
/// 与 WM 探测同路径），阈值内等应用原样回传的 pong（Qt/GTK 均逐字段 echo
/// 到 root，SubstructureNotifyMask 使本探针收到副本）。无 pong = hung，
/// 与 SendMessageTimeoutW(WM_NULL, SMTO_ABORTIFHUNG) 同语义。
bool pingWindow(Display *dpy, Atom pingAtom, Window win, int thresholdMs)
{
	const unsigned long token = g_pingToken.fetch_add(1);
	XEvent ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.display = dpy;
	ev.xclient.window = win;
	ev.xclient.message_type = pingAtom;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = long(token);		// 应用原样回传（echo 保真）
	ev.xclient.data.l[1] = long(win);		// 被测窗口自证
	if (XSendEvent(dpy, win, False, 0, &ev) == 0)
		return true;	// 发送失败按"不可探活"处理，不误报 hung

	QElapsedTimer wait;
	wait.start();
	for (;;) {
		XFlush(dpy);
		while (XPending(dpy) > 0) {
			XEvent got;
			XNextEvent(dpy, &got);
			if (got.type == ClientMessage
				&& got.xclient.message_type == pingAtom
				&& got.xclient.format == 32
				&& unsigned long(got.xclient.data.l[0]) == token)
				return true;
		}
		const qint64 remainMs = qint64(thresholdMs) - wait.elapsed();
		if (remainMs <= 0)
			return false;
		struct pollfd pfd = { XConnectionNumber(dpy), POLLIN, 0 };
		::poll(&pfd, 1, int(remainMs));
	}
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

void WindowFreezeProber::run()
{
	m_running = true;
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
	const Atom pingAtom = XInternAtom(dpy, "_NET_WM_PING", False);
	const Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
	// pong 回流：应用把 _NET_WM_PING 回发 root（SubstructureNotify 语义），
	// 选上该 mask 即收到副本（有无 WM 均成立——XSendEvent 按选择掩码广播）
	XSelectInput(dpy, root, SubstructureNotifyMask);

	// 三态状态机对齐 EventWatchdog：started（首检停滞）→ ongoing*（1s 节流）
	// → recovered（恢复）；目标消失收口为 freeze lost（"恢复"语义不诚实）
	bool freezing = false;
	qint64 startMs = 0;
	qint64 lastOngoingMs = 0;
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
			const QList<TargetWindow> targets =
				collectTargetWindows(dpy, root, pidSet, pidAtom);
			for (const TargetWindow &t : targets) {
				if (!pingWindow(dpy, pingAtom, t.win, m_thresholdMs)) {
					proc = TargetResolver::processNameOf(t.pid);
					if (proc.isEmpty())
						proc = QStringLiteral("pid:%1").arg(t.pid);
					hung = true;
					break;
				}
			}
		}

		const qint64 nowMs = clock.elapsed();
		if (freezing) {
			if (pids.isEmpty()) {
				freezing = false;
				QEW_LOG_WARN("[FreezeWatch] freeze lost receiver={:s} type=0",
							 recvName.toStdString());
			} else if (!hung) {
				freezing = false;
				QEW_LOG_WARN("[FreezeWatch] freeze recovered totalMs={} "
							 "receiver={:s} type=0 inProgress=false",
							 nowMs - startMs, recvName.toStdString());
			} else if (nowMs - lastOngoingMs >= 1000) {
				lastOngoingMs = nowMs;
				QEW_LOG_WARN("[FreezeWatch] freeze ongoing elapsedMs={} "
							 "receiver={:s} type=0 inProgress=false",
							 nowMs - startMs, recvName.toStdString());
			}
		} else if (hung) {
			freezing = true;
			startMs = nowMs;
			lastOngoingMs = nowMs;
			recvName = proc;
			// stalledMs 保守取阈值下界：外部探针只能保证"至少已停滞 threshold"
			QEW_LOG_WARN("[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
						 "receiver={:s} type=0 inProgress=false",
						 m_thresholdMs, m_thresholdMs, recvName.toStdString());
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
