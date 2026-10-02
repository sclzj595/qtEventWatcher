#include "RadarScheduler.h"

#include "RadarDiscover.h"
#include "WatchLogMacros.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qt_event_watcher {

namespace {

/// per-target 三态状态机（started → ongoing*(1s 节流) → recovered/lost）
struct TargetState
{
	bool freezing = false;
	qint64 startMs = 0;
	qint64 lastOngoingMs = 0;
	QString recvName;
};

/// receiver 组名：name@pid 实例级唯一（freeze 三态配对按 receiver 键合，
/// 同名多开须互不串扰；无名兜底 pid:N）
QString receiverOf(const QString &name, qint64 pid)
{
	return name.isEmpty() ? QStringLiteral("pid:%1").arg(pid)
						  : QStringLiteral("%1@%2").arg(name).arg(pid);
}

/// 排除表命中：进程名子串大小写不敏感匹配
bool excluded(const QString &name, const QStringList &patterns)
{
	for (const QString &p : patterns) {
		if (!p.isEmpty() && name.contains(p, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

/// 进程真实存活判定：OpenProcess 对"已终止但句柄未释放"的进程对象仍成功
/// （探活"成功"撞 kill 竞态时正是这种状态），须等内核对象判信号态——
/// 100ms 宽限等 TerminateProcess 完成对象置位（kill 恰落在探活在途时，
/// SendMessage 返回后微秒级检查会撞上拆除中窗口的 WAIT_TIMEOUT 假活）
bool aliveOf(qint64 pid)
{
	const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
								 FALSE, DWORD(pid));
	if (h == nullptr)
		return false;
	const DWORD w = WaitForSingleObject(h, 100);
	CloseHandle(h);
	return w == WAIT_TIMEOUT;
}

} // namespace

RadarScheduler::RadarScheduler(int thresholdMs, int intervalMs, qint64 selfPid,
							   const QStringList &excludeNames, QObject *parent)
	: QThread(parent)
	, m_thresholdMs(thresholdMs)
	, m_intervalMs(intervalMs)
	, m_selfPid(selfPid)
	, m_excludeNames(excludeNames)
	, m_running(false)
{
}

void RadarScheduler::stop()
{
	m_running = false;
}

void RadarScheduler::run()
{
	m_running = true;
	QElapsedTimer clock;
	clock.start();

	// 进程名缓存：只对新出现 pid 解析（nameMapOf 单次快照建全表），消失项随拍清理
	QHash<qint64, QString> nameCache;

	// 三态状态机 per-target；目标消失收口 freeze lost（"恢复"语义不诚实）
	QHash<qint64, TargetState> targets;

	while (m_running) {
		QElapsedTimer tick;
		tick.start();

		// 发现：可见顶层窗口 → pid 集 + 句柄归组（跳过自身 pid 双保险）
		const QList<RadarDiscover::RadarWindow> wins = RadarDiscover::sweepWindows();
		QSet<qint64> alive;
		QHash<qint64, QList<qint64>> byPid;
		for (const RadarDiscover::RadarWindow &w : wins) {
			if (w.pid <= 0 || w.pid == m_selfPid)
				continue;
			alive.insert(w.pid);
			byPid[w.pid].append(w.handle);
		}
		QList<qint64> alivePids = alive.values();
		std::sort(alivePids.begin(), alivePids.end());	// 确定性轮询序

		// 增量管理：消失目标收口 + 名字缓存清理
		for (auto it = targets.begin(); it != targets.end();) {
			if (!alive.contains(it.key())) {
				if (it.value().freezing) {
					QEW_LOG_WARN("[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
								 it.value().recvName.toStdString());
				}
				it = targets.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = nameCache.begin(); it != nameCache.end();) {
			if (!alive.contains(it.key()))
				it = nameCache.erase(it);
			else
				++it;
		}
		QList<qint64> unknownPids;
		for (qint64 pid : alivePids) {
			if (!nameCache.contains(pid))
				unknownPids.append(pid);
		}
		if (!unknownPids.isEmpty()) {
			// QHash::unite 是 Qt5 专属（Qt6 移除），显式 insert 兼容双版本
			const QHash<qint64, QString> resolved = RadarDiscover::nameMapOf(unknownPids);
			for (auto it = resolved.cbegin(); it != resolved.cend(); ++it)
				nameCache.insert(it.key(), it.value());
		}

		// 逐目标顺序探活（stop 在循环内检查，退出延迟 ≈ 单窗口探活上界）
		for (qint64 pid : alivePids) {
			if (!m_running)
				break;
			const QString name = nameCache.value(pid);
			if (excluded(name, m_excludeNames))
				continue;
			bool hung = false;
			for (qint64 h : byPid.value(pid)) {
				DWORD_PTR result = 0;
				// WM_NULL 探活：同 WindowFreezeProber（任务管理器"未响应"同源）
				const DWORD_PTR ret = SendMessageTimeoutW(
					reinterpret_cast<HWND>(static_cast<quintptr>(h)), WM_NULL, 0, 0,
					SMTO_ABORTIFHUNG, DWORD(m_thresholdMs), &result);
				if (ret == 0) {
					hung = true;
					break;
				}
				if (!m_running)
					break;
			}

			TargetState &st = targets[pid];
			const qint64 nowMs = clock.elapsed();
			if (st.freezing) {
				if (!hung) {
					st.freezing = false;
					// 探活"成功"可能撞上目标消亡竞态（消息送达瞬间进程被终止，
					// SendMessageTimeout 对垂死窗口返回非 0）——recovered 语义
					// 要求接收方仍在，死进程一律收口 freeze lost
					if (!aliveOf(pid)) {
						QEW_LOG_WARN("[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
									 st.recvName.toStdString());
					} else {
						QEW_LOG_WARN("[FreezeWatch] freeze recovered totalMs={} "
									 "receiver={:s} type=0 inProgress=false radar=1",
									 nowMs - st.startMs, st.recvName.toStdString());
					}
				} else if (nowMs - st.lastOngoingMs >= 1000) {
					st.lastOngoingMs = nowMs;
					QEW_LOG_WARN("[FreezeWatch] freeze ongoing elapsedMs={} "
								 "receiver={:s} type=0 inProgress=false radar=1",
								 nowMs - st.startMs, st.recvName.toStdString());
				}
			} else if (hung) {
				st.freezing = true;
				st.startMs = nowMs;
				st.lastOngoingMs = nowMs;
				st.recvName = receiverOf(name, pid);
				// stalledMs 保守取阈值下界：外部探针只能保证"至少已停滞 threshold"
				QEW_LOG_WARN("[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 m_thresholdMs, m_thresholdMs, st.recvName.toStdString());
			}
		}

		// 分片睡眠保证 stop 响应性（hung 拍 tick 本身可能占满 threshold×k）
		int remain = m_intervalMs - int(tick.elapsed());
		while (remain > 0 && m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
		}
	}
}

} // namespace qt_event_watcher

#elif defined(Q_OS_UNIX)	// Linux：X11 _NET_WM_PING 全机雷达（同 T1 探活原语）

#ifdef QEWT_SCOUT_X11

#include "X11Probe.h"

namespace qt_event_watcher {

namespace {

/// per-target 三态状态机（同 Windows 版）
struct TargetState
{
	bool freezing = false;
	qint64 startMs = 0;
	qint64 lastOngoingMs = 0;
	QString recvName;
};

/// receiver 组名：name@pid 实例级唯一（同 Windows 版）
QString receiverOf(const QString &name, qint64 pid)
{
	return name.isEmpty() ? QStringLiteral("pid:%1").arg(pid)
						  : QStringLiteral("%1@%2").arg(name).arg(pid);
}

/// 排除表命中（同 Windows 版）
bool excluded(const QString &name, const QStringList &patterns)
{
	for (const QString &p : patterns) {
		if (!p.isEmpty() && name.contains(p, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

/// 进程真实存活判定：/proc/<pid> 对 zombie（已终止待收尸）进程仍存在，
/// 须读 stat 的 state 字段排除 Z 态（解析锚定同 TargetResolver::statPpidOf）
bool aliveOf(qint64 pid)
{
	QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QString data = QString::fromUtf8(f.readAll());
	const int close = data.lastIndexOf(QLatin1Char(')'));
	if (close < 0 || close + 2 >= data.size())
		return false;
	const QStringList fields = data.mid(close + 2).split(QLatin1Char(' '));
	if (fields.isEmpty())
		return false;
	return fields.first() != QStringLiteral("Z");
}

} // namespace

RadarScheduler::RadarScheduler(int thresholdMs, int intervalMs, qint64 selfPid,
							   const QStringList &excludeNames, QObject *parent)
	: QThread(parent)
	, m_thresholdMs(thresholdMs)
	, m_intervalMs(intervalMs)
	, m_selfPid(selfPid)
	, m_excludeNames(excludeNames)
	, m_running(false)
{
}

void RadarScheduler::stop()
{
	m_running = false;
}

void RadarScheduler::run()
{
	m_running = true;
	QElapsedTimer clock;
	clock.start();
	Display *dpy = XOpenDisplay(nullptr);
	if (dpy == nullptr) {
		// Wayland native / 无 X display：外部探针无等价协议，诚实降级
		// （CDP 探针不受影响）；轮询空转维持线程生命周期
		QEW_LOG_WARN("[FreezeWatch] X11 display unavailable, radar freeze probe "
					 "disabled type=0 radar=1");
		int remain = m_intervalMs;
		while (m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
			if (remain <= 0)
				remain = m_intervalMs;
		}
		return;
	}

	// 雷达长驻、目标进出频繁：枚举与探活间隙目标窗口随时可能消亡，
	// BadWindow 会触发 Xlib 默认错误处理器直接终止进程——装 no-op handler，
	// 出错调用返回 0 由调用方按"发送失败/不可探活"分支处理
	XSetErrorHandler([](Display *, XErrorEvent *) -> int { return 0; });

	Window root = DefaultRootWindow(dpy);
	const Atom wmProtocolsAtom = XInternAtom(dpy, "WM_PROTOCOLS", False);
	const Atom pingAtom = XInternAtom(dpy, "_NET_WM_PING", False);
	const Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);

	// pong 回流：同 T1——应用 echo 到 root，回发 mask 含 StructureNotify，
	// 探针选 root 的 StructureNotifyMask 必达（掩码位辨析见 T1 注释）
	XSelectInput(dpy, root, StructureNotifyMask);

	QHash<qint64, QString> nameCache;
	QHash<qint64, TargetState> targets;

	while (m_running) {
		QElapsedTimer tick;
		tick.start();

		const QList<RadarDiscover::RadarWindow> wins
			= RadarDiscover::sweepWindows(dpy, root, pidAtom);
		QSet<qint64> alive;
		QHash<qint64, QList<qint64>> byPid;
		for (const RadarDiscover::RadarWindow &w : wins) {
			if (w.pid <= 0 || w.pid == m_selfPid)
				continue;
			alive.insert(w.pid);
			byPid[w.pid].append(w.handle);
		}
		QList<qint64> alivePids = alive.values();
		std::sort(alivePids.begin(), alivePids.end());

		for (auto it = targets.begin(); it != targets.end();) {
			if (!alive.contains(it.key())) {
				if (it.value().freezing) {
					QEW_LOG_WARN("[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
								 it.value().recvName.toStdString());
				}
				it = targets.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = nameCache.begin(); it != nameCache.end();) {
			if (!alive.contains(it.key()))
				it = nameCache.erase(it);
			else
				++it;
		}
		QList<qint64> unknownPids;
		for (qint64 pid : alivePids) {
			if (!nameCache.contains(pid))
				unknownPids.append(pid);
		}
		if (!unknownPids.isEmpty()) {
			// QHash::unite 是 Qt5 专属（Qt6 移除），显式 insert 兼容双版本
			const QHash<qint64, QString> resolved = RadarDiscover::nameMapOf(unknownPids);
			for (auto it = resolved.cbegin(); it != resolved.cend(); ++it)
				nameCache.insert(it.key(), it.value());
		}

		for (qint64 pid : alivePids) {
			if (!m_running)
				break;
			const QString name = nameCache.value(pid);
			if (excluded(name, m_excludeNames))
				continue;
			// 逐窗口探活，首个无 pong 即 hung（早退语义同 T1 Linux 版）
			bool hung = false;
			for (qint64 h : byPid.value(pid)) {
				if (!X11Probe::pingWindow(dpy, wmProtocolsAtom, pingAtom,
										  Window(h), m_thresholdMs)) {
					hung = true;
					break;
				}
				if (!m_running)
					break;
			}

			TargetState &st = targets[pid];
			const qint64 nowMs = clock.elapsed();
			if (st.freezing) {
				if (!hung) {
					st.freezing = false;
					// 探活"成功"撞目标消亡竞态同 Windows 版（XSendEvent 对垂死
					// 窗口返回 0 → pingWindow 按"不可探活"放行非 hung）——
					// recovered 语义要求接收方仍在，死进程一律收口 freeze lost
					if (!aliveOf(pid)) {
						QEW_LOG_WARN("[FreezeWatch] freeze lost receiver={:s} type=0 radar=1",
									 st.recvName.toStdString());
					} else {
						QEW_LOG_WARN("[FreezeWatch] freeze recovered totalMs={} "
									 "receiver={:s} type=0 inProgress=false radar=1",
									 nowMs - st.startMs, st.recvName.toStdString());
					}
				} else if (nowMs - st.lastOngoingMs >= 1000) {
					st.lastOngoingMs = nowMs;
					QEW_LOG_WARN("[FreezeWatch] freeze ongoing elapsedMs={} "
								 "receiver={:s} type=0 inProgress=false radar=1",
								 nowMs - st.startMs, st.recvName.toStdString());
				}
			} else if (hung) {
				st.freezing = true;
				st.startMs = nowMs;
				st.lastOngoingMs = nowMs;
				st.recvName = receiverOf(name, pid);
				QEW_LOG_WARN("[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
							 "receiver={:s} type=0 inProgress=false radar=1",
							 m_thresholdMs, m_thresholdMs, st.recvName.toStdString());
			}
		}

		int remain = m_intervalMs - int(tick.elapsed());
		while (remain > 0 && m_running) {
			msleep(remain > 50 ? 50 : remain);
			remain -= 50;
		}
	}

	XCloseDisplay(dpy);
}

} // namespace qt_event_watcher

#else // QEWT_SCOUT_X11 未定义：X11 依赖缺失，雷达禁用（CDP 不受影响）

namespace qt_event_watcher {

RadarScheduler::RadarScheduler(int, int, qint64, const QStringList &, QObject *parent)
	: QThread(parent)
{
}
void RadarScheduler::stop() {}
void RadarScheduler::run() {}

} // namespace qt_event_watcher

#endif // QEWT_SCOUT_X11

#else // 其他平台：雷达空实现

namespace qt_event_watcher {

RadarScheduler::RadarScheduler(int, int, qint64, const QStringList &, QObject *parent)
	: QThread(parent)
{
}
void RadarScheduler::stop() {}
void RadarScheduler::run() {}

} // namespace qt_event_watcher

#endif // Q_OS_WIN / Q_OS_UNIX
