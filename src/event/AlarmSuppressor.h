#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace qt_event_watcher
{

/// 风暴抑制决策结果（V3 Phase A2）
struct AlarmDecision
{
	bool emitNow = true;		///< true=窗口首条，按 WARN 输出；false=静默条，按 DEBUG 输出（RecordSink 仍全量采集）
	int suppressedFlushed = 0;	///< 窗口关闭时冲刷的静默累计条数（>0 时调用方需补一条 alarm storm 汇总，WARN）
};

/**
 * @brief 告警风暴抑制器（V3 Phase A2，PRD 21 §6）
 *
 * 语义：同类告警（Event: receiver+事件类型；MetaCall: sender+signal）在窗口
 * 内首条必出，后续静默累计；下次同类告警到达且窗口已过期时，冲刷 suppressed=N
 * 汇总，当前告警作为新窗口首条必出——不丢总量，只去重风暴。
 * 窗口由调用方传入（V4 B1：WatchConfig::alarmSuppressWindowMs 三通道热更新，
 * 默认值 = V3 A2 编译期常量 1000ms，语义不变）。
 *
 * 边界：
 * - 仅作用于告警日志的级别路由：静默条以 DEBUG 发出，文件/控制台 sink 保持
 *   用户级别过滤掉，WatchRecordStore sink 降至 DEBUG 接住（全量逐条，回放
 *   数据完整性优先）；EventStatistics::record 全量统计不受影响
 * - 判定位置在阈值判断之后、日志输出之前（既有性能约定）
 *
 * 线程安全：内部互斥（MetaCall 可达自任意事件循环线程）。
 * 懒冲刷：风暴彻底停止后，最后一窗的 suppressed=N 在该 key 下次告警时补出
 * （零定时器零常驻；全量记录已在 WatchRecordStore，无信息丢失）。
 */
class AlarmSuppressor
{
public:
	/// 兼容缺省窗口（= WatchConfig::DefaultAlarmSuppressWindowMs 同值）
	static constexpr int kWindowMs = 1000;

	/// 以 steady 时钟当前时刻评估（windowMs <= 0 时回退缺省窗口）
	/// 注意：无默认参数——两参（key, windowMs）与三参（key, nowMs, windowMs）
	/// 必须显式，避免字面量 0 在 int/int64 重载间误绑定（V4 B1 实测踩坑）
	AlarmDecision evaluate(const std::string& key, int windowMs);

	/// 合成时钟评估（测试确定性注入；nowSteadyMs 为 steady 毫秒）
	AlarmDecision evaluate(const std::string& key, std::int64_t nowSteadyMs, int windowMs);

	AlarmSuppressor() = default;
	AlarmSuppressor(const AlarmSuppressor&) = delete;
	AlarmSuppressor& operator=(const AlarmSuppressor&) = delete;

private:
	struct WindowState
	{
		std::int64_t startMs = -1;	///< -1 = 尚无窗口（哨兵，避免 t=0 边界误判）
		int suppressed = 0;
	};

	std::mutex m_mutex;
	std::unordered_map<std::string, WindowState> m_windows;
};

} // namespace qt_event_watcher
