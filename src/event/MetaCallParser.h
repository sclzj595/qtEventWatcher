#pragma once

#include <cstdint>

#include <QEvent>
#include <QObject>
#include <QString>
#include <QtGlobal>

namespace qt_event_watcher
{

/**
 * @brief 单条 MetaCall 解析结果（纯值快照，不保存 Qt 对象裸指针）
 *
 * - signalId / valid：值拷贝
 * - senderKey：仅作注册表查询键（禁止解引用——queued 投递后 sender 可能已析构）
 * - receiverClassName / receiverObjectName：parse() 内值快照
 *   （receiver 正处于 notify 调用链中，读取安全）
 * - sender 类名 / 信号签名：由 MetaCallSenderRegistry 在发射时刻快照（3B），
 *   parse() 不再解引用 sender
 */
struct MetaCallInfo
{
	int signalId = -1;
	const void* senderKey = nullptr;	///< 仅作键值使用，禁止 -> 访问
	QString receiverClassName;
	QString receiverObjectName;
	bool valid = false;

	/// 线程整数快照（PRD 06 §5）
	quintptr receiverThreadId = 0;	///< parse() 内快照：receiver 处于 notify 链中，读取安全
	quintptr senderThreadId = 0;	///< 由注册表在发射时刻快照填入；0 = 未知（invokeMethod / 未命中）
};

/**
 * @brief QMetaCallEvent 内部信息解析器（Qt 版本适配层）
 *
 * QMetaCallEvent 属于 Qt 私有实现（qobject_p.h），布局与访问方式随 Qt 版本可能变化。
 * 私有头访问集中在 MetaCallParser.cpp / MetaCallSenderRegistry.cpp（PRD 06）。
 *
 * 适配状态：
 * - Qt 5.15.x：通过 public inline 访问器 signalId() / sender() 解析（已对照本机 5.15.2 源码验证）
 * - Qt 6.5.3：待适配
 * - Qt 5.14.2：待适配
 *
 * 安全层级：
 * - signalId：值拷贝，无条件安全
 * - sender 身份：由 MetaCallSenderRegistry 在发射时刻（sender 存活窗口内）快照，
 *   日志路径零解引用（3B）
 */
class MetaCallParser
{
public:
	static MetaCallInfo parse(QObject* receiver, QEvent* event);

	/**
	 * @brief signalId → 信号签名（Qt 规范转换，私有头集中点实现）
	 * 供 MetaCallSenderRegistry 在发射时刻调用；
	 * 不在 parse() 中调用——那需要解引用 sender，存在悬空风险
	 */
	static QString resolveSignalSignature(const QMetaObject* senderMo, int signalId);
};

} // namespace qt_event_watcher
