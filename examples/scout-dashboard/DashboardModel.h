#pragma once

/// scout-dashboard 数据层（V7 S3 线）
///
/// 读 aggregator --out 导出的 JSON 快照文件，解析/归类/配对为仪表盘内存模型。
/// 数据契约 = aggregator JSON 文件形状（examples/aggregator/main.cpp 导出端：
/// {generatedAt, server, sessions:[{host_pid, received, dropped, health?, records[]}]}，
/// record={kind, seq, time, raw, fields:[[k,v]..], frames?}）。
/// 纯文件消费方：不依赖 QtEventWatcherCore，仅 Qt Core 公共 API（四矩阵零适配点
/// ——QJsonDocument/QJsonArray/QTime 在 Qt5.15/6.5 API 一致）。

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cstdint>

namespace qt_event_watcher {
namespace dashboard {

/// 记录归类（kind 枚举 + scout 事件细分，判定规则见 classifyRecord）
enum class EventClass
{
	Freeze = 0,      ///< kind==3：冻结三态/lost（自监控 EventWatchdog 与 scout T1 同形）
	CdpLongTask = 1, ///< scout T2：type=98 / event=cdpLongTask / source=scout-cdp
	CpuSpin = 2,     ///< scout T1b：type=99 / event=cpuSpin / source=scout
	SlowEvent = 3,   ///< 其余 kind==0（自监控普通慢事件）
	MetaCall = 4,    ///< kind==1
	Qss = 5,         ///< kind==2
	Other = 6        ///< 协议外 kind（防御，渲染跳过）
};

/// 归一化后的单条事件（fields=[[k,v]..] 线性查值后打平）
struct EventRecord
{
	int kind = 0;
	EventClass cls = EventClass::Other;
	QString time;          ///< 原文 "HH:mm:ss.zzz"（无日期，aggregator 导出端口径）
	qint64 relMs = 0;      ///< 相对会话 t0 的毫秒（跨午夜防御 +86400000；时间不可解析=0）
	QString state;         ///< 冻结行专属：started/ongoing/recovered/lost（按时长字段签名推导）
	QString event;         ///< fields.event（普通慢事件 = QEvent 名；空 = 无）
	QString source;        ///< fields.source（"scout"/"scout-cdp"/空=自监控）
	QString url;           ///< fields.url（仅 CDP）
	QString receiver;
	double costMs = 0.0;   ///< cpuSpin 行 = CPU% 口径（CpuSampler 单核满转启发）
	double thresholdMs = 0.0;
	double totalMs = 0.0;  ///< freeze recovered 行的冻结总时长（span 闭合值）
	QString raw;           ///< 原文兜底（parseFields 值含空格截断场景展示用）
};

/// freeze started→recovered/lost 配对成的时间段
struct FreezeSpan
{
	QString receiver;
	QString startTime;         ///< started 行原文时间
	qint64 startRelMs = 0;
	qint64 durationMs = -1;    ///< -1 = 未闭合（ongoing 截尾 / lost）
	QString endState;          ///< "recovered" | "lost" | "ongoing"
};

/// 单会话模型（对应 JSON 一个 sessions[] 元素）
struct SessionModel
{
	qint64 pid = 0;
	quint64 received = 0;
	quint64 dropped = 0;
	QJsonObject health;            ///< 原样透传给渲染层
	bool hasHealth = false;
	bool isScoutSession = false;   ///< 会话内任一记录 source=scout* → true（来源徽章）
	qint64 t0Ms = 0;               ///< 会话内最早记录的 msecsSinceStartOfDay
	QVector<EventRecord> events;   ///< JSON 顺序（seq 升序），不重排
	QVector<FreezeSpan> freezeSpans;
};

/// 全量仪表盘模型
struct DashboardModel
{
	QString generatedAt;
	QString server;
	QVector<SessionModel> sessions;
	int kindCounts[4] = { 0, 0, 0, 0 };   ///< kind 0..3 计数
	int cdpCount = 0;
	int cpuCount = 0;
	int totalRecords = 0;
};

/// 读 JSON 文件 → 模型。文件不可读/顶层解析失败返回 false 带 error；
/// record 级损坏静默跳过（对齐 aggregator 消费端协议容错口径）。
bool loadModel(const QString &jsonPath, DashboardModel &out, QString *error = nullptr);

/// 过滤视图（渲染/摘要/内嵌 JSON 共用的数据面口径）：kinds 空 = 不过滤
/// （kind 0..3 白名单），pidFilter <= 0 = 全部会话。freezeSpans 仅当
/// kinds 含 3（或全量）保留；计数按过滤后重算。原模型不变（值拷贝语义）。
DashboardModel filterModel(const DashboardModel &in, const QVector<int> &kinds,
						   qint64 pidFilter);

/// fields=[[k,v]..] 线性查值（缺失返回空串；对齐 AggregationReporter 同名工具）
QString recordField(const QJsonObject &rec, const QString &key);

/// 记录归类。优先级：kind==3 冻结 → T2 CDP → T1b CPU → kind 0/1/2 → Other。
/// type/event/source 三信号冗余判定任一命中：type 是数字签名，event/source
/// 是语义签名，互为容错。注意：自监控普通慢事件行也带 event 字段（QEvent
/// 枚举名），必须 event=="cpuSpin" 精确匹配，不能以"存在 event 字段"判定 scout。
EventClass classifyRecord(int kind, const QString &eventField,
						  const QString &sourceField, const QString &typeField);

/// HTML 转义（& < > " 四字符，对齐 AggregationReporter/HtmlReporter 同规则）
QString escapeHtml(const QString &s);

/// JSON 安全内嵌：Compact 序列化后 "</" → "<\/"（封死 </script> 提前闭合——
/// script 元素是 raw text，这是内嵌 JSON 唯一的注入面；\/ 为 JSON 合法转义，
/// QJsonDocument::fromJson 回读语义不变）
QString embedJsonSafe(const QJsonDocument &doc);

} // namespace dashboard
} // namespace qt_event_watcher
