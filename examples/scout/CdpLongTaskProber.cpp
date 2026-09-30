#include "CdpLongTaskProber.h"

#include "WatchLogMacros.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QWebSocket>

namespace qt_event_watcher {

namespace {

constexpr int kRetryMs = 2000;		// 发现/连接失败重试周期（固定，够轻）
constexpr int kPollMs = 500;		// 长任务回读周期

// 注入脚本：observer 收集 {d:时长ms, s:开始时刻ms}；buffered:true 回收注入前
// 已发生的条目。箭头函数在受支持的 Chromium（Electron/Edge 全系）可用（T2 实证）。
const char kInjectExpr[] =
	"window.__qewt_lt=[];"
	"new PerformanceObserver("
	"l=>{for(const e of l.getEntries())"
	"window.__qewt_lt.push({d:Math.round(e.duration),s:Math.round(e.startTime)})})"
	".observe({type:'longtask',buffered:true});'qewt-injected'";

// 回读表达式：splice 清窗取增量；页面导航重置 window 后 __qewt_lt 消失，
// 返回哨兵触发重注入（T2 实证语义）
const char kPollExpr[] =
	"typeof window.__qewt_lt==='undefined'"
	"?'need-inject':JSON.stringify(window.__qewt_lt.splice(0))";

} // namespace

CdpLongTaskProber::CdpLongTaskProber(int cdpPort, const QString &targetFilter,
									 int reportThresholdMs, QObject *parent)
	: QObject(parent)
	, m_port(cdpPort)
	, m_filter(targetFilter)
	, m_thresholdMs(reportThresholdMs > 0 ? reportThresholdMs : 50)
{
	m_retryTimer.setSingleShot(true);
	connect(&m_retryTimer, &QTimer::timeout, this, [this]() { discover(); });
	m_pollTimer.setInterval(kPollMs);
	connect(&m_pollTimer, &QTimer::timeout, this, [this]() { poll(); });
	m_nam = new QNetworkAccessManager(this);
}

void CdpLongTaskProber::start()
{
	m_running = true;
	QEW_LOG_INFO("[ScoutCdp] discovering http://127.0.0.1:{}/json/list "
				 "threshold={}ms filter={:s}",
				 m_port, m_thresholdMs, m_filter.toStdString());
	discover();
}

void CdpLongTaskProber::stop()
{
	m_running = false;
	m_pollTimer.stop();
	m_retryTimer.stop();
	if (m_ws != nullptr) {
		m_ws->close();
		m_ws->deleteLater();
		m_ws = nullptr;
	}
	m_observerUp = false;
}

void CdpLongTaskProber::discover()
{
	if (!m_running)
		return;
	const QUrl url(QStringLiteral("http://127.0.0.1:%1/json/list").arg(m_port));
	QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		if (!m_running)
			return;
		if (reply->error() != QNetworkReply::NoError) {
			// 端口不可达：只告警一次，静默重试（Electron 未起/未带参数冷启动）
			if (!m_loggedDiscoverFail) {
				m_loggedDiscoverFail = true;
				QEW_LOG_INFO("[ScoutCdp] /json/list unreachable ({}), "
							 "retrying every {}ms",
							 int(reply->error()), kRetryMs);
			}
			m_retryTimer.start(kRetryMs);
			return;
		}

		QJsonParseError perr = {};
		const QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll(), &perr);
		if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
			m_retryTimer.start(kRetryMs);
			return;
		}

		QString wsUrl;
		for (const QJsonValue &v : doc.array()) {
			const QJsonObject t = v.toObject();
			if (t.value(QStringLiteral("type")).toString()
					!= QLatin1String("page"))
				continue;
			const QString pageUrl = t.value(QStringLiteral("url")).toString();
			const QString title = t.value(QStringLiteral("title")).toString();
			if (!m_filter.isEmpty()
				&& !pageUrl.contains(m_filter, Qt::CaseInsensitive)
				&& !title.contains(m_filter, Qt::CaseInsensitive))
				continue;
			wsUrl = t.value(QStringLiteral("webSocketDebuggerUrl")).toString();
			if (!wsUrl.isEmpty()) {
				connectPage(QUrl(wsUrl),
							pageUrl.isEmpty() ? title : pageUrl);
				return;
			}
		}
		m_retryTimer.start(kRetryMs);	// 无匹配 page target，稍后再扫
	});
}

void CdpLongTaskProber::connectPage(const QUrl &wsUrl, const QString &pageUrl)
{
	m_pageUrl = pageUrl;
	m_ws = new QWebSocket(QString(), QWebSocketProtocol::Version13, this);
	connect(m_ws, &QWebSocket::connected, this, [this]() {
		if (!m_running)
			return;
		QEW_LOG_INFO("[ScoutCdp] page connected url={:s}", m_pageUrl.toStdString());
		m_loggedDiscoverFail = false;
		inject();
		m_pollTimer.start();
	});
	connect(m_ws, &QWebSocket::textMessageReceived,
			this, &CdpLongTaskProber::handleText);
	connect(m_ws, &QWebSocket::disconnected, this, [this]() {
		if (!m_running)
			return;
		m_pollTimer.stop();
		m_observerUp = false;
		QEW_LOG_INFO("[ScoutCdp] page disconnected, rediscovering in {}ms", kRetryMs);
		m_ws->deleteLater();
		m_ws = nullptr;
		m_retryTimer.start(kRetryMs);
	});
	// 连接失败（拒绝/超时）：errorOccurred（Qt6.5+）与 error 信号跨版本名不同，
	// 且失败后 disconnected 均会发出——重试统一由 disconnected 驱动，无需单独接
	m_ws->open(wsUrl);
}

void CdpLongTaskProber::inject()
{
	if (m_ws == nullptr || !m_running
		|| m_ws->state() != QAbstractSocket::ConnectedState)
		return;
	m_injectId = ++m_nextId;
	QJsonObject params;
	params.insert(QStringLiteral("expression"), QString::fromUtf8(kInjectExpr));
	params.insert(QStringLiteral("returnByValue"), true);
	QJsonObject msg;
	msg.insert(QStringLiteral("id"), m_injectId);
	msg.insert(QStringLiteral("method"), QStringLiteral("Runtime.evaluate"));
	msg.insert(QStringLiteral("params"), params);
	m_ws->sendTextMessage(
		QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void CdpLongTaskProber::poll()
{
	if (m_ws == nullptr || !m_running
		|| m_ws->state() != QAbstractSocket::ConnectedState)
		return;
	m_pollId = ++m_nextId;
	QJsonObject params;
	params.insert(QStringLiteral("expression"), QString::fromUtf8(kPollExpr));
	params.insert(QStringLiteral("returnByValue"), true);
	QJsonObject msg;
	msg.insert(QStringLiteral("id"), m_pollId);
	msg.insert(QStringLiteral("method"), QStringLiteral("Runtime.evaluate"));
	msg.insert(QStringLiteral("params"), params);
	m_ws->sendTextMessage(
		QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void CdpLongTaskProber::handleText(const QString &text)
{
	const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
	if (!msg.contains(QStringLiteral("id")))
		return;					// 事件推送（未订阅域），忽略
	const int id = msg.value(QStringLiteral("id")).toInt();

	if (id == m_injectId) {
		m_injectId = -1;
		const QJsonObject result = msg.value(QStringLiteral("result")).toObject();
		if (result.contains(QStringLiteral("exceptionDetails"))) {
			// 页面上下文异常：重连换新会话重注入
			QEW_LOG_INFO("[ScoutCdp] observer inject failed, reconnecting");
			if (m_ws != nullptr)
				m_ws->close();
			return;
		}
		m_observerUp = true;
		QEW_LOG_INFO("[ScoutCdp] longtask observer installed (threshold={}ms)",
					 m_thresholdMs);
	} else if (id == m_pollId) {
		m_pollId = -1;
		const QJsonObject result = msg.value(QStringLiteral("result")).toObject();
		const QJsonValue rv = result.value(QStringLiteral("result"))
								  .toObject().value(QStringLiteral("value"));
		if (rv.toString() == QLatin1String("need-inject")) {
			m_observerUp = false;
			inject();			// 页面导航重置：重装 observer
			return;
		}
		const QJsonArray arr =
			QJsonDocument::fromJson(rv.toString().toUtf8()).array();
		for (const QJsonValue &e : arr) {
			const int dur = e.toObject().value(QStringLiteral("d")).toInt();
			if (dur >= m_thresholdMs)
				emitLongTask(dur);
		}
	}
}

void CdpLongTaskProber::emitLongTask(int durMs)
{
	const std::string u = m_pageUrl.toStdString();
	// 行格式对齐自监控 slow event（hydrateRecord 通用解析可用）；
	// event=cdpLongTask type=98 source=scout-cdp 诚实标注来源
	QEW_LOG_WARN("[EventWatcher] slow event receiver={:s} object={:s} "
				 "event=cdpLongTask type=98 depth=0 costMs={} "
				 "exclusiveCostMs=0.000 curThread=0x0 recvThread=0x0 "
				 "match=true thresholdMs={} source=scout-cdp url={:s}",
				 u, u, durMs, m_thresholdMs, u);
}

} // namespace qt_event_watcher
