#pragma once

#include "DashboardModel.h"

#include <QString>

namespace qt_event_watcher {
namespace dashboard {

/// 渲染选项（过滤口径见 filterModel；title 仅展示层）
struct HtmlOptions
{
	QString title = QStringLiteral("Scout 可视化仪表盘");
	QVector<int> kinds;      ///< 保留的 kind 白名单（0..3），空 = 不过滤
	qint64 pidFilter = 0;    ///< 只保留该 host_pid 会话，0 = 不过滤
};

/**
 * @brief Scout 独立可视化仪表盘（V7 S3，docs/32 §9）
 *
 * 消费 DashboardModel（aggregator --out JSON 快照解析产物），输出单文件
 * 自包含 HTML（零外部依赖，双击即开断网可用）。与 AggregationReporter
 * （通用聚合报告）分工互补：本工具面向 scout 探针专属视图。
 *
 * 五章节：①会话概览与健康度（per-pid，来源徽章 + 目标 receiver 推断）
 * ②冻结时间轴（S3b 静态表格；S3c 起为 JS 泳道，此表保留为无 JS 降级）
 * ③CDP 长任务（per-url 汇总 + CSS 条形直方零 JS + 明细折叠）④CPU 与慢事件
 * （cpuSpin 表 + TOP receiver 对齐既有口径）⑤原始明细（<details> 折叠）
 *
 * 纯冷路径（PRD 14 哲学），仅在 CLI 显式导出时执行。
 */
bool exportDashboard(const DashboardModel &model, const QString &outPath,
					 const HtmlOptions &options, QString *error = nullptr);

} // namespace dashboard
} // namespace qt_event_watcher
