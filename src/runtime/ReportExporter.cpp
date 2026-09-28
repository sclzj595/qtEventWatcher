#include "ReportExporter.h"

#include "RuntimeDiagnostics.h"
#include "WatchConfig.h"
#include "WatchLogCapture.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace qt_event_watcher
{

namespace
{

/// key=value 日志解析（与 Demo 明细表同规则：空格分词、首 '=' 分割）
QHash<QString, QString> parseKv(const std::string& raw)
{
	QHash<QString, QString> kv;
	const QStringList tokens =
		QString::fromStdString(raw).split(QLatin1Char(' '), Qt::SkipEmptyParts);
	for (const QString& t : tokens) {
		const int eq = t.indexOf(QLatin1Char('='));
		if (eq > 0)
			kv.insert(t.left(eq), t.mid(eq + 1));
	}
	return kv;
}

const char* dependencyTag(DependencyStatus status)
{
	switch (status) {
	case DependencyStatus::Loaded:			return "Loaded";
	case DependencyStatus::PathUnexpected:	return "PathUnexpected";
	case DependencyStatus::VersionMismatch:	return "VersionMismatch";
	case DependencyStatus::Missing:			return "Missing";
	}
	return "?";
}

} // namespace

bool ReportExporter::exportReport(const QString& filePath, const WatchConfig* config,
								  QString* error)
{
	if (filePath.isEmpty()) {
		if (error)	*error = QStringLiteral("empty file path");
		return false;
	}
	// 后缀决定格式（大小写不敏感）；无后缀默认 TXT
	const QString suffix = QFileInfo(filePath).suffix();
	const bool json = suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
	return json ? exportJson(filePath, config, error)
				: exportTxt(filePath, config, error);
}

bool ReportExporter::exportTxt(const QString& filePath, const WatchConfig* config, QString* error)
{
	// 完整诊断一次采集，TXT 与 JSON 共用（PRD 16 §1：仅用户主动导出时执行重操作）
	const DiagnosticsReport report = RuntimeDiagnostics::collectFull();
	const auto& s = report.runtime;
	const auto& env = report.qtEnvironment;

	QString out;
	out += QStringLiteral("QtEventWatcher Diagnostic Report\n");
	out += QStringLiteral("================================\n\n");

	// ---- Application（PRD 17 §5.2）----
	out += QStringLiteral("Application\n-----------\n");
	out += QStringLiteral("Name: %1\n").arg(s.applicationName);
	out += QStringLiteral("Path: %1\n").arg(s.applicationFilePath);
	out += QStringLiteral("PID: %1\n").arg(s.applicationPid);
	out += QStringLiteral("Build Config: %1\n\n").arg(s.buildConfig);

	// ---- Environment ----
	out += QStringLiteral("Environment\n-----------\n");
	out += QStringLiteral("OS: %1 (%2)\n").arg(s.osProductName, s.osKernelVersion);
	out += QStringLiteral("Architecture: build %1 / current %2\n")
			   .arg(s.buildAbi, env.currentCpuArchitecture);
	out += QStringLiteral("Compiler: %1 %2 (target %3)\n")
			   .arg(s.compilerType, s.compilerVersion, s.compilerArch);
	out += QStringLiteral("Qt Version: compiled %1 / runtime %2 (%3)\n")
			   .arg(s.qtVersionCompiled, s.qtVersionRuntime,
					s.qtVersionMatch() ? QStringLiteral("match")
									   : QStringLiteral("MISMATCH"));
	out += QStringLiteral("Platform: %1\n\n").arg(s.platformName);

	// ---- Runtime Modules ----
	out += QStringLiteral("Runtime Modules (%1)\n--------------------\n")
			   .arg(report.modulesOk ? QString::number(report.modules.size())
									 : QStringLiteral("enumerate failed"));
	for (const ModuleInfo& m : report.modules)
		out += QStringLiteral("%1  (%2)\n").arg(m.name, m.fullPath);
	out += QLatin1Char('\n');

	// ---- Qt Plugins ----
	out += QStringLiteral("Qt Plugins (%1)\n---------------\n").arg(env.plugins.size());
	for (const QtPluginInfo& p : env.plugins) {
		out += QStringLiteral("%1  %2  [%3]\n")
				   .arg(p.category, p.name,
						p.loaded ? QStringLiteral("Loaded") : QStringLiteral("OnDisk"));
	}
	if (env.qwindowsMissing())
		out += QStringLiteral("!! qwindows.dll missing -> \"no Qt platform plugin could be initialized\"\n");
	out += QLatin1Char('\n');

	// ---- Dependency Information ----
	out += QStringLiteral("Dependency Information (PE Imports)\n-----------------------------------\n");
	if (!report.dependencies.ok) {
		out += report.dependencies.failReason + QLatin1Char('\n');
	} else {
		for (const DependencyInfo& d : report.dependencies.dependencies) {
			out += QStringLiteral("%1  [%2]  %3\n")
					   .arg(d.name, QString::fromLatin1(dependencyTag(d.status)),
							d.resolvedPath.isEmpty() ? QStringLiteral("-") : d.resolvedPath);
			if (!d.hint.isEmpty())
				out += QStringLiteral("    hint: %1\n").arg(d.hint);
		}
	}
	out += QLatin1Char('\n');

	// ---- Monitor ----
	out += QStringLiteral("Monitor\n-------\n");
	if (config != nullptr) {
		// QString::arg 多参重载仅支持同类型，混合 QString/int 需链式
		const QString enabledStr = QStringLiteral("Enabled");
		const QString disabledStr = QStringLiteral("Disabled");

		out += QStringLiteral("Watch_Fun: 0x%1\n")
				   .arg(config->watchFun(), 2, 16, QLatin1Char('0'));
		out += QStringLiteral("Slow Event: %1 (threshold %2 ms)\n")
				   .arg(config->isWatchEnabled(WatchConfig::WatchEvent) ? enabledStr : disabledStr)
				   .arg(config->slowEventThresholdMs());
		out += QStringLiteral("MetaCall: %1 (threshold %2 ms)\n")
				   .arg(config->isWatchEnabled(WatchConfig::WatchMetaCall) ? enabledStr : disabledStr)
				   .arg(config->slowMetaCallThresholdMs());
		out += QStringLiteral("Statistics: %1 (period %2 ms, count threshold %3, cost threshold %4 ms)\n")
				   .arg(config->isWatchEnabled(WatchConfig::WatchEventStatistics) ? enabledStr : disabledStr)
				   .arg(config->eventStatPeriodMs())
				   .arg(config->eventCountThreshold())
				   .arg(config->eventTotalCostThresholdMs());
		out += QStringLiteral("QSS: %1 (load %2 ms, setStyleSheet %3 ms, frequent count %4)\n")
				   .arg(config->isWatchEnabled(WatchConfig::WatchQssMonitor) ? enabledStr : disabledStr)
				   .arg(config->qssLoadThresholdMs())
				   .arg(config->setStyleSheetThresholdMs())
				   .arg(config->qssFrequentCountThreshold());
	} else {
		out += QStringLiteral("Unavailable (config not provided)\n");
	}
	out += QLatin1Char('\n');

	// ---- Recent Diagnostics（四类，PRD 17 §5.1）----
	out += QStringLiteral("Recent Diagnostics\n------------------\n");
	const auto entries = WatchLogCapture::instance().recent(32);
	if (entries.empty()) {
		out += QStringLiteral("(no captured warnings)\n");
	} else {
		for (const auto& e : entries) {
			// 分类判定与 WatchLogCapture::Impl::categoryOf 同规则
			const char* cat = "Other";
			if (e.text.rfind("[EventWatcher]", 0) == 0)			cat = "SlowEvent";
			else if (e.text.rfind("[MetaCallWatcher]", 0) == 0)	cat = "MetaCall";
			else if (e.text.rfind("[EventStatistics]", 0) == 0)	cat = "Statistics";
			else if (e.text.rfind("[QssStyleWatcher]", 0) == 0)	cat = "Qss";
			out += QStringLiteral("[%1] [%2] %3\n")
					   .arg(QString::fromStdString(e.time),
							QString::fromLatin1(cat),
							QString::fromStdString(e.text));
		}
	}
	out += QLatin1Char('\n');

	// 分类计数（PRD 17 §5.1 Event Statistics 汇总口径）
	out += QStringLiteral("Capture Counters\n----------------\n");
	out += QStringLiteral("SlowEvent: %1\n")
			   .arg(WatchLogCapture::instance().categoryCount(WatchLogCapture::CatSlowEvent));
	out += QStringLiteral("MetaCall: %1\n")
			   .arg(WatchLogCapture::instance().categoryCount(WatchLogCapture::CatMetaCall));
	out += QStringLiteral("Statistics: %1\n")
			   .arg(WatchLogCapture::instance().categoryCount(WatchLogCapture::CatEventStat));
	out += QStringLiteral("Qss: %1\n")
			   .arg(WatchLogCapture::instance().categoryCount(WatchLogCapture::CatQss));

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open %1 for write").arg(filePath);
		return false;
	}
	file.write(out.toUtf8());
	return true;
}

bool ReportExporter::exportJson(const QString& filePath, const WatchConfig* config, QString* error)
{
	const DiagnosticsReport report = RuntimeDiagnostics::collectFull();
	const auto& s = report.runtime;
	const auto& env = report.qtEnvironment;

	QJsonObject root;
	root.insert(QStringLiteral("generatedAt"), report.generatedAt);
	root.insert(QStringLiteral("reportVersion"), 1);

	// ---- Application / Environment / Compiler ----
	QJsonObject app;
	app.insert(QStringLiteral("name"), s.applicationName);
	app.insert(QStringLiteral("path"), s.applicationFilePath);
	app.insert(QStringLiteral("pid"), static_cast<qint64>(s.applicationPid));
	app.insert(QStringLiteral("buildConfig"), s.buildConfig);
	app.insert(QStringLiteral("os"), s.osProductName);
	app.insert(QStringLiteral("kernel"), s.osKernelVersion);
	app.insert(QStringLiteral("buildAbi"), s.buildAbi);
	app.insert(QStringLiteral("currentArch"), env.currentCpuArchitecture);
	root.insert(QStringLiteral("application"), app);

	QJsonObject compiler;
	compiler.insert(QStringLiteral("type"), s.compilerType);
	compiler.insert(QStringLiteral("version"), s.compilerVersion);
	compiler.insert(QStringLiteral("targetArch"), s.compilerArch);
	root.insert(QStringLiteral("compiler"), compiler);

	QJsonObject qt;
	qt.insert(QStringLiteral("versionCompiled"), s.qtVersionCompiled);
	qt.insert(QStringLiteral("versionRuntime"), s.qtVersionRuntime);
	qt.insert(QStringLiteral("versionMatch"), s.qtVersionMatch());
	qt.insert(QStringLiteral("platform"), s.platformName);
	qt.insert(QStringLiteral("pluginsPath"), env.pluginsPath);
	root.insert(QStringLiteral("qtEnvironment"), qt);

	// ---- Runtime Modules ----
	QJsonArray modules;
	for (const ModuleInfo& m : report.modules) {
		QJsonObject o;
		o.insert(QStringLiteral("name"), m.name);
		o.insert(QStringLiteral("path"), m.fullPath);
		o.insert(QStringLiteral("baseAddress"), static_cast<qint64>(m.baseAddress));
		o.insert(QStringLiteral("size"), static_cast<qint64>(m.size));
		modules.append(o);
	}
	root.insert(QStringLiteral("modules"), modules);
	root.insert(QStringLiteral("modulesOk"), report.modulesOk);

	// ---- Qt Plugins ----
	QJsonArray plugins;
	for (const QtPluginInfo& p : env.plugins) {
		QJsonObject o;
		o.insert(QStringLiteral("category"), p.category);
		o.insert(QStringLiteral("name"), p.name);
		o.insert(QStringLiteral("path"), p.fullPath);
		o.insert(QStringLiteral("loaded"), p.loaded);
		plugins.append(o);
	}
	root.insert(QStringLiteral("plugins"), plugins);
	root.insert(QStringLiteral("qwindowsMissing"), env.qwindowsMissing());

	// ---- Dependencies ----
	QJsonObject deps;
	deps.insert(QStringLiteral("ok"), report.dependencies.ok);
	deps.insert(QStringLiteral("failReason"), report.dependencies.failReason);
	QJsonArray depList;
	for (const DependencyInfo& d : report.dependencies.dependencies) {
		QJsonObject o;
		o.insert(QStringLiteral("name"), d.name);
		o.insert(QStringLiteral("status"), QString::fromLatin1(dependencyTag(d.status)));
		o.insert(QStringLiteral("path"), d.resolvedPath);
		if (!d.hint.isEmpty())
			o.insert(QStringLiteral("hint"), d.hint);
		depList.append(o);
	}
	deps.insert(QStringLiteral("items"), depList);
	root.insert(QStringLiteral("dependencies"), deps);

	// ---- Monitor ----
	if (config != nullptr) {
		QJsonObject monitor;
		monitor.insert(QStringLiteral("watchFun"), static_cast<qint64>(config->watchFun()));
		monitor.insert(QStringLiteral("slowEventEnabled"),
					   config->isWatchEnabled(WatchConfig::WatchEvent));
		monitor.insert(QStringLiteral("slowEventThresholdMs"), config->slowEventThresholdMs());
		monitor.insert(QStringLiteral("metaCallEnabled"),
					   config->isWatchEnabled(WatchConfig::WatchMetaCall));
		monitor.insert(QStringLiteral("slowMetaCallThresholdMs"),
					   config->slowMetaCallThresholdMs());
		monitor.insert(QStringLiteral("statisticsEnabled"),
					   config->isWatchEnabled(WatchConfig::WatchEventStatistics));
		monitor.insert(QStringLiteral("eventStatPeriodMs"), config->eventStatPeriodMs());
		monitor.insert(QStringLiteral("eventCountThreshold"), config->eventCountThreshold());
		monitor.insert(QStringLiteral("eventTotalCostThresholdMs"),
					   config->eventTotalCostThresholdMs());
		monitor.insert(QStringLiteral("qssEnabled"),
					   config->isWatchEnabled(WatchConfig::WatchQssMonitor));
		monitor.insert(QStringLiteral("qssLoadThresholdMs"), config->qssLoadThresholdMs());
		monitor.insert(QStringLiteral("setStyleSheetThresholdMs"),
					   config->setStyleSheetThresholdMs());
		monitor.insert(QStringLiteral("qssFrequentCountThreshold"),
					   config->qssFrequentCountThreshold());
		root.insert(QStringLiteral("monitor"), monitor);
	}

	// ---- Recent Diagnostics ----
	QJsonArray recent;
	for (const auto& e : WatchLogCapture::instance().recent(32)) {
		QJsonObject o;
		o.insert(QStringLiteral("time"), QString::fromStdString(e.time));
		o.insert(QStringLiteral("level"), e.level);
		const QHash<QString, QString> kv = parseKv(e.text);
		QString category = QStringLiteral("Other");
		if (e.text.rfind("[EventWatcher]", 0) == 0)			category = QStringLiteral("SlowEvent");
		else if (e.text.rfind("[MetaCallWatcher]", 0) == 0)	category = QStringLiteral("MetaCall");
		else if (e.text.rfind("[EventStatistics]", 0) == 0)	category = QStringLiteral("Statistics");
		else if (e.text.rfind("[QssStyleWatcher]", 0) == 0)	category = QStringLiteral("Qss");
		o.insert(QStringLiteral("category"), category);
		QJsonObject fields;
		for (auto it = kv.constBegin(); it != kv.constEnd(); ++it)
			fields.insert(it.key(), it.value());
		o.insert(QStringLiteral("fields"), fields);
		o.insert(QStringLiteral("raw"), QString::fromStdString(e.text));
		recent.append(o);
	}
	root.insert(QStringLiteral("recentDiagnostics"), recent);

	QJsonDocument doc(root);
	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)	*error = QStringLiteral("cannot open %1 for write").arg(filePath);
		return false;
	}
	file.write(doc.toJson(QJsonDocument::Indented));
	return true;
}

} // namespace qt_event_watcher
