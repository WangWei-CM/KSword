#pragma once

#include <QElapsedTimer>
#include <QString>

#include <functional>

// 性能采集是排查期工具，默认关闭，避免把埋点带进日常构建与运行。
// 需要再次采集时把下面的 0 改为 1（或在工程预处理器定义里加 KSWORD_PERF_TRACE_ENABLED=1），
// 重新构建后在 exe 同目录读取 ksword_perf_trace.jsonl；也可用 KSWORD_PERF_TRACE 指定路径。
// 关闭时 PerfScope 与各 Record 接口都退化为内联空实现，调用点无需改动；
// 少数带副作用的位置（信号连接、逐帧簿记）用同一个宏单独隔离。
#ifndef KSWORD_PERF_TRACE_ENABLED
#define KSWORD_PERF_TRACE_ENABLED 0
#endif

namespace ks::ui
{
#if KSWORD_PERF_TRACE_ENABLED

    // 记录一次操作边界耗时（毫秒）。token 为稳定记号，target 为 dock 键、表名等上下文。
    void RecordPerfSpan(const char* operationToken, const QString& targetLabel, qint64 durationMs);

    // 记录一次带附加计数的操作边界耗时，例如滚动动画的总帧数。
    void RecordPerfSpanWithCount(const char* operationToken, const QString& targetLabel,
        qint64 durationMs, qint64 itemCount);

    // 记录一次标记（不计时），用于把后续停顿对齐到用户操作，例如按下某个页签。
    void RecordPerfMark(const char* operationToken, const QString& targetLabel);

    // 获取全局单调时钟经过的毫秒数；分片计时需要与日志时间轴对齐时使用。
    qint64 PerfTraceNowMs();

    // 由宿主注册上下文提供者：返回当前聚焦的 dock/页签描述，供停顿记录携带现场。
    void SetPerfTraceContextProvider(std::function<QString()> provider);

    // 作用域计时器：构造时开始计时，析构时自动记录，允许提前结束或补充计数。
    class PerfScope final
    {
    public:
        PerfScope(const char* operationToken, QString targetLabel);
        ~PerfScope();

        PerfScope(const PerfScope&) = delete;
        PerfScope& operator=(const PerfScope&) = delete;

        // 补充该次操作处理的项目数（行数、帧数等）；不设置则不写入 count 字段。
        void setItemCount(const qint64 itemCount) { m_itemCount = itemCount; }

        // 立即结束计时并按当前耗时记录，重复调用只生效一次。
        void finish();

    private:
        const char* m_operationToken = nullptr;
        QString m_targetLabel;
        QElapsedTimer m_clock;
        qint64 m_itemCount = -1;
        bool m_finished = false;
    };

#else

    inline void RecordPerfSpan(const char*, const QString&, qint64) {}
    inline void RecordPerfSpanWithCount(const char*, const QString&, qint64, qint64) {}
    inline void RecordPerfMark(const char*, const QString&) {}
    inline qint64 PerfTraceNowMs() { return 0; }
    inline void SetPerfTraceContextProvider(std::function<QString()>) {}

    // 关闭时的作用域计时器空实现：签名与开启时一致，调用点不需要条件编译。
    class PerfScope final
    {
    public:
        PerfScope(const char*, QString) {}
        void setItemCount(qint64) {}
        void finish() {}
    };

#endif
}
