#pragma once

#include <cstdint>
#include <memory>

namespace qt_event_watcher
{

class WatchConfig;

/**
 * @brief 上行链路客户端（V4 D1 多进程聚合，监控端）
 *
 * 主线程 QTimer 周期驱动（默认 200ms）差量拉取 WatchRecordStore（seq 游标），
 * 以 JSON 行协议（NDJSON）批量推送 op=record.push 给中心收集器
 * （examples/aggregator 的 QLocalServer）。与 IpcConfigServer 同协议族反向
 * 复用：监控进程只推不收（fire-and-forget），收集器只收不控。
 *
 * 背压铁律（docs/25 §6）：任何失败只表现为"游标不前进"——记录滞留环形
 * 缓冲或被覆盖时跳过并累计 droppedCount（随下次推送附带上报），绝不阻塞
 * 监控/告警线程；告警路径本身零改动（热路径增量 = 0，优于 +50ns 验收线）。
 *
 * 断线重连：flushTick 内置指数退避（100ms 倍增至 5s 上限），静默无告警
 * 噪音；重连成功从断点续推（数据在环形缓冲内未覆盖即不丢）。
 *
 * 热更新：flushTick 每拍实时读 uplinkName/uplinkFlushMs——名字变更触发
 * 重连，变空断开闲置，flush 周期即时生效；连接名非空即启用。
 */
class UplinkClient
{
public:
	explicit UplinkClient(WatchConfig *config);
	~UplinkClient();	// Impl 持有 QObject（socket/timer），须 out-of-line

	UplinkClient(const UplinkClient &) = delete;
	UplinkClient &operator=(const UplinkClient &) = delete;

	/// 上行健康度（IPC get uplink / 诊断展示用）
	std::uint64_t pushedCount() const;		///< 已推送记录条数
	std::uint64_t droppedCount() const;		///< 环形覆盖跳过累计
	std::uint64_t reconnectCount() const;	///< 断线后重连成功次数

private:
	void flushTick();

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace qt_event_watcher
