#pragma once

#include <cstdint>
#include <QObject>
#include <QString>

namespace qt_event_watcher
{
	
/**
 * @brief 单次监控结果
 * 只有result  不带其他字段
 */
struct WatchEventInfo
{
	/// @brief 这个结构是瞬时结果，只在 notify() 当前调用链里面使用，不持有对象生命周期。
	QObject *receiver = nullptr;
	int eventType = 0;

	QString eventName;
	QString receiverClassName;
	QString receiverObjectName;

	std::int64_t elapsedNs = 0;			///< Inclusive Cost：本次 notify() 完整墙钟耗时（PRD 05 §6）
	std::int64_t exclusiveElapsedNs = 0;	///< Exclusive Cost：扣除嵌套 notify 子事件后的自身耗时
	int nestingDepth = 0;				///< notify 重入深度，最外层 = 0
	bool slow = false;

	/// 线程整数快照（PRD 05 §7）：不长期持有 QThread*
	quintptr currentThreadId = 0;		///< notify 执行线程
	quintptr receiverThreadId = 0;		///< receiver 归属线程

	/// Current == Receiver 时为 true；不一致本身即有诊断价值
	bool receiverThreadMatch() const {	return currentThreadId == receiverThreadId;	}

	double elapsedMs() const {	return static_cast<double>(elapsedNs) / 1000000.0;	 };
	double exclusiveElapsedMs() const {	return static_cast<double>(exclusiveElapsedNs) / 1000000.0;	};
};

} // namespace qt_event_watcher
