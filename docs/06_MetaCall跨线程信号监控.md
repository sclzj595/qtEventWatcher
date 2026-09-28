# Qt 卡顿排查辅助器 PRD 06：MetaCall 跨线程信号监控

## 1. 目标
定位 queued connection 形成的 `QEvent::MetaCall` 在接收线程中执行过久或调用过密的问题。

## 2. 监控内容
- MetaCall 事件耗时。
- signalId。
- sender 类名。
- receiver 类名。
- receiver objectName。
- 所在线程信息（可选）。

## 3. 私有结构解析
Qt 没有公开 API 提供完整 MetaCall sender / signalId 信息，因此需要针对指定 Qt 版本做兼容层。

硬性要求：
- Qt 5.14.2、5.15.2、6.5.3 分开适配。
- 所有私有结构访问集中在独立文件。
- 禁止在业务代码中散落偏移量。
- 偏移量必须经过对应 Qt 二进制版本验证。
- 解析失败必须安全跳过，不能因为监控导致崩溃。

## 4. 风险
MetaCall 属于 Qt 内部实现细节，升级 Qt 后可能失效。因此该能力定义为“版本适配能力”，不是永久 ABI 保证。

## 5. 线程信息采集（定稿）
MetaCall 是跨线程诊断场景，仅 `Worker → MainWindow 52ms` 不足以定位问题，必须补充线程链路：
- 采集三项：Current Thread（notify 执行线程）、Receiver Thread（`receiver->thread()`）、Sender Thread（best-effort）。
- **Sender Thread 安全限制**：sender 来自 Qt Private Event 裸指针，禁止为取 `sender->thread()` 冒险解引用已失效对象；仅在注册表发射时刻快照有效时输出，否则 `Sender Thread: Unknown`，不允许崩溃或猜测。
- **Receiver Thread 是可靠信息**：receiver 正处于 notify() 执行中，可直接读取，并与当前线程比对得到 `receiverThreadMatch = true / false`。
- 线程 ID 统一保存 `quintptr` 整数快照，不长期持有 `QThread*`；UI 显示 `Thread: 0x1A24` 或 `MainThread`。
- Thread Registry（线程命名）不做，后续按需引入。

诊断输出形态：

```text
Slow MetaCall
Sender: Worker            Sender Thread: WorkerThread
Signal: resultReady(QString)
Receiver: MainWindow      Receiver Thread: MainThread
Current Thread: MainThread
Cost: 52.8 ms
```
