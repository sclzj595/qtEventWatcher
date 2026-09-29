#include "UplinkClient.h"

#include "WatchConfig.h"
#include "WatchRecordStore.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>
#include <vector>

namespace qt_event_watcher
{

namespace
{
constexpr int kMaxBatchRecords = 64;		///< 单行最大记录条数（≤8KB 约束的主闸门）
constexpr int kMaxBatchBytes = 8 * 1024;	///< 单行字节上限（raw 过大时提前截批）
constexpr int kReconnectBaseMs = 100;		///< 断线重连退避基值（倍增至上限）
constexpr int kReconnectMaxMs = 5000;
constexpr int kHealthIntervalMs = 5000;		///< V5 B：健康度随拍节拍（constexpr 不入配置面，克制）
} // namespace

struct UplinkClient::Impl
{
	WatchConfig *config = nullptr;
	std::unique_ptr<QLocalSocket> socket;
	std::unique_ptr<QTimer> flushTimer;
	QElapsedTimer attemptClock;		///< 距上次连接尝试的时间（退避节拍）

	std::uint64_t lastSeq = 0;		///< 差量游标（WatchRecordStore seq）
	std::uint64_t pushed = 0;
	std::uint64_t dropped = 0;		///< 环形覆盖跳过累计
	std::uint64_t reconnects = 0;
	int backoffMs = 0;				///< 当前退避窗口；0 = 已连接或从未失败
	QString activeName;				///< 当前连接目标（名字变更触发重连）
	bool socketFailed = false;		///< 上一次 tick 观察到连接失效
	bool connected = false;			///< V5 B：上一 tick 是否处于连接态（断线检测统一入口）
	bool everConnected = false;		///< V5 B：是否建立过连接（重连计数前提）
	QElapsedTimer healthClock;		///< V5 B：距上次健康度上行的时间（构造即启动）
};

UplinkClient::UplinkClient(WatchConfig *config)
	: m_impl(std::make_unique<Impl>())
{
	m_impl->config = config;
	m_impl->socket = std::make_unique<QLocalSocket>();
	m_impl->flushTimer = std::make_unique<QTimer>();
	m_impl->flushTimer->setTimerType(Qt::CoarseTimer);
	QObject::connect(m_impl->flushTimer.get(), &QTimer::timeout, [this]() { flushTick(); });
	m_impl->flushTimer->start(m_impl->config->uplinkFlushMs());
	m_impl->healthClock.start();		///< V5 B：健康度节拍基准
}

UplinkClient::~UplinkClient() = default;

std::uint64_t UplinkClient::pushedCount() const { return m_impl->pushed; }
std::uint64_t UplinkClient::droppedCount() const { return m_impl->dropped; }
std::uint64_t UplinkClient::reconnectCount() const { return m_impl->reconnects; }

void UplinkClient::flushTick()
{
	Impl &s = *m_impl;

	// 热更新：名字实时读，变更即重连；空名 = 关闭（断开闲置，保留游标）
	const QString name = s.config->uplinkName().trimmed();
	if (name != s.activeName) {
		s.activeName = name;
		if (s.socket->state() != QLocalSocket::UnconnectedState)
			s.socket->abort();
		s.backoffMs = 0;
		s.socketFailed = false;
	}
	if (name.isEmpty())	return;

	const int flushMs = s.config->uplinkFlushMs();
	if (s.flushTimer->interval() != flushMs)
		s.flushTimer->start(flushMs);

	// 连接状态机：未连则按退避节拍尝试（异步 connectToServer，下拍再查）
	if (s.socket->state() == QLocalSocket::UnconnectedState) {
		s.connected = false;	// V5 B：断线检测统一入口（对端关闭/写错误/abort 均汇于此）
		if (s.socketFailed) {
			if (s.backoffMs == 0)	s.backoffMs = kReconnectBaseMs;
			else					s.backoffMs = qMin(s.backoffMs * 2, kReconnectMaxMs);
			if (s.attemptClock.isValid() &&
				s.attemptClock.elapsed() < s.backoffMs)
				return;		// 退避窗口内静默等待
		}
		s.attemptClock.start();
		s.socket->connectToServer(name);
		return;		// 连接未就绪，本拍不推（差量留环形缓冲）
	}
	if (s.socket->state() != QLocalSocket::ConnectedState) {
		s.connected = false;	// Connecting：等下一拍
		return;
	}

	// V5 B 重连计数：建立过连接后再次建连即计——覆盖对端进程退出（本地
	// 无写错误可观察，socketFailed 不置位）与写后 error 两种断线路径
	if (!s.connected) {
		s.connected = true;
		if (s.everConnected)
			++s.reconnects;
	}
	s.everConnected = true;

	if (s.socketFailed) {
		s.socketFailed = false;
		s.backoffMs = 0;
	}

	// V5 B 健康度随拍：flushTick 内 5s 节拍（零定时器），已连接才发——
	// 断线期不发，重连恢复后首个到期拍补出；计数器为发送时刻的采样值
	if (s.healthClock.elapsed() >= kHealthIntervalMs) {
		s.healthClock.restart();
		QJsonObject h;
		h.insert(QStringLiteral("op"), QStringLiteral("health"));
		h.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
		h.insert(QStringLiteral("pushed"), static_cast<qint64>(s.pushed));
		h.insert(QStringLiteral("dropped"), static_cast<qint64>(s.dropped));
		h.insert(QStringLiteral("reconnects"), static_cast<qint64>(s.reconnects));
		h.insert(QStringLiteral("lastSeq"), static_cast<qint64>(s.lastSeq));
		s.socket->write(QJsonDocument(h).toJson(QJsonDocument::Compact) + '\n');
	}

	// 差量拉取（环形覆盖段计入 dropped，游标跳至最旧现存——背压丢弃语义）
	std::uint64_t newSeq = s.lastSeq;
	std::size_t skipped = 0;
	const std::vector<WatchRecordStore::Record> records =
		WatchRecordStore::instance().snapshotSince(s.lastSeq, newSeq, skipped);
	if (skipped > 0)
		s.dropped += skipped;
	if (records.empty()) {
		s.lastSeq = newSeq;
		return;
	}

	// 分批组包：每行 ≤64 条且 ≤8KB；单行含本拍 skipped 汇总（仅首批携带）。
	// V5 A2 协议 raw 化：线上只带 kind/seq/time/raw——监控端全链路零解析
	// （A1 后环形本就不预解析，组包不再临时 hydrate），fields/frames 由
	// aggregator 侧 hydrateRecord 重建
	std::size_t index = 0;
	const qint64 pid = QCoreApplication::applicationPid();
	while (index < records.size()) {
		QJsonArray recordArray;
		qint64 batchBytes = 0;
		std::size_t batchCount = 0;
		while (index < records.size() && batchCount < kMaxBatchRecords
		       && batchBytes < kMaxBatchBytes) {
			const WatchRecordStore::Record &r = records[index];
			QJsonObject obj;
			obj.insert(QStringLiteral("kind"), r.kind);
			obj.insert(QStringLiteral("seq"), static_cast<qint64>(r.seq));
			obj.insert(QStringLiteral("time"), QString::fromStdString(r.time));
			obj.insert(QStringLiteral("raw"), QString::fromStdString(r.raw));
			recordArray.append(obj);
			batchBytes += static_cast<qint64>(r.raw.size()) + 96;	// raw + 结构开销估算
			++batchCount;
			++index;
		}

		QJsonObject line;
		line.insert(QStringLiteral("op"), QStringLiteral("record.push"));
		line.insert(QStringLiteral("pid"), pid);
		line.insert(QStringLiteral("records"), recordArray);
		if (skipped > 0 && index <= kMaxBatchRecords) {
			line.insert(QStringLiteral("dropped"), static_cast<qint64>(skipped));
			skipped = 0;		// 首行附带一次即可
		}
		const QByteArray packet =
			QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
		s.socket->write(packet);
	}

	s.pushed += records.size();
	s.lastSeq = newSeq;

	// 写后状态复查：对端已关闭时 buffered 写不立即报错，flush 前探状态
	if (s.socket->error() != QLocalSocket::UnknownSocketError)
		s.socketFailed = true;
}

} // namespace qt_event_watcher
