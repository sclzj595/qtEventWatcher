#pragma once

/// X11Probe - Scout X11 探活共享原语（V8 从 WindowFreezeProber.cpp 抽出）
/// 仅在 QEWT_SCOUT_X11 编译单元内包含。WindowFreezeProber（T1 单目标）与
/// RadarScheduler（V8 全机雷达）共用：
///   - pingWindow：EWMH 标准 _NET_WM_PING 探活（WM_PROTOCOLS 封装 + token 防串扰）
///   - collectTargetWindows：顶层窗口枚举 + _NET_WM_PID 过滤（空 pid 集 = 全收）
/// 进程内共享单调 token（函数局部静态，Meyer 单例）：多探针并存时跨 TU 不串扰。

#include <poll.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <atomic>
#include <cstring>

#include <QElapsedTimer>
#include <QList>
#include <QSet>

namespace qt_event_watcher {
namespace X11Probe {

/// 单调 ping token：本进程 pid 作高位基座（WM 真实 ping 用 X 服务器时间戳，
/// 值域远低于 pid<<32），逐次 +1 防上一拍迟到 pong 污染本拍判定
inline std::atomic<unsigned long> &pingToken()
{
	static std::atomic<unsigned long> token{ static_cast<unsigned long>(::getpid()) };
	return token;
}

/// 目标顶层窗口（窗口 id + 所属 pid，procName 归因用）
struct TargetWindow
{
	Window win = None;
	qint64 pid = 0;
};

/// 根窗口 client 顶层窗口枚举：_NET_CLIENT_LIST（EWMH，有 WM 时权威）
/// ∪ 根窗口直接子窗口（Xvfb 等无 WM 环境兜底——应用顶层窗口即 root 子窗口）。
/// pids 非空按 _NET_WM_PID 过滤目标 pid 集；空集 = 全收（V8 雷达全机发现契约，
/// 仅含带 _NET_WM_PID 的窗口）。
inline QList<TargetWindow> collectTargetWindows(Display *dpy, Window root,
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

	// _NET_WM_PID 过滤（fmt=32 → long 数组，LP64 下每元素 8 字节）；
	// 空 pid 集 = 全收契约（雷达发现），无 pid 属性的窗口两侧均不收
	QList<TargetWindow> matched;
	for (Window w : candidates) {
		qint64 pidVal = 0;
		unsigned char *pidData = nullptr;
		if (XGetWindowProperty(dpy, w, pidAtom, 0, 1, False, XA_CARDINAL,
							   &type, &fmt, &n, &remain, &pidData) == Success
			&& type == XA_CARDINAL && fmt == 32 && n >= 1 && pidData != nullptr) {
			pidVal = qint64(*reinterpret_cast<const unsigned long *>(pidData));
			XFree(pidData);
		}
		if (pidVal > 0 && (pids.isEmpty() || pids.contains(pidVal)))
			matched.append({ w, pidVal });
	}
	return matched;
}

/// 单窗口探活：按 EWMH 规范发 _NET_WM_PING——WM 发 ping 的标准封装是
/// type=WM_PROTOCOLS、l[0]=_NET_WM_PING（子协议）、l[1]=timestamp、
/// l[2]=window（裸 type=_NET_WM_PING 消息 Qt xcb 不认，按未知协议忽略——
/// CI Run 20/21 实证）。event_mask=0 → 送达创建该窗口的 client，与 WM 探测
/// 同路径。阈值内等应用整包 echo 到 root 的 pong（Qt qxcbwindow.cpp 仅改
/// window=root，data 原样保留；探针选 StructureNotifyMask 收副本，见
/// RadarScheduler/WindowFreezeProber run() 注释）。无 pong = hung，与
/// WM_NULL/SMTO_ABORTIFHUNG 同语义。
inline bool pingWindow(Display *dpy, Atom wmProtocolsAtom, Atom pingAtom,
					   Window win, int thresholdMs)
{
	const unsigned long token = pingToken().fetch_add(1);
	XEvent ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.display = dpy;
	ev.xclient.window = win;
	ev.xclient.message_type = wmProtocolsAtom;	// 封装：WM_PROTOCOLS
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = long(pingAtom);		// 子协议 = _NET_WM_PING
	ev.xclient.data.l[1] = long(token);			// EWMH: timestamp 位（应用原样回传）
	ev.xclient.data.l[2] = long(win);			// EWMH: 被测窗口自证
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
				&& got.xclient.message_type == wmProtocolsAtom
				&& got.xclient.format == 32
				&& static_cast<unsigned long>(got.xclient.data.l[0]) == pingAtom
				&& static_cast<unsigned long>(got.xclient.data.l[1]) == token)
				return true;
		}
		const qint64 remainMs = qint64(thresholdMs) - wait.elapsed();
		if (remainMs <= 0)
			return false;
		struct pollfd pfd = { XConnectionNumber(dpy), POLLIN, 0 };
		::poll(&pfd, 1, int(remainMs));
	}
}

} // namespace X11Probe
} // namespace qt_event_watcher
