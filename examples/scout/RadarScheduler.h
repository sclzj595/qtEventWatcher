#pragma once

/// RadarScheduler - Scout V8 系统级 ANR 雷达（docs/33）+ V1 per-target CPU（docs/34）
/// 常驻单线程轮询：每拍 RadarDiscover 全量发现"窗口归属 GUI 进程"集，与上拍
/// 差分出新增/消失目标（增量管理），对每个目标复用 T1 探活原语（Windows
/// SendMessageTimeoutW(WM_NULL) / Linux X11 _NET_WM_PING），三态状态机
/// per-target 逐字段对齐 WindowFreezeProber；告警行沿用 [FreezeWatch] 前缀 +
/// receiver=name@pid（实例级唯一，防同名多开三态配对串扰）+ radar=1 标注，
/// aggregator/HTML/仪表盘零改动渲染。
/// V1（docs/34 R3a）：冻结探活之外同拍做 per-target CPU 差分采样（口径对齐
/// CpuSampler 单核 cpu%），episode 迟滞复用 ProbeLogic::CpuEpisodeTracker，
/// cpuSpin 告警对齐 slow event 行格式 + radar=1 标注。
/// 设计约束：全机目标共享一个 worker 线程（逐目标顺序探活，hung 目标最长阻塞
/// threshold——拍耗 ≈ N×ε + k_hung×threshold），不随目标数增长线程/fd；
/// CPU 采样全为非阻塞 API（GetProcessTimes / /proc stat），O(N) 不破坏轮询模型；
/// stop() 在探活循环内逐目标/逐窗口检查，退出延迟 ≈ 单窗口探活上界。

#include <QThread>
#include <QStringList>
#include <atomic>

#include "ScoutAlarmEmitter.h"

namespace qt_event_watcher {

class RadarScheduler : public QThread
{
public:
	/// selfPid：雷达进程自身 pid（scout 无窗口，双保险过滤）；excludeNames：
	/// 进程名子串排除表（大小写不敏感，CLI 逗号分隔）；cpuThresholdPct/cpuRuns：
	/// per-target CPU 启发阈值与迟滞拍数（CLI 复用 --cpu-threshold/--cpu-runs）
	explicit RadarScheduler(int thresholdMs, int intervalMs, qint64 selfPid,
							const QStringList &excludeNames,
							int cpuThresholdPct = 95, int cpuRuns = 3,
							QObject *parent = nullptr);

	void stop();

	/// V1 R3b：配置文件热加载路径（空串 = 禁用）。run() 每 tick 检查 mtime，
	/// 变更即重载 threshold/interval/cpu/exclude（worker 线程独占读，无锁）
	void setConfigFile(const QString &path) { m_configFile = path; }

protected:
	void run() override;

private:
	int m_thresholdMs = 0;
	int m_intervalMs = 0;
	qint64 m_selfPid = 0;
	QStringList m_excludeNames;
	int m_cpuThresholdPct = 95;
	int m_cpuRuns = 3;
	QString m_configFile;
	ScoutAlarmEmitter m_alarm;		///< 告警风暴抑制门（docs/34 R3c）
	std::atomic_bool m_running{ false };
};

} // namespace qt_event_watcher
