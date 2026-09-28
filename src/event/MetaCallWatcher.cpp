#include "MetaCallWatcher.h"

#include "MetaCallFilter.h"
#include "MetaCallParser.h"
#include "MetaCallSenderRegistry.h"
#include "WatchConfig.h"
#include "WatchLogMacros.h"

#include <QObject>
#include <QThread>


namespace qt_event_watcher
{

MetaCallWatcher::MetaCallWatcher(WatchConfig *watchConfig)
	: m_config(watchConfig)
{
	m_filter = std::make_unique<MetaCallFilter>();

	// 仅在启动时 Bit1 开启才安装发射回调：
	// spy 回调进程级唯一（与 QSignalSpy 类工具互斥），安装状态以启动配置为准
	if (isEnabled()) {
		m_senderRegistry = std::make_unique<MetaCallSenderRegistry>();
		m_senderRegistry->install();
	}
}

MetaCallWatcher::~MetaCallWatcher() = default;

bool MetaCallWatcher::isEnabled() const
{
	return m_config != nullptr &&
	       m_config->isWatchEnabled(WatchConfig::WatchMetaCall);
}

MetaCallFilter *MetaCallWatcher::filter()
{
	return m_filter.get();
}

void MetaCallWatcher::process(QObject *receiver, QEvent *event, std::int64_t elapsedNs)
{
	if (!isEnabled())	return;
	if (receiver == nullptr || event == nullptr)	return;
	if (event->type() != QEvent::MetaCall)	return;
	if (elapsedNs < 0)	return;

	const std::int64_t thresholdNs = static_cast<std::int64_t>(m_config->slowMetaCallThresholdMs()) * 1000000LL;
	if (elapsedNs < thresholdNs)	return;

	// 仅命中告警后才解析（PRD 14：正常路径零成本）；
	// parse() 只做值快照，绝不解引用 sender（3B）
	const MetaCallInfo info = MetaCallParser::parse(receiver, event);

	// invokeMethod 形态（sender==nullptr）按独立开关屏蔽：
	// 用 senderKey 精确判定，不依赖身份查表结果
	if (m_filter != nullptr &&
	    m_filter->suppressAnonymousSenders() &&
	    info.senderKey == nullptr)
		return;

	// sender 身份来自发射时刻快照：按"指针值 + signalId"查表，零解引用
	QString senderClassName;
	QString signalSignature;
	quintptr senderThreadId = 0;	///< 0 = 未知（invokeMethod / 未命中 / 被抢占）
	if (m_senderRegistry != nullptr && info.senderKey != nullptr && info.signalId >= 0) {
		if (!m_senderRegistry->isCallbackActive()) {
			// 抢占/卸载检测：QSignalSpy 类工具注册后全局回调被覆盖，
			// 身份快照停止更新 → 降级为空字段，仅告警一次
			if (!m_spyHijackWarned) {
				m_spyHijackWarned = true;
				QEW_LOG_WARN(
				    "[MetaCallWatcher] signal spy callbacks hijacked/uninstalled, "
				    "sender identity degraded to empty fields");
			}
		} else {
			MetaCallSenderRegistry::SenderIdentity identity;
			if (m_senderRegistry->lookup(info.senderKey, info.signalId, identity)) {
				if (identity.senderClassName != nullptr)
					senderClassName = QString::fromLatin1(identity.senderClassName);
				signalSignature = identity.signalSignature;
				// C1 精确语义：归属线程 os id 优先；未观测到（0）回退发射线程快照
				senderThreadId = identity.ownerThreadId != 0
					? identity.ownerThreadId : identity.senderThreadId;
			}
		}
	}

	// blacklist 过滤：命中即丢弃本条告警（不影响其他 bit 的统计与告警）
	if (m_filter != nullptr &&
	    m_filter->check(senderClassName, signalSignature, info.receiverClassName) ==
	        MetaCallFilter::Verdict::Suppress)
		return;

	const double elapsedMs = static_cast<double>(elapsedNs) / 1000000.0;
	/*
	 * 跨线程特征（PRD 06 §5）：senderThread（注册表发射时刻快照）vs recvThread。
	 * 注意不能比 curThread == recvThread：两者都在 notify 线程内快照，恒相等，
	 * 比了等于没比（Qt6.5.3 压测暴露的存量问题）。
	 * senderThreadId == 0 表示未知（invokeMethod / 注册表未命中），match 降级输出 "-"。
	 */
	const bool senderThreadKnown = senderThreadId != 0;
	const char* threadMatch = !senderThreadKnown ? "-"
			: (senderThreadId == info.receiverThreadId ? "true" : "false");
	// curThread 仅作上下文字段保留（与 recvThread 对比无诊断意义）
	const quintptr currentThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());

	if (info.valid) {
		QEW_LOG_WARN(
		    "[MetaCallWatcher] slow MetaCall "
		    "sender={} signal={} signalId={} senderThread={:#x} "
		    "receiver={} object={} recvThread={:#x} curThread={:#x} match={} "
		    "costMs={:.3f} thresholdMs={}",
		    senderClassName.toStdString(),
		    signalSignature.toStdString(),
		    info.signalId,
		    senderThreadId,
		    info.receiverClassName.toStdString(),
		    info.receiverObjectName.toStdString(),
		    info.receiverThreadId,
		    currentThreadId,
		    threadMatch,
		    elapsedMs,
		    m_config->slowMetaCallThresholdMs());
	} else {
		// 解析降级：仅输出公开可得的 receiver 信息（PRD 06：解析失败安全跳过）
		QEW_LOG_WARN(
		    "[MetaCallWatcher] slow MetaCall "
		    "receiver={} object={} recvThread={:#x} curThread={:#x} match={} "
		    "costMs={:.3f} thresholdMs={}",
		    info.receiverClassName.toStdString(),
		    info.receiverObjectName.toStdString(),
		    info.receiverThreadId,
		    currentThreadId,
		    threadMatch,
		    elapsedMs,
		    m_config->slowMetaCallThresholdMs());
	}
}

} // namespace qt_event_watcher
