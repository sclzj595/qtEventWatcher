#include "QtEnvironment.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QSet>

namespace qt_event_watcher
{

QtEnvironmentSnapshot QtEnvironment::collect()
{
	QtEnvironmentSnapshot s;

	s.qtVersionRuntime = QString::fromLatin1(qVersion());
	s.buildAbi = QSysInfo::buildAbi();
	s.buildCpuArchitecture = QSysInfo::buildCpuArchitecture();
	s.currentCpuArchitecture = QSysInfo::currentCpuArchitecture();
	s.platformName = QGuiApplication::platformName();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	s.pluginsPath = QLibraryInfo::path(QLibraryInfo::PluginsPath);
#else
	s.pluginsPath = QLibraryInfo::location(QLibraryInfo::PluginsPath);
#endif

	// 双根扫描：QLibraryInfo::PluginsPath + exe 目录（windeployqt 部署布局）。
	// windeployqt 后 QLibraryInfo 指向 <exeDir>/plugins（通常不存在），
	// 真实插件位于 exe 目录的直接子目录（platforms/ 等），漏扫会误报 qwindows 缺失
	s.plugins = scanPlugins(s.pluginsPath, QCoreApplication::applicationDirPath());
	return s;
}

void QtEnvironment::markLoadedPlugins(QtEnvironmentSnapshot& snapshot, const QStringList& loadedPaths)
{
	for (QtPluginInfo& p : snapshot.plugins) {
		for (const QString& loaded : loadedPaths) {
			if (QString::compare(p.fullPath, loaded, Qt::CaseInsensitive) == 0) {
				p.loaded = true;
				break;
			}
		}
	}
}

QVector<QtPluginInfo> QtEnvironment::scanPlugins(const QString& pluginsPath, const QString& appDir)
{
	QVector<QtPluginInfo> result;
	QSet<QString> seen;	// 已记录的插件名（小写），双根去重

	for (const char* category : PluginCategories) {
		const QString cat = QString::fromLatin1(category);
		QStringList roots;
		if (!appDir.isEmpty())
			roots.append(appDir + QLatin1Char('/') + cat);
		if (!pluginsPath.isEmpty())
			roots.append(pluginsPath + QLatin1Char('/') + cat);

		for (const QString& dir : roots) {
			const QFileInfoList files =
				QDir(dir).entryInfoList(QStringList() << QStringLiteral("*.dll"), QDir::Files);
			for (const QFileInfo& fi : files) {
				const QString lower = fi.fileName().toLower();
				if (seen.contains(lower))
					continue;	// 同名插件以 exe 目录优先（实际加载者）
				seen.insert(lower);

				QtPluginInfo p;
				p.name = fi.fileName();
				p.category = cat;
				p.fullPath = fi.absoluteFilePath();
				p.exists = true;
				result.append(p);
			}
		}
		// 目录缺失/为空不补条目：platforms 空缺由 qwindowsMissing() 汇总提示，
		// 其余三类为可选插件，空目录无诊断意义
	}

	return result;
}

} // namespace qt_event_watcher
