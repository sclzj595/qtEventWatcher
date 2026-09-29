// aggregator —— V4 D2 中心收集器（多进程聚合控制台，与 basic_demo 分进程验收）
//
// 监听 QLocalServer（默认 QtEventWatcherAggregator），接收各监控进程
// UplinkClient 推送的 op=record.push（NDJSON），按 pid 分组聚合存储。
//
// 用法：
//   aggregator [--name <server>] [--duration <sec>] [--tail] [--out <file.json>]
//              [--html <file.html>]
//
// 退出时 stdout 打印聚合汇总（按 pid counts / dropped / 连接数），
// --out 同时落 JSON（按 pid 分组全量记录，含 host_pid 分组结构），
// --html 落自包含静态聚合报告（V5.1 C 线，AggregationReporter）。

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include "AggregationReporter.h"
#include "WatchRecordStore.h"

#include <cstdio>
#include <deque>
#include <iostream>
#include <map>

namespace {

constexpr int kServerNameArg = 0;	// 占位：保持参数索引可读性
constexpr std::size_t kSessionCapacity = 4096;	///< 每 pid 环形上限（对齐 WatchRecordStore）

/// V5 A2：监控端协议 raw 化（record.push 只带 kind/seq/time/raw），
/// 本端用 WatchRecordStore::hydrateRecord 消费侧重建 fields/frames——
/// 存储与 JSON 导出形状与 V4 保持一致（fields=[[k,v]..]/frames=[[mod,off]..]）
void rebuildStructured(QJsonObject &rec)
{
	qt_event_watcher::WatchRecordStore::Record tmp;
	tmp.kind = rec.value(QStringLiteral("kind")).toInt();
	tmp.raw = rec.value(QStringLiteral("raw")).toString().toStdString();
	qt_event_watcher::WatchRecordStore::hydrateRecord(tmp);
	QJsonArray fields;
	for (const auto &f : tmp.fields) {
		QJsonArray pair;
		pair.append(QString::fromStdString(f.first));
		pair.append(QString::fromStdString(f.second));
		fields.append(pair);
	}
	rec.insert(QStringLiteral("fields"), fields);
	if (!tmp.frames.empty()) {
		QJsonArray frames;
		for (const auto &fr : tmp.frames) {
			QJsonArray frame;
			frame.append(QString::fromStdString(fr.module));
			frame.append(static_cast<qint64>(fr.offset));
			frames.append(frame);
		}
		rec.insert(QStringLiteral("frames"), frames);
	}
}

/// kind 枚举 → 名称（对齐 WatchRecordStore::Kind）
const char *kindName(int kind)
{
	switch (kind) {
	case 0:	return "SlowEvent";
	case 1:	return "MetaCall";
	case 2:	return "Qss";
	case 3:	return "Freeze";
	default: return "Unknown";
	}
}

} // namespace

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);

	QString serverName = QStringLiteral("QtEventWatcherAggregator");
	int durationSec = 0;		// 0 = 一直运行（Ctrl+C 结束）
	bool tail = false;
	QString outPath;
	QString htmlPath;
	for (int i = 1; i < argc; ++i) {
		const QString arg = QString::fromLocal8Bit(argv[i]);
		if (arg == "--name" && i + 1 < argc)
			serverName = QString::fromLocal8Bit(argv[++i]);
		else if (arg == "--duration" && i + 1 < argc)
			durationSec = QString::fromLocal8Bit(argv[++i]).toInt();
		else if (arg == "--tail")
			tail = true;
		else if (arg == "--out" && i + 1 < argc)
			outPath = QString::fromLocal8Bit(argv[++i]);
		else if (arg == "--html" && i + 1 < argc)
			htmlPath = QString::fromLocal8Bit(argv[++i]);
	}

	std::map<qint64, Session> sessions;

	QLocalServer server;
	QLocalServer::removeServer(serverName);		// 崩溃残留清理（Windows 为 no-op，无害）
	if (!server.listen(serverName)) {
		std::cerr << "[FAIL] aggregator listen " << serverName.toStdString()
				  << ": " << server.errorString().toStdString() << std::endl;
		return 1;
	}
	std::cout << "[AGG] listening " << serverName.toStdString() << std::endl;

	// 单连接读取：NDJSON 行协议（与 IpcConfigServer 同帧界规则 canReadLine）
	QObject::connect(&server, &QLocalServer::newConnection, [&server, &sessions, tail]() {
		while (QLocalSocket *conn = server.nextPendingConnection()) {
			QObject::connect(conn, &QLocalSocket::disconnected, conn, &QLocalSocket::deleteLater);
			QObject::connect(conn, &QLocalSocket::readyRead, [conn, &sessions, tail]() {
				while (conn->canReadLine()) {
					const QByteArray line = conn->readLine().trimmed();
					if (line.isEmpty())	continue;
					const QJsonDocument doc = QJsonDocument::fromJson(line);
					if (!doc.isObject())	continue;			// 解析失败静默丢弃（协议容错）
					const QJsonObject obj = doc.object();
					const QString op = obj.value(QStringLiteral("op")).toString();
					const qint64 pid = obj.value(QStringLiteral("pid")).toInt();
					// V5 B：健康度快照（随拍上行）——存最新、不入 records 计数；
					// tail 模式下 reconnects 增量打一行（重连事件可视化）
					if (op == QStringLiteral("health")) {
						Session &hs = sessions[pid];
						const qint64 rc = obj.value(QStringLiteral("reconnects")).toInt(0);
						if (tail && hs.hasHealth &&
							rc > hs.health.value(QStringLiteral("reconnects")).toInt(0))
							std::cout << "[pid=" << pid << "] upstream reconnects -> "
									  << rc << std::endl;
						hs.health = obj;
						hs.hasHealth = true;
						continue;
					}
					if (op != QStringLiteral("record.push"))	continue;
					Session &session = sessions[pid];
					const QJsonArray records = obj.value(QStringLiteral("records")).toArray();
					for (const QJsonValue &v : records) {
						if (!v.isObject())	continue;
						QJsonObject rec = v.toObject();
						rebuildStructured(rec);		// V5 A2：raw → fields/frames 消费侧重建
						session.records.push_back(std::move(rec));
						++session.received;
						while (session.records.size() > kSessionCapacity) {
							session.records.pop_front();
							++session.dropped;
						}
						if (tail) {
							const QJsonObject r = session.records.back();
							const QString raw = r.value(QStringLiteral("raw")).toString();
							std::cout << "[pid=" << pid << "] "
									  << kindName(r.value(QStringLiteral("kind")).toInt())
									  << " " << r.value(QStringLiteral("time")).toString().toStdString()
									  << " " << raw.left(96).toStdString() << std::endl;
						}
					}
					// 对端环形覆盖上报（背压丢弃计数随首次推送附带）
					const qint64 dropped = obj.value(QStringLiteral("dropped")).toInt(0);
					if (dropped > 0) {
						sessions[pid].dropped += static_cast<std::uint64_t>(dropped);
						std::cout << "[pid=" << pid << "] upstream dropped=" << dropped << std::endl;
					}
				}
			});
		}
	});

	if (durationSec > 0) {
		QTimer::singleShot(durationSec * 1000, &app, [&app]() { QCoreApplication::exit(0); });
	}

	const int code = app.exec();

	// ---- 退出汇总 + JSON 导出（D3 聚合报告的简版载体：按 pid 分组）----
	std::uint64_t total = 0;
	for (const auto &kv : sessions)
		total += kv.second.received;
	std::cout << "[AGG] sessions=" << sessions.size()
			  << " received=" << total << std::endl;
	for (const auto &kv : sessions) {
		std::cout << "  pid=" << kv.first
				  << " records=" << kv.second.records.size()
				  << " received=" << kv.second.received
				  << " dropped=" << kv.second.dropped;
		if (kv.second.hasHealth) {
			const QJsonObject &h = kv.second.health;
			std::cout << " health{pushed=" << h.value(QStringLiteral("pushed")).toInt()
					  << " dropped=" << h.value(QStringLiteral("dropped")).toInt()
					  << " reconnects=" << h.value(QStringLiteral("reconnects")).toInt()
					  << " lastSeq=" << h.value(QStringLiteral("lastSeq")).toInt() << "}";
		}
		std::cout << std::endl;
	}

	if (!outPath.isEmpty()) {
		QJsonObject root;
		root.insert(QStringLiteral("generatedAt"),
					QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
		root.insert(QStringLiteral("server"), serverName);
		QJsonArray sessionArray;
		for (const auto &kv : sessions) {
			QJsonObject s;
			s.insert(QStringLiteral("host_pid"), kv.first);
			s.insert(QStringLiteral("dropped"),
					 static_cast<qint64>(kv.second.dropped));
			s.insert(QStringLiteral("received"),
					 static_cast<qint64>(kv.second.received));
			if (kv.second.hasHealth)
				s.insert(QStringLiteral("health"), kv.second.health);	// V5 B
			QJsonArray arr;
			for (const QJsonObject &r : kv.second.records)
				arr.append(r);
			s.insert(QStringLiteral("records"), arr);
			sessionArray.append(s);
		}
		root.insert(QStringLiteral("sessions"), sessionArray);
		QFile file(outPath);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
			std::cout << "[AGG] exported " << outPath.toStdString() << std::endl;
		} else {
			std::cerr << "[FAIL] export " << outPath.toStdString() << std::endl;
		}
	}

	// ---- V5.1 C 线：自包含静态聚合报告（纯冷路径，仅在显式 --html 时执行）----
	if (!htmlPath.isEmpty()) {
		QString error;
		if (AggregationReporter::exportHtml(htmlPath, sessions, serverName, &error))
			std::cout << "[AGG] html exported " << htmlPath.toStdString() << std::endl;
		else
			std::cerr << "[FAIL] html export " << error.toStdString() << std::endl;
	}

	return code == 0 ? 0 : 1;
}
