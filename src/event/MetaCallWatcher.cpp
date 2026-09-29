#include "MetaCallWatcher.h"

#include "AlarmSuppressor.h"
#include "MetaCallFilter.h"
#include "MetaCallParser.h"
#include "MetaCallSenderRegistry.h"
#include "StackCapture.h"
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
	// V4 B2：栈采集时机（与 EventWatcher 同构）——0=命中即采（默认，记录全量含 frames）
	// 1=仅窗口首条采（被抑制条免采，记录无 frames）2=关闭
	const int captureMode = m_config->stackCaptureMode();
	std::string stack;
	if (captureMode == 0)
		stack = StackCapture::format(StackCapture::capture());
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

	// V3 A2：风暴抑制——窗口（1s）内同 key（sender+signal）首条必出 WARN，
	// 静默条 DEBUG（RecordSink 全量采集，文件/控制台按用户级别去重）
	if (m_suppressor == nullptr)
		m_suppressor = std::make_unique<AlarmSuppressor>();
	const std::string stormKey = senderClassName.toStdString()
		+ '|' + std::to_string(info.signalId);
	const int windowMs = m_config->alarmSuppressWindowMs();	// V4 B1：窗口热更新实时读
	const AlarmDecision decision = m_suppressor->evaluate(stormKey, windowMs);
	if (captureMode == 1 && decision.emitNow)
		stack = StackCapture::format(StackCapture::capture());	// 仅窗口首条采（MetaCall 栈即槽执行栈）
	if (decision.suppressedFlushed > 0) {
		QEW_LOG_WARN(
		    "[MetaCallWatcher] alarm storm sender={} signalId={} "
		    "suppressed={} windowMs={}",
		    senderClassName.toStdString(),
		    info.signalId,
		    decision.suppressedFlushed,
		    windowMs);
	}

	if (decision.emitNow) {
		if (info.valid) {
			QEW_LOG_WARN(
			    "[MetaCallWatcher] slow MetaCall "
			    "sender={} signal={} signalId={} senderThread={:#x} "
			    "receiver={} object={} recvThread={:#x} curThread={:#x} match={} "
			    "costMs={:.3f} thresholdMs={} stack={}",
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
			    m_config->slowMetaCallThresholdMs(),
			    stack);
		} else {
			// 解析降级：仅输出公开可得的 receiver 信息（PRD 06：解析失败安全跳过）
			QEW_LOG_WARN(
			    "[MetaCallWatcher] slow MetaCall "
			    "receiver={} object={} recvThread={:#x} curThread={:#x} match={} "
			    "costMs={:.3f} thresholdMs={} stack={}",
			    info.receiverClassName.toStdString(),
			    info.receiverObjectName.toStdString(),
			    info.receiverThreadId,
			    currentThreadId,
			    threadMatch,
			    elapsedMs,
			    m_config->slowMetaCallThresholdMs(),
			    stack);
		}
	} else {
		if (info.valid) {
			QEW_LOG_DEBUG(
			    "[MetaCallWatcher] slow MetaCall "
			    "sender={} signal={} signalId={} senderThread={:#x} "
			    "receiver={} object={} recvThread={:#x} curThread={:#x} match={} "
			    "costMs={:.3f} thresholdMs={} stack={}",
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
			    m_config->slowMetaCallThresholdMs(),
			    stack);
		} else {
			QEW_LOG_DEBUG(
			    "[MetaCallWatcher] slow MetaCall "
			    "receiver={} object={} recvThread={:#x} curThread={:#x} match={} "
			    "costMs={:.3f} thresholdMs={} stack={}",
			    info.receiverClassName.toStdString(),
			    info.receiverObjectName.toStdString(),
			    info.receiverThreadId,
			    currentThreadId,
			    threadMatch,
			    elapsedMs,
			    m_config->slowMetaCallThresholdMs(),
			    stack);
		}
	}
}

} // namespace qt_event_watcher
