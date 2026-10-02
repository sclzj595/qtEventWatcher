#pragma once

/// TargetResolver - Scout S1 进程解析工具（header-only，Windows 专属）
/// --name/--pid → 目标 pid 集 + 后代进程枚举（Electron renderer 是直接子进程，
/// Tauri/WebView2 renderer 是孙进程，BFS 3 层全收），
/// 供 WindowFreezeProber（T1）与 CpuSampler（T1b）共用。

#include <QString>
#include <QList>
#include <QHash>

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

/// 后代进程枚举：单轮快照建 parent→children 映射 + BFS 逐层展开（默认 3 层）。
/// Tauri/WebView2 场景的渲染进程是目标的孙进程
/// （app.exe → msedgewebview2.exe 浏览器进程 → renderer/GPU 孙进程），
/// 直接子进程枚举会漏采 busy 的 renderer；3 层覆盖 app→宿主→渲染→内嵌子级。
inline QList<qint64> descendantPids(const QList<qint64> &roots, int maxDepth = 3)
{
	QList<qint64> result;
	if (roots.isEmpty() || maxDepth <= 0)
		return result;

	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return result;

	QHash<qint64, QList<qint64>> byParent;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			byParent[qint64(pe.th32ParentProcessID)].append(qint64(pe.th32ProcessID));
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);

	QList<qint64> frontier = roots;
	for (int depth = 0; depth < maxDepth && !frontier.isEmpty(); ++depth) {
		QList<qint64> next;
		for (qint64 pid : frontier) {
			const auto it = byParent.constFind(pid);
			if (it == byParent.constEnd())
				continue;
			for (qint64 child : it.value()) {
				// 根与已收结果去重（防 pid 环；进程数量级小，线性查足够）
				if (!roots.contains(child) && !result.contains(child)) {
					result.append(child);
					next.append(child);
				}
			}
		}
		frontier = next;
	}
	return result;
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

#elif defined(Q_OS_UNIX)	// Linux：/proc 遍历（API 语义对齐 Windows 版，调用方零感知）

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace qt_event_watcher {

namespace TargetResolver {

namespace {

/// /proc 下数字目录枚举 → pid 列表（对齐 Windows 版 Toolhelp 快照口径）
inline QList<qint64> enumeratePids()
{
	QList<qint64> pids;
	const auto entries = QDir(QStringLiteral("/proc"))
							 .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString &e : entries) {
		bool ok = false;
		const qint64 pid = e.toLongLong(&ok);
		if (ok)
			pids.append(pid);
	}
	return pids;
}

/// /proc/<pid>/comm 单行读（进程名 basename 语义，对齐 Windows 版 szExeFile）。
/// 内核截断边界：comm 最长 15 字符——超长进程名按前缀匹配场景需调用方自知。
inline QString commOf(qint64 pid)
{
	QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	return QString::fromUtf8(f.readAll()).trimmed();
}

/// /proc/<pid>/stat 解析 ppid（field 4）。comm（field 2）可含空格与 ')'——
/// 以最后一个 ')' 锚定 comm 结束（其后字段为数字不含括号），再切 state(3)、
/// ppid(4)；按 comm 内含 ')' 直接切字段会错位。
inline bool statPpidOf(qint64 pid, qint64 *ppidOut)
{
	QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QString data = QString::fromUtf8(f.readAll());
	const int close = data.lastIndexOf(QLatin1Char(')'));
	if (close < 0 || close + 2 >= data.size())
		return false;
	const QStringList rest = data.mid(close + 2).split(QLatin1Char(' '));
	if (rest.size() < 2)
		return false;
	bool ok = false;
	const qint64 ppid = rest.at(1).toLongLong(&ok);
	if (!ok)
		return false;
	if (ppidOut)
		*ppidOut = ppid;
	return true;
}

} // namespace

/// 目标解析：name 非空按 /proc/<pid>/comm 全量匹配（大小写不敏感）；
/// 否则校验 pid 存活（/proc/<pid> 存在性探活，对齐 Windows 版 OpenProcess
/// LIMITED 权限探活语义）。目标不存在返回空表（探针据此进入"目标消失"分支）。
inline QList<qint64> resolvePids(const QString &name, qint64 pid)
{
	QList<qint64> pids;
	if (pid > 0) {
		if (QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid)))
			pids.append(pid);
		return pids;
	}
	if (name.isEmpty())
		return pids;
	for (qint64 p : enumeratePids()) {
		if (commOf(p).compare(name, Qt::CaseInsensitive) == 0)
			pids.append(p);
	}
	return pids;
}

/// 后代进程枚举：单轮 /proc 快照建 parent→children 映射 + BFS 逐层展开
/// （默认 3 层，去重防 pid 环）——与 Windows 版同一套展开逻辑。
inline QList<qint64> descendantPids(const QList<qint64> &roots, int maxDepth = 3)
{
	QList<qint64> result;
	if (roots.isEmpty() || maxDepth <= 0)
		return result;

	QHash<qint64, QList<qint64>> byParent;
	for (qint64 pid : enumeratePids()) {
		qint64 ppid = 0;
		if (statPpidOf(pid, &ppid))
			byParent[ppid].append(pid);
	}

	QList<qint64> frontier = roots;
	for (int depth = 0; depth < maxDepth && !frontier.isEmpty(); ++depth) {
		QList<qint64> next;
		for (qint64 pid : frontier) {
			const auto it = byParent.constFind(pid);
			if (it == byParent.constEnd())
				continue;
			for (qint64 child : it.value()) {
				if (!roots.contains(child) && !result.contains(child)) {
					result.append(child);
					next.append(child);
				}
			}
		}
		frontier = next;
	}
	return result;
}

/// pid → 进程名（找不到返回空串；用于 receiver 字段取名）
inline QString processNameOf(qint64 pid)
{
	return commOf(pid);
}

} // namespace TargetResolver

} // namespace qt_event_watcher

#endif // Q_OS_WIN / Q_OS_UNIX
