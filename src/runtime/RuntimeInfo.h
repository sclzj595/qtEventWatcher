#pragma once

#include <QtGlobal>
#include <QString>

namespace qt_event_watcher
{

/**
 * @brief 应用 / Qt / 编译器环境快照（PRD 16 §3）
 * 纯值结构：仅在用户主动触发诊断时采集，不进入 notify() 热路径（PRD 16 §1）。
 */
struct RuntimeSnapshot
{
	/// --- Application ---
	QString applicationName;		///< QCoreApplication::applicationName()
	QString applicationFilePath;	///< 可执行文件完整路径
	quint64 applicationPid = 0;
	QString buildAbi;				///< QSysInfo::buildAbi()，编译目标 ABI
	QString buildConfig;			///< Debug / Release（编译期宏判定）

	/// --- Qt ---
	QString qtVersionCompiled;		///< QT_VERSION_STR（编译时头文件版本）
	QString qtVersionRuntime;		///< qVersion()（实际加载的 Qt5Core.dll 版本）
	QString platformName;			///< QGuiApplication::platformName()，如 "windows"
	QString osProductName;			///< QSysInfo::prettyProductName()
	QString osKernelVersion;		///< kernelType + kernelVersion

	/// --- Compiler ---
	QString compilerType;			///< MSVC / MinGW-w64 / GCC
	QString compilerVersion;		///< 如 "VS2019 (1929)"
	QString compilerArch;			///< 编译目标架构，如 x86_64

	/// 编译时与运行时 Qt 版本不一致本身即有诊断价值（DLL 版本错配的先兆）
	bool qtVersionMatch() const {	return qtVersionCompiled == qtVersionRuntime;	}
};

/// @brief 运行环境基础信息采集器（PRD 16 §3，无状态、无副作用）
class RuntimeInfo
{
public:
	static RuntimeSnapshot collect();

private:
	static QString detectCompiler();
	static QString detectCompilerVersion();
	static QString detectCompilerArch();
	static QString detectBuildConfig();
};

} // namespace qt_event_watcher
