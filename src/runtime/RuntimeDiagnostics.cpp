#include "RuntimeDiagnostics.h"

#include <QDateTime>

#include <exception>

namespace qt_event_watcher
{

RuntimeDiagnostics::Overview RuntimeDiagnostics::collectOverview()
{
	Overview overview;
	try {
		overview.runtime = RuntimeInfo::collect();
		overview.qtEnvironment = QtEnvironment::collect();
	} catch (const std::exception& e) {
		// PRD 16 §8：诊断自身异常不得影响主程序；快照留空由 UI 呈现缺省值
		overview.runtime.applicationName =
			QStringLiteral("RuntimeDiagnostics failed: %1").arg(QString::fromLatin1(e.what()));
	} catch (...) {
		overview.runtime.applicationName =
			QStringLiteral("RuntimeDiagnostics failed");
	}
	return overview;
}

DiagnosticsReport RuntimeDiagnostics::collectFull()
{
	DiagnosticsReport report;
	try {
		report.runtime = RuntimeInfo::collect();
		report.qtEnvironment = QtEnvironment::collect();

		// 模块枚举（失败留空列表，PRD 16 §8 允许降级）
		report.modules = ModuleEnumerator::enumerate();
		report.modulesOk = !report.modules.isEmpty();
		QtEnvironment::markLoadedPlugins(report.qtEnvironment,
										 ModuleEnumerator::loadedPaths());

		// PE Import 分析（内部自带 failReason 降级路径）
		report.dependencies =
			PeDependencyAnalyzer::analyze(report.runtime.applicationFilePath);
	} catch (const std::exception& e) {
		report.dependencies.ok = false;
		report.dependencies.failReason =
			QStringLiteral("DependencyAnalyzer failed: %1")
				.arg(QString::fromLatin1(e.what()));
	} catch (...) {
		report.dependencies.ok = false;
		report.dependencies.failReason =
			QStringLiteral("DependencyAnalyzer failed");
	}

	report.generatedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
	return report;
}

} // namespace qt_event_watcher
