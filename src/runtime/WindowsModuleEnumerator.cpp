#include "ModuleEnumerator.h"

#ifdef Q_OS_WIN

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#ifdef __MINGW32__
// MinGW-w64 的 psapi.h 不含 K32* 内联转发声明（MSVC 专属机制，
// kernel32 直接导出）；映射到 psapi.dll 导出的无前缀等价 API，
// 并在 CMake 中链接 psapi 库
#define K32EnumProcessModulesEx  EnumProcessModulesEx
#define K32GetModuleFileNameExW  GetModuleFileNameExW
#define K32GetModuleInformation  GetModuleInformation
#endif

#include <QFileInfo>
#include <vector>

namespace qt_event_watcher
{

QVector<ModuleInfo> ModuleEnumerator::enumerate()
{
	return enumerateWindows();
}

QVector<ModuleInfo> ModuleEnumerator::enumerateWindows()
{
	QVector<ModuleInfo> result;

	const HANDLE process = GetCurrentProcess();

	// 第一遍：取所需字节数（LIST_MODULES_ALL 含 32/64 位模块，本进程内均为同架构）
	DWORD neededBytes = 0;
	if (!K32EnumProcessModulesEx(process, nullptr, 0, &neededBytes, LIST_MODULES_ALL)
		|| neededBytes == 0) {
		return result;	// 枚举失败返回空（PRD 16 §8：分析失败不允许崩溃）
	}

	std::vector<HMODULE> modules(neededBytes / sizeof(HMODULE));
	if (!K32EnumProcessModulesEx(process, modules.data(),
								 static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
								 &neededBytes, LIST_MODULES_ALL)) {
		return result;
	}

	result.reserve(static_cast<int>(modules.size()));
	for (HMODULE handle : modules) {
		wchar_t pathBuffer[MAX_PATH] = {};
		if (K32GetModuleFileNameExW(process, handle, pathBuffer, MAX_PATH) == 0)
			continue;

		MODULEINFO mi = {};
		if (!K32GetModuleInformation(process, handle, &mi, sizeof(mi)))
			continue;

		ModuleInfo info;
		info.fullPath = QString::fromWCharArray(pathBuffer);
		info.name = QFileInfo(info.fullPath).fileName();
		info.baseAddress = reinterpret_cast<quint64>(mi.lpBaseOfDll);
		info.size = mi.SizeOfImage;
		result.append(info);
	}

	return result;
}

QStringList ModuleEnumerator::loadedPaths()
{
	QStringList paths;
	const QVector<ModuleInfo> modules = enumerate();
	paths.reserve(modules.size());
	for (const ModuleInfo& m : modules)
		paths.append(m.fullPath);
	return paths;
}

} // namespace qt_event_watcher

#else // 非 Windows：PRD 16 §9 平台矩阵，Linux 模块枚举后续实现

namespace qt_event_watcher
{

QVector<ModuleInfo> ModuleEnumerator::enumerate() {	return {};	}
QStringList ModuleEnumerator::loadedPaths() {	return {};	}

} // namespace qt_event_watcher

#endif // Q_OS_WIN
