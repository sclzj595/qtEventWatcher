#include "WatchRecordStore.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <cstdio>
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
};

namespace
{

using Impl = WatchRecordStore::Impl;

std::string formatTime(const spdlog::log_clock::time_point& tp)
{
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
						tp.time_since_epoch()).count();
	const std::time_t secs = static_cast<std::time_t>(ms / 1000);
	std::tm tm {};
#ifdef _WIN32
	localtime_s(&tm, &secs);
#else
	localtime_r(&secs, &tm);
#endif
	char buf[16] = {};
	std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
				  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
	return buf;
}

class RecordSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
	explicit RecordSink(std::shared_ptr<Impl> impl)
		: m_impl(std::move(impl))
	{
		set_level(spdlog::level::warn);	// 告警即记录（慢事件/MetaCall 告警均为 WARN）
	}

protected:
	void sink_it_(const spdlog::details::log_msg &msg) override
	{
		std::string text(msg.payload.begin(), msg.payload.end());

		int kind = 0;
		if (!Impl::kindOf(text, kind))	return;

		WatchRecordStore::Record record;
		record.kind = kind;
		record.time = formatTime(msg.time);
		record.raw = std::move(text);
		if (kind == WatchRecordStore::KindQss)
			Impl::extractQssOp(record.raw, record.fields);
		Impl::parseFields(record.raw, record.fields);

		std::lock_guard<std::mutex> lock(m_impl->mutex);
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

std::vector<WatchRecordStore::Record> WatchRecordStore::snapshot(std::size_t maxCount) const
{
	std::vector<Record> out;
	if (!m_impl)	return out;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	const std::size_t total = m_impl->ring.size();
	const std::size_t start = (maxCount > 0 && total > maxCount) ? total - maxCount : 0;
	out.reserve(total - start);
	for (std::size_t i = start; i < total; ++i)
		out.push_back(m_impl->ring[i]);
	return out;
}

std::size_t WatchRecordStore::count() const
{
	if (!m_impl)	return 0;
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->ring.size();
}

} // namespace qt_event_watcher
