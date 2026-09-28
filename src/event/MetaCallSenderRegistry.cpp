#include "MetaCallSenderRegistry.h"

#include "MetaCallParser.h"

#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QThread>

/*
 * Qt 私有头访问点（PRD 06 适配层，与 MetaCallParser.cpp 并列的例外）：
 * - QSignalSpyCallbackSet / qt_register_signal_spy_callbacks 位于 qobject_p.h
 * - 注册函数 Q_CORE_EXPORT（QtCore 导出），MSVC / MinGW 均可链接
 * - 全进程仅允许注册一组 spy 回调（QSignalSpy 同款机制，存在互斥约束）
 */
#include <private/qobject_p.h>

#include <algorithm>
#include <atomic>

namespace qt_event_watcher
{

namespace
{
constexpr std::size_t kRingCapacity = 512;

std::atomic<MetaCallSenderRegistry *> g_registryInstance{nullptr};

// 必须静态存储期：Qt 保存的是指针（QSignalSpyCallbackSet*），不拷贝
QSignalSpyCallbackSet g_spySet = {};
} // namespace

// 与头文件 friend 声明同scope：qt_event_watcher（不能放进匿名命名空间，
// 否则与 friend 声明注入的名字构成二义性重载）
void signalBeginTrampoline(QObject *caller, int signalIndex, void ** /*argv*/)
{
	if (MetaCallSenderRegistry *registry =
	        g_registryInstance.load(std::memory_order_relaxed)) {
		registry->onSignalBegin(caller, signalIndex);
	}
}

MetaCallSenderRegistry::MetaCallSenderRegistry()
{
	m_ring.resize(kRingCapacity);
}

MetaCallSenderRegistry::~MetaCallSenderRegistry()
{
	// 仅在仍持有全局回调时卸载：若已被 QSignalSpy 类工具抢占（全局指针不再
	// 指向 g_spySet），空集覆盖会误清抢占方的注册（进程级唯一资源）
	if (isCallbackActive()) {
		QSignalSpyCallbackSet emptySet = {};
		qt_register_signal_spy_callbacks(&emptySet);
	}
	g_registryInstance.store(nullptr, std::memory_order_release);
}

void MetaCallSenderRegistry::install()
{
	g_spySet = {};
	g_spySet.signal_begin_callback = &signalBeginTrampoline;
	g_spySet.signal_end_callback = nullptr;
	g_spySet.slot_begin_callback = nullptr;
	g_spySet.slot_end_callback = nullptr;

	g_registryInstance.store(this, std::memory_order_release);
	qt_register_signal_spy_callbacks(&g_spySet);
}

bool MetaCallSenderRegistry::isCallbackActive() const
{
	// 全局回调指针仍指向本表安装的集合，且回调函数未被改动
	return qt_signal_spy_callback_set.loadAcquire() == &g_spySet &&
	       g_spySet.signal_begin_callback == &signalBeginTrampoline;
}

bool MetaCallSenderRegistry::lookup(const void *sender, int signalId, SenderIdentity &out) const
{
	if (sender == nullptr || signalId < 0)	return false;

	std::lock_guard<std::mutex> lock(m_mutex);
	for (const Record &record : m_ring) {
		if (record.sender == sender && record.signalId == signalId && record.mo != nullptr) {
			out.senderClassName = record.className;
			out.senderThreadId = record.threadId;
			// C1 精确语义：归属线程 os id（ThreadRegistry 查得）；
			// 未观测到（0）时消费方回退 senderThreadId（发射线程快照）
			quintptr ownerOsId = 0;
			if (record.ownerThread != nullptr &&
			    m_threadRegistry.lookup(record.ownerThread, ownerOsId)) {
				out.ownerThreadId = ownerOsId;
			}
			for (const auto &entry : m_signatureTable) {
				if (entry.first == record.mo &&
				    signalId < static_cast<int>(entry.second.size())) {
					out.signalSignature = entry.second[static_cast<std::size_t>(signalId)];
					break;
				}
			}
			return true;
		}
	}
	return false;
}

void MetaCallSenderRegistry::onSignalBegin(QObject *sender, int signalId)
{
	// 发射时刻 sender 必然存活（正在 emit），此处解引用是安全的；
	// 日志时刻的解引用才是竞态窗口（3B 消除的目标）
	const QMetaObject *mo = sender->metaObject();
	if (mo == nullptr)	return;
	const char *className = mo->className();

	std::lock_guard<std::mutex> lock(m_mutex);

	// 签名表：metaObject 为静态数据，永不失效，可安全长期持有
	auto entryIt = std::find_if(m_signatureTable.begin(), m_signatureTable.end(),
	    [mo](const std::pair<const QMetaObject *, std::vector<QString>> &entry) {
		    return entry.first == mo;
	    });
	if (entryIt == m_signatureTable.end()) {
		m_signatureTable.emplace_back(mo, std::vector<QString>());
		entryIt = m_signatureTable.end() - 1;
	}
	if (static_cast<int>(entryIt->second.size()) <= signalId)
		entryIt->second.resize(static_cast<std::size_t>(signalId) + 1);
	if (entryIt->second[static_cast<std::size_t>(signalId)].isEmpty())
		entryIt->second[static_cast<std::size_t>(signalId)] =
		    MetaCallParser::resolveSignalSignature(mo, signalId);

	// 环形快照：固定容量覆写最旧，发射路径零动态分配
	Record &record = m_ring[m_ringNext];
	record.sender = sender;
	record.mo = mo;
	record.signalId = signalId;
	record.className = className;
	// 发射线程快照（emit 执行线程；跨线程直接 emit 时 ≠ 归属线程，见下）
	record.threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
	/*
	 * C1 精确归属语义：发射时刻 sender 存活 → sender->thread() 解引用安全；
	 * 归属 QThread* 登记 ThreadRegistry（首见注册/destroyed 注销/os id 回填），
	 * 日志路径仅以指针值查表。常态（归属线程内发射）owner os id == threadId；
	 * 跨线程直接 emit 时 lookup 给出真正的归属线程。
	 */
	QThread *ownerThread = sender->thread();
	m_threadRegistry.observe(ownerThread);
	record.ownerThread = ownerThread;
	m_ringNext = (m_ringNext + 1) % m_ring.size();
}

} // namespace qt_event_watcher
