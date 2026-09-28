#include "WatchLogger.h"

#include "WatchLogCapture.h"
#include "WatchRecordStore.h"

#include <QDir>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace qt_event_watcher {

namespace {

// spd暴露出来的等级接口  对等一下WatchLogLevel的 enum
spdlog::level::level_enum toSpdlogLevel(WatchLogLevel level) {
	switch (level) {
		case WatchLogLevel::Trace:		return spdlog::level::trace;
		case WatchLogLevel::Debug:		return spdlog::level::debug;
		case WatchLogLevel::Info:		return spdlog::level::info;
		case WatchLogLevel::Warn:		return spdlog::level::warn;
		case WatchLogLevel::Error:		return spdlog::level::err;
		case WatchLogLevel::Critical:	return spdlog::level::critical;
		case WatchLogLevel::Off:		return spdlog::level::off;
		
		default:						return spdlog::level::info;
	}
}

}  // anonymous namespace

WatchLogger& WatchLogger::instance() {
	static WatchLogger logger;
	return logger;
}

bool WatchLogger::initialize(const std::string& logDirectory,
                const std::string& loggerName,
                WatchLogLevel logLevel) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_)	return true;

	try {
		// 目录创建用 QDir::mkpath（不用 std::filesystem：GCC 8.1 libstdc++ 的
		// fs_path.h operator!= 推导 bug 会导致 MinGW81 构建失败）
		if (!logDirectory.empty())	QDir().mkpath(QString::fromStdString(logDirectory));
		auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		std::vector<spdlog::sink_ptr> sinks;
		sinks.push_back(consoleSink);
		// 告警内存捕获：供 UI/诊断工具实时读取（PRD 17 统计导出的最小前置）
		sinks.push_back(WatchLogCapture::instance().makeSink());
		// 结构化记录存储：V2 A1 批量导出数据源（慢事件/MetaCall 全量环形缓冲）
		sinks.push_back(WatchRecordStore::instance().makeSink());

		if (!logDirectory.empty()) {
			const std::string logFile = logDirectory + "/QtEventWatcher.log";
			auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile, 5 * 1024 * 1024, 3);
			sinks.push_back(fileSink);
		}

		auto logger = std::make_shared<spdlog::logger>(loggerName, sinks.begin(), sinks.end());

		logger->set_level(toSpdlogLevel(logLevel));
		// 慢事件/异常等 WARN 级别日志立即刷盘，避免进程异常退出时丢失
		logger->flush_on(spdlog::level::warn);

		/// [2026-9-2 22:42:11.01] [info] 日志输出案例
		logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

		logger_ = logger;
		initialized_ = true;
		lastErrorMsg_.clear();
		logger->info("QtEventWatcher logger initialized.");
		return true;
	} catch (const std::exception& e) {
		initialized_ = false;
        logger_.reset();

        lastErrorMsg_ = "Logger initialization failed: ";
        lastErrorMsg_ += e.what();
        return false;
	}
}

void WatchLogger::shutdown() {
	// 维持锁界限
	std::lock_guard<std::mutex> lock(mutex_);

	if (!initialized_)	return;
	if (logger_) {
		logger_->info("logger shutdown");
		logger_->flush();	// 刷盘
	}
	logger_.reset();
	initialized_ = false;
}

bool WatchLogger::isInitialized() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return initialized_;
}

std::string WatchLogger::lastErrorMessage() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return lastErrorMsg_;
}

std::shared_ptr<spdlog::logger> WatchLogger::logger() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return logger_;
}

} // namespace qt_event_watcher
