#pragma once

/// TargetResolver - Scout S1 进程解析工具（header-only，Windows 专属）
/// --name/--pid → 目标 pid 集 + 直接子进程枚举（Electron renderer 即子进程），
/// 供 WindowFreezeProber（T1）与 CpuSampler（T1b）共用。

#include <QString>
#include <QList>

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>

namespace qt_event_watcher {

namespace TargetResolver {

/// 目标解析：name 非空按进程名全量匹配（大小写不敏感）；否则校验 pid 存活。
/// 目标不存在返回空表（探针据此进入"目标消失"分支）。
inline QList<qint64> resolvePids(const QString &name, qint64 pid)
{
	QList<qint64> pids;
	if (pid > 0) {
		// pid 模式：OpenProcess 探活（LIMITED 权限即可，覆盖多数受保护进程）
		if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid))) {
			CloseHandle(h);
			pids.append(pid);
		}
		return pids;
	}
	if (name.isEmpty())
		return pids;

	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return pids;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			const QString exe = QString::fromWCharArray(pe.szExeFile);
			if (exe.compare(name, Qt::CaseInsensitive) == 0)
				pids.append(qint64(pe.th32ProcessID));
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return pids;
}

/// 直接子进程枚举（一轮快照；Electron renderer/GPU 即目标之子）
inline QList<qint64> childPids(const QList<qint64> &parents)
{
	QList<qint64> children;
	if (parents.isEmpty())
		return children;

	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return children;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			if (parents.contains(qint64(pe.th32ParentProcessID)))
				children.append(qint64(pe.th32ProcessID));
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return children;
}

/// pid → 进程名（找不到返回空串；用于 receiver 字段取名）
inline QString processNameOf(qint64 pid)
{
	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return QString();
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	QString name;
	if (Process32FirstW(snap, &pe)) {
		do {
			if (qint64(pe.th32ProcessID) == pid) {
				name = QString::fromWCharArray(pe.szExeFile);
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return name;
}

} // namespace TargetResolver

} // namespace qt_event_watcher

#endif // Q_OS_WIN
