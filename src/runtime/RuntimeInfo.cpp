#include "RuntimeInfo.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSysInfo>

namespace qt_event_watcher
{

RuntimeSnapshot RuntimeInfo::collect()
{
	RuntimeSnapshot s;

	// --- Application ---
	s.applicationName = QCoreApplication::applicationName();
	s.applicationFilePath = QCoreApplication::applicationFilePath();
	s.applicationPid = static_cast<quint64>(QCoreApplication::applicationPid());
	s.buildAbi = QSysInfo::buildAbi();
	s.buildConfig = detectBuildConfig();

	// --- Qt ---
	s.qtVersionCompiled = QStringLiteral(QT_VERSION_STR);
	s.qtVersionRuntime = QString::fromLatin1(qVersion());
	s.platformName = QGuiApplication::platformName();
	s.osProductName = QSysInfo::prettyProductName();
	s.osKernelVersion = QStringLiteral("%1 %2")
							.arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());

	// --- Compiler ---
	s.compilerType = detectCompiler();
	s.compilerVersion = detectCompilerVersion();
	s.compilerArch = detectCompilerArch();

	return s;
}

QString RuntimeInfo::detectCompiler()
{
#if defined(_MSC_VER)
	return QStringLiteral("MSVC");
#elif defined(__MINGW64__)
	return QStringLiteral("MinGW-w64");
#elif defined(__MINGW32__)
	return QStringLiteral("MinGW32");
#elif defined(__GNUC__)
	return QStringLiteral("GCC");
#else
	return QStringLiteral("Unknown");
#endif
}

QString RuntimeInfo::detectCompilerVersion()
{
#if defined(_MSC_VER)
	// _MSC_VER：1900=VS2015，1910~1919=VS2017，1920~1929=VS2019，1930+=VS2022
	// if constexpr：_MSC_VER 是编译期常量，运行时 if 会触发 C4127（V6 Q2）
	constexpr int v = _MSC_VER;
	if constexpr (v >= 1930)		return QStringLiteral("VS2022 (%1)").arg(v);
	else if constexpr (v >= 1920)	return QStringLiteral("VS2019 (%1)").arg(v);
	else if constexpr (v >= 1910)	return QStringLiteral("VS2017 (%1)").arg(v);
	else if constexpr (v >= 1900)	return QStringLiteral("VS2015 (%1)").arg(v);
	else							return QString::number(v);
#elif defined(__GNUC__)
	return QStringLiteral("%1.%2.%3")
		.arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#else
	return QStringLiteral("Unknown");
#endif
}

QString RuntimeInfo::detectCompilerArch()
{
#if defined(_M_X64) || defined(__x86_64__)
	return QStringLiteral("x86_64");
#elif defined(_M_IX86) || defined(__i386__)
	return QStringLiteral("x86");
#elif defined(_M_ARM64) || defined(__aarch64__)
	return QStringLiteral("arm64");
#else
	return QStringLiteral("Unknown");
#endif
}

QString RuntimeInfo::detectBuildConfig()
{
#if defined(_DEBUG)
	return QStringLiteral("Debug");
#elif defined(NDEBUG)
	return QStringLiteral("Release");
#else
	return QStringLiteral("Unknown");
#endif
}

} // namespace qt_event_watcher
