#pragma once

#include <QElapsedTimer>
#include <QString>

class QObject;

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

    // 记录一个观察时间点；它不能为此前的心跳区间提供因果归因。
    void RecordPerfMark(const char* operationToken, const QString& targetLabel);

    // 获取全局单调时钟经过的毫秒数；分片计时需要与日志时间轴对齐时使用。
    qint64 PerfTraceNowMs();

    // 纳秒墙钟区间；队列探针表示事件入口至回投执行的延迟，不是 Paint/Layout CPU 耗时。
    qint64 PerfTraceNowNs();
    void RecordPerfSpanNs(const char* operationToken, const QString& targetLabel,
        qint64 durationNs, qint64 itemCount = -1);

    // GUI 宿主初始化诊断；Dock 标记直接维护观察缓存，不注册 UI 查询回调。
    void InitializePerfTrace();

    // Called after exec() returns, while Qt still exists. Diagnostic builds wait
    // at most maxWaitMs for the writer's successful flush/receipt; false means
    // no completion proof. Does not change the application's exit result.
    bool FinishPerfTrace(int maxWaitMs = 100) noexcept;

    // Wrap only the real GUI notify call. CPU cycles remain cycles; thread time
    // is the OS's 100ns counter, not a conversion of wall time or clock cycles.
    // Self values subtract nested notify bodies; diagnostic overhead is reported
    // separately and is still present in the observed dispatch.
    class PerfNotifyScope final
    {
    public:
        PerfNotifyScope(QObject* receiver, int eventType) noexcept;
        ~PerfNotifyScope() noexcept;
        PerfNotifyScope(const PerfNotifyScope&) = delete;
        PerfNotifyScope& operator=(const PerfNotifyScope&) = delete;
    private:
        PerfNotifyScope* m_parent = nullptr;
        QString m_target;
        quint64 m_scopeId = 0;
        quint64 m_startedCycles = 0, m_startedCpu100ns = 0;
        quint64 m_childCycles = 0, m_childCpu100ns = 0;
        qint64 m_startedAtNs = 0, m_childWallNs = 0, m_setupNs = 0;
        int m_eventType = -1;
        bool m_active = false, m_cyclesValid = false, m_cpuValid = false;
        bool m_childCyclesValid = true, m_childCpuValid = true;
    };

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
        qint64 m_startedAtNs = 0;
        quint64 m_scopeId = 0;
        qint64 m_itemCount = -1;
        bool m_finished = false;
    };

#else

    inline void RecordPerfSpan(const char*, const QString&, qint64) {}
    inline void RecordPerfSpanWithCount(const char*, const QString&, qint64, qint64) {}
    inline void RecordPerfMark(const char*, const QString&) {}
    inline qint64 PerfTraceNowMs() { return 0; }
    inline qint64 PerfTraceNowNs() { return 0; }
    inline void RecordPerfSpanNs(const char*, const QString&, qint64, qint64 = -1) {}
    inline void InitializePerfTrace() {}
    inline bool FinishPerfTrace(int = 100) noexcept { return false; }

    class PerfNotifyScope final
    {
    public:
        PerfNotifyScope(QObject*, int) noexcept {}
    };

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
