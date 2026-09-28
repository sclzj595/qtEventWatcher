#pragma once

#include <string>
#include <mutex>
#include <memory>

namespace spdlog {
	class logger;
}

namespace qt_event_watcher {

enum class WatchLogLevel
{
	Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off
};

class WatchLogger
{
public:
	static WatchLogger& instance();
	bool initialize(const std::string& logDirectory,
                    const std::string& loggerName = "QtEventWatcher",
                    WatchLogLevel logLevel = WatchLogLevel::Info);

    void shutdown();

    bool isInitialized() const;
    std::string lastErrorMessage() const;

    std::shared_ptr<spdlog::logger> logger() const;

    WatchLogger(const WatchLogger&) = delete;
    WatchLogger& operator=(const WatchLogger&) = delete;

private:
	WatchLogger() = default;
	~WatchLogger() = default;

	bool initialized_ = false;
    std::string lastErrorMsg_;
    std::shared_ptr<spdlog::logger> logger_;

	mutable std::mutex mutex_;
};

}  // namespace qt_event_watcher