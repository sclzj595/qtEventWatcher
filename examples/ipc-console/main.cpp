/**
 * @brief IPC 控制台（V2 Phase B 发送端示例）
 *
 * 连接被测程序的 IpcConfigServer（QLocalSocket + JSON 行协议），
 * 从命令行参数或 stdin 发送配置请求并打印响应。
 *
 * 用法：
 *   ipc_console.exe --pid 12345                       # 交互模式（stdin 每行一个 JSON）
 *   ipc_console.exe --pid 12345 --exec '{"op":"get"}' # 单发模式
 *   ipc_console.exe --name QtEventWatcher.12345 ...   # 显式 server 名
 *
 * 示例请求：
 *   {"op":"get"}
 *   {"op":"set","watchFun":0,"slowEventThresholdMs":10}
 *   {"op":"set","watchFun":15}
 *   {"op":"filter.add","sender":"Worker"}
 *   {"op":"filter.add","anonymous":true}
 *   {"op":"filter.clear"}
 */
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStringList>
#include <QTextStream>

#include <iostream>

int main(int argc, char* argv[])
{
	QCoreApplication app(argc, argv);
	const QStringList args = app.arguments();

	QString serverName;
	QString execPayload;
	for (int i = 1; i < args.size(); ++i) {
		if ((args.at(i) == QLatin1String("--pid") || args.at(i) == QLatin1String("-p"))
			&& i + 1 < args.size()) {
			serverName = QStringLiteral("QtEventWatcher.%1").arg(args.at(++i));
		} else if ((args.at(i) == QLatin1String("--name") || args.at(i) == QLatin1String("-n"))
				   && i + 1 < args.size()) {
			serverName = args.at(++i);
		} else if ((args.at(i) == QLatin1String("--exec") || args.at(i) == QLatin1String("-e"))
				   && i + 1 < args.size()) {
			execPayload = args.at(++i);
		}
	}
	if (serverName.isEmpty()) {
		std::cerr << "usage: ipc_console (--pid <pid> | --name <server>) [--exec '<json>']\n"
				  << "stdin mode (interactive): one JSON request per line, 'quit' to exit\n";
		return 2;
	}

	QLocalSocket socket;
	socket.connectToServer(serverName);
	if (!socket.waitForConnected(2000)) {
		std::cerr << "connect failed: " << qPrintable(socket.errorString())
				  << " (server=" << qPrintable(serverName) << ")\n";
		return 1;
	}
	std::cout << "connected: " << qPrintable(serverName) << "\n";

	QTextStream in(stdin, QIODevice::ReadOnly);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	in.setCodec("UTF-8");
#endif

	const auto sendLine = [&](const QString& line) -> bool {
		const QByteArray trimmed = line.toUtf8().trimmed();
		if (trimmed.isEmpty())			return true;
		if (trimmed == "quit" || trimmed == "exit")	return false;
		if (!trimmed.startsWith('{')) {
			std::cout << "(skip: not a JSON object line)\n";
			return true;
		}
		socket.write(trimmed + '\n');
		if (!socket.waitForBytesWritten(2000)) {
			std::cerr << "write failed: " << qPrintable(socket.errorString()) << "\n";
			return false;
		}
		// 解析失败类请求服务端静默丢弃不回包：短等待后无响应属预期
		socket.waitForReadyRead(1000);
		while (socket.canReadLine()) {
			const QByteArray resp = socket.readLine().trimmed();
			std::cout << "resp: " << resp.constData() << "\n";
		}
		return socket.state() == QLocalSocket::ConnectedState;
	};

	if (!execPayload.isEmpty()) {
		const bool ok = sendLine(execPayload);
		return ok ? 0 : 1;
	}

	std::cout << "interactive mode; one JSON per line; 'quit' to exit\n";
	while (true) {
		std::cout << "> " << std::flush;
		const QString line = in.readLine();
		if (in.atEnd() && line.isEmpty())	break;		// EOF
		if (!sendLine(line))	break;
	}
	std::cout << "bye\n";
	return 0;
}
