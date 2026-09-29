#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class QEvent;
class QObject;

namespace qt_event_watcher
{

class WatchConfig;

/**
 * @brief UI 冻结看门狗（V3 Phase B，ANR 模式）
 *
 * 原理：主线程挂载周期心跳（QTimer，CusApplication 持有并调用 beat()），
 * 看门狗线程周期检查心跳时间戳——主线程事件循环冻结（长任务/死锁/模态阻塞）
 * 时心跳停跳，now - lastBeat > freezeThresholdMs 即判定冻结。
 * "空闲"不误报：空闲时事件循环阻塞在事件等待，定时器到期仍会唤醒派发。
 *
 * 取证（PRD 21 §6 B 线"看门狗+取证"）：
 * - notify 入口 eventStarted() 记录"正在处理的事件"快照（receiver 类名 + 事件类型），
 *   出口 eventFinished() 清除；冻结时快照即"卡在哪个事件"。
 *   快照仅由心跳线程写入（构造线程绑定）：CusApplication::notify 是全进程虚函数，
 *   worker 线程派发事件同样进入，不做线程绑定会被无关事件覆盖取证现场
 * - 三态告警（全部由看门狗线程发出，spdlog sink 均 _mt 线程安全）：
 *   freeze started（WARN，附快照）→ ongoing（每 1s 一条，自带节流）→
 *   recovered（WARN，含总时长）
 *
 * 线程模型：beat()/eventStarted()/eventFinished() 仅主线程调用；
 * run() 为独立 std::thread；停止经 stop() 置位 + join（析构兜底）。
 * 开关门控：run() 每轮与快照入口实时查询 WatchFreeze 位（支持 INI/IPC 热更新，
 * 关闭时心跳基线跟随刷新，重新开启不误报陈旧冻结）。
 *
 * 开销预算（PRD 14）：主线程每事件 1 次原子读 + （开关开启时）1 次快照写；
 * 看门狗线程 100ms 粒度轮询，无锁竞争热点。
 */
class EventWatchdog
{
public:
	static constexpr int kPollIntervalMs = 100;	///< 看门狗轮询粒度
	static constexpr int kOngoingIntervalMs = 1000;	///< 冻结进行中告警节流

	explicit EventWatchdog(const WatchConfig* config);
	~EventWatchdog();	// 线程 join 必须在实现文件（stop 兜底）

	EventWatchdog(const EventWatchdog&) = delete;
	EventWatchdog& operator=(const EventWatchdog&) = delete;

	/// 启动看门狗线程（心跳基线=启动时刻；重复调用无效果）
	void start();

	/// 停止并 join（析构自动调用；幂等）
	void stop();

	/// 主线程心跳（CusApplication 心跳 QTimer 触发；冻结时自然停跳）
	void beat();

	/// notify 入口：记录"正在处理的事件"快照（主线程调用）
	void eventStarted(const QObject* receiver, QEvent* event);

	/// notify 出口：清除进行中标记（主线程调用）
	void eventFinished();

private:
	/// "正在处理的事件"快照（互斥保护；主线程唯一写者，看门狗读）
	struct EventSnapshot
	{
		bool inProgress = false;
		int eventType = -1;
		char receiverClass[48] = {};	///< 截断安全；类名以 ASCII 为主
	};

	EventSnapshot snapshotCopy() const;

	void run();

	const WatchConfig* m_config;
	std::thread m_thread;
	std::thread::id m_ownerThreadId;	///< 快照归属线程 = 构造线程 = 心跳线程
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_started{false};
	std::atomic<std::int64_t> m_lastBeatMs{0};	///< steady 毫秒

	mutable std::mutex m_snapshotMutex;
	EventSnapshot m_snapshot;
};

} // namespace qt_event_watcher
