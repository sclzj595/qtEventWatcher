#include "WatchConfig.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSettings>

#include "WatchLogMacros.h"

namespace qt_event_watcher {

namespace {

/// 常量
constexpr char ConfigGroup[]                    = "QtEventWatcher";
constexpr char EnvWatchFun[]                    = "QT_EVENT_WATCHER_WATCH_FUN";
constexpr char EnvSlowEventThresholdMs[]        = "QT_EVENT_WATCHER_SLOW_EVENT_THRESHOLD_MS";
constexpr char EnvSlowMetaCallThresholdMs[]     = "QT_EVENT_WATCHER_SLOW_METACALL_THRESHOLD_MS";
constexpr char EnvEventStatPeriodMs[]           = "QT_EVENT_WATCHER_EVENT_STAT_PERIOD_MS";
constexpr char EnvEventCountThreshold[]         = "QT_EVENT_WATCHER_EVENT_COUNT_THRESHOLD";
constexpr char EnvEventTotalCostThresholdMs[]   = "QT_EVENT_WATCHER_EVENT_TOTAL_COST_THRESHOLD_MS";
constexpr char EnvQssLoadThresholdMs[]          = "QT_EVENT_WATCHER_QSS_LOAD_THRESHOLD_MS";
constexpr char EnvSetStyleSheetThresholdMs[]    = "QT_EVENT_WATCHER_SET_STYLESHEET_THRESHOLD_MS";
constexpr char EnvQssFrequentCountThreshold[]   = "QT_EVENT_WATCHER_QSS_FREQUENT_COUNT_THRESHOLD";
constexpr char EnvConfigPollIntervalMs[]        = "QT_EVENT_WATCHER_CONFIG_POLL_INTERVAL_MS";
constexpr char EnvFreezeThresholdMs[]           = "QT_EVENT_WATCHER_FREEZE_THRESHOLD_MS";
constexpr char EnvHeartbeatIntervalMs[]         = "QT_EVENT_WATCHER_HEARTBEAT_INTERVAL_MS";
constexpr char EnvAlarmSuppressWindowMs[]       = "QT_EVENT_WATCHER_ALARM_SUPPRESS_WINDOW_MS";
constexpr char EnvStackCaptureMode[]            = "QT_EVENT_WATCHER_STACK_CAPTURE_MODE";
constexpr char EnvUplinkName[]                  = "QT_EVENT_WATCHER_UPLINK_NAME";
constexpr char EnvUplinkFlushMs[]               = "QT_EVENT_WATCHER_UPLINK_FLUSH_MS";
constexpr char KeyWatchFun[]                    = "Watch_Fun";
constexpr char KeySlowEventThresholdMs[]        = "SlowEventThresholdMs";
constexpr char KeySlowMetaCallThresholdMs[]     = "SlowMetaCallThresholdMs";
constexpr char KeyEventStatPeriodMs[]           = "EventStatPeriodMs";
constexpr char KeyEventCountThreshold[]         = "EventCountThreshold";
constexpr char KeyEventTotalCostThresholdMs[]   = "EventTotalCostThresholdMs";
constexpr char KeyQssLoadThresholdMs[]          = "QssLoadThresholdMs";
constexpr char KeySetStyleSheetThresholdMs[]    = "SetStyleSheetThresholdMs";
constexpr char KeyQssFrequentCountThreshold[]   = "QssFrequentCountThreshold";
constexpr char KeyConfigPollIntervalMs[]        = "ConfigPollIntervalMs";
constexpr char KeyFreezeThresholdMs[]           = "FreezeThresholdMs";
constexpr char KeyHeartbeatIntervalMs[]         = "HeartbeatIntervalMs";
constexpr char KeyAlarmSuppressWindowMs[]       = "AlarmSuppressWindowMs";
constexpr char KeyStackCaptureMode[]            = "StackCaptureMode";
constexpr char KeyUplinkName[]                  = "UplinkName";
constexpr char KeyUplinkFlushMs[]               = "UplinkFlushMs";

} // namespace

WatchConfig::WatchConfig() = default;

/// Load
bool WatchConfig::loadFromFile(const QString &filePath)
{
	if (filePath.isEmpty())				return false;
	if (!QFileInfo::exists(filePath))	return false;

	QSettings settings(filePath, QSettings::IniFormat);
	settings.beginGroup(QString::fromLatin1(ConfigGroup));

	{
		QWriteLocker locker(&m_lock);

        if (settings.contains(QString::fromLatin1(KeyWatchFun))) {
            const auto value = settings.value(QString::fromLatin1(KeyWatchFun), static_cast<quint32>(m_values.watchFun)).toUInt();
            m_values.watchFun = static_cast<WatchFunMask>(value);
        }

        if (settings.contains(QString::fromLatin1(KeySlowEventThresholdMs))) {
            const int value = settings.value(QString::fromLatin1(KeySlowEventThresholdMs), m_values.slowEventThresholdMs).toInt();
            m_values.slowEventThresholdMs = sanitizePositive(value, DefaultSlowEventThresholdMs);
        }

        if (settings.contains(QString::fromLatin1(KeySlowMetaCallThresholdMs))) {
            const int value = settings.value(QString::fromLatin1(KeySlowMetaCallThresholdMs), m_values.slowMetaCallThresholdMs).toInt();
            m_values.slowMetaCallThresholdMs = sanitizePositive(value, DefaultSlowMetaCallThresholdMs);
        }

        if (settings.contains(QString::fromLatin1(KeyEventStatPeriodMs))) {
            const int value = settings.value(QString::fromLatin1(KeyEventStatPeriodMs), m_values.eventStatPeriodMs).toInt();
            m_values.eventStatPeriodMs = sanitizePositive(value, DefaultEventStatPeriodMs);
        }

        if (settings.contains(QString::fromLatin1(KeyEventCountThreshold))) {
            const int value = settings.value(QString::fromLatin1(KeyEventCountThreshold), m_values.eventCountThreshold).toInt();
            m_values.eventCountThreshold = sanitizePositive(value, DefaultEventCountThreshold);
        }

        if (settings.contains(QString::fromLatin1(KeyEventTotalCostThresholdMs))) {
            const int value = settings.value(QString::fromLatin1(KeyEventTotalCostThresholdMs), m_values.eventTotalCostThresholdMs).toInt();
            m_values.eventTotalCostThresholdMs = sanitizePositive(value, DefaultEventTotalCostThresholdMs);
        }

        if (settings.contains(QString::fromLatin1(KeyQssLoadThresholdMs))) {
            const int value = settings.value(QString::fromLatin1(KeyQssLoadThresholdMs), m_values.qssLoadThresholdMs).toInt();
            m_values.qssLoadThresholdMs = sanitizePositive(value, DefaultQssLoadThresholdMs);
        }

        if (settings.contains(QString::fromLatin1(KeySetStyleSheetThresholdMs))) {
            const int value = settings.value(QString::fromLatin1(KeySetStyleSheetThresholdMs), m_values.setStyleSheetThresholdMs).toInt();
            m_values.setStyleSheetThresholdMs = sanitizePositive(value, DefaultSetStyleSheetThresholdMs);
        }

        if (settings.contains(QString::fromLatin1(KeyQssFrequentCountThreshold))) {
            const int value = settings.value(QString::fromLatin1(KeyQssFrequentCountThreshold), m_values.qssFrequentCountThreshold).toInt();
            m_values.qssFrequentCountThreshold = sanitizePositive(value, DefaultQssFrequentCountThreshold);
        }

        if (settings.contains(QString::fromLatin1(KeyConfigPollIntervalMs))) {
            const int value = settings.value(QString::fromLatin1(KeyConfigPollIntervalMs), m_values.configPollIntervalMs).toInt();
            m_values.configPollIntervalMs = sanitizePositive(value, DefaultConfigPollIntervalMs);
        }

        if (settings.contains(QString::fromLatin1(KeyFreezeThresholdMs))) {
            const int value = settings.value(QString::fromLatin1(KeyFreezeThresholdMs), m_values.freezeThresholdMs).toInt();
            m_values.freezeThresholdMs = sanitizePositive(value, DefaultFreezeThresholdMs);
        }

        if (settings.contains(QString::fromLatin1(KeyHeartbeatIntervalMs))) {
            const int value = settings.value(QString::fromLatin1(KeyHeartbeatIntervalMs), m_values.heartbeatIntervalMs).toInt();
            m_values.heartbeatIntervalMs = sanitizePositive(value, DefaultHeartbeatIntervalMs);
        }

        if (settings.contains(QString::fromLatin1(KeyAlarmSuppressWindowMs))) {
            const int value = settings.value(QString::fromLatin1(KeyAlarmSuppressWindowMs), m_values.alarmSuppressWindowMs).toInt();
            m_values.alarmSuppressWindowMs = sanitizePositive(value, DefaultAlarmSuppressWindowMs);
        }

        if (settings.contains(QString::fromLatin1(KeyStackCaptureMode))) {
            const int value = settings.value(QString::fromLatin1(KeyStackCaptureMode), m_values.stackCaptureMode).toInt();
            m_values.stackCaptureMode = (value >= 0 && value <= 2) ? value : DefaultStackCaptureMode;
        }

        // V4 D1：上行链路（名字空串 = 关闭）
        if (settings.contains(QString::fromLatin1(KeyUplinkName))) {
            m_values.uplinkName = settings.value(QString::fromLatin1(KeyUplinkName)).toString().trimmed();
        }
        if (settings.contains(QString::fromLatin1(KeyUplinkFlushMs))) {
            const int value = settings.value(QString::fromLatin1(KeyUplinkFlushMs), m_values.uplinkFlushMs).toInt();
            m_values.uplinkFlushMs = sanitizePositive(value, DefaultUplinkFlushMs);
        }
	}

	settings.endGroup();
	return !settings.status();
}

void WatchConfig::loadFromEnvironment()
{
    QWriteLocker locker(&m_lock);

    loadValueFromEnvironment(EnvWatchFun,                  m_values.watchFun, DefaultWatchFun);
    loadValueFromEnvironment(EnvSlowEventThresholdMs,      m_values.slowEventThresholdMs, DefaultSlowEventThresholdMs);
    loadValueFromEnvironment(EnvSlowMetaCallThresholdMs,   m_values.slowMetaCallThresholdMs, DefaultSlowMetaCallThresholdMs);
    loadValueFromEnvironment(EnvEventStatPeriodMs,         m_values.eventStatPeriodMs, DefaultEventStatPeriodMs);
    loadValueFromEnvironment(EnvEventCountThreshold,       m_values.eventCountThreshold, DefaultEventCountThreshold);
    loadValueFromEnvironment(EnvEventTotalCostThresholdMs, m_values.eventTotalCostThresholdMs, DefaultEventTotalCostThresholdMs);
    loadValueFromEnvironment(EnvQssLoadThresholdMs,        m_values.qssLoadThresholdMs, DefaultQssLoadThresholdMs);
    loadValueFromEnvironment(EnvSetStyleSheetThresholdMs,  m_values.setStyleSheetThresholdMs, DefaultSetStyleSheetThresholdMs);
    loadValueFromEnvironment(EnvQssFrequentCountThreshold, m_values.qssFrequentCountThreshold, DefaultQssFrequentCountThreshold);
    loadValueFromEnvironment(EnvConfigPollIntervalMs,      m_values.configPollIntervalMs, DefaultConfigPollIntervalMs);
    loadValueFromEnvironment(EnvFreezeThresholdMs,         m_values.freezeThresholdMs, DefaultFreezeThresholdMs);
    loadValueFromEnvironment(EnvHeartbeatIntervalMs,       m_values.heartbeatIntervalMs, DefaultHeartbeatIntervalMs);
    loadValueFromEnvironment(EnvAlarmSuppressWindowMs,     m_values.alarmSuppressWindowMs, DefaultAlarmSuppressWindowMs);
    // StackCaptureMode：0/1/2 之外回退默认（手工 clamp，env 加载器只提供 int 通道）
    {
        int mode = DefaultStackCaptureMode;
        loadValueFromEnvironment(EnvStackCaptureMode, mode, DefaultStackCaptureMode);
        m_values.stackCaptureMode = (mode >= 0 && mode <= 2) ? mode : DefaultStackCaptureMode;
    }
    // V4 D1：上行链路（名字非空才覆盖，空 env 不关闭已加载的 INI 配置）
    {
        const QString name = qEnvironmentVariable(EnvUplinkName);
        if (!name.isEmpty())
            m_values.uplinkName = name.trimmed();
        loadValueFromEnvironment(EnvUplinkFlushMs, m_values.uplinkFlushMs, DefaultUplinkFlushMs);
    }
}

bool WatchConfig::load(const QString &filePath)
{
	reset();
	// 默认  ->  环境
	loadFromEnvironment();
	// conf file 
	bool fileLoaded = false;

	// 热更新基准路径：显式参数 > 环境变量 QT_EVENT_WATCHER_CONFIG_FILE > ./QtEventWatcher.ini
	QString iniPath;
	{
		QReadLocker locker(&m_lock);
		iniPath = filePath.isEmpty() ? m_configFile : filePath;
	}
	if (iniPath.isEmpty()) {
		const QByteArray envPath = qgetenv("QT_EVENT_WATCHER_CONFIG_FILE");
		if (!envPath.isEmpty())	iniPath = QString::fromLocal8Bit(envPath);
	}
	if (iniPath.isEmpty())	iniPath = QStringLiteral("QtEventWatcher.ini");

	fileLoaded = loadFromFile(iniPath);

	const QFileInfo fi(iniPath);
	{
		QWriteLocker locker(&m_lock);
		m_configFile = iniPath;
		m_lastMtimeMs = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
	}

	QEW_LOG_INFO(
	    "[WatchConfig] loaded file={} watchFun={:#x} slowEvent={}ms slowMetaCall={}ms "
	    "statPeriod={}ms poll={}ms",
	    iniPath.toStdString(), watchFun(), slowEventThresholdMs(), slowMetaCallThresholdMs(),
	    eventStatPeriodMs(), configPollIntervalMs());

	return fileLoaded || !fi.exists();
}

/// @brief Watch_Fun
WatchConfig::WatchFunMask WatchConfig::watchFun() const
{
	QReadLocker locker(&m_lock);
    return m_values.watchFun;
}

void WatchConfig::setWatchFun(WatchFunMask value)
{
	QWriteLocker locker(&m_lock);
    m_values.watchFun = value;
}

bool WatchConfig::isWatchEnabled(WatchFunction function) const
{
	QReadLocker locker(&m_lock);
	return (m_values.watchFun & static_cast<WatchFunMask>(function)) != 0;
}

void WatchConfig::setWatchEnabled(WatchFunction function, bool enabled)
{
	QWriteLocker locker(&m_lock);
    const auto mask = static_cast<WatchFunMask>(function);
    if (enabled)	m_values.watchFun |= mask;
    else			m_values.watchFun &= ~mask;
}

void WatchConfig::enableWatch(WatchFunction function)
{
	setWatchEnabled(function, true);
}

void WatchConfig::disableWatch(WatchFunction function)
{
	setWatchEnabled(function, false);
}

/// @brief Thresholds 
int WatchConfig::slowEventThresholdMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.slowEventThresholdMs;
}

void WatchConfig::setSlowEventThresholdMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.slowEventThresholdMs = sanitizePositive(value, DefaultSlowEventThresholdMs);
}

int WatchConfig::slowMetaCallThresholdMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.slowMetaCallThresholdMs;
}

void WatchConfig::setSlowMetaCallThresholdMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.slowMetaCallThresholdMs = sanitizePositive(value, DefaultSlowMetaCallThresholdMs);
}

int WatchConfig::eventStatPeriodMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.eventStatPeriodMs;
}

void WatchConfig::setEventStatPeriodMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.eventStatPeriodMs = sanitizePositive(value, DefaultEventStatPeriodMs);
}

int WatchConfig::eventCountThreshold() const
{
	QReadLocker locker(&m_lock);
	return m_values.eventCountThreshold;
}

void WatchConfig::setEventCountThreshold(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.eventCountThreshold = sanitizePositive(value, DefaultEventCountThreshold);
}

int WatchConfig::eventTotalCostThresholdMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.eventTotalCostThresholdMs;
}

void WatchConfig::setEventTotalCostThresholdMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.eventTotalCostThresholdMs = sanitizePositive(value, DefaultEventTotalCostThresholdMs);
}

int WatchConfig::qssLoadThresholdMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.qssLoadThresholdMs;
}

void WatchConfig::setQssLoadThresholdMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.qssLoadThresholdMs = sanitizePositive(value, DefaultQssLoadThresholdMs);
}

int WatchConfig::setStyleSheetThresholdMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.setStyleSheetThresholdMs;
}

void WatchConfig::setStyleSheetThresholdMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.setStyleSheetThresholdMs = sanitizePositive(value, DefaultSetStyleSheetThresholdMs);
}

int WatchConfig::qssFrequentCountThreshold() const
{
	QReadLocker locker(&m_lock);
	return m_values.qssFrequentCountThreshold;
}

void WatchConfig::setQssFrequentCountThreshold(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.qssFrequentCountThreshold = sanitizePositive(value, DefaultQssFrequentCountThreshold);
}

int WatchConfig::configPollIntervalMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.configPollIntervalMs;
}

void WatchConfig::setConfigPollIntervalMs(int value)
{
	QWriteLocker locker(&m_lock);
    m_values.configPollIntervalMs = sanitizePositive(value, DefaultConfigPollIntervalMs);
}

int WatchConfig::freezeThresholdMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.freezeThresholdMs;
}

void WatchConfig::setFreezeThresholdMs(int value)
{
	QWriteLocker locker(&m_lock);
	m_values.freezeThresholdMs = sanitizePositive(value, DefaultFreezeThresholdMs);
}

int WatchConfig::heartbeatIntervalMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.heartbeatIntervalMs;
}

void WatchConfig::setHeartbeatIntervalMs(int value)
{
	QWriteLocker locker(&m_lock);
	m_values.heartbeatIntervalMs = sanitizePositive(value, DefaultHeartbeatIntervalMs);
}

int WatchConfig::alarmSuppressWindowMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.alarmSuppressWindowMs;
}

void WatchConfig::setAlarmSuppressWindowMs(int value)
{
	QWriteLocker locker(&m_lock);
	m_values.alarmSuppressWindowMs = sanitizePositive(value, DefaultAlarmSuppressWindowMs);
}

int WatchConfig::stackCaptureMode() const
{
	QReadLocker locker(&m_lock);
	return m_values.stackCaptureMode;
}

void WatchConfig::setStackCaptureMode(int value)
{
	QWriteLocker locker(&m_lock);
	m_values.stackCaptureMode = (value >= 0 && value <= 2) ? value : DefaultStackCaptureMode;
}

QString WatchConfig::uplinkName() const
{
	QReadLocker locker(&m_lock);
	return m_values.uplinkName;
}

void WatchConfig::setUplinkName(const QString &name)
{
	QWriteLocker locker(&m_lock);
	m_values.uplinkName = name.trimmed();	// 空串 = 关闭上行链路
}

int WatchConfig::uplinkFlushMs() const
{
	QReadLocker locker(&m_lock);
	return m_values.uplinkFlushMs;
}

void WatchConfig::setUplinkFlushMs(int value)
{
	QWriteLocker locker(&m_lock);
	m_values.uplinkFlushMs = sanitizePositive(value, DefaultUplinkFlushMs);
}

void WatchConfig::reset()
{
	QWriteLocker locker(&m_lock);
	m_values = Values {};
}

int WatchConfig::sanitizeNonNegative(int value, int fallback)
{
	return value >= 0 ? value : fallback;
}

int WatchConfig::sanitizePositive(int value, int fallback)
{
	return value > 0 ? value : fallback;
}

void WatchConfig::loadValueFromEnvironment(const char *name, int &target, int fallback)
{
	const QByteArray value = qgetenv(name);
	if (value.isEmpty())	return;

	bool ok = false;
	const int parsed = QString::fromUtf8(value).toInt(&ok);
	if (!ok)	return;

	target = sanitizePositive(parsed, fallback);
}

void WatchConfig::loadValueFromEnvironment(const char *name, WatchFunMask &target, WatchFunMask fallback)
{
	const QByteArray value = qgetenv(name);
	if (value.isEmpty())	return;

	bool ok = false;
	const quint64 parsed = QString::fromUtf8(value).toULongLong(&ok);
	if (!ok)	return;

	target = static_cast<WatchFunMask>(parsed);
}

// ---- 配置热更新（PRD 11 §5 / PRD 13 §4）----

void WatchConfig::setConfigFile(const QString &filePath)
{
	QWriteLocker locker(&m_lock);
	m_configFile = filePath;
	m_lastMtimeMs = 0;	// 路径变更后强制下次轮询重算
}

QString WatchConfig::configFile() const
{
	QReadLocker locker(&m_lock);
	return m_configFile;
}

void WatchConfig::applyEnvironmentToValues(Values &next)
{
	loadValueFromEnvironment(EnvWatchFun,                  next.watchFun, DefaultWatchFun);
	loadValueFromEnvironment(EnvSlowEventThresholdMs,      next.slowEventThresholdMs, DefaultSlowEventThresholdMs);
	loadValueFromEnvironment(EnvSlowMetaCallThresholdMs,   next.slowMetaCallThresholdMs, DefaultSlowMetaCallThresholdMs);
	loadValueFromEnvironment(EnvEventStatPeriodMs,         next.eventStatPeriodMs, DefaultEventStatPeriodMs);
	loadValueFromEnvironment(EnvEventCountThreshold,       next.eventCountThreshold, DefaultEventCountThreshold);
	loadValueFromEnvironment(EnvEventTotalCostThresholdMs, next.eventTotalCostThresholdMs, DefaultEventTotalCostThresholdMs);
	loadValueFromEnvironment(EnvQssLoadThresholdMs,        next.qssLoadThresholdMs, DefaultQssLoadThresholdMs);
	loadValueFromEnvironment(EnvSetStyleSheetThresholdMs,  next.setStyleSheetThresholdMs, DefaultSetStyleSheetThresholdMs);
	loadValueFromEnvironment(EnvQssFrequentCountThreshold, next.qssFrequentCountThreshold, DefaultQssFrequentCountThreshold);
	loadValueFromEnvironment(EnvConfigPollIntervalMs,      next.configPollIntervalMs, DefaultConfigPollIntervalMs);
	loadValueFromEnvironment(EnvFreezeThresholdMs,         next.freezeThresholdMs, DefaultFreezeThresholdMs);
	loadValueFromEnvironment(EnvHeartbeatIntervalMs,       next.heartbeatIntervalMs, DefaultHeartbeatIntervalMs);
	loadValueFromEnvironment(EnvAlarmSuppressWindowMs,     next.alarmSuppressWindowMs, DefaultAlarmSuppressWindowMs);
	{
		int mode = DefaultStackCaptureMode;
		loadValueFromEnvironment(EnvStackCaptureMode, mode, DefaultStackCaptureMode);
		next.stackCaptureMode = (mode >= 0 && mode <= 2) ? mode : DefaultStackCaptureMode;
	}
	// V4 D1：上行链路（名字非空才覆盖）
	{
		const QString name = qEnvironmentVariable(EnvUplinkName);
		if (!name.isEmpty())
			next.uplinkName = name.trimmed();
		loadValueFromEnvironment(EnvUplinkFlushMs, next.uplinkFlushMs, DefaultUplinkFlushMs);
	}
}

bool WatchConfig::applyIniFileToValues(const QString &filePath, Values &next)
{
	QSettings settings(filePath, QSettings::IniFormat);
	settings.beginGroup(QString::fromLatin1(ConfigGroup));

	// 单项非法仅回退该项默认值（PRD 11 §5.4），不得影响其他项
	auto warnFallback = [](const char *key, const QVariant &raw, int fallback) {
		QEW_LOG_WARN("[WatchConfig] invalid value {}='{}' in ini, fallback to {}",
		             key, raw.toString().toStdString(), fallback);
	};
	auto readPositive = [&](const char *key, int &target, int fallback) {
		if (!settings.contains(QString::fromLatin1(key)))	return;
		const QVariant raw = settings.value(QString::fromLatin1(key));
		bool ok = false;
		const int parsed = raw.toInt(&ok);
		if (!ok) {
			warnFallback(key, raw, fallback);
			return;
		}
		const int sanitized = sanitizePositive(parsed, fallback);
		if (sanitized != parsed)	warnFallback(key, raw, sanitized);
		target = sanitized;
	};

	if (settings.contains(QString::fromLatin1(KeyWatchFun))) {
		const QVariant raw = settings.value(QString::fromLatin1(KeyWatchFun));
		bool ok = false;
		const quint32 parsed = raw.toUInt(&ok);
		if (!ok)	warnFallback(KeyWatchFun, raw, 0);
		else		next.watchFun = static_cast<WatchFunMask>(parsed);
	}

	readPositive(KeySlowEventThresholdMs,      next.slowEventThresholdMs, DefaultSlowEventThresholdMs);
	readPositive(KeySlowMetaCallThresholdMs,   next.slowMetaCallThresholdMs, DefaultSlowMetaCallThresholdMs);
	readPositive(KeyEventStatPeriodMs,         next.eventStatPeriodMs, DefaultEventStatPeriodMs);
	readPositive(KeyEventCountThreshold,       next.eventCountThreshold, DefaultEventCountThreshold);
	readPositive(KeyEventTotalCostThresholdMs, next.eventTotalCostThresholdMs, DefaultEventTotalCostThresholdMs);
	readPositive(KeyQssLoadThresholdMs,        next.qssLoadThresholdMs, DefaultQssLoadThresholdMs);
	readPositive(KeySetStyleSheetThresholdMs,  next.setStyleSheetThresholdMs, DefaultSetStyleSheetThresholdMs);
	readPositive(KeyQssFrequentCountThreshold, next.qssFrequentCountThreshold, DefaultQssFrequentCountThreshold);
	readPositive(KeyConfigPollIntervalMs,      next.configPollIntervalMs, DefaultConfigPollIntervalMs);
	readPositive(KeyFreezeThresholdMs,         next.freezeThresholdMs, DefaultFreezeThresholdMs);
	readPositive(KeyHeartbeatIntervalMs,       next.heartbeatIntervalMs, DefaultHeartbeatIntervalMs);
	readPositive(KeyAlarmSuppressWindowMs,     next.alarmSuppressWindowMs, DefaultAlarmSuppressWindowMs);
	if (settings.contains(QString::fromLatin1(KeyStackCaptureMode))) {
		const int value = settings.value(QString::fromLatin1(KeyStackCaptureMode), next.stackCaptureMode).toInt();
		next.stackCaptureMode = (value >= 0 && value <= 2) ? value : DefaultStackCaptureMode;
	}
	// V4 D1：上行链路
	if (settings.contains(QString::fromLatin1(KeyUplinkName))) {
		next.uplinkName = settings.value(QString::fromLatin1(KeyUplinkName)).toString().trimmed();
	}
	if (settings.contains(QString::fromLatin1(KeyUplinkFlushMs))) {
		const int value = settings.value(QString::fromLatin1(KeyUplinkFlushMs), next.uplinkFlushMs).toInt();
		next.uplinkFlushMs = sanitizePositive(value, DefaultUplinkFlushMs);
	}

	settings.endGroup();
	return settings.status() == QSettings::NoError;
}

bool WatchConfig::reloadIfChanged()
{
	QString iniPath;
	qint64 lastMtime = 0;
	{
		QReadLocker locker(&m_lock);
		iniPath = m_configFile;
		lastMtime = m_lastMtimeMs;
	}
	if (iniPath.isEmpty())	return false;

	QFileInfo fi(iniPath);
	const qint64 mtime = fi.exists() && fi.isFile() ? fi.lastModified().toMSecsSinceEpoch() : 0;
	if (mtime == lastMtime)	return false;	// 快路径：无变化零成本

	// 重算 Default → Environment → INI，成功后一次性整体替换（PRD 11 §5.2/§5.3）
	Values next;
	applyEnvironmentToValues(next);
	const bool fileLoaded = fi.isFile() ? applyIniFileToValues(iniPath, next) : true;

	{
		QWriteLocker locker(&m_lock);
		m_values = next;
		m_lastMtimeMs = mtime;
	}

	QEW_LOG_INFO(
	    "[WatchConfig] configuration reloaded file={} watchFun={:#x} slowEvent={}ms "
	    "slowMetaCall={}ms statPeriod={}ms poll={}ms",
	    iniPath.toStdString(), next.watchFun, next.slowEventThresholdMs,
	    next.slowMetaCallThresholdMs, next.eventStatPeriodMs, next.configPollIntervalMs);
	return true;
}

} // namespace qt_event_watcher
