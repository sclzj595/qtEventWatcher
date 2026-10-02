#pragma once

/// RadarConfig - Scout V1 雷达配置文件（docs/34 §3.3）
/// INI [radar] 节：exclude / thresholdMs / intervalMs / cpuThreshold / cpuRuns
/// 解析为手写逐行（QSettings 在 worker 线程构造行为不确定，且单测需要完全
/// 确定的语义）：';' 或 '#' 开头为注释行；值内首个 ';' 起为行内注释
/// （exclude 为进程名子串，约定不含 ';'）；仅 [radar] 节内的键生效。
/// 优先级：默认 < 配置文件 < 显式 CLI（main.cpp 用显式位合成初始值）；
/// 热加载：RadarScheduler 每 tick 检查 mtime，变更即重载（worker 线程独占
/// 读，无锁）。

#include <QFile>
#include <QString>
#include <QStringList>

namespace qt_event_watcher {

struct RadarConfig
{
	QStringList exclude;
	bool excludeSet = false;	///< exclude 键是否出现（空值 = 显式清空）
	int thresholdMs = 0;		///< 0 = 未设置（保留现值）
	int intervalMs = 0;
	int cpuThreshold = 0;
	int cpuRuns = 0;

	bool hasAny() const
	{
		return excludeSet || thresholdMs > 0 || intervalMs > 0
			|| cpuThreshold > 0 || cpuRuns > 0;
	}
};

/// 解析 INI 配置文件 [radar] 节。文件打不开返回 false；解析到的键增量写入
/// out（数字键 >0 才视为有效设置，非法值静默忽略——保持调用方现值）。
inline bool loadRadarConfig(const QString &path, RadarConfig &out)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	bool inRadar = false;
	while (!f.atEnd()) {
		const QString line = QString::fromUtf8(f.readLine()).trimmed();
		if (line.isEmpty() || line.startsWith(QLatin1Char(';'))
			|| line.startsWith(QLatin1Char('#')))
			continue;
		if (line.startsWith(QLatin1Char('['))) {
			inRadar = line.compare(QStringLiteral("[radar]"),
								   Qt::CaseInsensitive) == 0;
			continue;
		}
		if (!inRadar)
			continue;
		const int eq = line.indexOf(QLatin1Char('='));
		if (eq <= 0)
			continue;
		const QString key = line.left(eq).trimmed();
		// 行内注释剥离：首个 ';' 起全部丢弃
		const QString val
			= line.mid(eq + 1).section(QLatin1Char(';'), 0, 0).trimmed();

		if (key.compare(QStringLiteral("exclude"), Qt::CaseInsensitive) == 0) {
			out.exclude = val.split(QLatin1Char(','), Qt::SkipEmptyParts);
			for (QString &e : out.exclude)
				e = e.trimmed();
			out.exclude.removeAll(QString());
			out.excludeSet = true;
		} else if (key.compare(QStringLiteral("thresholdMs"),
							   Qt::CaseInsensitive) == 0) {
			const int v = val.toInt();
			if (v > 0)
				out.thresholdMs = v;
		} else if (key.compare(QStringLiteral("intervalMs"),
							   Qt::CaseInsensitive) == 0) {
			const int v = val.toInt();
			if (v > 0)
				out.intervalMs = v;
		} else if (key.compare(QStringLiteral("cpuThreshold"),
							   Qt::CaseInsensitive) == 0) {
			const int v = val.toInt();
			if (v > 0)
				out.cpuThreshold = v;
		} else if (key.compare(QStringLiteral("cpuRuns"),
							   Qt::CaseInsensitive) == 0) {
			const int v = val.toInt();
			if (v > 0)
				out.cpuRuns = v;
		}
	}
	return true;
}

} // namespace qt_event_watcher
