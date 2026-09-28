#include "EventWatcher.h"

#include "EventStatistics.h"
#include "WatchConfig.h"
#include "WatchLogMacros.h"

#include <QMetaObject>
#include <QObject>
#include <QThread>

namespace qt_event_watcher
{

EventWatcher::EventWatcher(WatchConfig *config, EventStatistics *statistics)
	: m_config(config), m_statistics(statistics)
{
}

bool EventWatcher::watch(WatchEventInfo &info, std::int64_t elapsedNs, std::int64_t exclusiveElapsedNs)
{
	if (m_config == nullptr)	return false;
	info.elapsedNs = elapsedNs;
	info.exclusiveElapsedNs = exclusiveElapsedNs;

	// 单事件监控：仅 Watch_Fun bit0 开启时判断慢事件并逐次告警（默认基于 Inclusive Cost，PRD 05 §6）
	if (m_config->isWatchEnabled(WatchConfig::WatchEvent)) {
		const std::int64_t thresholdNs = static_cast<std::int64_t>(m_config->slowEventThresholdMs()) * 1000000LL;
		info.slow = info.elapsedNs >= thresholdNs;
		if (info.slow) {
			QEW_LOG_WARN(
	            "[EventWatcher] slow event "
	            "receiver={} object={} event={} type={} depth={} costMs={:.3f} exclusiveCostMs={:.3f} "
	            "curThread={:#x} recvThread={:#x} match={} thresholdMs={}",
	            info.receiverClassName.toStdString(),
	            info.receiverObjectName.toStdString(),
	            info.eventName.toStdString(),
	            info.eventType,
	            info.nestingDepth,
	            info.elapsedMs(),
	            info.exclusiveElapsedMs(),
	            info.currentThreadId,
	            info.receiverThreadId,
	            info.receiverThreadMatch(),
	            m_config->slowEventThresholdMs());
		}
	}

	// 周期聚合：EventStatistics::record() 内部按 bit2 自门控，周期结束统一输出；
	// C2：Exclusive（扣直接子事件的自身耗时）并入聚合，支撑线程内自耗时归因
	if (m_statistics != nullptr) {
		m_statistics->record(info.eventType, info.eventName, info.elapsedNs, exclusiveElapsedNs);
	}
	return info.slow;
}

WatchEventInfo EventWatcher::watchEvent(QObject *receiver, QEvent *event)
{
	WatchEventInfo info;
	info.receiver = receiver;

	if (receiver == nullptr || event == nullptr)	return info;

	info.eventName = eventName(event->type());
    info.receiverClassName = receiverClassName(receiver);
    info.receiverObjectName = receiverObjectName(receiver);
    info.eventType = static_cast<int>(event->type());

    // 线程整数快照（PRD 05 §7）：notify 执行线程与 receiver 归属线程
    info.currentThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    info.receiverThreadId = reinterpret_cast<quintptr>(receiver->thread()->currentThreadId());

	/*
	 * 注意：
	 * EventWatcher 本身不能调用 receiver->event(event)。
	 * 真正的 Event 分发仍然由 QGuiApplication::notify() 完成。
	 * 当前函数只负责构造监控上下文（info）。
	 * 实际耗时测量由 CusApplication::notify()
	 * 用 EventGuard 包住真正的 notify 调用完成，
	 * 并把 elapsedNs 交给 watch() 统一判断与记录。
	 */
    return info;
}

bool EventWatcher::isEnabled() const
{
	if (m_config == nullptr)	return false;
	// 单事件监控与周期统计共用同一条 notify 计时链路，任一开启即接入
	return m_config->isWatchEnabled(WatchConfig::WatchEvent)
	    || m_config->isWatchEnabled(WatchConfig::WatchEventStatistics);
}

QString EventWatcher::eventName(QEvent::Type type)
{
	switch (type) {
    	case QEvent::None: 					return QStringLiteral("None");
    	case QEvent::Timer: 				return QStringLiteral("Timer");
    	case QEvent::MouseButtonPress: 		return QStringLiteral("MouseButtonPress");
    	case QEvent::MouseButtonRelease: 	return QStringLiteral("MouseButtonRelease");
    	case QEvent::MouseButtonDblClick: 	return QStringLiteral("MouseButtonDblClick");
    	case QEvent::MouseMove: 			return QStringLiteral("MouseMove");
    	case QEvent::KeyPress: 				return QStringLiteral("KeyPress");
    	case QEvent::KeyRelease: 			return QStringLiteral("KeyRelease");
    	case QEvent::FocusIn: 				return QStringLiteral("FocusIn");
    	case QEvent::FocusOut: 				return QStringLiteral("FocusOut");
    	case QEvent::Paint: 				return QStringLiteral("Paint");
    	case QEvent::Move:  				return QStringLiteral("Move");
    	case QEvent::Resize: 				return QStringLiteral("Resize");
    	case QEvent::Show:  				return QStringLiteral("Show");
    	case QEvent::Hide: 					return QStringLiteral("Hide");
    	case QEvent::Close: 				return QStringLiteral("Close");
    	case QEvent::Wheel: 				return QStringLiteral("Wheel");
    	case QEvent::Enter: 				return QStringLiteral("Enter");
    	case QEvent::Leave: 				return QStringLiteral("Leave");
    	case QEvent::DragEnter: 			return QStringLiteral("DragEnter");
    	case QEvent::DragMove: 				return QStringLiteral("DragMove");
    	case QEvent::DragLeave: 			return QStringLiteral("DragLeave");
    	case QEvent::Drop: 					return QStringLiteral("Drop");
    	case QEvent::ContextMenu: 			return QStringLiteral("ContextMenu");
    	case QEvent::InputMethod: 			return QStringLiteral("InputMethod");
    	case QEvent::MetaCall: 				return QStringLiteral("MetaCall");
    	case QEvent::DynamicPropertyChange: return QStringLiteral("DynamicPropertyChange");

    	default:	return QStringLiteral("Event(%1)").arg(static_cast<int>(type));
    }
}

QString EventWatcher::receiverClassName(QObject *receiver)
{
	if (receiver == nullptr)	return {};
	return QString::fromLatin1(receiver->metaObject()->className());
}

QString EventWatcher::receiverObjectName(QObject *receiver)
{
	if (receiver == nullptr)	return {};
	return receiver->objectName();
}


}	// namespace qt_event_watcher
 