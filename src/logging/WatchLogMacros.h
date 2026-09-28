#pragma once

#include "WatchLogger.h"

#include <spdlog/spdlog.h>

#define QEW_LOG_TRACE(...)                                             \
    do {                                                               \
        auto _qew_log_l_ =                                             \
            qt_event_watcher::WatchLogger::instance().logger();       \
        if (_qew_log_l_)                                               \
            SPDLOG_LOGGER_TRACE(_qew_log_l_, __VA_ARGS__);             \
    } while (false)

#define QEW_LOG_DEBUG(...)                                             \
    do {                                                               \
        auto _qew_log_l_ =                                             \
            qt_event_watcher::WatchLogger::instance().logger();       \
        if (_qew_log_l_)                                               \
            SPDLOG_LOGGER_DEBUG(_qew_log_l_, __VA_ARGS__);             \
    } while (false)

#define QEW_LOG_INFO(...)                                              \
    do {                                                               \
        auto _qew_log_l_ =                                             \
            qt_event_watcher::WatchLogger::instance().logger();       \
        if (_qew_log_l_)                                               \
            SPDLOG_LOGGER_INFO(_qew_log_l_, __VA_ARGS__);              \
    } while (false)

#define QEW_LOG_WARN(...)                                              \
    do {                                                               \
        auto _qew_log_l_ =                                             \
            qt_event_watcher::WatchLogger::instance().logger();       \
        if (_qew_log_l_)                                               \
            SPDLOG_LOGGER_WARN(_qew_log_l_, __VA_ARGS__);              \
    } while (false)

#define QEW_LOG_ERROR(...)                                             \
    do {                                                               \
        auto _qew_log_l_ =                                             \
            qt_event_watcher::WatchLogger::instance().logger();       \
        if (_qew_log_l_)                                               \
            SPDLOG_LOGGER_ERROR(_qew_log_l_, __VA_ARGS__);             \
    } while (false)

#define QEW_LOG_CRITICAL(...)                                          \
    do {                                                               \
        auto _qew_log_l_ =                                             \
            qt_event_watcher::WatchLogger::instance().logger();       \
        if (_qew_log_l_)                                               \
            SPDLOG_LOGGER_CRITICAL(_qew_log_l_, __VA_ARGS__);          \
    } while (false)