#pragma once

#include <cstdint>
#include <memory>

#include <QEvent>

namespace qt_event_watcher
{

class WatchConfig;
class MetaCallFilter;
class MetaCallSenderRegistry;
class AlarmSuppressor;

/**
 * @brief	MetaCall Event 监控器（Bit1）
 * - 判断 MetaCall 功能是否开启
 * - 判断 MetaCall 是否超过阈值
 * - sender 身份经 MetaCallSenderRegistry 发射时刻快照，日志路径零解引用（3B）
 * - MetaCallFilter blacklist 过滤（3B+），命中即丢弃告警
 */
class MetaCallWatcher
{
public:
	explicit MetaCallWatcher(WatchConfig* watchConfig);
	~MetaCallWatcher();	// unique_ptr 成员需要完整类型，析构必须在实现文件中定义
	MetaCallWatcher(const MetaCallWatcher&) = delete;
	MetaCallWatcher& operator=(const MetaCallWatcher&) = delete;

	/// Func
	bool isEnabled() const;
	void process(QObject* receiver, QEvent* event, std::int64_t elapsedNs);

	/// blacklist 规则配置入口（非拥有指针；GUI 线程 exec() 前配置）
	MetaCallFilter* filter();

private:
	WatchConfig* m_config = nullptr;
	std::unique_ptr<MetaCallSenderRegistry> m_senderRegistry;
	std::unique_ptr<MetaCallFilter> m_filter;
	std::unique_ptr<AlarmSuppressor> m_suppressor;	///< V3 A2 告警风暴抑制（懒构造，仅告警路径访问）
	bool m_spyHijackWarned = false;	///< 抢占告警仅输出一次
};

} // namespace qt_event_watcher
