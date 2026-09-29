#pragma once

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <deque>
#include <map>

/**
 * 聚合会话数据模型（V4 D2 定形，V5.1 从 main.cpp 移出共用）：
 * records 为 QJsonObject 记录（kind/seq/time/raw + 消费侧重建的
 * fields=[[k,v]..] / frames=[[mod,off]..]，见 main.cpp rebuildStructured）。
 */
struct Session
{
	std::deque<QJsonObject> records;
	std::uint64_t dropped = 0;		///< 环形覆盖丢弃（对端 skipped + 本端溢出）
	std::uint64_t received = 0;
	// V5 B：op=health 随拍上行的最新健康度快照（不入 records 计数）
	QJsonObject health;
	bool hasHealth = false;
};

/**
 * @brief 多进程聚合 HTML 报告（V5.1 C 线，docs/29）
 *
 * aggregator 局部渲染模块：直接消费 Session 数据模型，与 HtmlReporter
 * （单进程，绑定 WatchConfig/EventStatistics/WatchRecordStore 单例）分工互补。
 * 跨进程无诊断四规则与周期统计语义（docs/29 §3 "明确不做"），以进程健康度表
 * 与 TOP 接收者替代。
 *
 * 自包含静态 HTML（纯 HTML+CSS 零 JS 零依赖，escapeHtml 防注入），章节：
 * - 概览：sessions / received / dropped 合计 + 跨进程分 kind 计数
 * - 进程健康度：每 pid 的 records/received/dropped + V5 B health 快照
 * - TOP 接收者：跨进程合并 SlowEvent 按累计 costMs TOP-10
 * - 冻结时间线：Freeze 记录三态表（新增 pid 列）
 * - 明细记录：跨进程合并最近 200 条（<details> 折叠）
 *
 * 仅在 --out/--html 显式导出时执行，纯冷路径（PRD 14 哲学）。
 */
class AggregationReporter
{
public:
	static bool exportHtml(const QString& filePath,
						   const std::map<qint64, Session>& sessions,
						   const QString& serverName,
						   QString* error = nullptr);

	AggregationReporter() = delete;
};
