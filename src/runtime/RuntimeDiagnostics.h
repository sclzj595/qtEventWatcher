#pragma once

#include "QtEnvironment.h"
#include "ModuleEnumerator.h"
#include "PeDependencyAnalyzer.h"
#include "RuntimeInfo.h"

#include <QString>
#include <QVector>

namespace qt_event_watcher
{

/**
 * @brief 完整诊断报告（PRD 16 §2 四模块聚合，纯值）
 */
struct DiagnosticsReport
{
	RuntimeSnapshot runtime;
	QtEnvironmentSnapshot qtEnvironment;
	QVector<ModuleInfo> modules;	///< 空 = 枚举失败或平台不支持
	DependencyAnalysis dependencies;///< ok=false 时查看 failReason
	QString generatedAt;			///< 报告生成时间（ISO 带毫秒）
	bool modulesOk = false;			///< 模块枚举成功
};

/**
 * @brief 运行环境诊断门面（PRD 16 §1：不占 Watch_Fun bit、不进 notify() 热路径）
 * 两级采集：
 * - collectOverview()：打开运行环境页面时调用（轻量，无模块枚举/PE 解析）
 * - collectFull()：仅用户主动触发（模块枚举 + PE Import 分析）
 * 全链路异常兜底，不向调用方抛出（PRD 16 §8）。
 */
class RuntimeDiagnostics
{
public:
	struct Overview
	{
		RuntimeSnapshot runtime;
		QtEnvironmentSnapshot qtEnvironment;
	};

	static Overview collectOverview();
	static DiagnosticsReport collectFull();
};

} // namespace qt_event_watcher
