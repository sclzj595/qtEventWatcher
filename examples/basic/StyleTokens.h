#pragma once

#include <QString>

/*
 * StyleTokens —— Demo 界面设计令牌（单一来源，PRD 19 §5）
 *
 * Light / Dark 双主题：ThemeTokens 承载分层亮度（Background → Surface →
 * Elevated Surface），语义色（Success/Warning/Error/Info）。
 * buildAppStyle(theme, accent) 组装为 QSS，经 QssStyleWatcher::setStyleSheet
 * 应用（同时被 Bit3 监控链路观测）。
 * ensureStyleFile(filePath, theme) 将当前令牌序列化为外部 style.qss。
 *
 * 视觉禁令（PRD 19 §4）：无纯黑工业风、无满屏边框、告警色仅少量强调。
 * 间距体系 4/8/12/16/24/32；圆角 Card 10 / Button 8 / Badge 6。
 */
namespace style_tokens
{

// ---- 主题令牌组 ----
struct ThemeTokens
{
	QString name;		///< "dark" / "light"
	QString bg;			///< Background（窗口底 / 侧栏）
	QString surface;	///< Surface（卡片 / 顶栏 / 表格）
	QString surfaceHi;	///< Elevated Surface（hover）
	QString border;
	QString text;
	QString textDim;
	QString ok;			///< Success（运行中 / 正常）
	QString warn;		///< Warning（Amber，注意）
	QString error;		///< Error（异常）
	QString info;		///< Info（蓝，信息）
};

/// 预置主题（PRD 19 §9：Dark 分层亮度，Light 浅灰底 + 白 Surface）
const ThemeTokens& themeDark();
const ThemeTokens& themeByName(const QString& name);	///< 未知名回退 dark
const ThemeTokens& themeLight();

/// 跟随系统亮度判定（QPalette window lightness）
bool systemPrefersDark();

// ---- accent presets（强调色，语义色之外的少量点缀）----
inline constexpr const char* AccentBlue  = "#4c8dff";
inline constexpr const char* AccentTeal  = "#2dd4bf";
inline constexpr const char* AccentAmber = "#f5a623";

// ---- shape / spacing / typography（PRD 19 §5.2~5.4）----
inline constexpr int RadiusCard  = 10;
inline constexpr int RadiusCtrl  = 8;
inline constexpr int RadiusBadge = 6;
inline constexpr int PadSm       = 8;
inline constexpr int PadMd       = 16;
inline constexpr int PadLg       = 24;
inline constexpr int FontBody    = 13;
inline constexpr int FontTitle   = 17;
inline constexpr int FontKpi     = 22;
inline constexpr int HeaderH     = 56;
inline constexpr int SideNavW    = 156;
inline constexpr int KpiCardH    = 92;

inline constexpr const char* FontFamily = "\"Segoe UI\", \"Microsoft YaHei UI\"";
inline constexpr const char* FontMono   = "\"Consolas\", \"Courier New\"";

/// 用指定主题 + accent 组装整窗 QSS
QString buildAppStyle(const QString& theme, const QString& accent);

/// 外部样式文件不存在时按当前主题令牌生成（启动加载路径的数据源）
bool ensureStyleFile(const QString& filePath, const QString& theme);

} // namespace style_tokens
