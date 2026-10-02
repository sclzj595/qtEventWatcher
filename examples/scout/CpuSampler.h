#pragma once

/// CpuSampler - Scout T1b CPU 启发探针（V7 S1）
/// GetProcessTimes 采样目标进程 + 直接子进程（Electron renderer/GPU 即子进程），
/// 单核口径 cpu% = Δ(kernel+user) / Δt；连续 runs 拍超阈值发一条 slow event 行
/// （event=cpuSpin source=scout 诚实标注来源），episode 内只发一条、回落重置。
/// 与 T1 互补：T1 测"卡死"（消息泵停摆），T1b 测"忙死"（窗口仍响应但单核满转）。

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QTimer>

#include "ProbeLogic.h"
#include "ScoutAlarmEmitter.h"

namespace qt_event_watcher {

class CpuSampler : public QObject
{
	Q_OBJECT
public:
	/// targetName 非空按进程名匹配（含多开+子进程）；否则 targetPid + 子进程
	CpuSampler(const QString &targetName, qint64 targetPid,
			   int cpuThresholdPct, int runsNeeded, int intervalMs,
			   QObject *parent = nullptr);

	void start();
	void stop();

private:
	void sample();

	QString m_targetName;
	qint64 m_targetPid = 0;
	ProbeLogic::CpuEpisodeTracker m_episode;		///< episode 迟滞纯逻辑（docs/34 R1）
	ScoutAlarmEmitter m_alarm;						///< 告警风暴抑制门（docs/34 R3c）
	QTimer m_timer;
	QElapsedTimer m_sampleClock;					///< 相邻两拍间隔（cpu% 分母）
#if defined(Q_OS_WIN)
	QHash<qint64, qint64> m_lastTotal100ns;			///< pid → 上拍 kernel+user 累计（100ns 单位）
#elif defined(Q_OS_UNIX)
	QHash<qint64, double> m_lastCpuMs;				///< pid → 上拍 utime+stime 累计（ms；/proc stat tick 换算）
#endif
};

} // namespace qt_event_watcher
