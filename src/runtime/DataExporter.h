#pragma once

#include <QString>

namespace qt_event_watcher
{

class WatchConfig;
class EventStatistics;

/**
 * @brief 批量数据导出（V2 Phase A1/A2，PRD 21 §5）
 *
 * 与 ReportExporter（最近诊断快照报告）分工：本导出输出 WatchRecordStore
 * 中的全量结构化记录（慢事件 / MetaCall）与 EventStatistics 周期统计
 * （TOP-N / 分位数），面向外部回放分析。
 *
 * 格式由文件后缀决定：.json → 结构化 JSON（含统计段）；.csv（或其余后缀）
 * → 宽表 CSV（仅事件记录，回放格式稳定）。仅在用户/测试显式调用时执行，
 * 不进入监控热路径。
 */
class DataExporter
{
public:
	static bool exportData(const QString& filePath, const WatchConfig* config,
						   const EventStatistics* statistics = nullptr,
						   QString* error = nullptr);

	DataExporter() = delete;
};

} // namespace qt_event_watcher
