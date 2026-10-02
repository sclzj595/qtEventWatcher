#pragma once

/// CdpLongTaskProber - Scout T2 CDP 渲染进程长任务探针（V7 S2）
/// 经 Chrome DevTools Protocol（目标以 --remote-debugging-port=<port> 启动）
/// 连接 Chromium 系页面（Electron / Edge / Chrome），注入
/// PerformanceObserver(longtask) 并周期 splice 回读长任务条目，映射为
/// [EventWatcher] slow event 行（event=cdpLongTask type=98 source=scout-cdp），
/// 复用既有采集/上行/报告链——aggregator / HTML / 导出零改动渲染。
///
/// 语义约束（T2 实证，docs/32 §S2）：
///   - 只消费页面真实任务：Runtime.evaluate 通道自身任务不产生 longtask 条目，
///     探针无需过滤自身噪声
///   - 目标页面 hidden（最小化/遮挡）时 observer 投递停滞——条目延迟，不丢失
///   - 页面导航重置 window 状态：回读命中 need-inject 哨兵自动重注入
///   - 已运行的 Electron 应用全局单例会吞 --remote-debugging-port 参数，
///     目标必须带该参数冷启动
///
/// 全异步（QNetworkAccessManager + QWebSocket），不阻塞 scout 主线程事件循环。
/// 已知边界：每条目一行，无探针侧风暴抑制（依赖 RecordSink 环形 4096 吸收；
/// 持续高频长任务页面可放宽 --cdp-threshold 降噪）。

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "ScoutAlarmEmitter.h"

class QNetworkAccessManager;
class QWebSocket;

namespace qt_event_watcher {

class CdpLongTaskProber : public QObject
{
	Q_OBJECT
public:
	/// cdpPort=0 时不应构造（main 层守卫）；targetFilter 空则取首个 page target，
	/// 非空按 url/title 子串匹配（大小写不敏感）；reportThresholdMs 为上报下界
	/// （longtask 定义下界 50ms，仅收更严的值）
	CdpLongTaskProber(int cdpPort, const QString &targetFilter,
					  int reportThresholdMs, QObject *parent = nullptr);

	void start();
	void stop();

private:
	ScoutAlarmEmitter m_alarm;		///< 告警风暴抑制门（docs/34 R3c）
	void discover();				// HTTP /json/list → 选 page target
	void connectPage(const QUrl &wsUrl, const QString &pageUrl);
	void inject();					// Runtime.evaluate 安装 PerformanceObserver
	void poll();					// Runtime.evaluate splice 回读
	void handleText(const QString &text);	// JSON-RPC 响应分发
	void emitLongTask(int durMs);

	int m_port = 0;
	QString m_filter;				///< url/title 子串过滤（空 = 首个 page）
	int m_thresholdMs = 50;			///< 上报下界（dur >= threshold 才发行）
	bool m_running = false;

	QNetworkAccessManager *m_nam = nullptr;
	QWebSocket *m_ws = nullptr;
	QTimer m_retryTimer;			///< 发现/连接失败重试（2s 固定，单发）
	QTimer m_pollTimer;				///< 长任务回读（500ms）
	QUrl m_wsUrl;
	QString m_pageUrl;				///< 当前页 url（行 receiver/object/url 字段）
	int m_nextId = 0;				///< JSON-RPC 请求 id 计数
	int m_injectId = -1;			///< 待响应的注入请求 id
	int m_pollId = -1;				///< 待响应的回读请求 id
	bool m_observerUp = false;		///< observer 注入成功标记
	bool m_loggedDiscoverFail = false;	///< 端口不可达只告警一次（重试期静默）
};

} // namespace qt_event_watcher
