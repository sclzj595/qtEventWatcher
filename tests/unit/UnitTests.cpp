/*
 * UnitTests —— V6 Q1 纯逻辑单元测试
 *
 * 覆盖面（docs/31 §2）：与 UI/事件循环无耦合的纯函数与决策器——
 *   A. WatchRecordStore::parseFields / parseFrames / hydrateRecord（解析族）
 *   B. AlarmSuppressor 三参 evaluate 合成时钟（状态机全路径）
 *   C. kindOf 门控（行为级：makeSink + spdlog logger → snapshot 断言）
 *   D. 环形 4096 覆盖 + snapshotSince 差量游标语义
 *   E. WatchConfig 默认值 / env 解析（纯十进制、0x 拒绝、非法回退）/ INI
 *
 * 风格：自写 QEWT 断言（不引入 QtTest）；QCoreApplication 仅为例行
 * （QSettings INI 需要），不进事件循环。
 */

#include "QEWT.h"

#include "AlarmSuppressor.h"
#include "WatchConfig.h"
#include "WatchRecordStore.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>

#include <string>
#include <vector>

using namespace qt_event_watcher;

namespace {

using Fields = std::vector<WatchRecordStore::Field>;
using Frames = std::vector<WatchRecordStore::Frame>;

const std::string *findField(const Fields &fields, const std::string &key)
{
	for (const auto &f : fields)
		if (f.first == key)
			return &f.second;
	return nullptr;
}

// ============ A. 解析族 ============

void testParseFieldsEmpty()
{
	Fields out;
	WatchRecordStore::parseFields("", out);
	QEWT_CHECK(out.empty());
}

void testParseFieldsBasic()
{
	Fields out;
	WatchRecordStore::parseFields("a=1 b=2", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{2});
	QEWT_CHECK(out[0].first == "a" && out[0].second == "1");
	QEWT_CHECK(out[1].first == "b" && out[1].second == "2");
}

void testParseFieldsMultiSpace()
{
	Fields out;
	WatchRecordStore::parseFields("  a=1   b=2  ", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{2});
	QEWT_CHECK(out[0].first == "a");
	QEWT_CHECK(out[1].first == "b");
}

void testParseFieldsNoEqTokenSkipped()
{
	// 无 '=' 的 token（foo / with / space）跳过，不产出半截字段
	Fields out;
	WatchRecordStore::parseFields("foo bar=2", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{1});
	QEWT_CHECK(out[0].first == "bar" && out[0].second == "2");

	Fields out2;
	WatchRecordStore::parseFields("key=v x next=2", out2);
	QEWT_CHECK_EQ(out2.size(), std::size_t{2});
	QEWT_CHECK(out2[0].first == "key" && out2[0].second == "v");	// 值含空格截断
	QEWT_CHECK(out2[1].first == "next" && out2[1].second == "2");
}

void testParseFieldsEmptyValueKept()
{
	// "a=" → 空值字段保留（eq==start 的 "=5" 才跳过）
	Fields out;
	WatchRecordStore::parseFields("a=", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{1});
	QEWT_CHECK(out[0].first == "a" && out[0].second.empty());
}

void testParseFieldsEqStartSkipped()
{
	Fields out;
	WatchRecordStore::parseFields("=5 a=1", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{1});
	QEWT_CHECK(out[0].first == "a");
}

void testParseFramesEmpty()
{
	Frames out;
	WatchRecordStore::parseFrames("", out);
	QEWT_CHECK(out.empty());
}

void testParseFramesSingle()
{
	Frames out;
	WatchRecordStore::parseFrames("QtEventWatcherCore!0x1a2b", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{1});
	QEWT_CHECK(out[0].module == "QtEventWatcherCore");
	QEWT_CHECK_EQ(out[0].offset, std::uint64_t{0x1a2b});
}

void testParseFramesMulti()
{
	Frames out;
	WatchRecordStore::parseFrames("a!0x1,b!0x2,c!0xffff", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{3});
	QEWT_CHECK(out[0].module == "a" && out[0].offset == 1);
	QEWT_CHECK(out[2].module == "c" && out[2].offset == 0xffff);
}

void testParseFramesBadFramesSkipped()
{
	// 坏帧：无 '!'（bad）、'!' 在首位（空模块）——跳过不失败
	Frames out;
	WatchRecordStore::parseFrames("mod!0x1,bad,!0x4,ok!0x8", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{2});
	QEWT_CHECK(out[0].module == "mod");
	QEWT_CHECK(out[1].module == "ok");
}

void testParseFramesGarbageOffset()
{
	// 防御性容错：offset 非十六进制 → 0（不抛、不丢帧）
	Frames out;
	WatchRecordStore::parseFrames("m!zz", out);
	QEWT_CHECK_EQ(out.size(), std::size_t{1});
	QEWT_CHECK_EQ(out[0].offset, std::uint64_t{0});
}

void testHydrateSlowEvent()
{
	WatchRecordStore::Record r;
	r.kind = WatchRecordStore::KindSlowEvent;
	r.raw = "[EventWatcher] slow event receiver=StressGen object=stress1 event=Timer "
	        "type=43 depth=1 costMs=10.500 "
	        "stack=QtEventWatcherCore!0x1a2b,user32!0x3c4d";
	WatchRecordStore::hydrateRecord(r);

	QEWT_CHECK_EQ(r.fields.size(), std::size_t{7});
	const std::string *recv = findField(r.fields, "receiver");
	QEWT_CHECK(recv != nullptr && *recv == "StressGen");
	QEWT_CHECK_EQ(r.frames.size(), std::size_t{2});
	QEWT_CHECK(r.frames[0].module == "QtEventWatcherCore");
	QEWT_CHECK(r.frames[1].module == "user32" && r.frames[1].offset == 0x3c4d);
}

void testHydrateQssOpExtracted()
{
	WatchRecordStore::Record r;
	r.kind = WatchRecordStore::KindQss;
	r.raw = "[QssStyleWatcher] SlowQssLoad | Widget=theme.css costMs=88.500";
	WatchRecordStore::hydrateRecord(r);

	QEWT_CHECK(!r.fields.empty());
	QEWT_CHECK(r.fields[0].first == "op" && r.fields[0].second == "SlowQssLoad");
	QEWT_CHECK(r.frames.empty());	// 无 stack= 字段
}

void testHydrateIdempotent()
{
	// 幂等：重复调用先清空再解析，结果不翻倍
	WatchRecordStore::Record r;
	r.kind = WatchRecordStore::KindSlowEvent;
	r.raw = "[EventWatcher] slow event receiver=A object=o event=Timer type=1 depth=0 "
	        "costMs=1.000 stack=m!0x1";
	WatchRecordStore::hydrateRecord(r);
	const std::size_t fields1 = r.fields.size();
	const std::size_t frames1 = r.frames.size();
	WatchRecordStore::hydrateRecord(r);
	QEWT_CHECK_EQ(r.fields.size(), fields1);
	QEWT_CHECK_EQ(r.frames.size(), frames1);
	QEWT_CHECK(fields1 > 0);
}

// ============ B. AlarmSuppressor 合成时钟 ============

void testSuppressorFirstAndWindow()
{
	AlarmSuppressor s;
	// t=0 哨兵边界：startMs<0 分支，必出且不冲刷（首窗无累计）
	AlarmDecision d1 = s.evaluate("K", 0, 1000);
	QEWT_CHECK(d1.emitNow);
	QEWT_CHECK_EQ(d1.suppressedFlushed, 0);
	// 窗口 [0,1000) 内静默
	QEWT_CHECK(!s.evaluate("K", 500, 1000).emitNow);
	QEWT_CHECK(!s.evaluate("K", 999, 1000).emitNow);	// 差 1ms 未过期
	// now-start >= window 过期：冲刷累计 + 本条新窗首条
	AlarmDecision d2 = s.evaluate("K", 1000, 1000);
	QEWT_CHECK(d2.emitNow);
	QEWT_CHECK_EQ(d2.suppressedFlushed, 2);
	// 新窗 [1000,2000) 内继续静默，下轮过期冲刷 1 条
	QEWT_CHECK(!s.evaluate("K", 1001, 1000).emitNow);
	AlarmDecision d3 = s.evaluate("K", 2000, 1000);
	QEWT_CHECK(d3.emitNow);
	QEWT_CHECK_EQ(d3.suppressedFlushed, 1);
}

void testSuppressorKeyIsolation()
{
	AlarmSuppressor s;
	QEWT_CHECK(s.evaluate("A", 100, 1000).emitNow);
	QEWT_CHECK(!s.evaluate("A", 200, 1000).emitNow);
	// 不同 key 独立窗口
	AlarmDecision dB = s.evaluate("B", 200, 1000);
	QEWT_CHECK(dB.emitNow);
	QEWT_CHECK_EQ(dB.suppressedFlushed, 0);
}

void testSuppressorFirstWindowFlushZero()
{
	// 首条即过期时刻（无静默累计）→ 冲刷 0
	AlarmSuppressor s;
	AlarmDecision d = s.evaluate("N", 5000, 1000);
	QEWT_CHECK(d.emitNow);
	QEWT_CHECK_EQ(d.suppressedFlushed, 0);
}

void testSuppressorNonPositiveWindowFallsBack()
{
	AlarmSuppressor s;
	// windowMs<=0 → 回退缺省 1000ms
	QEWT_CHECK(s.evaluate("M", 0, 0).emitNow);
	QEWT_CHECK(!s.evaluate("M", 999, 0).emitNow);
	AlarmDecision d = s.evaluate("M", 1000, 0);
	QEWT_CHECK(d.emitNow);
	QEWT_CHECK_EQ(d.suppressedFlushed, 1);
	// 新窗无累计再过期 → 冲刷 0
	AlarmDecision d2 = s.evaluate("M", 2000, 0);
	QEWT_CHECK(d2.emitNow);
	QEWT_CHECK_EQ(d2.suppressedFlushed, 0);
}

// ============ C. kindOf 门控（行为级） ============

void testRecordStoreSinkGating()
{
	auto &store = WatchRecordStore::instance();
	auto sink = store.makeSink();
	spdlog::logger logger("qewt_unit_gating", sink);
	logger.set_level(spdlog::level::debug);

	const std::size_t before = store.count();
	logger.debug("[EventWatcher] slow event receiver=A object=o event=Timer type=1 depth=0 costMs=40.000");
	logger.warn("[EventWatcher] alarm storm receiver=A event=Timer type=1 suppressed=4 windowMs=1000");
	logger.info("plain info message must not be captured");
	logger.debug("[MetaCallWatcher] slow MetaCall sender=s signal=sig() signalId=-1 senderThread=0x1 receiver=r object=o recvThread=0x2 curThread=0x2 costMs=45.000");
	logger.debug("[QssStyleWatcher] SlowQssLoad | Widget=x costMs=90.000");
	logger.debug("[FreezeWatch] freeze started receiver=Main type=1");
	logger.debug("[EventWatcher] slow event receiver=B object=o event=Timer type=1 depth=0 costMs=41.000");

	auto snap = store.snapshot();
	QEWT_CHECK_EQ(snap.size(), before + 5);	// storm 与 info 被门控排除
	std::vector<int> kinds;
	for (std::size_t i = before; i < snap.size(); ++i)
		kinds.push_back(snap[i].kind);
	QEWT_CHECK_EQ(kinds.size(), std::size_t{5});
	QEWT_CHECK_EQ(kinds[0], WatchRecordStore::KindSlowEvent);
	QEWT_CHECK_EQ(kinds[1], WatchRecordStore::KindMetaCall);
	QEWT_CHECK_EQ(kinds[2], WatchRecordStore::KindQss);
	QEWT_CHECK_EQ(kinds[3], WatchRecordStore::KindFreeze);
	QEWT_CHECK_EQ(kinds[4], WatchRecordStore::KindSlowEvent);
}

// ============ D. 环形覆盖 + 差量游标 ============

void testRecordStoreRingCapAndDelta()
{
	auto &store = WatchRecordStore::instance();
	auto sink = store.makeSink();
	spdlog::logger logger("qewt_unit_ring", sink);
	logger.set_level(spdlog::level::debug);

	constexpr int kTotal = 4150;	// > 4096，触发环形覆盖
	for (int i = 0; i < kTotal; ++i)
		logger.debug("[EventWatcher] slow event receiver=Ring object=o{} event=Timer type=1 depth=0 costMs=40.000", i);

	QEWT_CHECK_EQ(store.count(), std::size_t{4096});

	std::uint64_t lastSeq = 0;
	std::size_t skipped = 0;
	auto delta = store.snapshotSince(0, lastSeq, skipped);
	QEWT_CHECK_EQ(delta.size(), std::size_t{4096});
	QEWT_CHECK(lastSeq > 0);
	// 账目闭合：送达（delta）+ 覆盖丢失（skipped）= 总序号（本用例前
	// gating 用例已推 5 条 → 5+4150=4155，覆盖 59 条计入 skipped）
	QEWT_CHECK_EQ(delta.size() + skipped, lastSeq);

	// 差量推进：新游标之后无新条目
	std::uint64_t lastSeq2 = 0;
	std::size_t skipped2 = 0;
	auto delta2 = store.snapshotSince(lastSeq, lastSeq2, skipped2);
	QEWT_CHECK(delta2.empty());
	QEWT_CHECK_EQ(skipped2, std::size_t{0});
	QEWT_CHECK_EQ(lastSeq2, lastSeq);

	// snapshot(maxCount) 截断语义
	auto head = store.snapshot(10);
	QEWT_CHECK_EQ(head.size(), std::size_t{10});
	// seq 保序递增
	QEWT_CHECK(head.front().seq < head.back().seq);
}

// ============ E. WatchConfig ============

void testWatchConfigDefaults()
{
	WatchConfig c;
	QEWT_CHECK_EQ(c.watchFun(), WatchConfig::WatchFunMask{0});
	QEWT_CHECK_EQ(c.slowEventThresholdMs(), WatchConfig::DefaultSlowEventThresholdMs);
	QEWT_CHECK_EQ(c.slowMetaCallThresholdMs(), WatchConfig::DefaultSlowMetaCallThresholdMs);
	QEWT_CHECK_EQ(c.freezeThresholdMs(), WatchConfig::DefaultFreezeThresholdMs);
	QEWT_CHECK_EQ(c.alarmSuppressWindowMs(), WatchConfig::DefaultAlarmSuppressWindowMs);
	QEWT_CHECK_EQ(c.stackCaptureMode(), WatchConfig::DefaultStackCaptureMode);
	QEWT_CHECK(c.uplinkName().isEmpty());	// 上行默认关闭
	QEWT_CHECK(!c.isWatchEnabled(WatchConfig::WatchEvent));
}

void testWatchConfigEnvDecimalAndBits()
{
	qputenv("QT_EVENT_WATCHER_WATCH_FUN", "15");
	WatchConfig c;
	c.loadFromEnvironment();
	QEWT_CHECK_EQ(c.watchFun(), WatchConfig::WatchFunMask{15});
	QEWT_CHECK(c.isWatchEnabled(WatchConfig::WatchEvent));
	QEWT_CHECK(c.isWatchEnabled(WatchConfig::WatchMetaCall));
	QEWT_CHECK(c.isWatchEnabled(WatchConfig::WatchEventStatistics));
	QEWT_CHECK(c.isWatchEnabled(WatchConfig::WatchQssMonitor));
	QEWT_CHECK(!c.isWatchEnabled(WatchConfig::WatchFreeze));	// 15 = 低 4 位
}

void testWatchConfigEnvHexRejected()
{
	// 纯十进制契约：0x 前缀解析失败 → 落默认（V4 GUI 压测 0x0f 教训）
	qputenv("QT_EVENT_WATCHER_WATCH_FUN", "0x0f");
	WatchConfig c;
	c.loadFromEnvironment();
	QEWT_CHECK_EQ(c.watchFun(), WatchConfig::WatchFunMask{0});
}

void testWatchConfigEnvInvalidFallback()
{
	qputenv("QT_EVENT_WATCHER_WATCH_FUN", "abc");
	qputenv("QT_EVENT_WATCHER_SLOW_EVENT_THRESHOLD_MS", "-5");
	WatchConfig c;
	c.loadFromEnvironment();
	QEWT_CHECK_EQ(c.watchFun(), WatchConfig::WatchFunMask{0});
	QEWT_CHECK_EQ(c.slowEventThresholdMs(), WatchConfig::DefaultSlowEventThresholdMs);

	qunsetenv("QT_EVENT_WATCHER_SLOW_EVENT_THRESHOLD_MS");
}

void testWatchConfigIni()
{
	const QString path = QDir::temp().absoluteFilePath("qewt_unit_config.ini");
	QSettings writer(path, QSettings::IniFormat);
	writer.beginGroup("QtEventWatcher");
	writer.setValue("Watch_Fun", 31);
	writer.setValue("SlowEventThresholdMs", 50);
	writer.endGroup();
	writer.sync();

	WatchConfig c;
	QEWT_CHECK(c.loadFromFile(path));
	QEWT_CHECK_EQ(c.watchFun(), WatchConfig::WatchFunMask{31});
	QEWT_CHECK_EQ(c.slowEventThresholdMs(), 50);

	QFile::remove(path);
}

const qewt::Case kCases[] = {
	{"parseFields/empty", testParseFieldsEmpty},
	{"parseFields/basic", testParseFieldsBasic},
	{"parseFields/multi-space", testParseFieldsMultiSpace},
	{"parseFields/no-eq-token-skipped", testParseFieldsNoEqTokenSkipped},
	{"parseFields/empty-value-kept", testParseFieldsEmptyValueKept},
	{"parseFields/eq-start-skipped", testParseFieldsEqStartSkipped},
	{"parseFrames/empty", testParseFramesEmpty},
	{"parseFrames/single", testParseFramesSingle},
	{"parseFrames/multi", testParseFramesMulti},
	{"parseFrames/bad-frames-skipped", testParseFramesBadFramesSkipped},
	{"parseFrames/garbage-offset", testParseFramesGarbageOffset},
	{"hydrate/slow-event-full", testHydrateSlowEvent},
	{"hydrate/qss-op-extracted", testHydrateQssOpExtracted},
	{"hydrate/idempotent", testHydrateIdempotent},
	{"suppressor/first-and-window", testSuppressorFirstAndWindow},
	{"suppressor/key-isolation", testSuppressorKeyIsolation},
	{"suppressor/first-window-flush-zero", testSuppressorFirstWindowFlushZero},
	{"suppressor/non-positive-window", testSuppressorNonPositiveWindowFallsBack},
	{"recordstore/sink-gating", testRecordStoreSinkGating},
	{"recordstore/ring-cap-and-delta", testRecordStoreRingCapAndDelta},
	{"watchconfig/defaults", testWatchConfigDefaults},
	{"watchconfig/env-decimal-and-bits", testWatchConfigEnvDecimalAndBits},
	{"watchconfig/env-hex-rejected", testWatchConfigEnvHexRejected},
	{"watchconfig/env-invalid-fallback", testWatchConfigEnvInvalidFallback},
	{"watchconfig/ini", testWatchConfigIni},
};

} // namespace

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("QtEventWatcherUnitTests"));
	return qewt::runAll(kCases, int(sizeof(kCases) / sizeof(kCases[0])));
}
