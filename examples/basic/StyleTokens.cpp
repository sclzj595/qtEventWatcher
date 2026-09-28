#include "StyleTokens.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QTextStream>

namespace style_tokens
{

const ThemeTokens& themeDark()
{
	// Zinc 系分层亮度（PRD 19 §4：禁止纯黑+荧光色工业风）
	static const ThemeTokens t = []() {
		ThemeTokens v;
		v.name = QStringLiteral("dark");
		v.bg = QStringLiteral("#0e1116");
		v.surface = QStringLiteral("#151a21");
		v.surfaceHi = QStringLiteral("#1d242d");
		v.border = QStringLiteral("#2a313b");
		v.text = QStringLiteral("#e6edf3");
		v.textDim = QStringLiteral("#8b949e");
		v.ok = QStringLiteral("#3fb950");
		v.warn = QStringLiteral("#d29922");
		v.error = QStringLiteral("#f85149");
		v.info = QStringLiteral("#4c8dff");
		return v;
	}();
	return t;
}

const ThemeTokens& themeLight()
{
	// 浅灰背景 + 白 Surface（PRD 19 §9：不用纯白+纯黑刺眼组合）
	static const ThemeTokens t = []() {
		ThemeTokens v;
		v.name = QStringLiteral("light");
		v.bg = QStringLiteral("#f2f3f5");
		v.surface = QStringLiteral("#ffffff");
		v.surfaceHi = QStringLiteral("#e9ecef");
		v.border = QStringLiteral("#d8dce2");
		v.text = QStringLiteral("#1f2328");
		v.textDim = QStringLiteral("#6e7781");
		v.ok = QStringLiteral("#1a7f37");
		v.warn = QStringLiteral("#9a6700");
		v.error = QStringLiteral("#cf222e");
		v.info = QStringLiteral("#0969da");
		return v;
	}();
	return t;
}

const ThemeTokens& themeByName(const QString& name)
{
	return name.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0
			   ? themeLight() : themeDark();
}

bool systemPrefersDark()
{
	const int lightness = QGuiApplication::palette().color(QPalette::Window).lightness();
	return lightness < 128;
}

QString buildAppStyle(const QString& theme, const QString& accent)
{
	const ThemeTokens& T = themeByName(theme);
	QString q;

	// ---- 基础 ----
	q += QStringLiteral(
	    "QWidget {"
	    " background:%1; color:%2; font-size:%3px; font-family:%4; }"
	    "QMainWindow, #CentralWidget { background:%1; }")
	    .arg(T.bg, T.text)
	    .arg(FontBody)
	    .arg(FontFamily);

	// ---- 顶栏（chrome bar）----
	q += QStringLiteral(
	    "#HeaderBar { background:%1; border-bottom:1px solid %2; }"
	    "QLabel#HeaderTitle { font-size:%3px; font-weight:600; }"
	    "QLabel#SubLabel { color:%4; }")
	    .arg(T.surface, T.border)
	    .arg(FontTitle)
	    .arg(T.textDim);

	// ---- 左侧导航 ----
	q += QStringLiteral(
	    "#SideNav { background:%1; border-right:1px solid %2; }"
	    "QPushButton[nav=\"true\"] { background:transparent; border:none;"
	    " border-radius:%3px; padding:%4px %5px; color:%6; text-align:left; }"
	    "QPushButton[nav=\"true\"]:hover { background:%7; color:%8; }"
	    "QPushButton[nav=\"true\"]:checked { background:%7; color:%9; font-weight:600; }")
	    .arg(T.bg, T.border)
	    .arg(RadiusCtrl)
	    .arg(PadSm)
	    .arg(PadMd)
	    .arg(T.textDim, T.surfaceHi, T.text)
	    .arg(accent);

	// ---- KPI 卡片 ----
	q += QStringLiteral(
	    "#KpiCard { background:%1; border:1px solid %2;"
	    " border-left:3px solid %3; border-radius:%4px; }"
	    "QLabel#KpiValue { font-size:%5px; font-weight:600; color:%6; }"
	    "QLabel#KpiTitle { color:%7; font-size:12px; }")
	    .arg(T.surface, T.border)
	    .arg(accent)
	    .arg(RadiusCard)
	    .arg(FontKpi)
	    .arg(T.text)
	    .arg(T.textDim);

	// ---- 卡片 / 分组框 ----
	q += QStringLiteral(
	    "#CardPanel { background:%1; border:1px solid %2; border-radius:%3px; }"
	    "QGroupBox { background:%1; border:1px solid %2; border-radius:%3px;"
	    " margin-top:14px; padding:%4px; font-weight:600; }"
	    "QGroupBox::title { subcontrol-origin: margin; left:12px; padding:0 4px; color:%5; }")
	    .arg(T.surface, T.border)
	    .arg(RadiusCard)
	    .arg(PadMd)
	    .arg(T.textDim);

	// ---- 按钮 ----
	q += QStringLiteral(
	    "QPushButton { background:%1; border:1px solid %2; border-radius:%3px;"
	    " padding:%4px %5px; color:%6; }"
	    "QPushButton:hover { background:%7; border-color:%8; }"
	    "QPushButton:pressed { background:%2; }"
	    "QPushButton:disabled { color:%9; border-color:%2; }"
	    "QPushButton[accent=\"true\"] { background:%10; border:none; color:#ffffff; font-weight:600; }"
	    "QPushButton[accent=\"true\"]:hover { background:%10; }"
	    "QPushButton[accent=\"true\"]:pressed { background:%10; }"
	    "QPushButton[danger=\"true\"] { color:%11; border-color:%11; }")
	    .arg(T.surface, T.border)
	    .arg(RadiusCtrl)
	    .arg(PadSm)
	    .arg(PadMd)
	    .arg(T.text, T.surfaceHi, T.border, T.textDim)
	    .arg(accent)
	    .arg(T.error);

	// ---- 告警流列表（等宽字体，日志质感）----
	q += QStringLiteral(
	    "QListWidget { background:%1; border:1px solid %2; border-radius:%3px;"
	    " padding:%4px; font-family:%5; font-size:12px; }"
	    "QListWidget::item { color:%6; padding:4px 6px; border-radius:4px; }"
	    "QListWidget::item:hover { background:%7; }"
	    "QListWidget::item:selected { background:%7; color:%6; }")
	    .arg(T.surface, T.border)
	    .arg(RadiusCard)
	    .arg(PadSm)
	    .arg(FontMono)
	    .arg(T.text, T.surfaceHi);

	// ---- 明细表（密集行、少边框，PRD 19 §2/§7）----
	q += QStringLiteral(
	    "QTableWidget { background:%1; border:1px solid %2; border-radius:%3px;"
	    " gridline-color:%2; selection-background-color:%4; selection-color:%5; }"
	    "QTableWidget::item { padding:2px 6px; border:none; }"
	    "QHeaderView::section { background:%1; color:%6; border:none;"
	    " border-bottom:1px solid %2; padding:4px 6px; font-weight:600; }"
	    "QTableCornerButton::section { background:%1; border:none; }")
	    .arg(T.surface, T.border)
	    .arg(RadiusCard)
	    .arg(T.surfaceHi, T.text)
	    .arg(T.textDim);

	// ---- 等宽文本区（运行环境报告）----
	q += QStringLiteral(
	    "QPlainTextEdit { background:%1; border:1px solid %2; border-radius:%3px;"
	    " padding:%4px; font-family:%5; font-size:12px; color:%6; }")
	    .arg(T.surface, T.border)
	    .arg(RadiusCard)
	    .arg(PadSm)
	    .arg(FontMono)
	    .arg(T.text);

	// ---- 状态标签 / 状态栏 / 空态 ----
	q += QStringLiteral(
	    "QLabel#StatusLabel { color:%1; padding:%2px; border-radius:%3px;"
	    " background:%4; border:1px solid %5; }"
	    "QLabel#EmptyState { color:%6; padding:%7px; }"
	    "QLabel#PageHint { color:%6; }"
	    "QStatusBar { background:%4; color:%1; border-top:1px solid %5; }")
	    .arg(T.textDim)
	    .arg(PadSm)
	    .arg(RadiusCtrl)
	    .arg(T.surface, T.border)
	    .arg(T.textDim)
	    .arg(PadSm);

	// ---- DEMO / TEST 徽标（测试场景视觉隔离，PRD 18 §5）----
	q += QStringLiteral(
	    "QLabel#DemoBadge { background:%1; color:%2; border-radius:%3px;"
	    " padding:2px 10px; font-weight:700; font-size:12px; }")
	    .arg(T.warn)
	    .arg(T.bg)
	    .arg(RadiusBadge);

	// ---- 细滚动条 ----
	q += QStringLiteral(
	    "QScrollBar:vertical { background:transparent; width:8px; margin:0; }"
	    "QScrollBar::handle:vertical { background:%1; border-radius:4px; min-height:24px; }"
	    "QScrollBar::handle:vertical:hover { background:%2; }"
	    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
	    "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }"
	    "QScrollBar:horizontal { background:transparent; height:8px; margin:0; }"
	    "QScrollBar::handle:horizontal { background:%1; border-radius:4px; min-width:24px; }"
	    "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }"
	    "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background:transparent; }")
	    .arg(T.border, T.textDim);

	return q;
}

bool ensureStyleFile(const QString& filePath, const QString& theme)
{
	QFile file(filePath);
	if (file.exists())
		return true;

	QDir().mkpath(QFileInfo(filePath).absolutePath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return false;

	QTextStream out(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	out.setEncoding(QStringConverter::Utf8);
#else
	out.setCodec("UTF-8");
#endif
	// 外部文件与运行时令牌同源：默认 Blue accent
	out << "/* QtEventWatcher demo style - generated from StyleTokens */\n"
	    << buildAppStyle(theme, QString::fromLatin1(AccentBlue));
	return true;
}

} // namespace style_tokens
