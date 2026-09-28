#include "ThreadRegistry.h"

#include <QObject>
#include <QThread>

namespace qt_event_watcher
{

void ThreadRegistry::observe(QThread* ownerThread)
{
	if (ownerThread == nullptr)	return;

	const void* key = ownerThread;
	const quintptr currentId = reinterpret_cast<quintptr>(QThread::currentThreadId());
	// QThread::isCurrentThread() 为 Qt 6.9+ API，此处用跨版本等价写法
	const bool isCurrent = (ownerThread == QThread::currentThread());

	bool needConnect = false;
	{
		QMutexLocker locker(&m_mutex);
		auto it = m_threads.find(key);
		if (it != m_threads.end()) {
			// 已注册：归属线程本尊出现时回填 os id（跨线程首发场景）
			if (it->osThreadId == 0 && isCurrent)
				it->osThreadId = currentId;
			return;
		}
		if (m_threads.size() >= kMaxThreads)	return;	// 满员降级，不覆盖已有条目

		Entry entry;
		entry.osThreadId = isCurrent ? currentId : 0;
		m_threads.insert(key, entry);
		needConnect = true;		// 锁外 connect（见下）
	}

	/*
	 * 析构注销：destroyed 在 owner 析构中发射 → 条目移除，表内键值
	 * 永远指向存活中的 QThread（或刚析构完的键值已被移除）。
	 * 锁内不 connect（Qt 内部锁与 m_mutex 顺序无保证）；owner 在发射
	 * 时刻必然存活，此处（同一调用栈内）析构窗口实际不存在。
	 * 连接句柄回填条目：本注册表先于 QThread 析构时（如 QThread 作为
	 * QApplication 子对象在 ~QObject 阶段才销毁），由 ~ThreadRegistry
	 * 统一 disconnect，避免 destroyed 回调打在已释放对象上（无 context
	 * receiver 的连接仅随 sender 失效）。
	 */
	if (needConnect) {
		const QMetaObject::Connection conn = QObject::connect(
			ownerThread, &QObject::destroyed,
			[this, key]() { unregister(key); });
		QMutexLocker locker(&m_mutex);
		auto it = m_threads.find(key);
		if (it != m_threads.end())
			it->destroyedConn = conn;
	}
}

ThreadRegistry::~ThreadRegistry()
{
	// 关闭顺序防御：断开全部 destroyed 连接（此刻起表内键值不再可信）
	QMutexLocker locker(&m_mutex);
	for (auto it = m_threads.begin(); it != m_threads.end(); ++it)
		QObject::disconnect(it->destroyedConn);
}

bool ThreadRegistry::lookup(const void* ownerThreadPtr, quintptr& osThreadId) const
{
	QMutexLocker locker(&m_mutex);
	auto it = m_threads.find(ownerThreadPtr);
	if (it == m_threads.end())	return false;
	osThreadId = it->osThreadId;
	return true;
}

int ThreadRegistry::size() const
{
	QMutexLocker locker(&m_mutex);
	return m_threads.size();
}

void ThreadRegistry::unregister(const void* ownerThreadPtr)
{
	QMutexLocker locker(&m_mutex);
	m_threads.remove(ownerThreadPtr);
}

} // namespace qt_event_watcher
