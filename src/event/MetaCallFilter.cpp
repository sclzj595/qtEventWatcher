#include "MetaCallFilter.h"

namespace qt_event_watcher
{

MetaCallFilter::Verdict MetaCallFilter::check(
    const QString &senderClassName,
    const QString &signalSignature,
    const QString &receiverClassName) const
{
	if (!senderClassName.isEmpty() && m_senders.contains(senderClassName))
		return Verdict::Suppress;
	if (!signalSignature.isEmpty() && m_signals.contains(signalSignature))
		return Verdict::Suppress;
	if (!receiverClassName.isEmpty() && m_receivers.contains(receiverClassName))
		return Verdict::Suppress;
	if (!senderClassName.isEmpty() && !signalSignature.isEmpty() &&
	    m_pairs.contains(senderClassName + QChar(0x1F) + signalSignature))
		return Verdict::Suppress;
	return Verdict::Log;
}

void MetaCallFilter::addSuppressedSender(const QString &senderClassName)
{
	if (!senderClassName.isEmpty())
		m_senders.insert(senderClassName);
}

void MetaCallFilter::addSuppressedSignal(const QString &signalSignature)
{
	if (!signalSignature.isEmpty())
		m_signals.insert(signalSignature);
}

void MetaCallFilter::addSuppressedReceiver(const QString &receiverClassName)
{
	if (!receiverClassName.isEmpty())
		m_receivers.insert(receiverClassName);
}

void MetaCallFilter::addSuppressedPair(const QString &senderClassName, const QString &signalSignature)
{
	if (!senderClassName.isEmpty() && !signalSignature.isEmpty())
		m_pairs.insert(senderClassName + QChar(0x1F) + signalSignature);
}

} // namespace qt_event_watcher
