#pragma once

/// ScoutAlarmEmitter - Scout V1 探针侧风暴抑制门（docs/34 §3.4）
/// 复用核心库 AlarmSuppressor（1s 窗口首条必出 + 懒冲刷 suppressed=N 汇总），
/// key = kind:receiver（freeze:<receiver> / cpu:<receiver> / cdp:<url>）。
/// 状态配对语义保障：冻结 started/ongoing/recovered/lost 是配对行——窗口内
/// 首条必出保证配对头不被吞，仅压制同 key 洪峰重复（系统级卡死时 CDP
/// longtask 连发等）。
/// 级别路由与核心库语义一致：emitNow → WARN（用户可见）；静默条 → DEBUG
/// （文件/控制台 sink 按用户级过滤掉，RecordSink debug 全量接住——回放完整
/// 性优先）；窗口关闭冲刷 suppressed>0 → WARN 汇总补发（懒冲刷：风暴停止后
/// 该 key 下次告警到达时补出，零定时器零常驻）。
/// 每探针实例各持一个（key 空间按实例隔离；AlarmSuppressor 内部互斥双保险）。

#include "AlarmSuppressor.h"
#include "WatchLogMacros.h"

#include <string>
#include <utility>

namespace qt_event_watcher {

class ScoutAlarmEmitter
{
public:
	/// key = kind:receiver；fmtStr/args 与 QEW_LOG_* 同语法（spdlog fmt）。
	/// 静默条自动降级 DEBUG，窗口冲刷自动补 WARN 汇总——调用点单行接入。
	/// 注意：方法名不可叫 emit——Qt 将 emit 定义为空宏（signal 关键字），
	/// 会把函数签名整行撕碎（V1 R3c 实测编译炸穿下游所有 Qt 头）。
	template <typename... Args>
	void emitAlarm(const std::string &key, const char *fmtStr, Args &&...args)
	{
		const AlarmDecision d
			= m_suppressor.evaluate(key, AlarmSuppressor::kWindowMs);
		if (d.emitNow)
			QEW_LOG_WARN(fmtStr, std::forward<Args>(args)...);
		else
			QEW_LOG_DEBUG(fmtStr, std::forward<Args>(args)...);
		if (d.suppressedFlushed > 0)
			QEW_LOG_WARN("[Scout] alarm storm suppressed={} key={:s}",
						 d.suppressedFlushed, key);
	}

	/// 直通发射（不进抑制器）：freeze 三态配对边界（started/recovered/lost）
	/// 专用。配对语义强约束——任一条被吞都会破坏下游冻结时间线配对：
	/// CI 慢机上 started 落在忙转尾段、recovered 下一拍即到（间隔 <1s 窗口），
	/// 同 key 第二条被静默降 DEBUG 后控制台不可见（Run 26 实证回归）。
	/// 高频过程条（ongoing 心跳 / cpuSpin / cdp longtask）仍走 emitAlarm。
	/// key 与 emitAlarm 同形仅为调用点替换一致性，直通路径不使用。
	template <typename... Args>
	void emitAlarmNow(const std::string &key, const char *fmtStr, Args &&...args)
	{
		(void)key;
		QEW_LOG_WARN(fmtStr, std::forward<Args>(args)...);
	}

private:
	AlarmSuppressor m_suppressor;
};

} // namespace qt_event_watcher
