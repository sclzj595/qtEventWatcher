#pragma once

#include <QHash>
#include <QString>

class QElapsedTimer;
class QWidget;

namespace qt_event_watcher
{

class WatchConfig;

/**
 * @brief QSS 资源监控器（Bit3，PRD 08/09/10）
 *
 * 职责：
 * - 外部 QSS 加载埋点：beginLoadQss / endLoadQss（PRD 08）
 *   IO 与样式处理耗时拆分为代理指标：样式部分取加载窗口内
 *   setStyleSheet 包装调用的实测耗时，IO ≈ Total - Style
 *   （Qt 内部样式引擎不可拆解，PRD 08 §4 已声明代理口径）
 * - setStyleSheet 包装接口：统计调用次数 / 耗时 / 文本长度（PRD 09）
 * - 周期聚合：高频 Widget TOP N + 资源累计（PRD 10）
 *
 * 约束：
 * - 由 CusApplication 构造时 setup(WatchConfig*) 注入配置
 * - 仅 GUI 线程调用（notify 链路与业务样式代码），不加锁
 * - Bit3 关闭时 setStyleSheet 直通 QWidget::setStyleSheet，零统计成本
 * - 未 setup 时所有接口安全空操作
 */
class QssStyleWatcher
{
public:
	static QssStyleWatcher* instance();

	QssStyleWatcher(const QssStyleWatcher&) = delete;
	QssStyleWatcher& operator=(const QssStyleWatcher&) = delete;

	/// 注入配置（CusApplication 构造时调用一次）
	void setup(WatchConfig* config);

	/// Func
	void beginLoadQss(const QString& filePath);
	void endLoadQss();

	bool setStyleSheet(QWidget* widget, const QString& style);

	/// 立即输出周期统计并重置（调试/测试用；正常由周期边界自动触发）
	void flushStatistics();

private:
	QssStyleWatcher();
	~QssStyleWatcher();

	struct WidgetStat
	{
		QString className;
		QString objectName;
		int count = 0;
		double maxCostMs = 0.0;
		int maxStyleLen = 0;
	};

	bool isEnabled() const;
	void maybeFlushPeriod();

	WatchConfig* m_config = nullptr;

	// 加载窗口状态（PRD 08：begin → 业务读取+setStyleSheet → end）
	bool m_loadActive = false;
	QString m_loadFilePath;
	qint64 m_loadFileSizeBytes = 0;
	QElapsedTimer* m_loadTimer = nullptr;	///< 堆分配避免头暴露 QElapsedTimer
	double m_styleCostInLoadMs = 0.0;		///< 加载窗口内包装调用的样式耗时累计

	// 周期聚合（PRD 10 §1/§2）
	QHash<QString, WidgetStat> m_periodStats;
	QElapsedTimer* m_periodTimer = nullptr;

	// 资源累计（PRD 10 §3）
	int m_totalLoadCount = 0;
	int m_totalFileCount = 0;
	qint64 m_maxFileSizeBytes = 0;
	double m_maxIoCostMs = 0.0;
	double m_maxApplyCostMs = 0.0;
};

} // namespace qt_event_watcher
