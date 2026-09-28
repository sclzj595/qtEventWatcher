#pragma once

#include <memory>

#include <QApplication>
#include <QTimer>

#include "WatchConfig.h"

namespace qt_event_watcher
{

class EventStatistics;
class EventWatcher;
class MetaCallWatcher;
class MetaCallFilter;
class IpcConfigServer;

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

	/// 只读配置访问（报告导出 Monitor 段等只读场景）
	const WatchConfig* watchConfig() const {	return &m_config;	}

	/// 周期统计快照入口（导出用；生命周期归 CusApplication）
	EventStatistics* eventStatistics() const {	return m_eventStatistics.get();	}

private:
	WatchConfig m_config;

    std::unique_ptr<EventStatistics> m_eventStatistics;
    std::unique_ptr<EventWatcher> m_eventWatcher;
    std::unique_ptr<MetaCallWatcher> m_metaCallWatcher;
    std::unique_ptr<QTimer> m_configTimer;   ///< INI 热更新轮询（PRD 11 §5）
	std::unique_ptr<IpcConfigServer> m_ipcServer;	///< IPC 配置服务（V2 Phase B）
	
};

} // namespace qt_event_watcher
