#pragma once

#include <QString>

namespace qt_event_watcher
{

class WatchConfig;
class EventStatistics;

/**
 * @brief 自包含 HTML 报告（V3 Phase C2，PRD 21 §6 C 线"数据消费"）
 *
 * 单文件静态渲染（纯 HTML+CSS，零外部依赖/零 JS，离线可打开、可直接归档），
 * 面向"一次导出、人工阅读"的交付场景；与 DataExporter（机器回放：
 * CSV/JSON/SQLite）和 ReportExporter（运行环境诊断）分工互补。
 *
 * 章节结构：
 * - 概览：记录计数（slowEvents / metaCalls / freezeEvents）+ 监控配置
 * - 诊断结论：DiagnosticSummarizer::analyze 四规则 findings（严重级别徽标）
 * - 统计 TOP：跨周期合并累计耗时 TOP-10（含 Exclusive Cost）
 * - 冻结时间线：FreezeWatch 三态告警行（started/ongoing/recovered）
 * - 明细记录：最近 200 条（<details> 折叠，kind 徽标着色）
 *
 * 仅在用户/测试显式调用时执行，不进入监控热路径（PRD 14 哲学）。
 */
class HtmlReporter
{
public:
	static bool exportHtml(const QString& filePath, const WatchConfig* config,
						   const EventStatistics* statistics = nullptr,
						   QString* error = nullptr);

	HtmlReporter() = delete;
};

} // namespace qt_event_watcher
