#include "WatchLogCapture.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>

namespace qt_event_watcher
{

struct WatchLogCapture::Impl
{
	static constexpr std::size_t kCapacity = 32;

	std::mutex mutex;
	std::deque<Entry> ring;
	std::array<std::atomic<std::uint64_t>, CatCount> counts{};

	static int categoryOf(const std::string &text)
	{
		// 前缀标签由各 watcher 日志格式保证（[XXX] 紧跟 %v 开头）
		if (text.rfind("[EventWatcher]", 0) == 0)			return CatSlowEvent;
		if (text.rfind("[MetaCallWatcher]", 0) == 0)		return CatMetaCall;
		if (text.rfind("[EventStatistics]", 0) == 0)		return CatEventStat;
		if (text.rfind("[QssStyleWatcher]", 0) == 0)		return CatQss;
		if (text.rfind("[FreezeWatch]", 0) == 0)			return CatFreeze;
		return CatOther;
	}
};

namespace
{

using Impl = WatchLogCapture::Impl;

/// "HH:mm:ss.zzz"（无第三方格式化依赖，localtime 线程安全性由 CRT 提供）
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

class CaptureSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
	explicit CaptureSink(std::shared_ptr<Impl> impl)
		: m_impl(std::move(impl))
	{
		set_level(spdlog::level::warn);	// 只捕获 WARN+，与 PRD 12 级别语义一致
	}

protected:
	void sink_it_(const spdlog::details::log_msg &msg) override
	{
		WatchLogCapture::Entry entry;
		entry.level = static_cast<int>(msg.level);
		entry.time = formatTime(msg.time);
		entry.text.assign(msg.payload.begin(), msg.payload.end());

		const int category = Impl::categoryOf(entry.text);
		m_impl->counts[static_cast<std::size_t>(category)]
		    .fetch_add(1, std::memory_order_relaxed);

		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->ring.push_back(std::move(entry));
		while (m_impl->ring.size() > Impl::kCapacity)
			m_impl->ring.pop_front();
	}

	void flush_() override {}

private:
	std::shared_ptr<WatchLogCapture::Impl> m_impl;
};

} // namespace

WatchLogCapture &WatchLogCapture::instance()
{
	static WatchLogCapture capture;
	return capture;
}

std::shared_ptr<spdlog::sinks::sink> WatchLogCapture::makeSink()
{
	if (!m_impl)
	{
		auto impl = std::make_shared<Impl>();
		for (auto &count : impl->counts)
			count.store(0, std::memory_order_relaxed);
		m_impl = std::move(impl);
	}
	return std::make_shared<CaptureSink>(m_impl);
}

std::vector<WatchLogCapture::Entry> WatchLogCapture::recent(std::size_t maxCount) const
{
	std::vector<Entry> out;
	if (!m_impl)	return out;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	const std::size_t total = m_impl->ring.size();
	const std::size_t start = total > maxCount ? total - maxCount : 0;
	out.reserve(total - start);
	for (std::size_t i = start; i < total; ++i)
		out.push_back(m_impl->ring[i]);
	return out;
}

std::uint64_t WatchLogCapture::categoryCount(int category) const
{
	if (category < 0 || category >= CatCount || !m_impl)	return 0;
	return m_impl->counts[static_cast<std::size_t>(category)].load(std::memory_order_relaxed);
}

} // namespace qt_event_watcher
