#pragma once

/// WindowFreezeProber - Scout T1 跨栈窗口冻结探针（V7 S1）
/// 进程外轮询目标进程顶层窗口的消息泵响应性（SendMessageTimeout WM_NULL，
/// 阈值自控——IsHungAppWindow 内置 5s 不可调故弃用），三态告警行逐字段对齐
/// EventWatchdog 冻结语义（started stalledMs / ongoing elapsedMs / recovered
/// totalMs）——Qt/Electron/WPF/Win32 通用，被监控程序无需任何配合。
/// 常驻 worker 线程：hung 窗口上 SendMessageTimeout 最长阻塞 threshold，
/// 不能占用主线程（沿 EventWatchdog 线程先例）。

#include <QThread>
#include <QList>
#include <QString>
#include <atomic>

#include "ScoutAlarmEmitter.h"

namespace qt_event_watcher {

class WindowFreezeProber : public QThread
{
public:
	/// targetName 非空按进程名全量匹配（含多开）；否则按 targetPid 单进程
	WindowFreezeProber(const QString &targetName, qint64 targetPid,
					   int thresholdMs, int intervalMs, QObject *parent = nullptr);

	void stop();

protected:
	void run() override;

private:
	ScoutAlarmEmitter m_alarm;		///< 告警风暴抑制门（docs/34 R3c）
#ifdef Q_OS_WIN
	/// 枚举目标 pid 集的可见顶层窗口逐个探活；命中 hung 窗口返回 true 并
	/// 填 procName（窗口所属进程名，作 receiver 字段）
	bool probeTopWindows(const QList<qint64> &pids, QString &procName) const;
#endif

	QString m_targetName;
	qint64 m_targetPid = 0;
	int m_thresholdMs = 0;
	int m_intervalMs = 0;
	std::atomic_bool m_running{ false };
};

} // namespace qt_event_watcher
