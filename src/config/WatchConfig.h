#pragma once

#include <cstdint>

#include <QReadWriteLock>
#include <QString>

namespace qt_event_watcher
{
	
/**
 * @brief 全局监控配置
 * 
 * 1. 程序运行时 setter
 * 2. 配置文件
 * 3. 环境变量
 * 4. 编译期默认值
 * 
 * 日志经 QEW_LOG_* 空安全宏输出（Logger 未初始化时自动静默，无硬依赖）
 */
class WatchConfig
{

public:
	using WatchFunMask = std::uint32_t;

	/**
	 * @brief 监控func
	 * Watch_Fun 使用 bit mask 表示不同监控功能  位掩码要对应清楚 注释清楚
	 * 位定义对齐 PRD 11：
	 * Watch_Fun=1 慢事件 / =2 MetaCall / =4 周期统计 / =8 QSS / =15 全部
	 */
	enum WatchFunction : WatchFunMask {
		WatchNone				= 0,
		WatchEvent				= 1u << 0,	// WatchFun_OneEveDeal 单事件监控
		WatchMetaCall			= 1u << 1,	// WatchFun_MetaCall
		WatchEventStatistics	= 1u << 2,	// WatchFun_EventStat 周期统计
		WatchQssMonitor			= 1u << 3,	// WatchFun_QssMonitor QSS 加载/样式/高频刷新
		WatchFreeze				= 1u << 4,	// WatchFun_Freeze UI 冻结看门狗（V3 B 线）

		WatchAll =
			WatchEvent          |
			WatchMetaCall       |
			WatchEventStatistics |
			WatchQssMonitor     |
			WatchFreeze
	};

	// default conf
	static constexpr WatchFunMask DefaultWatchFun = 0;
    static constexpr int DefaultSlowEventThresholdMs = 30;
    static constexpr int DefaultSlowMetaCallThresholdMs = 30;
    static constexpr int DefaultEventStatPeriodMs = 1000;
    static constexpr int DefaultEventCountThreshold = 100;
    static constexpr int DefaultEventTotalCostThresholdMs = 100;
    static constexpr int DefaultQssLoadThresholdMs = 50;
    static constexpr int DefaultSetStyleSheetThresholdMs = 30;
    static constexpr int DefaultQssFrequentCountThreshold = 10;
    static constexpr int DefaultConfigPollIntervalMs = 60000;
    static constexpr int DefaultFreezeThresholdMs = 2000;      ///< 心跳停滞超过该值判定冻结（V3 B 线）
    static constexpr int DefaultHeartbeatIntervalMs = 250;     ///< 主线程心跳周期
    static constexpr int DefaultAlarmSuppressWindowMs = 1000;  ///< 告警风暴抑制窗口（V4 B1，原 V3 A2 编译期常量）
    /// 调用栈采集时机（V4 B2）：0=阈值命中即采（默认，记录全量含 frames）
    /// 1=仅窗口首条采（被抑制条免采，其记录无 frames 字段——记录完整性换风暴期 CPU）
    /// 2=关闭栈采集（所有告警记录无 frames）
    static constexpr int DefaultStackCaptureMode = 0;
    /// 上行链路（V4 D1 多进程聚合）：服务名非空即启用（连接 aggregator 的
    /// QLocalServer），空 = 关闭；flush 周期毫秒（差量拉取 WatchRecordStore）
    static constexpr int DefaultUplinkFlushMs = 200;

public:
	WatchConfig();

	WatchConfig(const WatchConfig&) = delete;
	WatchConfig& operator=(const WatchConfig&) = delete;

	~WatchConfig() = default;

	// load 
	/**
	 * @brief conf文件中去加载
	 * 不存在不报错 走默认值  兜底
	 * fmt
	 * [QtEventWatcher]
     * Watch_Fun=31
     * SlowEventThresholdMs=30
	 */
	bool loadFromFile(const QString& filePath);

	/**
	 * @brief 从环境变量加载配置
	 * 
	 * name
	 * QT_EVENT_WATCHER_WATCH_FUN
     * QT_EVENT_WATCHER_SLOW_EVENT_THRESHOLD_MS
     * QT_EVENT_WATCHER_SLOW_METACALL_THRESHOLD_MS
	 */
	void loadFromEnvironment();

    /**
     * @brief 按照默认优先级初始化配置。
     * 默认：
     * default
     *   ↓
     * environment
     *   ↓
     * config file
     * runtime setter 应在 load() 完成后调用。
     */
    bool load(const QString& filePath = QString());

    // ---- 配置热更新（PRD 11 §5 / PRD 13 §4）----
    /// INI 是唯一热更新源；环境变量仅在 load()/reload 重算时作为输入（进程环境运行期不变）
    void setConfigFile(const QString& filePath);
    QString configFile() const;

    /**
     * @brief 热更新轮询入口：配置文件 mtime 变化 → 重算 Default→Environment→INI → 原子整体替换
     *
     * V1 语义（PRD 11 §5.2）：reload 覆盖运行时 setter（无 Override 层）；
     * 单项非法仅回退该项默认值，不影响其他项（PRD 11 §5.4）。
     * @return true 本次调用完成了一次重载
     */
    bool reloadIfChanged();

public:
	/// Watch Fun 
	WatchFunMask watchFun() const;
    void setWatchFun(WatchFunMask value);

    bool isWatchEnabled(WatchFunction function) const;
    void setWatchEnabled(WatchFunction function, bool enabled);

    void enableWatch(WatchFunction function);
    void disableWatch(WatchFunction function);

	/// Thresholds 
	int slowEventThresholdMs() const;
    void setSlowEventThresholdMs(int value);

    int slowMetaCallThresholdMs() const;
    void setSlowMetaCallThresholdMs(int value);

    int eventStatPeriodMs() const;
    void setEventStatPeriodMs(int value);

    int eventCountThreshold() const;
    void setEventCountThreshold(int value);

    int eventTotalCostThresholdMs() const;
    void setEventTotalCostThresholdMs(int value);

    int qssLoadThresholdMs() const;
    void setQssLoadThresholdMs(int value);

    int setStyleSheetThresholdMs() const;
    void setStyleSheetThresholdMs(int value);

    int qssFrequentCountThreshold() const;
    void setQssFrequentCountThreshold(int value);

    int configPollIntervalMs() const;
    void setConfigPollIntervalMs(int value);

    int freezeThresholdMs() const;
    void setFreezeThresholdMs(int value);

    int heartbeatIntervalMs() const;
    void setHeartbeatIntervalMs(int value);

    int alarmSuppressWindowMs() const;
    void setAlarmSuppressWindowMs(int value);

    int stackCaptureMode() const;
    void setStackCaptureMode(int value);

    QString uplinkName() const;
    void setUplinkName(const QString &name);
    int uplinkFlushMs() const;
    void setUplinkFlushMs(int value);

	/// Reset 
	void reset();	// 编译期默认值

private:
	struct Values {
		WatchFunMask watchFun = DefaultWatchFun;
        int slowEventThresholdMs = DefaultSlowEventThresholdMs;
        int slowMetaCallThresholdMs = DefaultSlowMetaCallThresholdMs;
        int eventStatPeriodMs = DefaultEventStatPeriodMs;
        int eventCountThreshold = DefaultEventCountThreshold;
        int eventTotalCostThresholdMs = DefaultEventTotalCostThresholdMs;
        int qssLoadThresholdMs = DefaultQssLoadThresholdMs;
        int setStyleSheetThresholdMs = DefaultSetStyleSheetThresholdMs;
        int qssFrequentCountThreshold = DefaultQssFrequentCountThreshold;
        int configPollIntervalMs = DefaultConfigPollIntervalMs;
        int freezeThresholdMs = DefaultFreezeThresholdMs;
        int heartbeatIntervalMs = DefaultHeartbeatIntervalMs;
        int alarmSuppressWindowMs = DefaultAlarmSuppressWindowMs;
        int stackCaptureMode = DefaultStackCaptureMode;
        QString uplinkName;                                 ///< 空 = 关闭上行链路（V4 D1）
        int uplinkFlushMs = DefaultUplinkFlushMs;
	};

private:
	static int sanitizeNonNegative(int value, int fallback);
    static int sanitizePositive(int value, int fallback);

    static void applyEnvironmentToValues(Values& next);
    static bool applyIniFileToValues(const QString& filePath, Values& next);

    static void loadValueFromEnvironment(const char* name, int& target, int fallback);
    static void loadValueFromEnvironment(const char* name, WatchFunMask& target, WatchFunMask fallback);

private:
    mutable QReadWriteLock m_lock;
    Values m_values;
    QString m_configFile;       ///< 热更新轮询目标（PRD 11 §5.1）
    qint64 m_lastMtimeMs = 0;   ///< 上次加载/重载时的文件 mtime，0=文件不存在

};


} // namespace qt_event_watcher
