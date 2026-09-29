#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qt_event_watcher
{

/// 调用栈单帧快照（V3 Phase A1）：模块内偏移寻址，符号解析放导出侧/离线工具
struct StackFrame
{
	std::string module;			///< 模块文件名（空格替换 '_'，保证日志 key=value token 完整）
	std::uint64_t offset = 0;	///< 模块内偏移（RVA）；模块归属未知时为原始运行时地址
};

/**
 * @brief 慢告警调用栈采集（V3 Phase A1，仅告警路径调用，热路径零成本）
 *
 * 归因从"事件级"升级到"函数级"：慢事件 / 慢 MetaCall 命中阈值后采集当前
 * 调用栈（≤32 帧），输出 {模块名, 模块内偏移} 快照。
 *
 * 采集侧零 dbghelp 依赖（SymInitialize 首次加载可达百 ms 级，禁止进告警路径，
 * 否则监控器在用户最卡的时刻放大卡顿）；默认输出 模块名!偏移，
 * 符号解析放导出侧离线工具（llvm-symbolizer 等，需 PDB / MinGW debug info）。
 */
class StackCapture
{
public:
	static constexpr int kMaxFrames = 32;

	/// 采集当前调用栈（栈顶在前，含 capture 自身与告警函数两帧监控器帧）；
	/// 非 Windows 返回空 vector（可移植性约定同 C3 Linux 验证）
	static std::vector<StackFrame> capture();

	/// 帧序列 → "mod!0xoff,mod!0xoff"（空栈返回空串）
	static std::string format(const std::vector<StackFrame>& frames);

	StackCapture() = delete;
};

} // namespace qt_event_watcher
