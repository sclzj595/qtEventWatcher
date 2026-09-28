#pragma once

#include <QtGlobal>
#include <QString>
#include <QStringList>
#include <QVector>

namespace qt_event_watcher
{

/**
 * @brief 单条 DLL 导入依赖的诊断状态（PRD 16 §4~§6）
 */
enum class DependencyStatus : quint8
{
	Loaded,				///< 已加载（进程模块列表命中）
	PathUnexpected,		///< 加载路径异常（如 Qt DLL 来自非预期目录）
	VersionMismatch,	///< 版本不匹配（运行时 Qt 与编译时不一致）
	Missing				///< 未加载且磁盘上找不到
};

/**
 * @brief 单条导入依赖诊断结果（纯值）
 */
struct DependencyInfo
{
	QString name;			///< 导入 DLL 名，如 Qt5Core.dll
	DependencyStatus status = DependencyStatus::Loaded;
	QString resolvedPath;	///< Loaded：实际加载路径；PathUnexpected(磁盘找到)：磁盘路径
	QString hint;			///< 异常/缺失提示（搜索位置与可能原因）
};

/**
 * @brief 依赖分析总结果（PRD 16 §5：构建依赖与运行时依赖分离呈现）
 * 构建依赖 = PE Import Directory 的直接导入清单；
 * 运行时依赖 = 逐条判定的实际加载状态。
 */
struct DependencyAnalysis
{
	bool ok = false;		///< PE 解析成功
	QString failReason;		///< 失败时为 "DependencyAnalyzer failed: ..."（PRD 16 §8）
	QString exePath;
	QVector<DependencyInfo> dependencies;

	int missingCount() const
	{
		int n = 0;
		for (const DependencyInfo& d : dependencies)
			if (d.status == DependencyStatus::Missing) ++n;
		return n;
	}

	/// 需要关注的问题数（Missing + PathUnexpected + VersionMismatch）
	int problemCount() const
	{
		int n = 0;
		for (const DependencyInfo& d : dependencies)
			if (d.status != DependencyStatus::Loaded) ++n;
		return n;
	}
};

/**
 * @brief PE Import 依赖分析器（PRD 16 §6，Windows 第一阶段）
 * 只解析 exe 直接导入形成基础依赖树，不复制专业 Dependencies 工具的全部能力。
 * 任何解析失败仅置 failReason，绝不抛出/崩溃（PRD 16 §8）。
 */
class PeDependencyAnalyzer
{
public:
	/// 分析指定 exe（通常为 QCoreApplication::applicationFilePath()）
	static DependencyAnalysis analyze(const QString& exePath);

private:
	/// 读 PE Import Directory，返回直接导入 DLL 名列表
	static QStringList readImports(const QString& exePath, bool& ok, QString& failReason);
};

} // namespace qt_event_watcher
