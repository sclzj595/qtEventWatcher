#pragma once

/// RadarDiscover - Scout V8 雷达发现器（header-only）
/// 全机"窗口归属 GUI 进程"发现：Windows = EnumWindows 可见顶层窗口单趟枚举
/// + 单次 Toolhelp 快照建 pid→name 全表；Linux = X11 顶层窗口枚举 +
/// _NET_WM_PID 归属（复用 X11Probe::collectTargetWindows 空 pid 集 = 全收契约）
/// + /proc/<pid>/comm 取名。仅负责发现，探活在 RadarScheduler。

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace qt_event_watcher {

namespace RadarDiscover {

/// 顶层窗口句柄 + 所属 pid（Windows=HWND / Linux=X11 Window，统一存 qint64）
struct RadarWindow
{
	qint64 handle = 0;
	qint64 pid = 0;
};

/// 可见顶层窗口单趟枚举（GUI 进程判定的原料；服务/后台进程无窗口自然排除）
inline QList<RadarWindow> sweepWindows()
{
	QList<RadarWindow> out;
	const auto cb = [](HWND hwnd, LPARAM lp) -> BOOL {
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != 0 && IsWindowVisible(hwnd)) {
			reinterpret_cast<QList<RadarWindow> *>(lp)->append(
				{ qint64(reinterpret_cast<qintptr>(hwnd)), qint64(pid) });
		}
		return TRUE;
	};
	EnumWindows(cb, reinterpret_cast<LPARAM>(&out));
	return out;
}

/// pid 集 → 进程名全表：单次 Toolhelp 快照（对齐 TargetResolver 快照口径，
/// 避免 per-pid 快照的 N 次全进程遍历）
inline QHash<qint64, QString> nameMapOf(const QList<qint64> &pids)
{
	QHash<qint64, QString> out;
	if (pids.isEmpty())
		return out;
	const QSet<qint64> want(pids.cbegin(), pids.cend());
	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return out;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			const qint64 pid = qint64(pe.th32ProcessID);
			if (want.contains(pid))
				out.insert(pid, QString::fromWCharArray(pe.szExeFile));
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return out;
}

} // namespace RadarDiscover

} // namespace qt_event_watcher

#elif defined(Q_OS_UNIX)	// Linux：X11 枚举（须 QEWT_SCOUT_X11，调用方先判宏）

#ifdef QEWT_SCOUT_X11

#include "TargetResolver.h"
#include "X11Probe.h"

namespace qt_event_watcher {

namespace RadarDiscover {

/// 顶层窗口句柄 + 所属 pid（与 Windows 版同构）
struct RadarWindow
{
	qint64 handle = 0;
	qint64 pid = 0;
};

/// X11 顶层窗口枚举 + _NET_WM_PID 归属（空 pid 集 = 全收契约；无 pid 属性
/// 的窗口不收，Qt/GTK 应用建窗时均写该属性）
inline QList<RadarWindow> sweepWindows(Display *dpy, Window root, Atom pidAtom)
{
	QList<RadarWindow> out;
	const QList<X11Probe::TargetWindow> targets =
		X11Probe::collectTargetWindows(dpy, root, QSet<qint64>(), pidAtom);
	for (const X11Probe::TargetWindow &t : targets)
		out.append({ qint64(t.win), t.pid });
	return out;
}

/// pid 集 → 进程名全表（/proc/<pid>/comm 逐个读取，代价与 pid 数线性）
inline QHash<qint64, QString> nameMapOf(const QList<qint64> &pids)
{
	QHash<qint64, QString> out;
	for (qint64 pid : pids)
		out.insert(pid, TargetResolver::processNameOf(pid));
	return out;
}

} // namespace RadarDiscover

} // namespace qt_event_watcher

#endif // QEWT_SCOUT_X11

#endif // Q_OS_WIN / Q_OS_UNIX
