#include "AlarmSuppressor.h"

#include <chrono>

namespace qt_event_watcher
{

AlarmDecision AlarmSuppressor::evaluate(const std::string& key, int windowMs)
{
	const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return evaluate(key, nowMs, windowMs);
}

AlarmDecision AlarmSuppressor::evaluate(const std::string& key, std::int64_t nowSteadyMs, int windowMs)
{
	if (windowMs <= 0)
		windowMs = kWindowMs;	// 非法值回退缺省（与 sanitizePositive 兜底同构）

	std::lock_guard<std::mutex> lock(m_mutex);

	WindowState& window = m_windows[key];
	AlarmDecision decision;

	// startMs < 0 = 尚无窗口（哨兵，避免 t=0 边界误判为窗口内）
	if (window.startMs < 0 || nowSteadyMs - window.startMs >= windowMs) {
		// 窗口关闭：冲刷上一窗静默累计，本条作为新窗口首条必出
		decision.suppressedFlushed = window.startMs >= 0 ? window.suppressed : 0;
		window.startMs = nowSteadyMs;
		window.suppressed = 0;
	} else {
		// 窗口内静默累计（首条在窗口创建时已按 emitNow=true 输出）
		++window.suppressed;
		decision.emitNow = false;
	}
	return decision;
}

} // namespace qt_event_watcher
