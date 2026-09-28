#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace qt_event_watcher
{

/**
 * @brief 单个 Qt Plugin 诊断条目（PRD 16 §7）
 */
struct QtPluginInfo
{
	QString name;		///< 如 qwindows.dll
	QString category;	///< platforms / imageformats / styles / iconengines
	QString fullPath;
	bool exists = false;	///< 插件目录中存在该文件
	bool loaded = false;	///< 已在进程模块列表中（由 markLoadedPlugins 填充）
};

/**
 * @brief Qt 环境快照：版本 / ABI / Plugin 状态（PRD 16 §2、§7）
 */
struct QtEnvironmentSnapshot
{
	QString qtVersionRuntime;	///< qVersion()
	QString buildAbi;			///< QSysInfo::buildAbi()
	QString buildCpuArchitecture;
	QString currentCpuArchitecture;
	QString platformName;
	QString pluginsPath;		///< QLibraryInfo::PluginsPath

	QVector<QtPluginInfo> plugins;

	/// qwindows.dll 缺失对应典型错误："no Qt platform plugin could be initialized"
	bool qwindowsMissing() const
	{
		for (const QtPluginInfo& p : plugins) {
			if (p.name.compare(QStringLiteral("qwindows.dll"), Qt::CaseInsensitive) == 0
				&& p.exists)
				return false;
		}
		return true;
	}
};

/// @brief Qt 版本 / ABI / Plugin 状态采集器
class QtEnvironment
{
public:
	static QtEnvironmentSnapshot collect();

	/// 用进程已加载模块路径集合标记 plugin loaded 状态（阶段2 ModuleEnumerator 接入）
	static void markLoadedPlugins(QtEnvironmentSnapshot& snapshot, const QStringList& loadedPaths);

private:
	/// 扫描各 category 插件：先 exe 目录（windeployqt 布局）再 QLibraryInfo 路径，同名去重
	static QVector<QtPluginInfo> scanPlugins(const QString& pluginsPath, const QString& appDir);

	static constexpr const char* PluginCategories[] =
		{ "platforms", "imageformats", "styles", "iconengines" };
};

} // namespace qt_event_watcher
