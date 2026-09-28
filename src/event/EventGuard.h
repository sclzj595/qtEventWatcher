#pragma once 

#include <cstdint>
#include <QElapsedTimer>

namespace qt_event_watcher
{
	
/**
 * @brief 单个 Event的 耗时保护器
 * 计时 
 * 不管 日志 配置 统计 慢事件判断
 */
class EventGuard
{
	
public:
	EventGuard();
	
	void start();
	std::int64_t elapsedNs() const;
	double elapsedMs() const;

private:
	QElapsedTimer m_timer;
};


} // namespace qt_event_watcher
