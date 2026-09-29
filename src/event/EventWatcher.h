#pragma once

#include "WatchEventInfo.h"

#include <QEvent>

#include <memory>

namespace qt_event_watcher
{

class WatchConfig;
class EventStatistics;
class AlarmSuppressor;

/// @brief Qt 基础监控器
/**
 * 负责：
 * - 判断 Event 监控是否开启
 * - 测量 Event 耗时
 * - 识别慢 Event
 * - 将 Event 交给统计器
 */
class EventWatcher
{
public:
	EventWatcher(WatchConfig* config, EventStatistics* statistics);
	~EventWatcher();	// m_suppressor 为不完整类型，析构置于 .cpp

	EventWatcher(const EventWatcher&) = delete;
	EventWatcher& operator=(const EventWatcher&) = delete;

	// Fun
	/// @param elapsedNs Inclusive Cost；exclusiveElapsedNs 扣除嵌套子事件后的自身耗时
	bool watch(WatchEventInfo& info, std::int64_t elapsedNs, std::int64_t exclusiveElapsedNs);
	WatchEventInfo watchEvent(QObject* receiver, QEvent* event);
	bool isEnabled() const;

private:
	static QString eventName(QEvent::Type type);
	static QString receiverClassName(QObject* receiver);
	static QString receiverObjectName(QObject* receiver);

	WatchConfig* m_config = nullptr;
	EventStatistics* m_statistics = nullptr;
	std::unique_ptr<AlarmSuppressor> m_suppressor;	///< V3 A2 告警风暴抑制（懒构造，仅告警路径访问）

};


} // namespace qt_event_watcher
