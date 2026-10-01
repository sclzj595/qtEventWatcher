#include "CpuSampler.h"

#include "TargetResolver.h"
#include "WatchLogMacros.h"

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qt_event_watcher {

CpuSampler::CpuSampler(const QString &targetName, qint64 targetPid,
					   int cpuThresholdPct, int runsNeeded, int intervalMs,
					   QObject *parent)
	: QObject(parent)
	, m_targetName(targetName)
	, m_targetPid(targetPid)
	, m_thresholdPct(cpuThresholdPct)
	, m_runsNeeded(runsNeeded > 0 ? runsNeeded : 1)
{
	m_timer.setInterval(intervalMs > 0 ? intervalMs : 250);
	connect(&m_timer, &QTimer::timeout, this, [this]() { sample(); });
	m_sampleClock.start();
}

void CpuSampler::start()
{
	m_timer.start();
}

void CpuSampler::stop()
{
	m_timer.stop();
}

void CpuSampler::sample()
{
	QList<qint64> pids = TargetResolver::resolvePids(m_targetName, m_targetPid);
	if (pids.isEmpty()) {
		// 目标消失：观测断点，episode 状态复位（重连后重新起算）
		m_streak = 0;
		m_streakDown = 0;
		m_emitted = false;
		m_lastTotal100ns.clear();
		return;
	}
	pids.append(TargetResolver::descendantPids(pids));	// 3 层后代（Tauri/WebView2 renderer 是孙进程）

	const qint64 elapsedMs = m_sampleClock.elapsed();
	m_sampleClock.restart();

	// 累计差分：kernel+user 均为进程生命周期累计值（100ns 单位），相减得拍内用量；
	// 新出现进程首拍只有基线无差分，自然跳过
	qint64 delta100ns = 0;
	QHash<qint64, qint64> current;
	for (qint64 pid : pids) {
		const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
		if (h == nullptr)
			continue;
		FILETIME ftCreate = {}, ftExit = {}, ftKernel = {}, ftUser = {};
		if (GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
			ULARGE_INTEGER k, u;
			k.LowPart = ftKernel.dwLowDateTime;		k.HighPart = ftKernel.dwHighDateTime;
			u.LowPart = ftUser.dwLowDateTime;		u.HighPart = ftUser.dwHighDateTime;
			const qint64 total = qint64(k.QuadPart + u.QuadPart);
			if (m_lastTotal100ns.contains(pid))
				delta100ns += total - m_lastTotal100ns.value(pid);
			current.insert(pid, total);
		}
		CloseHandle(h);
	}
	m_lastTotal100ns = current;		// 仅保留存活 pid，防泄漏

	// 单核口径：100ns → ms（÷10000）再除以拍间隔
	const double cpuPct = elapsedMs > 0
		? 100.0 * (double(delta100ns) / 10000.0) / double(elapsedMs) : 0.0;

	if (cpuPct >= m_thresholdPct) {
		m_streakDown = 0;
		if (++m_streak >= m_runsNeeded && !m_emitted) {
			m_emitted = true;
			QString proc = !m_targetName.isEmpty()
				? m_targetName : TargetResolver::processNameOf(m_targetPid);
			if (proc.isEmpty())
				proc = QStringLiteral("pid:%1").arg(m_targetPid);
			// 行格式对齐自监控 slow event（hydrateRecord 通用解析可用）；
			// event=cpuSpin / source=scout 诚实标注启发式来源与口径
			QEW_LOG_WARN("[EventWatcher] slow event receiver={:s} object={:s} "
						 "event=cpuSpin type=99 depth=0 costMs={:.1f} "
						 "exclusiveCostMs=0.000 curThread=0x0 recvThread=0x0 "
						 "match=true thresholdMs={} source=scout",
						 proc.toStdString(), proc.toStdString(),
						 cpuPct, m_thresholdPct);
		}
	} else {
		// 迟滞收口：连续 4 拍低于阈值才关闭 episode（采样噪声的单拍抖动
		// 不重置——实测 6s 单次忙转曾因单拍 <95% 被撕成 3 条告警）
		m_streak = 0;
		if (++m_streakDown >= 4) {
			m_streakDown = 0;
			m_emitted = false;
		}
	}
}

} // namespace qt_event_watcher

#else // 非 Windows：空实现（Scout 为 Windows 专属能力）

namespace qt_event_watcher {

CpuSampler::CpuSampler(const QString &, qint64, int, int, int, QObject *parent)
	: QObject(parent)
{
}
void CpuSampler::start() {}
void CpuSampler::stop() {}
void CpuSampler::sample() {}

} // namespace qt_event_watcher

#endif // Q_OS_WIN
