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
	, m_episode(cpuThresholdPct, runsNeeded)
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
		m_episode.reset();
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

	const ProbeLogic::CpuEvent ev = m_episode.onTick(cpuPct);
	if (ev.kind == ProbeLogic::CpuEvent::Spin) {
		QString proc = !m_targetName.isEmpty()
			? m_targetName : TargetResolver::processNameOf(m_targetPid);
		if (proc.isEmpty())
			proc = QStringLiteral("pid:%1").arg(m_targetPid);
		// 行格式对齐自监控 slow event（hydrateRecord 通用解析可用）；
		// event=cpuSpin / source=scout 诚实标注启发式来源与口径
		m_alarm.emitAlarm("cpu:" + proc.toStdString(),
					 "[EventWatcher] slow event receiver={:s} object={:s} "
					 "event=cpuSpin type=99 depth=0 costMs={:.1f} "
					 "exclusiveCostMs=0.000 curThread=0x0 recvThread=0x0 "
					 "match=true thresholdMs={} source=scout",
					 proc.toStdString(), proc.toStdString(),
					 ev.cpuPct, m_episode.thresholdPct);
	}
}

} // namespace qt_event_watcher

#elif defined(Q_OS_UNIX)	// Linux：/proc/<pid>/stat utime+stime 差分，口径对齐 Windows 版

#include "WatchLogMacros.h"

#include <unistd.h>

#include <QFile>
#include <QStringList>

namespace qt_event_watcher {

namespace {

/// /proc/<pid>/stat field 14+15（utime+stime，tick）→ 累计 CPU 毫秒。
/// comm（field 2）可含空格与 ')'——以最后一个 ')' 锚定 comm 结束后再切字段
/// （其后字段为数字不含括号），否则进程名带空格时 utime/stime 错位。
/// ')' 后序列：state(3) ppid(4) ... utime(14)→下标11 stime(15)→下标12。
bool cpuTimeMsOf(qint64 pid, double *outMs)
{
	QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QString data = QString::fromUtf8(f.readAll());
	const int close = data.lastIndexOf(QLatin1Char(')'));
	if (close < 0 || close + 2 >= data.size())
		return false;
	const QStringList fields = data.mid(close + 2).split(QLatin1Char(' '));
	if (fields.size() < 13)
		return false;
	bool okU = false;
	bool okS = false;
	const double utime = fields.at(11).toDouble(&okU);
	const double stime = fields.at(12).toDouble(&okS);
	if (!okU || !okS)
		return false;
	static const double ticksToMs = 1000.0 / double(sysconf(_SC_CLK_TCK));
	*outMs = (utime + stime) * ticksToMs;
	return true;
}

} // namespace

CpuSampler::CpuSampler(const QString &targetName, qint64 targetPid,
					   int cpuThresholdPct, int runsNeeded, int intervalMs,
					   QObject *parent)
	: QObject(parent)
	, m_targetName(targetName)
	, m_targetPid(targetPid)
	, m_episode(cpuThresholdPct, runsNeeded)
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
		m_episode.reset();
		m_lastCpuMs.clear();
		return;
	}
	pids.append(TargetResolver::descendantPids(pids));	// 3 层后代（Electron/WebView2 渲染进程树同覆盖）

	const qint64 elapsedMs = m_sampleClock.elapsed();
	m_sampleClock.restart();

	// 累计差分：utime+stime 是进程生命周期累计值（tick 换算 ms），相减得拍内
	// 用量；新出现进程首拍只有基线无差分，自然跳过（与 Windows 版同口径）
	double deltaMs = 0.0;
	QHash<qint64, double> current;
	for (qint64 pid : pids) {
		double cpuMs = 0.0;
		if (!cpuTimeMsOf(pid, &cpuMs))
			continue;
		if (m_lastCpuMs.contains(pid))
			deltaMs += cpuMs - m_lastCpuMs.value(pid);
		current.insert(pid, cpuMs);
	}
	m_lastCpuMs = current;		// 仅保留存活 pid，防泄漏

	// 单核口径：拍内 CPU 毫秒 / 拍间隔
	const double cpuPct = elapsedMs > 0
		? 100.0 * deltaMs / double(elapsedMs) : 0.0;

	const ProbeLogic::CpuEvent ev = m_episode.onTick(cpuPct);
	if (ev.kind == ProbeLogic::CpuEvent::Spin) {
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
					 ev.cpuPct, m_episode.thresholdPct);
	}
}

} // namespace qt_event_watcher

#else // 其他平台：空实现（Scout 探针未覆盖的平台）

namespace qt_event_watcher {

CpuSampler::CpuSampler(const QString &, qint64, int, int, int, QObject *parent)
	: QObject(parent)
{
}
void CpuSampler::start() {}
void CpuSampler::stop() {}
void CpuSampler::sample() {}

} // namespace qt_event_watcher

#endif // Q_OS_WIN / Q_OS_UNIX
