#include "CusApplication.h"

#include "EventStatistics.h"
#include "EventWatcher.h"
#include "EventWatchdog.h"
#include "WatchEventInfo.h"
#include "MetaCallFilter.h"
#include "MetaCallWatcher.h"
#include "QssStyleWatcher.h"
#include "EventGuard.h"
#include "WatchConfig.h"
#include "IpcConfigServer.h"
#include "UplinkClient.h"

#include <QTimer>

namespace qt_event_watcher
{

CusApplication::CusApplication(int &argc, char **argv)
	: QApplication(argc, argv)
	, m_config(std::make_unique<WatchConfig>())
{
	m_config->load();
	m_eventStatistics = std::make_unique<EventStatistics>(m_config.get());
	m_eventWatcher = std::make_unique<EventWatcher>(m_config.get(), m_eventStatistics.get());
	m_metaCallWatcher = std::make_unique<MetaCallWatcher>(m_config.get());
	QssStyleWatcher::instance()->setup(m_config.get());

	// INI 热更新轮询（PRD 11 §5）：mtime 变化 → reloadIfChanged 原子替换；
	// 轮询间隔本身可被热更新，reload 后按新间隔重启
	m_configTimer = std::make_unique<QTimer>();
	connect(m_configTimer.get(), &QTimer::timeout, this, [this]() {
		if (m_config->reloadIfChanged())
			m_configTimer->start(m_config->configPollIntervalMs());
		// V4 D1：上行链路热更跟随（名字非空即启用，变空断开闲置；
		// flush 周期由 UplinkClient 每拍实时读，无需此处干预）
		const bool wantUplink = !m_config->uplinkName().isEmpty();
		if (wantUplink && m_uplink == nullptr)
			m_uplink = std::make_unique<UplinkClient>(m_config.get());
		else if (!wantUplink && m_uplink != nullptr)
			m_uplink.reset();
	});
	m_configTimer->start(m_config->configPollIntervalMs());

	// 上行链路（V4 D1）：启动配置非空即启用（差异于 configTimer 首拍——
	// 启用即起步，不等轮询周期）
	if (!m_config->uplinkName().isEmpty())
		m_uplink = std::make_unique<UplinkClient>(m_config.get());

	// IPC 配置服务（V2 Phase B）：QLocalSocket + JSON 行协议，外部进程实时调控。
	// 请求处理直接走 setter/filter（主线程事件循环），与热更新同构；启动失败仅
	// WARN 不影响监控（PRD 14 哲学）
	m_ipcServer = std::make_unique<IpcConfigServer>(m_config.get(), metaCallFilter());
	if (!m_ipcServer->start()) {
		m_ipcServer.reset();	// 服务不可用时保持无 IPC 状态运行
	}

	// UI 冻结看门狗（V3 B 线）：常驻线程 + run() 内实时查 WatchFreeze 位
	// （bit4 热更新即时生效）；心跳 QTimer 挂主线程，冻结即停跳（ANR 检测源）。
	// 心跳周期热更新跟随（同 configTimer 重启模式）
	m_watchdog = std::make_unique<EventWatchdog>(m_config.get());
	m_watchdog->start();
	m_heartbeatTimer = std::make_unique<QTimer>();
	connect(m_heartbeatTimer.get(), &QTimer::timeout, this, [this]() {
		m_watchdog->beat();
		m_heartbeatTimer->start(m_config->heartbeatIntervalMs());
	});
	m_heartbeatTimer->start(m_config->heartbeatIntervalMs());
}

// unique_ptr 成员需要完整类型，析构必须在实现文件中定义
CusApplication::~CusApplication() = default;

MetaCallFilter *CusApplication::metaCallFilter() const
{
	return m_metaCallWatcher != nullptr ? m_metaCallWatcher->filter() : nullptr;
}

bool CusApplication::watchEnabled(int watchFunctionBit) const
{
	// 判空：基类构造/析构阶段 m_config 尚未创建/已销毁（同 notify 成员规则）
	return m_config != nullptr &&
	       m_config->isWatchEnabled(static_cast<WatchConfig::WatchFunction>(watchFunctionBit));
}

namespace
{

/*
 * 嵌套 notify 计数与子事件耗时累计（PRD 05 §6）。
 * notify() 可重入（processEvents() 等），状态必须按线程隔离；
 * 两个 thread_local 原子操作的开销符合 PRD 14 低开销要求。
 *
 * 语义：
 * - t_depth：当前 notify 重入深度，最外层 = 0
 * - t_childElapsedNs：当前帧内已完成的所有**直接**嵌套子事件 Inclusive 耗时之和
 *   子事件退出时把自己的 Inclusive 累加给父帧 → 父帧 Exclusive 只扣直接子事件，
 *   孙事件开销已在子帧内部扣除（A_excl = 120 - 80，而非 120 - 80 - 20）
 */
thread_local int t_nestingDepth = 0;
thread_local std::int64_t t_childElapsedNs = 0;

} // namespace

bool CusApplication::notify(QObject *receiver, QEvent *event)
{
	const int nestingDepth = t_nestingDepth++;
	const std::int64_t childBaseNs = t_childElapsedNs;	// 保存父帧已累计值
	t_childElapsedNs = 0;								// 为本帧重新累计

	WatchEventInfo eventInfo;
	if (m_eventWatcher != nullptr && m_eventWatcher->isEnabled()) {
		eventInfo = m_eventWatcher->watchEvent(receiver, event);
		eventInfo.nestingDepth = nestingDepth;
	}

	EventGuard guard;
	// V3 B 线：冻结取证快照——"正在处理的事件"（QApplication::notify 返回即清除）。
	// 基类 QApplication 构造/析构阶段（平台插件初始化等）仍会派发事件进入本函数，
	// 此时 m_watchdog 尚未创建或已销毁，必须判空
	if (m_watchdog != nullptr) {
		m_watchdog->eventStarted(receiver, event);
	}
	const bool result = QApplication::notify(receiver, event);
	if (m_watchdog != nullptr) {
		m_watchdog->eventFinished();
	}
	const std::int64_t elapsedNs = guard.elapsedNs();

	// 本帧自身耗时 = Inclusive - 直接嵌套子事件 Inclusive 之和
	const std::int64_t exclusiveNs = elapsedNs - t_childElapsedNs;
	// 向父帧上抛本帧 Inclusive（父帧的 Exclusive 只扣直接子事件）
	t_childElapsedNs = childBaseNs + elapsedNs;
	--t_nestingDepth;

	if (m_eventWatcher != nullptr && m_eventWatcher->isEnabled()) {
		m_eventWatcher->watch(eventInfo, elapsedNs, exclusiveNs);
	}

	if (m_metaCallWatcher != nullptr && m_metaCallWatcher->isEnabled()) {
		m_metaCallWatcher->process(receiver, event, elapsedNs);
	}

	return result;
}

} // namespace qt_event_watcher