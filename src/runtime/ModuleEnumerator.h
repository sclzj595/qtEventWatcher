#pragma once

#include <QtGlobal>
#include <QString>
#include <QStringList>
#include <QVector>

namespace qt_event_watcher
{

/**
 * @brief 当前进程已加载模块条目（PRD 16 §4）
 * 纯值结构：baseAddress/size 仅为诊断展示，不用于任何指针运算。
 */
struct ModuleInfo
{
	QString name;			///< 如 Qt5Core.dll
	QString fullPath;		///< 加载来源完整路径
	quint64 baseAddress = 0;	///< 模块基址
	quint64 size = 0;		///< SizeOfImage 字节数
};

/**
 * @brief 运行时模块枚举器（PRD 16 §4）
 * 平台隔离：Windows 用 PSAPI（K32 系列，kernel32 自带）；
 * 非 Windows 平台返回空列表（Linux /proc/<pid>/maps 后续实现）。
 */
class ModuleEnumerator
{
public:
	/// 枚举当前进程全部已加载模块；失败返回空列表（PRD 16 §8：不崩溃）
	static QVector<ModuleInfo> enumerate();

	/// 便捷接口：仅返回已加载模块完整路径（Plugin loaded 标记用）
	static QStringList loadedPaths();

private:
	static QVector<ModuleInfo> enumerateWindows();
};

} // namespace qt_event_watcher
