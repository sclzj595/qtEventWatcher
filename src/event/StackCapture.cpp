#include "StackCapture.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace qt_event_watcher
{

namespace
{

/// HMODULE → 模块文件名缓存：GetModuleFileNameW 持 loader lock，
/// 同模块多帧只拷贝一次（告警风暴 32 帧×N 条/s 场景显著降本）。
/// 生命周期备注：模块卸载后 HMODULE 复用可能命中旧名——诊断快照场景可容忍
/// （重载同名 DLL 概率极低，且栈帧本身仍可用）。
std::string moduleName(HMODULE module)
{
	static std::mutex mutex;
	static std::unordered_map<HMODULE, std::string> cache;

	std::lock_guard<std::mutex> lock(mutex);
	const auto it = cache.find(module);
	if (it != cache.end())
		return it->second;

	wchar_t path[1024] = {};
	std::string name = "?";
	if (GetModuleFileNameW(module, path, 1024) > 0) {
		std::wstring wide(path);
		const std::size_t slash = wide.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			wide.erase(0, slash + 1);
		// 窄化：模块名以 ASCII 为主，非 ASCII 字符逐个丢弃（不影响地址归因）；
		// 空格替换 '_'：保证日志 key=value 单 token 完整（WatchRecordStore 按空格分词）
		name.clear();
		for (wchar_t c : wide) {
			if (c == L' ')
				name += '_';
			else if (c > 0 && c < 128)
				name += static_cast<char>(c);
		}
		if (name.empty())
			name = "?";
	}
	cache.emplace(module, name);
	return name;
}

} // namespace

std::vector<StackFrame> StackCapture::capture()
{
	std::vector<StackFrame> frames;
	void* addresses[kMaxFrames] = {};
	// RtlCaptureStackBackTrace：kernel32 导出，winnt.h 声明（Win8+ SDK / MinGW-w64 均有）
	const USHORT depth = RtlCaptureStackBackTrace(0, kMaxFrames, addresses, nullptr);
	frames.reserve(depth);
	for (USHORT i = 0; i < depth; ++i) {
		StackFrame frame;
		frame.offset = reinterpret_cast<std::uint64_t>(addresses[i]);
		HMODULE module = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
								| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								reinterpret_cast<LPCWSTR>(addresses[i]), &module)
			&& module != nullptr) {
			frame.module = moduleName(module);
			frame.offset -= reinterpret_cast<std::uint64_t>(module);
		}
		frames.push_back(std::move(frame));
	}
	return frames;
}

std::string StackCapture::format(const std::vector<StackFrame>& frames)
{
	if (frames.empty())
		return {};
	std::string out;
	char buf[32] = {};
	for (const StackFrame& f : frames) {
		if (!out.empty())
			out += ',';
		std::snprintf(buf, sizeof(buf), "!0x%llx",
					  static_cast<unsigned long long>(f.offset));
		out += f.module;
		out += buf;
	}
	return out;
}

} // namespace qt_event_watcher

#elif defined(__linux__)	// Linux（glibc）：backtrace + dladdr，语义对齐 Windows 版

#include <dlfcn.h>
#include <execinfo.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace qt_event_watcher
{

namespace
{

/// 模块基址 → 模块文件名缓存：dladdr 每帧调用有 dso 查找成本，同基址只查一次
/// （告警风暴 32 帧×N 条/s 场景显著降本，语义对齐 Windows 版 HMODULE 缓存）。
/// 生命周期备注：dlclose 后 dli_fbase 复用可能命中旧名——诊断快照场景可容忍
/// （重载同名 so 概率极低，且栈帧本身仍可用）。
std::string moduleName(void* base)
{
	static std::mutex mutex;
	static std::unordered_map<void*, std::string> cache;

	std::lock_guard<std::mutex> lock(mutex);
	const auto it = cache.find(base);
	if (it != cache.end())
		return it->second;

	std::string name = "?";
	Dl_info info = {};
	if (dladdr(base, &info) && info.dli_fname != nullptr) {
		// basename（'/' 分隔）；空格替换 '_'：保证日志 key=value 单 token 完整
		// （WatchRecordStore 按空格分词）；路径为 UTF-8，原样保留不影响归因
		std::string path = info.dli_fname;
		const std::size_t slash = path.find_last_of('/');
		if (slash != std::string::npos)
			path.erase(0, slash + 1);
		name.clear();
		for (char c : path) {
			if (c == ' ')
				name += '_';
			else
				name += c;
		}
		if (name.empty())
			name = "?";
	}
	cache.emplace(base, name);
	return name;
}

} // namespace

std::vector<StackFrame> StackCapture::capture()
{
	std::vector<StackFrame> frames;
	void* addresses[kMaxFrames] = {};
	// backtrace()：glibc 内建（execinfo.h），取当前调用栈 ≤32 帧（含 capture
	// 自身与告警函数两帧监控器帧，与 Windows 版同口径）
	const int depth = ::backtrace(addresses, kMaxFrames);
	frames.reserve(depth > 0 ? depth : 0);
	for (int i = 0; i < depth; ++i) {
		StackFrame frame;
		frame.offset = reinterpret_cast<std::uint64_t>(addresses[i]);
		Dl_info info = {};
		if (dladdr(addresses[i], &info) && info.dli_fbase != nullptr) {
			frame.module = moduleName(info.dli_fbase);
			frame.offset -= reinterpret_cast<std::uint64_t>(info.dli_fbase);
		}
		frames.push_back(std::move(frame));
	}
	return frames;
}

std::string StackCapture::format(const std::vector<StackFrame>& frames)
{
	if (frames.empty())
		return {};
	std::string out;
	char buf[32] = {};
	for (const StackFrame& f : frames) {
		if (!out.empty())
			out += ',';
		std::snprintf(buf, sizeof(buf), "!0x%llx",
					  static_cast<unsigned long long>(f.offset));
		out += f.module;
		out += buf;
	}
	return out;
}

} // namespace qt_event_watcher

#else	// 其他平台：空实现（可移植性约定同 C3）

namespace qt_event_watcher
{

std::vector<StackFrame> StackCapture::capture()
{
	return {};
}

std::string StackCapture::format(const std::vector<StackFrame>& frames)
{
	(void)frames;
	return {};
}

} // namespace qt_event_watcher

#endif	// _WIN32 / __linux__
