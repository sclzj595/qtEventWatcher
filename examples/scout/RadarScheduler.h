#pragma once

/// RadarScheduler - Scout V8 系统级 ANR 雷达（docs/33）
/// 常驻单线程轮询：每拍 RadarDiscover 全量发现"窗口归属 GUI 进程"集，与上拍
/// 差分出新增/消失目标（增量管理），对每个目标复用 T1 探活原语（Windows
/// SendMessageTimeoutW(WM_NULL) / Linux X11 _NET_WM_PING），三态状态机
/// per-target 逐字段对齐 WindowFreezeProber；告警行沿用 [FreezeWatch] 前缀 +
/// receiver=name@pid（实例级唯一，防同名多开三态配对串扰）+ radar=1 标注，
/// aggregator/HTML/仪表盘零改动渲染。
/// 设计约束：全机目标共享一个 worker 线程（逐目标顺序探活，hung 目标最长阻塞
/// threshold——拍耗 ≈ N×ε + k_hung×threshold），不随目标数增长线程/fd；
/// stop() 在探活循环内逐目标/逐窗口检查，退出延迟 ≈ 单窗口探活上界。

#include <QThread>
#include <QStringList>
#include <atomic>

namespace qt_event_watcher {

class RadarScheduler : public QThread
{
public:
	/// selfPid：雷达进程自身 pid（scout 无窗口，双保险过滤）；excludeNames：
	/// 进程名子串排除表（大小写不敏感，CLI 逗号分隔）
	explicit RadarScheduler(int thresholdMs, int intervalMs, qint64 selfPid,
							const QStringList &excludeNames, QObject *parent = nullptr);

	void stop();

protected:
	void run() override;

private:
	int m_thresholdMs = 0;
	int m_intervalMs = 0;
	qint64 m_selfPid = 0;
	QStringList m_excludeNames;
	std::atomic_bool m_running{ false };
};

} // namespace qt_event_watcher
