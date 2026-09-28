#pragma once

#include <QSet>
#include <QString>

namespace qt_event_watcher
{

/**
 * @brief MetaCall 告警过滤器（blacklist 层）
 *
 * 位于"阈值命中之后、日志输出之前"（PRD 14：仅告警路径付费）：
 *
 *   slow MetaCall 命中
 *        │
 *        ▼
 *   MetaCallFilter::check(senderClass, signalSig, receiverClass)
 *        │
 *   Suppress → 丢弃 / Log → WARN
 *
 * - 规则维度：sender 类名 / 信号签名 / receiver 类名 / sender+signal 组合
 * - 匹配语义：精确匹配（不做通配符，避免引入歧义）
 * - 匿名 sender（invokeMethod 形态，signalId=-1）的屏蔽由
 *   suppressAnonymousSenders 开关控制，在 watcher 中按 senderKey 精确判定
 * - 线程约定：规则在 GUI 线程 exec() 前配置；check() 仅在 GUI 线程调用
 *   （notify 链路内），因此不加锁
 * - 业务侧高频噪音源（如 CallbackRegistrationHelper 类组件）通过本层
 *   注册屏蔽规则，不修改监控核心
 */
class MetaCallFilter
{
public:
	enum class Verdict
	{
		Log,		///< 输出 WARN
		Suppress	///< 丢弃本条告警
	};

	Verdict check(const QString& senderClassName,
	              const QString& signalSignature,
	              const QString& receiverClassName) const;

	/// Func
	void addSuppressedSender(const QString& senderClassName);
	void addSuppressedSignal(const QString& signalSignature);
	void addSuppressedReceiver(const QString& receiverClassName);
	void addSuppressedPair(const QString& senderClassName, const QString& signalSignature);
	/// 运行时清空全部规则并复位匿名开关（IPC filter.clear；GUI 线程调用）
	void clearAll()
	{
		m_senders.clear();
		m_signals.clear();
		m_receivers.clear();
		m_pairs.clear();
		m_suppressAnonymous = false;
	}
	void setSuppressAnonymousSenders(bool on) { m_suppressAnonymous = on; }
	bool suppressAnonymousSenders() const { return m_suppressAnonymous; }

private:
	QSet<QString> m_senders;
	QSet<QString> m_signals;
	QSet<QString> m_receivers;
	QSet<QString> m_pairs;	///< key: sender + '\x1F' + signal
	bool m_suppressAnonymous = false;
};

} // namespace qt_event_watcher
