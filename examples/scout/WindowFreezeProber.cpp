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

#else // 非 Windows：探针空实现（Scout 为 Windows 专属能力，沿 ModuleEnumerator 先例）

namespace qt_event_watcher {

WindowFreezeProber::WindowFreezeProber(const QString &, qint64, int, int, QObject *parent)
	: QThread(parent)
{
}
void WindowFreezeProber::stop() {}
void WindowFreezeProber::run() {}

} // namespace qt_event_watcher

#endif // Q_OS_WIN
