#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace spdlog {
namespace sinks {
class sink;
}
}

namespace qt_event_watcher
{

/**
 * @brief 告警内存捕获器（spdlog sink，WARN 及以上）
 *
 * 供演示界面/诊断工具在不读日志文件的前提下实时获取监控输出。
 * 集成点唯一（WatchLogger::initialize 挂载），监控核心零改动；
 * 纯内存操作，日志失败不影响业务（PRD 14）。
 *
 * 分类计数按日志前缀 [EventWatcher]/[MetaCallWatcher]/
 * [EventStatistics]/[QssStyleWatcher] 归类，仅 WARN 级别路径执行。
 */
class WatchLogCapture
{
public:
	enum Category : int
	{
		CatSlowEvent = 0,
		CatMetaCall,
		CatEventStat,
		CatQss,
		CatFreeze,		///< [FreezeWatch]（V3 B 线）
		CatOther,
		CatCount
	};

	struct Entry
	{
		int level = 3;			///< spdlog level enum 值（3=warn, 4=err...）
		std::string time;		///< "HH:mm:ss.zzz"（sink 采集时刻，UI 明细 Time 列数据源）
		std::string text;		///< 原始消息（pattern 之前的 %v 内容）
	};

	static WatchLogCapture &instance();

	/// 实现体（完整定义仅在 .cpp；CaptureSink 需要访问，故为 public 前向）
	struct Impl;

	/// 生成 sink（WatchLogger::initialize 调用一次并挂到 logger）
	std::shared_ptr<spdlog::sinks::sink> makeSink();

	/// 最近告警快照（新的在后），最多 count 条
	std::vector<Entry> recent(std::size_t maxCount = 12) const;

	/// 分类累计告警数
	std::uint64_t categoryCount(int category) const;

	WatchLogCapture(const WatchLogCapture &) = delete;
	WatchLogCapture &operator=(const WatchLogCapture &) = delete;

private:
	WatchLogCapture() = default;

	std::shared_ptr<Impl> m_impl;	///< 实现隐藏（sink 与查询共享）
};

} // namespace qt_event_watcher
