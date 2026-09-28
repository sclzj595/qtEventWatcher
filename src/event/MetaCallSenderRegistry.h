#pragma once

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QtGlobal>

#include "ThreadRegistry.h"

namespace qt_event_watcher
{

/**
 * @brief Sender 身份注册表（3B：sender 生命周期安全）
 *
 * 问题：QMetaCallEvent::sender() 是裸指针。queued 投递后、日志时刻之前，
 * sender 可能已析构（如槽内 delete this / 跨线程销毁），日志路径任何
 * sender->metaObject() 解引用都是 UB。QPointer 也无法补救：
 * QPointer 只能保护"注册前就跟踪"的对象，从已悬空裸指针构造仍是 UB。
 *
 * 方案：把身份解析挪到发射时刻——此时 sender 正在 emit，必然存活。
 * 通过 qt_register_signal_spy_callbacks（QSignalSpy 同款机制）注册
 * signal_begin 回调，在回调里快照 {sender 指针值, signalId, metaObject,
 * className, 信号签名}。日志路径仅以"指针值 + signalId"为键查表，
 * 永不解引用 sender：
 * - 命中：使用发射时刻快照输出，零竞态
 * - 未命中：降级为空字段，绝不猜测
 *
 * 约束：
 * - 进程级只允许一组 spy 回调（与 QSignalSpy / Qt Test 互斥，Qt 保存指针不拷贝）
 * - 回调在所有信号发射热路径上执行，内部仅做查表与固定缓冲覆写，零动态分配
 *   （签名表按 metaObject 首见时分配一次；metaObject/className 均为静态数据）
 * - 仅在 WatchMetaCall 开启时安装，安装状态以进程启动时配置为准
 */
class MetaCallSenderRegistry
{
public:
	/// 日志路径可用的 sender 身份快照
	struct SenderIdentity
	{
		const char* senderClassName = nullptr;	///< moc 静态数据指针，不随对象销毁失效
		QString signalSignature;				///< 如 "timeout()"，未解析为空
		quintptr senderThreadId = 0;			///< 发射时刻线程快照；0 = 未知
		/**
		 * sender 归属线程 os id（C1 精确语义，ThreadRegistry 查得）：
		 * 归属线程从未被观测到时为 0，消费方应回退 senderThreadId。
		 * 与 senderThreadId 差异仅在"跨线程直接 emit"（归属 ≠ 执行）场景。
		 */
		quintptr ownerThreadId = 0;
	};

	MetaCallSenderRegistry();
	~MetaCallSenderRegistry();

	MetaCallSenderRegistry(const MetaCallSenderRegistry&) = delete;
	MetaCallSenderRegistry& operator=(const MetaCallSenderRegistry&) = delete;

	/// 安装发射回调（构造后调用一次；析构自动卸载）
	void install();

	/**
	 * @brief 回调是否仍由本注册表持有（抢占检测）
	 * spy 回调进程级唯一：其他组件（QSignalSpy 类工具、GammaRay 等）注册
	 * 会覆盖全局回调指针，本表停止更新 → 查询应降级。
	 */
	bool isCallbackActive() const;

	/**
	 * @brief 以"指针值 + signalId"查询发射时刻身份快照（不解引用 sender）
	 * @param sender QMetaCallEvent 携带的裸指针，仅作键值使用
	 * @return true 命中；false 未观测到该次发射（调用方应降级）
	 */
	bool lookup(const void* sender, int signalId, SenderIdentity& out) const;

private:
	friend void signalBeginTrampoline(QObject*, int, void**);
	void onSignalBegin(QObject* sender, int signalId);

	struct Record
	{
		const void* sender = nullptr;	///< 发射时刻的 sender 指针值（仅作键，不解引用）
		const QMetaObject* mo = nullptr;
		int signalId = -1;
		const char* className = nullptr;
		quintptr threadId = 0;			///< 发射时刻线程（emit 所在线程 = sender 线程）
		const void* ownerThread = nullptr;	///< sender->thread() 指针值（C1，发射时刻安全）
	};

	mutable std::mutex m_mutex;
	std::vector<Record> m_ring;		///< 固定容量环形快照（覆写最旧）
	std::size_t m_ringNext = 0;
	///< 签名表：metaObject（静态）→ 按绝对 signalId 索引的签名
	std::vector<std::pair<const QMetaObject*, std::vector<QString>>> m_signatureTable;

	///< sender 归属线程注册表（C1：发射时刻注册，destroyed 注销）
	ThreadRegistry m_threadRegistry;
};

} // namespace qt_event_watcher
