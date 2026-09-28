#include "MetaCallParser.h"

#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	// Qt 6.5.3（本机 F:/IDE.2/QT/6.5.3/msvc2019_64 已对照源码 qobject_p.h L349）：
	// QAbstractMetaCallEvent 同样定义于 qobject_p.h，public inline 访问器
	// sender()/signalId() 与 5.15.2 同名同义；成员声明顺序不同（signalId_ 在前），
	// 本实现只经访问器取值，零偏移假设（PRD 06 硬约束）。
	// 另：QMetaObjectPrivate::signal(m, id) 返回值由 const QMetaMethodPrivate*
	// 改为直接 QMetaMethod —— resolveSignalSignature 的赋值语句两者通用。
#elif QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	// Qt 5.14.2（经官方 v5.14.2 tag qobject_p.h L484-506 核实：QAbstractMetaCallEvent
	// 基类与 sender()/signalId() 访问器存在，结构同 5.15.2）/ 5.15.2（本机源码已对照）
#endif
#include <private/qmetaobject_p.h>
#include <private/qobject_p.h>

/*
 * Qt 私有头访问集中点（PRD 06）：
 * - 本文件是整个工程唯一允许 include Qt private 头的位置
 * - QAbstractMetaCallEvent / QMetaCallEvent 定义于 qobject_p.h（非公开 API）
 * - include 路径由 Qt5Core_PRIVATE_INCLUDE_DIRS 提供，自带 Qt 版本号，
 *   升级 Qt 而未适配时将直接编译失败，而不是运行期崩溃
 */

namespace qt_event_watcher
{

MetaCallInfo MetaCallParser::parse(QObject *receiver, QEvent *event)
{
	MetaCallInfo info;

	if (receiver == nullptr || event == nullptr)	return info;
	if (event->type() != QEvent::MetaCall)	return info;

	/*
     * receiver 当前处于 QObject::event()/notify() 调用链中，
     * 这里仅保存日志所需的值快照，不保存 receiver 指针。
     */
	const QMetaObject* receiverMo = receiver->metaObject();
	if (receiverMo != nullptr) {
		info.receiverClassName = QString::fromLatin1(receiverMo->className());
	}
	info.receiverObjectName = receiver->objectName();
	// receiver 线程归属快照（可靠信息，PRD 06 §5）：receiver 处于 notify 链中
	info.receiverThreadId = reinterpret_cast<quintptr>(receiver->thread()->currentThreadId());


#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	/*
	 * Qt 5.14.2 / 5.15.2 / 6.5.3 共用访问器路径（三版本头文件逐项核实，
	 * 访问器同名同义，非假设通用；5.14.2 经官方 v5.14.2 tag qobject_p.h
	 * L484-506 核实，5.15.2 / 6.5.3 本机源码对照）。QEvent::MetaCall 事件的实际类型是
	 * QAbstractMetaCallEvent 的派生类。转型方式与 Qt 源码一致（qobject.cpp /
	 * qobject.cpp 的 QObject::event 中 static_cast<QAbstractMetaCallEvent*>(e)）；
	 * 此处用 dynamic_cast 做额外校验，解析失败安全跳过（PRD 06）。
	 *
	 * sender() / signalId() 是 qobject_p.h 中的 public inline 访问器：
	 * - 仅读值/拷贝指针，不涉及成员偏移猜测
	 * - 全部 inline，不链接任何 Qt 私有符号
	 */
	const QAbstractMetaCallEvent *metaCallEvent = dynamic_cast<const QAbstractMetaCallEvent *>(event);
	if (metaCallEvent == nullptr)	return info;

	// Tier A：signalId 值拷贝，无条件安全
	info.signalId = metaCallEvent->signalId();
	/*
     * sender 仅取"指针值"作注册表查询键，绝不解引用（3B）：
     * 此刻 sender 可能已析构（queued 投递后槽内自毁等场景），
     * 任何 -> 访问都是 UB。
     * 身份（类名/信号签名）由 MetaCallSenderRegistry 在发射时刻
     * （sender 必然存活的窗口内）快照提供。
     */
	info.senderKey = metaCallEvent->sender();
	info.valid = true;

#else
#error "QtEventWatcher: requires Qt >= 5.14 (QAbstractMetaCallEvent accessor path unverified on older versions)"
#endif
    return info;
}

QString MetaCallParser::resolveSignalSignature(const QMetaObject *senderMo, int signalId)
{
	if (senderMo == nullptr || signalId < 0)	return QString();

	/*
	 * signalId → 信号签名（Qt 5.15 规范转换，已对照本机源码确认）：
	 * QMetaCallEvent 携带的 signalId 来自 doActivate 的
	 * signal_index = QMetaObjectPrivate::signalOffset(m) + local_signal_index
	 * Qt 自身把它转回 QMetaMethod 时使用的正是 QMetaObjectPrivate::signal
	 * （queued_activate / dumpObjectInfo 同款），不能用公开 API method(signalId) 近似：
	 * 实测 signalId=3 用 method() 会错误解析成 deleteLater()。
	 * m 为 nullptr 时该函数 Q_ASSERT 失败（Release 下 UB），因此必须判空。
	 */
	const QMetaMethod signalMethod = QMetaObjectPrivate::signal(senderMo, signalId);
	if (!signalMethod.isValid())	return QString();

	return QString::fromLatin1(signalMethod.methodSignature());
}

} // namespace qt_event_watcher
