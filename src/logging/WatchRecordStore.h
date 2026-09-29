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
 * 本存储面向"全量回放分析"，容量 4096 条环形缓冲，同时保留 raw 原文兜底。
 *
 * V5 A1 解析放消费侧：环形缓冲只存 kind/seq/time/raw，字段/栈帧解析后移到
 * snapshot()/snapshotSince() 出口（锁外逐条 hydrate）——sink 端零解析，
 * 快照返回的 Record 保证已含完整 fields/frames（"全量记录"语义不变）。
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
		KindFreeze = 3,		///< V3 B 线：冻结看门狗 started/ongoing/recovered
	};

	/// 保序 key=value 字段（QHash 无序，导出与回放需要稳定顺序）
	using Field = std::pair<std::string, std::string>;

	/// 调用栈单帧（V3 A1：stack= 字段结构化拆解；模块!偏移寻址，符号解析放离线工具）
	struct Frame
	{
		std::string module;
		std::uint64_t offset = 0;
	};

	struct Record
	{
		int kind = KindSlowEvent;
		std::uint64_t seq = 0;	///< 进程内递增序号（V4 D1 差量游标；0 = V3 存量快照）
		std::string time;		///< "HH:mm:ss.zzz"（sink 采集时刻）
		std::string raw;		///< %v 原文（解析降级兜底）
		std::vector<Field> fields;
		std::vector<Frame> frames;	///< 调用栈帧（V3 A1；空 = 该记录无 stack 字段）
	};

	static WatchRecordStore &instance();

	/// 实现体（完整定义仅在 .cpp；RecordSink 需要访问）
	struct Impl;

	/// 生成 sink（WatchLogger::initialize 调用一次并挂到 logger）
	std::shared_ptr<spdlog::sinks::sink> makeSink();

	/// 原文 → 结构化字段/栈帧（V5 A1 解析放消费侧：快照出口与上行消费方
	/// 共用本方法重建；幂等——重复调用先清空再解析）
	static void hydrateRecord(Record &record);

	/// 空格分词 key=value 解析（V5 A1 公有化：消费侧复用，值含空格截断）
	static void parseFields(const std::string &body, std::vector<Field> &out);

	/// stack= 值解析（V5 A1 公有化）："mod!0x1a2b,mod!0x3c4d" → frames
	static void parseFrames(const std::string &stackText, std::vector<Frame> &out);

	/// 全量快照（新的在后），最多 maxCount 条（0 = 全部）；返回记录保证已 hydrate
	std::vector<Record> snapshot(std::size_t maxCount = 0) const;

	/// 差量快照（V4 D1 上行链路）：返回 seq > lastSeq 的记录（新的在后），
	/// 游标经 newLastSeq 带出；lastSeq 对应段已被环形覆盖时丢失量计入
	/// skipped 且游标跳至最旧现存条目（背压丢弃语义，绝不重推也不阻塞）；
	/// 返回记录保证已 hydrate
	std::vector<Record> snapshotSince(std::uint64_t lastSeq,
		std::uint64_t &newLastSeq, std::size_t &skipped) const;

	/// 当前缓冲记录数
	std::size_t count() const;

	WatchRecordStore(const WatchRecordStore &) = delete;
	WatchRecordStore &operator=(const WatchRecordStore &) = delete;

private:
	WatchRecordStore() = default;

	std::shared_ptr<Impl> m_impl;
};

} // namespace qt_event_watcher
