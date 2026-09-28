#pragma once

#include <QHash>
#include <QMetaObject>
#include <QMutex>

class QThread;

namespace qt_event_watcher
{

/**
 * @brief Sender 归属线程注册表（V2 Phase C1）
 *
 * 问题：QThread 没有公开 API 获取"某个 QThread 对象"的 OS 线程 id——
 * QThread::currentThreadId() 只给当前执行线程。MetaCall 场景要精确的
 * "sender 归属线程"（sender->thread()），而发射可能发生在非归属线程
 * （跨线程直接 emit），此时发射线程快照 ≠ 归属线程。
 *
 * 方案（PRD 21 Phase C1：发射时刻注册，析构时刻注销）：
 * - 发射时刻 sender 必然存活 → 安全解引用 sender->thread()（仅此一处，
 *   日志路径仍然零解引用）
 * - 首见 QThread* 注册条目 {osThreadId}：若该线程正在执行（isCurrentThread）
 *   立即拿到 os id；否则置 0 待回填（该线程下次发射信号时补齐）
 * - QThread destroyed → 注销条目：表内键值生命周期 ⊆ QThread 生命周期，
 *   日志路径查表永不踩悬空
 * - 容量上限：满员后停止注册，降级为发射线程快照（行为同 C1 之前）
 *
 * 热路径约束（PRD 14）：observe 每次 onSignalBegin 调用一次，仅 hash 查表
 * 与偶发首次注册（同签名表"首见分配"模式）。
 */
class ThreadRegistry
{
public:
	ThreadRegistry() = default;
	~ThreadRegistry();

	ThreadRegistry(const ThreadRegistry&) = delete;
	ThreadRegistry& operator=(const ThreadRegistry&) = delete;

	/// 发射时刻调用：注册 sender->thread()；已知条目在 isCurrentThread 时回填 os id
	void observe(QThread* ownerThread);

	/// 日志路径：以 QThread* 指针值查归属线程 os id；0 = 未观测（调用方降级）
	bool lookup(const void* ownerThreadPtr, quintptr& osThreadId) const;

	/// 当前注册数（测试用）
	int size() const;

private:
	struct Entry
	{
		quintptr osThreadId = 0;	///< 归属线程 os id；0 = 尚未在该线程观测到
		QMetaObject::Connection destroyedConn;	///< destroyed 连接句柄，析构时统一断开
	};

	void unregister(const void* ownerThreadPtr);

	static constexpr int kMaxThreads = 64;

	mutable QMutex m_mutex;
	QHash<const void*, Entry> m_threads;
};

} // namespace qt_event_watcher
