#include "WatchRecordStore.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>

namespace qt_event_watcher
{

struct WatchRecordStore::Impl
{
	static constexpr std::size_t kCapacity = 4096;

	std::mutex mutex;
	std::deque<Record> ring;
	std::uint64_t nextSeq = 1;	///< V4 D1：进程内递增序号（差量游标基准）

	/// 慢事件 / MetaCall / QSS 告警前缀 → kind；其他前缀不采集（周期统计归消费方）
	static bool kindOf(const std::string &text, int &kind)
	{
		if (text.rfind("[EventWatcher] slow event ", 0) == 0) {
			kind = KindSlowEvent;
			return true;
		}
		if (text.rfind("[MetaCallWatcher] slow MetaCall ", 0) == 0) {
			kind = KindMetaCall;
			return true;
		}
		if (text.rfind("[QssStyleWatcher] ", 0) == 0) {
			kind = KindQss;
			return true;
		}
		if (text.rfind("[FreezeWatch] ", 0) == 0) {
			kind = KindFreeze;
			return true;
		}
		return false;
	}

	/// QSS 操作名提取："[QssStyleWatcher] SlowQssLoad | ..." → op=SlowQssLoad
	static void extractQssOp(const std::string &text, std::vector<Field> &fields)
	{
		const std::string body = text.substr(sizeof("[QssStyleWatcher] ") - 1);
		const std::size_t end = body.find_first_of(" |");
		if (end == std::string::npos || end == 0)	return;
		fields.emplace_back("op", body.substr(0, end));
	}

	/// 空格分词、首 '=' 分割（与 Demo 明细表/ReportExporter 同规则；
	/// 值含空格时该字段在结构化列中截断，raw 原文始终完整保留）
	static void parseFields(const std::string &body, std::vector<Field> &out)
	{
		std::size_t pos = 0;
		const std::size_t len = body.size();
		while (pos < len) {
			while (pos < len && body[pos] == ' ')	++pos;
			const std::size_t start = pos;
			while (pos < len && body[pos] != ' ')	++pos;
			const std::size_t eq = body.find('=', start);
			if (eq >= pos || eq == std::string::npos || eq == start)	continue;
			out.emplace_back(body.substr(start, eq - start), body.substr(eq + 1, pos - eq - 1));
		}
	}

	/// stack= 值解析（V3 A1）："mod!0x1a2b,mod!0x3c4d" → frames；
	/// 坏帧跳过（采集端已保证无空格 token，此处防御性容错不失败）
	static void parseFrames(const std::string &stackText,
							std::vector<WatchRecordStore::Frame> &out)
	{
		std::size_t start = 0;
		while (start <= stackText.size()) {
			std::size_t end = stackText.find(',', start);
			if (end == std::string::npos)	end = stackText.size();
			const std::string token = stackText.substr(start, end - start);
			const std::size_t bang = token.rfind('!');
			if (bang != std::string::npos && bang > 0) {
				WatchRecordStore::Frame frame;
				frame.module = token.substr(0, bang);
				frame.offset = std::strtoull(token.c_str() + bang + 1, nullptr, 16);
				out.push_back(std::move(frame));
			}
			if (end == stackText.size())	break;
			start = end + 1;
		}
	}
};

namespace
{

using Impl = WatchRecordStore::Impl;

std::string formatTime(const spdlog::log_clock::time_point& tp)
{
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
						tp.time_since_epoch()).count();
	const std::time_t secs = static_cast<std::time_t>(ms / 1000);
	/*
	 * V5 A1 秒段缓存：localtime_s 是本函数大头（内核级时区换算），
	 * 同一秒内只渲染一次 HH:mm:ss。RecordSink 为 base_sink<std::mutex>，
	 * sink_it_ 天然串行——普通 static 无竞态。
	 */
	static std::time_t cachedSecs = -1;
	static char cachedHms[16] = {};
	char buf[16] = {};
	if (secs != cachedSecs) {
		std::tm tm {};
#ifdef _WIN32
		localtime_s(&tm, &secs);
#else
		localtime_r(&secs, &tm);
#endif
		std::snprintf(cachedHms, sizeof(cachedHms), "%02d:%02d:%02d",
					  tm.tm_hour, tm.tm_min, tm.tm_sec);
		cachedSecs = secs;
	}
	std::snprintf(buf, sizeof(buf), "%s.%03d", cachedHms, static_cast<int>(ms % 1000));
	return buf;
}

class RecordSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
	explicit RecordSink(std::shared_ptr<Impl> impl)
		: m_impl(std::move(impl))
	{
		// V3 A2：降至 debug——静默条（风暴抑制，WARN 首条之外的 DEBUG 告警）
		// 也全量采集；非告警日志由 kindOf 前缀门控排除
		set_level(spdlog::level::debug);
	}

protected:
		void sink_it_(const spdlog::details::log_msg &msg) override
		{
			std::string text(msg.payload.begin(), msg.payload.end());

			int kind = 0;
			if (!Impl::kindOf(text, kind))	return;

			// V5 A1 解析放消费侧：环形缓冲只存 kind/seq/time/raw，
			// 字段/栈帧解析后移到 snapshot()/snapshotSince() 出口（锁外 hydrate）
			WatchRecordStore::Record record;
			record.kind = kind;
			record.time = formatTime(msg.time);
			record.raw = std::move(text);

			std::lock_guard<std::mutex> lock(m_impl->mutex);
			record.seq = m_impl->nextSeq++;
			m_impl->ring.push_back(std::move(record));
			while (m_impl->ring.size() > Impl::kCapacity)
				m_impl->ring.pop_front();
		}

	void flush_() override {}

private:
	std::shared_ptr<WatchRecordStore::Impl> m_impl;
};

} // namespace

WatchRecordStore &WatchRecordStore::instance()
{
	static WatchRecordStore store;
	return store;
}

std::shared_ptr<spdlog::sinks::sink> WatchRecordStore::makeSink()
{
	if (!m_impl)
		m_impl = std::make_shared<Impl>();
	return std::make_shared<RecordSink>(m_impl);
}

/// 原文 → 结构化字段/栈帧（V5 A1：快照出口与上行消费方共用；
/// 顺序与 V4 采集端逐条等价：QSS op 提取 → parseFields → stack= 拆帧）
void WatchRecordStore::hydrateRecord(Record &record)
{
	record.fields.clear();
	record.frames.clear();
	if (record.kind == KindQss)
		Impl::extractQssOp(record.raw, record.fields);
	Impl::parseFields(record.raw, record.fields);
	for (const Field &f : record.fields) {
		if (f.first == "stack") {
			Impl::parseFrames(f.second, record.frames);
			break;
		}
	}
}

void WatchRecordStore::parseFields(const std::string &body, std::vector<Field> &out)
{
	Impl::parseFields(body, out);
}

void WatchRecordStore::parseFrames(const std::string &stackText, std::vector<Frame> &out)
{
	Impl::parseFrames(stackText, out);
}

std::vector<WatchRecordStore::Record> WatchRecordStore::snapshot(std::size_t maxCount) const
{
	std::vector<Record> out;
	if (!m_impl)	return out;

	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		const std::size_t total = m_impl->ring.size();
		const std::size_t start = (maxCount > 0 && total > maxCount) ? total - maxCount : 0;
		out.reserve(total - start);
		for (std::size_t i = start; i < total; ++i)
			out.push_back(m_impl->ring[i]);
	}

	// V5 A1：锁外 hydrate——解析成本不占存储互斥，也缩短快照持锁时长
	for (Record &r : out)
		hydrateRecord(r);
	return out;
}

std::vector<WatchRecordStore::Record> WatchRecordStore::snapshotSince(
	std::uint64_t lastSeq, std::uint64_t &newLastSeq, std::size_t &skipped) const
{
	std::vector<Record> out;
	newLastSeq = lastSeq;
	skipped = 0;
	if (!m_impl)	return out;

	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		if (m_impl->ring.empty())	return out;

		// 游标过旧且段已被覆盖：跳过丢失量，从最旧现存条目续推（背压丢弃）
		std::size_t start = 0;
		const std::uint64_t oldestSeq = m_impl->ring.front().seq;
		if (oldestSeq > lastSeq + 1) {
			skipped = static_cast<std::size_t>(oldestSeq - (lastSeq + 1));
		} else {
			// 二分定位第一个 seq > lastSeq（deque 随机访问，seq 严格递增）
			std::size_t lo = 0;
			std::size_t hi = m_impl->ring.size();
			while (lo < hi) {
				const std::size_t mid = lo + (hi - lo) / 2;
				if (m_impl->ring[mid].seq <= lastSeq)	lo = mid + 1;
				else									hi = mid;
			}
			start = lo;
		}
		for (std::size_t i = start; i < m_impl->ring.size(); ++i)
			out.push_back(m_impl->ring[i]);
		if (!out.empty())
			newLastSeq = out.back().seq;
	}

	// V5 A1：锁外 hydrate（同 snapshot）
	for (Record &r : out)
		hydrateRecord(r);
	return out;
}

std::size_t WatchRecordStore::count() const
{
	if (!m_impl)	return 0;
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->ring.size();
}

} // namespace qt_event_watcher
