#include "CusApplication.h"

#include "EventStatistics.h"
#include "EventWatcher.h"
#include "WatchEventInfo.h"
#include "MetaCallFilter.h"
#include "MetaCallWatcher.h"
#include "QssStyleWatcher.h"
#include "EventGuard.h"
#include "WatchConfig.h"
#include "IpcConfigServer.h"

#include <QTimer>

namespace qt_event_watcher 
{

CusApplication::CusApplication(int &argc, char **argv)
	: QApplication(argc, argv)
{
	m_config.load();
	m_eventStatistics = std::make_unique<EventStatistics>(&m_config);
	m_eventWatcher = std::make_unique<EventWatcher>(&m_config, m_eventStatistics.get());
	m_metaCallWatcher = std::make_unique<MetaCallWatcher>(&m_config);
	QssStyleWatcher::instance()->setup(&m_config);

	// INI 热更新轮询（PRD 11 §5）：mtime 变化 → reloadIfChanged 原子替换；
	// 轮询间隔本身可被热更新，reload 后按新间隔重启
	m_configTimer = std::make_unique<QTimer>();
	connect(m_configTimer.get(), &QTimer::timeout, this, [this]() {
		if (m_config.reloadIfChanged())
			m_configTimer->start(m_config.configPollIntervalMs());
	});
	m_configTimer->start(m_config.configPollIntervalMs());

	// IPC 配置服务（V2 Phase B）：QLocalSocket + JSON 行协议，外部进程实时调控。
	// 请求处理直接走 setter/filter（主线程事件循环），与热更新同构；启动失败仅
	// WARN 不影响监控（PRD 14 哲学）
	m_ipcServer = std::make_unique<IpcConfigServer>(&m_config, metaCallFilter());
	if (!m_ipcServer->start()) {
		m_ipcServer.reset();	// 服务不可用时保持无 IPC 状态运行
	}
}

// unique_ptr 成员需要完整类型，析构必须在实现文件中定义
CusApplication::~CusApplication() = default;

MetaCallFilter *CusApplication::metaCallFilter() const
{
	return m_metaCallWatcher != nullptr ? m_metaCallWatcher->filter() : nullptr;
}

bool CusApplication::watchEnabled(int watchFunctionBit) const
{
	return m_config.isWatchEnabled(static_cast<WatchConfig::WatchFunction>(watchFunctionBit));
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
	const bool result = QApplication::notify(receiver, event);
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