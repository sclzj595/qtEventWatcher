#include "IpcConfigServer.h"

#include "MetaCallFilter.h"
#include "WatchConfig.h"
#include "WatchLogMacros.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>

#include <limits>

namespace qt_event_watcher
{

namespace
{

constexpr int kMaxLineBytes = 8192;		///< 单行上限：超限视为恶意/异常客户端，丢弃并断开

/// 数字键统一处理：存在且为数字类型 → 应用 setter；类型错误/超 int 范围 → 忽略该键（WARN）
template <typename Setter>
void applyNumber(const QJsonObject& obj, const char* key, const char* logName, Setter setter)
{
	if (!obj.contains(QLatin1String(key)))	return;
	const QJsonValue v = obj.value(QLatin1String(key));
	if (!v.isDouble()) {
		QEW_LOG_WARN("[IpcConfig] ignored non-numeric field '{}' in request", logName);
		return;
	}
	const double d = v.toDouble();
	/*
	 * 超 int 范围的 double→int 转换是 UB（如 watchFun=4294967295），
	 * 且 WatchConfig::setWatchFun 无 sanitize 兜底——必须在转换前拒绝。
	 * 比较形式同时覆盖 NaN（全部比较为 false）。
	 */
	if (!(d >= static_cast<double>(std::numeric_limits<int>::min()) &&
	      d <= static_cast<double>(std::numeric_limits<int>::max()))) {
		QEW_LOG_WARN("[IpcConfig] ignored out-of-range field '{}' ({})", logName, d);
		return;
	}
	setter(static_cast<int>(d));
}

} // namespace

IpcConfigServer::IpcConfigServer(WatchConfig* config, MetaCallFilter* filter)
	: m_config(config)
	, m_filter(filter)
{
}

IpcConfigServer::~IpcConfigServer() = default;

bool IpcConfigServer::start(const QString& name, QString* error)
{
	if (m_server != nullptr)	return true;	// 已启动
	if (m_config == nullptr) {
		if (error)	*error = QStringLiteral("config is null");
		return false;
	}

	// 监听名：显式参数 > 环境变量 > QtEventWatcher.<pid>（多实例隔离）
	QString serverName = name;
	if (serverName.isEmpty()) {
		const QByteArray envName = qgetenv("QT_EVENT_WATCHER_IPC_NAME");
		if (!envName.isEmpty())
			serverName = QString::fromLocal8Bit(envName);
	}
	if (serverName.isEmpty())
		serverName = QStringLiteral("QtEventWatcher.%1").arg(QCoreApplication::applicationPid());

	QLocalServer::removeServer(serverName);		// 残留 socket 清理（Unix 语义；Windows 无副作用）

	m_server = std::make_unique<QLocalServer>();
	if (!m_server->listen(serverName)) {
		if (error)	*error = m_server->errorString();
		QEW_LOG_WARN("[IpcConfig] server listen failed name={} error={}",
					 serverName.toStdString(), m_server->errorString().toStdString());
		m_server.reset();
		return false;
	}
	m_serverName = serverName;

	// 信号槽驱动，主线程事件循环内执行（与 MetaCallFilter 的 GUI 线程约定兼容）。
	// 本类非 QObject：无 context 的 lambda connect，生命周期跟随信号源
	// （m_server / socket 均归本类所有，析构时连接自动失效，捕获 this 安全）
	QObject::connect(m_server.get(), &QLocalServer::newConnection, [this]() {
		onNewConnection();
	});

	QEW_LOG_INFO("[IpcConfig] server listening name={}", serverName.toStdString());
	return true;
}

QString IpcConfigServer::serverName() const
{
	return m_serverName;
}

void IpcConfigServer::onNewConnection()
{
	while (m_server != nullptr && m_server->hasPendingConnections()) {
		QLocalSocket* socket = m_server->nextPendingConnection();
		if (socket == nullptr)	continue;
		socket->setParent(m_server.get());		// 生命周期归 server

		QObject::connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
		QObject::connect(socket, &QLocalSocket::readyRead, [this, socket]() {
			onSocketReadyRead(socket);
		});
	}
}

void IpcConfigServer::onSocketReadyRead(QLocalSocket* socket)
{
	/*
	 * 洪泛防御：canReadLine 仅在缓冲含 '\n' 时为真，恶意客户端发送
	 * 无换行字节流可绕过下方超限检查，QLocalSocket 默认读缓冲无上限
	 * → 内存无界增长。积压超行上限且无完整行 = 必然非法，断开。
	 */
	if (socket->bytesAvailable() > kMaxLineBytes && !socket->canReadLine()) {
		QEW_LOG_WARN("[IpcConfig] flood without newline ({} bytes buffered), dropping client",
					 socket->bytesAvailable());
		socket->abort();
		return;
	}

	// 按行聚合：粘包/半包安全；超限整块丢弃并断开（防御异常客户端）
	while (socket->canReadLine()) {
		const QByteArray line = socket->readLine();
		if (line.size() > kMaxLineBytes) {
			QEW_LOG_WARN("[IpcConfig] oversized line ({} bytes), dropping client", line.size());
			socket->abort();
			return;
		}
		handleLine(socket, line);
	}
}

void IpcConfigServer::handleLine(QLocalSocket* socket, const QByteArray& line)
{
	const QByteArray trimmed = line.trimmed();
	if (trimmed.isEmpty())	return;

	// 定稿安全约束：解析失败静默丢弃 + WARN，不回包、不影响服务
	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseError);
	if (doc.isNull() || !doc.isObject()) {
		QEW_LOG_WARN("[IpcConfig] dropped malformed request ({}): {}",
					 parseError.errorString().toStdString(),
					 trimmed.left(120).toStdString());
		return;
	}

	const QJsonObject obj = doc.object();
	const QString op = obj.value(QStringLiteral("op")).toString();
	bool applied = false;
	if (op == QLatin1String("set")) {
		applied = applySet(obj);
	} else if (op == QLatin1String("filter.add")) {
		applied = applyFilterAdd(obj);
	} else if (op == QLatin1String("filter.clear")) {
		applied = applyFilterClear();
	} else if (op == QLatin1String("get")) {
		applied = true;
	} else {
		QEW_LOG_WARN("[IpcConfig] dropped request with unknown op '{}'", op.toStdString());
		return;
	}

	if (applied)
		sendJson(socket, configSnapshot());
}

bool IpcConfigServer::applySet(const QJsonObject& obj)
{
	if (m_config == nullptr)	return false;

	// 与 INI 热更新同构：直接走 setter（内部 QWriteLocker 原子替换 + sanitize 回退）
	applyNumber(obj, "watchFun", "watchFun", [this](int v) {
		m_config->setWatchFun(static_cast<WatchConfig::WatchFunMask>(static_cast<quint32>(v)));
	});
	applyNumber(obj, "slowEventThresholdMs", "slowEventThresholdMs",
				[this](int v) { m_config->setSlowEventThresholdMs(v); });
	applyNumber(obj, "slowMetaCallThresholdMs", "slowMetaCallThresholdMs",
				[this](int v) { m_config->setSlowMetaCallThresholdMs(v); });
	applyNumber(obj, "eventStatPeriodMs", "eventStatPeriodMs",
				[this](int v) { m_config->setEventStatPeriodMs(v); });
	applyNumber(obj, "eventCountThreshold", "eventCountThreshold",
				[this](int v) { m_config->setEventCountThreshold(v); });
	applyNumber(obj, "eventTotalCostThresholdMs", "eventTotalCostThresholdMs",
				[this](int v) { m_config->setEventTotalCostThresholdMs(v); });
	applyNumber(obj, "qssLoadThresholdMs", "qssLoadThresholdMs",
				[this](int v) { m_config->setQssLoadThresholdMs(v); });
	applyNumber(obj, "setStyleSheetThresholdMs", "setStyleSheetThresholdMs",
				[this](int v) { m_config->setStyleSheetThresholdMs(v); });
	applyNumber(obj, "qssFrequentCountThreshold", "qssFrequentCountThreshold",
				[this](int v) { m_config->setQssFrequentCountThreshold(v); });
	applyNumber(obj, "configPollIntervalMs", "configPollIntervalMs",
				[this](int v) { m_config->setConfigPollIntervalMs(v); });
	return true;
}

bool IpcConfigServer::applyFilterAdd(const QJsonObject& obj)
{
	if (m_filter == nullptr) {
		QEW_LOG_WARN("[IpcConfig] filter.add ignored: filter unavailable (Bit1 off)");
		return false;
	}

	if (obj.contains(QStringLiteral("sender"))) {
		m_filter->addSuppressedSender(obj.value(QStringLiteral("sender")).toString());
	}
	if (obj.contains(QStringLiteral("signal"))) {
		m_filter->addSuppressedSignal(obj.value(QStringLiteral("signal")).toString());
	}
	if (obj.contains(QStringLiteral("receiver"))) {
		m_filter->addSuppressedReceiver(obj.value(QStringLiteral("receiver")).toString());
	}
	if (obj.value(QStringLiteral("pair")).isArray()) {
		const QJsonArray pair = obj.value(QStringLiteral("pair")).toArray();
		if (pair.size() == 2)
			m_filter->addSuppressedPair(pair.at(0).toString(), pair.at(1).toString());
	}
	if (obj.contains(QStringLiteral("anonymous"))) {
		m_filter->setSuppressAnonymousSenders(
			obj.value(QStringLiteral("anonymous")).toBool(false));
	}
	return true;
}

bool IpcConfigServer::applyFilterClear()
{
	if (m_filter == nullptr) {
		QEW_LOG_WARN("[IpcConfig] filter.clear ignored: filter unavailable (Bit1 off)");
		return false;
	}
	m_filter->clearAll();
	return true;
}

QJsonObject IpcConfigServer::configSnapshot() const
{
	QJsonObject snap;
	snap.insert(QStringLiteral("ok"), true);
	if (m_config != nullptr) {
		snap.insert(QStringLiteral("watchFun"),
					QString::asprintf("0x%02x", m_config->watchFun()));
		snap.insert(QStringLiteral("slowEventThresholdMs"), m_config->slowEventThresholdMs());
		snap.insert(QStringLiteral("slowMetaCallThresholdMs"), m_config->slowMetaCallThresholdMs());
		snap.insert(QStringLiteral("eventStatPeriodMs"), m_config->eventStatPeriodMs());
		snap.insert(QStringLiteral("eventCountThreshold"), m_config->eventCountThreshold());
		snap.insert(QStringLiteral("eventTotalCostThresholdMs"),
					m_config->eventTotalCostThresholdMs());
		snap.insert(QStringLiteral("qssLoadThresholdMs"), m_config->qssLoadThresholdMs());
		snap.insert(QStringLiteral("setStyleSheetThresholdMs"),
					m_config->setStyleSheetThresholdMs());
		snap.insert(QStringLiteral("qssFrequentCountThreshold"),
					m_config->qssFrequentCountThreshold());
		snap.insert(QStringLiteral("configPollIntervalMs"), m_config->configPollIntervalMs());
	}
	snap.insert(QStringLiteral("filterAvailable"), m_filter != nullptr);
	return snap;
}

void IpcConfigServer::sendJson(QLocalSocket* socket, const QJsonObject& obj)
{
	if (socket == nullptr)	return;
	socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

} // namespace qt_event_watcher
