#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace spdlog {
namespace sinks {
class sink;
}
}

namespace qt_event_watcher
{

/**
 * @brief 结构化记录存储（spdlog sink，WARN 级；V2 Phase A1 批量导出的数据源）
 *
 * 与 WatchLogCapture（UI 最近告警快照，32 条环形）分工：
 * 本存储面向"全量回放分析"，容量 4096 条环形缓冲，按 key=value 解析
 * 慢事件 / MetaCall 告警为结构化字段，同时保留 raw 原文兜底。
 *
 * 零热路径耦合：监控器只输出既有格式日志，本存储作为挂载 sink 被动采集；
 * 慢事件/MetaCall 告警本身阈值门控，解析开销可忽略（PRD 14）。
 */
class WatchRecordStore
{
public:
	enum Kind : int
	{
		KindSlowEvent = 0,
		KindMetaCall = 1,
		KindQss = 2,
	};

	/// 保序 key=value 字段（QHash 无序，导出与回放需要稳定顺序）
	using Field = std::pair<std::string, std::string>;

	struct Record
	{
		int kind = KindSlowEvent;
		std::string time;		///< "HH:mm:ss.zzz"（sink 采集时刻）
		std::string raw;		///< %v 原文（解析降级兜底）
		std::vector<Field> fields;
	};

	static WatchRecordStore &instance();

	/// 实现体（完整定义仅在 .cpp；RecordSink 需要访问）
	struct Impl;

	/// 生成 sink（WatchLogger::initialize 调用一次并挂到 logger）
	std::shared_ptr<spdlog::sinks::sink> makeSink();

	/// 全量快照（新的在后），最多 maxCount 条（0 = 全部）
	std::vector<Record> snapshot(std::size_t maxCount = 0) const;

	/// 当前缓冲记录数
	std::size_t count() const;

	WatchRecordStore(const WatchRecordStore &) = delete;
	WatchRecordStore &operator=(const WatchRecordStore &) = delete;

private:
	WatchRecordStore() = default;

	std::shared_ptr<Impl> m_impl;
};

} // namespace qt_event_watcher
