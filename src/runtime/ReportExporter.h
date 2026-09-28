#pragma once

#include <QString>

namespace qt_event_watcher
{

class WatchConfig;

/**
 * @brief 诊断报告导出器（PRD 17 §5）
 *
 * 内容：Application / Qt Environment / Compiler / Architecture /
 * Runtime Modules / Qt Plugins / Dependency Information /
 * Monitor Configuration / Recent Diagnostics（四类）。
 *
 * 定位：纯 Core 组装层（RuntimeDiagnostics + WatchLogCapture + WatchConfig），
 * 无任何 UI 类型（PRD 18 §1）；导出失败仅返回 false + error，不抛出（PRD 16 §8 同源约束）。
 * 格式由文件后缀决定：.json → 结构化 JSON；其余 → TXT（PRD 17 §5.2 版式）。
 */
class ReportExporter
{
public:
	/// @param config 监控配置（Monitor 段数据源）；可为 nullptr（该段输出 Unavailable）
	/// @param error 失败原因输出；可为 nullptr
	static bool exportReport(const QString& filePath, const WatchConfig* config,
							 QString* error = nullptr);

private:
	static bool exportTxt(const QString& filePath, const WatchConfig* config, QString* error);
	static bool exportJson(const QString& filePath, const WatchConfig* config, QString* error);
};

} // namespace qt_event_watcher
