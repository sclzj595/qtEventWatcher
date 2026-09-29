#include "EventWatchdog.h"

#include "WatchConfig.h"
#include "WatchLogMacros.h"

#include <QEvent>
#include <QObject>

#include <chrono>
#include <cstring>

namespace qt_event_watcher
{

namespace
{

std::int64_t steadyNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

EventWatchdog::EventWatchdog(const WatchConfig* config)
	: m_config(config)
	, m_ownerThreadId(std::this_thread::get_id())	// 构造线程 = 心跳线程（CusApplication 主线程）
{
}

EventWatchdog::~EventWatchdog()
{
	stop();
}

void EventWatchdog::start()
{
	bool expected = false;
	if (!m_started.compare_exchange_strong(expected, true))
		return;

	// 心跳基线 = 启动时刻：主线程 QTimer 尚未首跳时避免启动即误报
	m_lastBeatMs.store(steadyNowMs(), std::memory_order_release);
	m_running.store(true, std::memory_order_release);
	m_thread = std::thread(&EventWatchdog::run, this);
}

void EventWatchdog::stop()
{
	if (!m_started.exchange(false))
		return;

	m_running.store(false, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();
}

void EventWatchdog::beat()
{
	m_lastBeatMs.store(steadyNowMs(), std::memory_order_release);
}

EventWatchdog::EventSnapshot EventWatchdog::snapshotCopy() const
{
	std::lock_guard<std::mutex> lock(m_snapshotMutex);
	return m_snapshot;
}

void EventWatchdog::eventStarted(const QObject* receiver, QEvent* event)
{
	// 取证快照只属于心跳线程（主线程）：CusApplication::notify 是全进程共用
	// 的虚函数，worker 线程（如 moveToThread 的定时器）派发事件同样会进入，
	// 若不隔离会用无关事件覆盖"主线程卡在哪个事件"的取证现场
	if (std::this_thread::get_id() != m_ownerThreadId)
		return;

	// 开关实时查询：关闭时零快照成本（一次读锁），热更新即时生效
	if (m_config == nullptr || !m_config->isWatchEnabled(WatchConfig::WatchFreeze))
		return;

	EventSnapshot snap;
	snap.inProgress = true;
	snap.eventType = event != nullptr ? static_cast<int>(event->type()) : -1;
	if (receiver != nullptr && receiver->metaObject() != nullptr) {
		const char* name = receiver->metaObject()->className();
		std::strncpy(snap.receiverClass, name, sizeof(snap.receiverClass) - 1);
	} else {
		std::strncpy(snap.receiverClass, "(unknown)", sizeof(snap.receiverClass) - 1);
	}

	std::lock_guard<std::mutex> lock(m_snapshotMutex);
	m_snapshot = snap;
}

void EventWatchdog::eventFinished()
{
	if (std::this_thread::get_id() != m_ownerThreadId)
		return;	// 非心跳线程（同 eventStarted）

	if (m_config == nullptr || !m_config->isWatchEnabled(WatchConfig::WatchFreeze))
		return;

	std::lock_guard<std::mutex> lock(m_snapshotMutex);
	m_snapshot.inProgress = false;
}

void EventWatchdog::run()
{
	bool freezing = false;
	std::int64_t freezeStartMs = 0;		// = 最后一次心跳时刻（恢复时 totalMs 精确）
	std::int64_t lastOngoingMs = 0;
	std::int64_t lastSeenBeatMs = m_lastBeatMs.load(std::memory_order_acquire);

	while (m_running.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));

		// 开关每轮实时查询（热更新生效点）；关闭时同步基线与快照，
		// 重新开启不误报陈旧冻结；冻结中关闭则静默收尾
		if (!m_config->isWatchEnabled(WatchConfig::WatchFreeze)) {
			if (freezing)	freezing = false;
			lastSeenBeatMs = m_lastBeatMs.load(std::memory_order_acquire);
			m_lastBeatMs.store(steadyNowMs(), std::memory_order_release);
			continue;
		}

		const std::int64_t nowMs = steadyNowMs();
		const std::int64_t beatAt = m_lastBeatMs.load(std::memory_order_acquire);
		// 恢复判定以"心跳前进"为准（ANR 正解）：beat 时间戳变化 = 主线程
		// 事件循环已恢复派发，不依赖心跳节拍精度
		const bool beatAdvanced = beatAt != lastSeenBeatMs;
		lastSeenBeatMs = beatAt;

		if (!freezing) {
			if (!beatAdvanced && nowMs - beatAt >= m_config->freezeThresholdMs()) {
				freezing = true;
				freezeStartMs = beatAt;
				lastOngoingMs = nowMs;
				const EventSnapshot snap = snapshotCopy();
				QEW_LOG_WARN(
					"[FreezeWatch] freeze started thresholdMs={} stalledMs={} "
					"receiver={} type={} inProgress={}",
					m_config->freezeThresholdMs(), nowMs - beatAt,
					snap.receiverClass, snap.eventType, snap.inProgress ? "true" : "false");
			}
		} else {
			if (beatAdvanced) {
				const int totalMs = static_cast<int>(nowMs - freezeStartMs);
				freezing = false;
				const EventSnapshot snap = snapshotCopy();
				QEW_LOG_WARN(
					"[FreezeWatch] freeze recovered totalMs={} receiver={} type={} "
					"inProgress={}",
					totalMs, snap.receiverClass, snap.eventType,
					snap.inProgress ? "true" : "false");
			} else if (nowMs - lastOngoingMs >= kOngoingIntervalMs) {
				// 冻结进行中：1s 节流更新（自带节流，不经 AlarmSuppressor）
				lastOngoingMs = nowMs;
				const int elapsedMs = static_cast<int>(nowMs - freezeStartMs);
				const EventSnapshot snap = snapshotCopy();
				QEW_LOG_WARN(
					"[FreezeWatch] freeze ongoing elapsedMs={} receiver={} type={} "
					"inProgress={}",
					elapsedMs, snap.receiverClass, snap.eventType,
					snap.inProgress ? "true" : "false");
			}
		}
	}
}

} // namespace qt_event_watcher
