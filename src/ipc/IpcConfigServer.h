#pragma once

#include <QJsonObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLocalSocket;

namespace qt_event_watcher
{

class WatchConfig;
class MetaCallFilter;

/**
 * @brief IPC 配置服务端（V2 Phase B，PRD 21 §5）
 *
 * QLocalServer + JSON 行协议（NDJSON）：外部进程实时调控被测程序的
 * 监控行为。与 INI 热更新同构——请求处理直接调 WatchConfig setter
 * （内部 QWriteLocker 原子替换）与 MetaCallFilter 规则接口；
 * 全部信号槽驱动，主线程事件循环内执行，零监控热路径耦合。
 *
 * 协议（每行一个 JSON 对象，\n 分隔）：
 *   {"op":"get"}
 *   {"op":"set", "watchFun":15, "slowEventThresholdMs":10, ...}（部分键生效）
 *   {"op":"filter.add", "sender":"X" | "signal":"sig()" |
 *                       "receiver":"Y" | "pair":["S","sig()"] | "anonymous":true}
 *   {"op":"filter.clear"}
 *
 * 响应：合法请求回 {"ok":true, ...当前配置快照}；
 * 解析失败（坏 JSON / op 未知 / 行超限）→ 静默丢弃变更 + WARN，不回包
 * （PRD 21 Phase B 安全约束定稿）。
 *
 * 无认证：本机 QLocalSocket（Windows 命名管道）在 PRD 14 威胁模型内。
 */
class IpcConfigServer
{
public:
	/// 非拥有指针；filter 可为空（Bit1 未开启时 blacklist 能力降级不可用）
	IpcConfigServer(WatchConfig* config, MetaCallFilter* filter);
	~IpcConfigServer();

	/// 监听名：显式参数 > 环境变量 QT_EVENT_WATCHER_IPC_NAME > QtEventWatcher.<pid>
	bool start(const QString& name = {}, QString* error = nullptr);

	QString serverName() const;

private:
	void onNewConnection();
	void onSocketReadyRead(QLocalSocket* socket);
	void handleLine(QLocalSocket* socket, const QByteArray& line);

	/// op 分发；返回 false = op 未知/参数缺失（调用方丢弃 + WARN）
	bool applySet(const QJsonObject& obj);
	bool applyFilterAdd(const QJsonObject& obj);
	bool applyFilterClear();
	QJsonObject configSnapshot() const;

	void sendJson(QLocalSocket* socket, const QJsonObject& obj);

	WatchConfig* m_config = nullptr;
	MetaCallFilter* m_filter = nullptr;
	std::unique_ptr<QLocalServer> m_server;
	QString m_serverName;
};

} // namespace qt_event_watcher
