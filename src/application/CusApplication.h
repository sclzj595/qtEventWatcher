#pragma once

#include <memory>

#include <QApplication>
#include <QTimer>

namespace qt_event_watcher
{

class EventStatistics;
class EventWatcher;
class MetaCallWatcher;
class MetaCallFilter;
class IpcConfigServer;
class EventWatchdog;
class WatchConfig;
class UplinkClient;

class CusApplication : public QApplication
{
public:
	CusApplication(int &argc, char** argv);
	~CusApplication() override;

	/// Func
	bool notify(QObject* receiver, QEvent* event) override;

	/// MetaCall blacklist 规则入口（非拥有指针；Bit1 关闭时仍可预配置规则）
	MetaCallFilter* metaCallFilter() const;

	/// 监控位查询（Demo UI 状态提示用）
	bool watchEnabled(int watchFunctionBit) const;

	/// 只读配置访问（报告导出 Monitor 段等只读场景；指针透传，V4 C1 起
	/// WatchConfig 前置声明——consumer 解引用需自行 include WatchConfig.h）
	const WatchConfig* watchConfig() const {	return m_config.get();	}

	/// 周期统计快照入口（导出用；生命周期归 CusApplication）
	EventStatistics* eventStatistics() const {	return m_eventStatistics.get();	}

private:
	// V4 C1：unique_ptr 藏匿实现——头文件仅前置声明，WatchConfig.h 不再经
	// 本公共头传播（公共面收窄）。析构 out-of-line（.cpp 内完整类型）
	std::unique_ptr<WatchConfig> m_config;

    std::unique_ptr<EventStatistics> m_eventStatistics;
    std::unique_ptr<EventWatcher> m_eventWatcher;
    std::unique_ptr<MetaCallWatcher> m_metaCallWatcher;
    std::unique_ptr<QTimer> m_configTimer;   ///< INI 热更新轮询（PRD 11 §5）
	std::unique_ptr<IpcConfigServer> m_ipcServer;	///< IPC 配置服务（V2 Phase B）
	std::unique_ptr<EventWatchdog> m_watchdog;		///< UI 冻结看门狗（V3 B 线，常驻线程内查开关）
	std::unique_ptr<QTimer> m_heartbeatTimer;		///< 主线程心跳（冻结即停跳，ANR 检测源）
	std::unique_ptr<UplinkClient> m_uplink;			///< 上行链路（V4 D1；uplinkName 非空即启用，热更跟随）
};

} // namespace qt_event_watcher
