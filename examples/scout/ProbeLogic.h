#pragma once

/// ProbeLogic - Scout 探针纯逻辑共享库（V9 / Scout V1 抽离，docs/34 §3.1）
///
/// 冻结三态状态机 + CPU episode 迟滞 + receiver/排除表——从 WindowFreezeProber
/// （Win/X11 两份）、RadarScheduler（Win/X11 两份）、CpuSampler（Win/proc 两份）
/// 的平台壳中抽出：此前三态逻辑重复 4 份、迟滞重复 2 份且内嵌在 run() 循环里
/// 不可单测。抽离后平台层（Win32 / X11 / /proc）保持薄壳，纯逻辑进四矩阵单测。
///
/// 硬约束：日志行格式逐字节不变——本头只产事件不产 I/O，格式化留在平台壳的
/// QEW_LOG_WARN 调用点；行为等价由既有 e2e 断言 + 四矩阵回归守护。

#include <functional>

#include <QString>
#include <QStringList>

namespace qt_event_watcher {
namespace ProbeLogic {

/// receiver 组名：name@pid 实例级唯一（freeze 三态配对按 receiver 键合，
/// 同名多开须互不串扰；无名兜底 pid:N）
inline QString receiverOf(const QString &name, qint64 pid)
{
	return name.isEmpty() ? QStringLiteral("pid:%1").arg(pid)
						  : QStringLiteral("%1@%2").arg(name).arg(pid);
}

/// 排除表命中：进程名子串大小写不敏感匹配（空 pattern 跳过）
inline bool excluded(const QString &name, const QStringList &patterns)
{
	for (const QString &p : patterns) {
		if (!p.isEmpty() && name.contains(p, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

/// 冻结探针事件（Platform 壳按 kind 格式化为既有行格式）：
/// Started  → "freeze started thresholdMs={} stalledMs={}"
/// Ongoing  → "freeze ongoing elapsedMs={totalMs}"
/// Recovered→ "freeze recovered totalMs={totalMs}"
/// Lost     → "freeze lost"
struct FreezeEvent
{
	enum Kind { None, Started, Ongoing, Recovered, Lost };
	Kind kind = None;
	qint64 totalMs = 0;		///< Recovered：startMs→now 持续时长；Ongoing：已持续时长
	qint64 stalledMs = 0;	///< Started：保守阈值下界
};

/// 冻结三态状态机（started → ongoing*(1s 节流) → recovered/lost），三态语义
/// 逐字段对齐 EventWatchdog（自监控侧）。
///
/// alive 语义（V8 竞态修复的统一收口）：recovered 前须真判存活——探活"成功"
/// 可能撞目标消亡竞态（垂死窗口返回非 0），死进程一律收口 freeze lost 杜绝假
/// recovered。T1 单目标 = pid 解析非空；雷达 = 内核对象信号态 / zombie 判定。
///
/// alive 以 std::function 惰性传入：仅 freezing && !hung 才调用——雷达
/// aliveOf 每次 OpenProcess + 100ms 等待，非冻结恢复拍不能每目标无谓付出；
/// hung 拍（仍在卡）不发 lost，维持 ongoing 节流语义。
struct FreezeTracker
{
	explicit FreezeTracker(int thresholdMs, int ongoingIntervalMs = 1000)
		: thresholdMs(thresholdMs)
		, ongoingIntervalMs(ongoingIntervalMs)
	{
	}

	FreezeEvent onTick(bool hung, qint64 nowMs, const std::function<bool()> &alive)
	{
		FreezeEvent ev;
		if (freezing) {
			if (!hung) {
				if (!alive()) {
					// 目标已死（解析空表/信号态置位）："恢复"语义不诚实，收口 lost
					freezing = false;
					ev.kind = FreezeEvent::Lost;
				} else {
					freezing = false;
					ev.kind = FreezeEvent::Recovered;
					ev.totalMs = nowMs - startMs;
				}
			} else if (nowMs - lastOngoingMs >= ongoingIntervalMs) {
				lastOngoingMs = nowMs;
				ev.kind = FreezeEvent::Ongoing;
				ev.totalMs = nowMs - startMs;
			}
		} else if (hung) {
			freezing = true;
			startMs = nowMs;
			lastOngoingMs = nowMs;
			ev.kind = FreezeEvent::Started;
			// stalledMs 保守取阈值下界：外部探针只能保证"至少已停滞 threshold"
			ev.stalledMs = thresholdMs;
		}
		return ev;
	}

	/// 目标从在册集消失（雷达增量管理 / T1 解析空表）——freezing 中收口 lost
	FreezeEvent onTargetGone()
	{
		FreezeEvent ev;
		if (freezing) {
			freezing = false;
			ev.kind = FreezeEvent::Lost;
		}
		return ev;
	}

	bool freezing = false;
	qint64 startMs = 0;
	qint64 lastOngoingMs = 0;
	int thresholdMs = 0;
	int ongoingIntervalMs = 1000;
};

/// CPU 探针事件（Spin → 平台壳格式化为 "slow event ... event=cpuSpin"）
struct CpuEvent
{
	enum Kind { None, Spin };
	Kind kind = None;
	double cpuPct = 0.0;	///< 告警拍实测 cpu%（costMs 字段承载）
};

/// CPU episode 迟滞：连续 runs 拍超阈值发一条 + 连续 4 拍低于阈值才收口
/// （采样噪声的单拍抖动不重置 emitted——实测 6s 单次忙转曾因单拍 <95% 被
/// 撕成 3 条告警）。episode 内至多一条 Spin。
struct CpuEpisodeTracker
{
	explicit CpuEpisodeTracker(int thresholdPct, int runsNeeded)
		: thresholdPct(thresholdPct)
		, runsNeeded(runsNeeded > 0 ? runsNeeded : 1)
	{
	}

	/// 目标消失：观测断点，episode 状态复位（重连后重新起算）
	void reset()
	{
		streak = 0;
		streakDown = 0;
		emitted = false;
	}

	CpuEvent onTick(double cpuPct)
	{
		CpuEvent ev;
		if (cpuPct >= thresholdPct) {
			streakDown = 0;
			if (++streak >= runsNeeded && !emitted) {
				emitted = true;
				ev.kind = CpuEvent::Spin;
				ev.cpuPct = cpuPct;
			}
		} else {
			streak = 0;
			if (++streakDown >= settleRuns) {
				streakDown = 0;
				emitted = false;
			}
		}
		return ev;
	}

	int thresholdPct = 0;
	int runsNeeded = 1;
	int settleRuns = 4;		///< 迟滞收口拍数
	int streak = 0;			///< 连续超阈值拍数
	int streakDown = 0;		///< 连续低于阈值拍数
	bool emitted = false;	///< 本 episode 已发告警（防刷屏）
};

} // namespace ProbeLogic
} // namespace qt_event_watcher
