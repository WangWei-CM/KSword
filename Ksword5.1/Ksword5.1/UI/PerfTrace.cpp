#include "./PerfTrace.h"

#if KSWORD_PERF_TRACE_ENABLED

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <limits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <io.h>

namespace
{
    std::atomic_bool notifyTraceReady{false};
    thread_local ks::ui::PerfNotifyScope* currentNotifyScope = nullptr;

    struct NotifyCpuSample
    {
        quint64 cycles = 0, time100ns = 0;
        bool cyclesValid = false, timeValid = false;
    };

    NotifyCpuSample sampleNotifyCpu()
    {
        NotifyCpuSample sample;
        ULONG64 cycles = 0;
        sample.cyclesValid = ::QueryThreadCycleTime(::GetCurrentThread(), &cycles) != FALSE;
        sample.cycles = cycles;
        FILETIME creation{}, exit{}, kernel{}, user{};
        sample.timeValid = ::GetThreadTimes(::GetCurrentThread(), &creation, &exit, &kernel, &user) != FALSE;
        if (sample.timeValid)
            sample.time100ns = (static_cast<quint64>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime)
                + (static_cast<quint64>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        return sample;
    }

    const QElapsedTimer& referenceClock()
    {
        static const QElapsedTimer clock = []()
        {
            QElapsedTimer started;
            started.start();
            return started;
        }();
        return clock;
    }

    constexpr int kHeartbeatIntervalMs = 25;
    constexpr qint64 kStallThresholdNs = 50000000;
    constexpr qint64 kStallIgnoreNs = 10000000000;
    constexpr int kMaxAncestorWalk = 16;
    constexpr size_t kPendingRecordLimit = 2048;

    // Only values cross to the writer. No QObject, context provider or widget is captured.
    struct TraceRecord
    {
        QString event;
        QString operation;
        QString target;
        QString context;
        qint64 atNs = 0;
        qint64 beginNs = -1;
        qint64 endNs = -1;
        qint64 durationNs = -1;
        qint64 count = -1;
        quint64 scopeId = 0;
        quint64 parentScopeId = 0;
        int eventType = -1;
        qint64 selfWallNs = -1, threadCycles = -1, selfThreadCycles = -1;
        qint64 threadCpu100ns = -1, selfThreadCpu100ns = -1;
        qint64 probeSetupNs = -1, probeUntilEnqueueNs = -1;
    };

    struct TraceWriterState
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::condition_variable finishedReady;
        std::deque<TraceRecord> records;
        bool closed = false;
        bool finished = false;
        bool completionReceipt = false;
        quint64 accepted = 0;
        quint64 dropped = 0;
        QString path;
        QString fallbackPath;
        QString sessionId;
        QString executable;
        QString applicationVersion;
        QString utc;
    };

    // The existing trace gets one value-owned writer, not a second UI service.
    // It owns QFile/JSON/hash work. Normal dispatch never waits for disk; only
    // diagnostic shutdown can make a bounded wait after the event loop returns.
    bool writeTrace(const std::shared_ptr<TraceWriterState>& state)
    {
        QFile file(state->path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            file.setFileName(state->fallbackPath);
            file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        }
        const auto writeRecord = [&file](const QJsonObject& record)
        {
            if (!file.isOpen())
                return false;
            const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
            qint64 offset = 0;
            while (offset < line.size())
            {
                const qint64 written = file.write(line.constData() + offset, line.size() - offset);
                if (written <= 0)
                {
                    file.close();
                    return false;
                }
                offset += written;
            }
            return true;
        };

        const auto flushToOs = [](QFileDevice& output)
        {
            if (!output.flush()) return false;
            const auto handle = ::_get_osfhandle(output.handle());
            return handle != -1 && ::FlushFileBuffers(reinterpret_cast<HANDLE>(handle)) != FALSE;
        };

        QJsonObject session;
        session.insert(QStringLiteral("event"), QStringLiteral("session"));
        session.insert(QStringLiteral("op"), QStringLiteral("perf_session_start"));
        session.insert(QStringLiteral("session_id"), state->sessionId);
        session.insert(QStringLiteral("utc"), state->utc);
        session.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
        session.insert(QStringLiteral("build_stamp"), QStringLiteral(__DATE__ " " __TIME__));
        session.insert(QStringLiteral("version"), state->applicationVersion);
        session.insert(QStringLiteral("exe"), state->executable);
        session.insert(QStringLiteral("writer_thread"), QString::number(
            reinterpret_cast<quintptr>(QThread::currentThreadId())));
        QFile executable(state->executable);
        if (executable.open(QIODevice::ReadOnly))
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&executable))
                session.insert(QStringLiteral("exe_sha256"), QString::fromLatin1(hash.result().toHex()));
        }
        if (!writeRecord(session)) return false;

        quint64 writtenRecords = 0, droppedRecords = 0;

        for (;;)
        {
            std::deque<TraceRecord> records;
            quint64 dropped = 0;
            bool closed = false;
            quint64 accepted = 0;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->ready.wait(lock, [&state]()
                {
                    return state->closed || !state->records.empty() || state->dropped != 0;
                });
                records.swap(state->records);
                dropped = std::exchange(state->dropped, 0);
                closed = state->closed;
                accepted = state->accepted;
            }
            if (dropped != 0)
            {
                QJsonObject loss;
                loss.insert(QStringLiteral("event"), QStringLiteral("dropped"));
                loss.insert(QStringLiteral("session_id"), state->sessionId);
                loss.insert(QStringLiteral("count"), QString::number(dropped));
                if (!writeRecord(loss)) return false;
                droppedRecords += dropped;
            }
            for (const TraceRecord& value : records)
            {
                QJsonObject record;
                record.insert(QStringLiteral("session_id"), state->sessionId);
                record.insert(QStringLiteral("event"), value.event);
                record.insert(QStringLiteral("op"), value.operation);
                record.insert(QStringLiteral("t"), value.atNs / 1000000.0);
                record.insert(QStringLiteral("t_ns"), static_cast<double>(value.atNs));
                if (!value.target.isEmpty())
                    record.insert(QStringLiteral("target"), value.target);
                if (!value.context.isEmpty())
                    record.insert(QStringLiteral("cached_context_at_record_end"), value.context);
                if (value.scopeId != 0)
                    record.insert(QStringLiteral("scope_id"), QString::number(value.scopeId));
                if (value.beginNs >= 0)
                    record.insert(QStringLiteral("begin_ns"), static_cast<double>(value.beginNs));
                if (value.endNs >= 0)
                    record.insert(QStringLiteral("end_ns"), static_cast<double>(value.endNs));
                if (value.durationNs >= 0)
                {
                    record.insert(QStringLiteral("duration_ns"), static_cast<double>(value.durationNs));
                    record.insert(QStringLiteral("ms"), value.durationNs / 1000000.0);
                    record.insert(QStringLiteral("measurement"), QStringLiteral("elapsed_wall"));
                }
                if (value.count >= 0)
                    record.insert(QStringLiteral("count"), static_cast<double>(value.count));
                if (value.event == QStringLiteral("stall"))
                {
                    record.insert(QStringLiteral("measurement"), QStringLiteral("heartbeat_overrun"));
                    record.insert(QStringLiteral("attribution"), QStringLiteral("unattributed_interval"));
                    record.insert(QStringLiteral("interval_ns"), static_cast<double>(value.endNs - value.beginNs));
                }
                if (value.eventType >= 0)
                {
                    record.insert(QStringLiteral("measurement"), QStringLiteral("qt_notify_dispatch"));
                    record.insert(QStringLiteral("event_type"), value.eventType);
                    record.insert(QStringLiteral("parent_scope_id"), QString::number(value.parentScopeId));
                    record.insert(QStringLiteral("self_wall_ns"), static_cast<double>(value.selfWallNs));
                    if (value.threadCycles >= 0)
                        record.insert(QStringLiteral("thread_cycles"), QString::number(value.threadCycles));
                    if (value.selfThreadCycles >= 0)
                        record.insert(QStringLiteral("self_thread_cycles"), QString::number(value.selfThreadCycles));
                    if (value.threadCpu100ns >= 0)
                        record.insert(QStringLiteral("thread_cpu_100ns"), QString::number(value.threadCpu100ns));
                    if (value.selfThreadCpu100ns >= 0)
                        record.insert(QStringLiteral("self_thread_cpu_100ns"), QString::number(value.selfThreadCpu100ns));
                    record.insert(QStringLiteral("probe_setup_ns"), static_cast<double>(value.probeSetupNs));
                    record.insert(QStringLiteral("probe_until_enqueue_ns"), static_cast<double>(value.probeUntilEnqueueNs));
                    record.insert(QStringLiteral("physical_present_measured"), false);
                }
                if (!writeRecord(record)) return false;
                ++writtenRecords;
            }
            if (closed)
            {
                if (writtenRecords != accepted) return false;
                QJsonObject end;
                end.insert(QStringLiteral("event"), QStringLiteral("writer_drain_end"));
                end.insert(QStringLiteral("session_id"), state->sessionId);
                end.insert(QStringLiteral("written_records"), QString::number(writtenRecords));
                end.insert(QStringLiteral("dropped_records"), QString::number(droppedRecords));
                // This marker alone is not a flush acknowledgement. The separate
                // receipt is published only after the complete trace flush succeeds.
                if (!writeRecord(end) || !flushToOs(file)) return false;
                QJsonObject receipt = end;
                receipt.insert(QStringLiteral("event"), QStringLiteral("writer_flush_receipt"));
                receipt.insert(QStringLiteral("trace_path"), QFileInfo(file).absoluteFilePath());
                receipt.insert(QStringLiteral("trace_bytes"), QString::number(file.size()));
                receipt.insert(QStringLiteral("accepted_records"), QString::number(accepted));
                receipt.insert(QStringLiteral("flush"), QStringLiteral("QFile_flush_and_FlushFileBuffers"));
                receipt.insert(QStringLiteral("physical_present_measured"), false);
                QSaveFile completion(file.fileName() + QStringLiteral(".completed-")
                    + state->sessionId + QStringLiteral(".json"));
                completion.setDirectWriteFallback(false);
                const QByteArray bytes = QJsonDocument(receipt).toJson(QJsonDocument::Compact) + '\n';
                return completion.open(QIODevice::WriteOnly)
                    && completion.write(bytes) == bytes.size()
                    && flushToOs(completion) && completion.commit();
            }
            if (!file.flush()) return false;
        }
    }
}

namespace ks::ui
{
    class PerfTrace final : public QObject
    {
    public:
        static PerfTrace& instance()
        {
            static PerfTrace trace;
            return trace;
        }

        void recordSpan(const char* operation, const QString& target, qint64 durationNs,
            qint64 count, quint64 scopeId = 0, qint64 startedAtNs = -1)
        {
            TraceRecord record = makeRecord("span", operation, target);
            record.beginNs = startedAtNs >= 0 ? startedAtNs : record.atNs - durationNs;
            record.endNs = record.atNs;
            record.durationNs = startedAtNs >= 0 ? record.atNs - startedAtNs : durationNs;
            record.scopeId = scopeId;
            record.count = count;
            enqueue(std::move(record));
        }

        quint64 beginScope(const char* operation, const QString& target, qint64 atNs)
        {
            const quint64 id = m_nextScopeId.fetch_add(1, std::memory_order_relaxed);
            TraceRecord record = makeRecord("scope_begin", operation, target);
            record.scopeId = id;
            record.beginNs = atNs;
            enqueue(std::move(record));
            return id;
        }

        void recordMark(const char* operation, const QString& target)
        {
            // The existing Dock signal already supplies identity; never rediscover it by UI scans.
            if (std::strncmp(operation, "perf_dock_", 10) == 0)
            {
                std::lock_guard<std::mutex> lock(m_contextMutex);
                m_cachedContext = target;
            }
            enqueue(makeRecord("mark", operation, target));
        }

        quint64 nextNotifyId() { return m_nextScopeId.fetch_add(1, std::memory_order_relaxed); }

        void recordNotify(TraceRecord record)
        {
            record.event = QStringLiteral("notify_dispatch");
            record.operation = QStringLiteral("perf_notify_dispatch");
            {
                std::lock_guard<std::mutex> lock(m_contextMutex);
                record.context = m_cachedContext;
            }
            enqueue(std::move(record));
        }

        bool finishWriter(int maxWaitMs)
        {
            closeWriter();
            std::unique_lock<std::mutex> lock(m_writer->mutex);
            return m_writer->finishedReady.wait_for(lock,
                std::chrono::milliseconds(std::clamp(maxWaitMs, 0, 1000)),
                [this]() { return m_writer->finished; }) && m_writer->completionReceipt;
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event && watched && event->type() == QEvent::MouseButtonPress)
            {
                const QString label = tabBarLabelFor(watched);
                if (!label.isEmpty())
                    recordMark("perf_tab_press", label);
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        PerfTrace() : m_writer(std::make_shared<TraceWriterState>())
        {
            m_writer->path = qEnvironmentVariable("KSWORD_PERF_TRACE");
            if (m_writer->path.isEmpty())
                m_writer->path = QCoreApplication::applicationDirPath() + QStringLiteral("/ksword_perf_trace.jsonl");
            m_writer->fallbackPath = QDir::tempPath() + QStringLiteral("/ksword_perf_trace.jsonl");
            m_writer->sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_writer->executable = QCoreApplication::applicationFilePath();
            m_writer->applicationVersion = QCoreApplication::applicationVersion();
            m_writer->utc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            (void)referenceClock();
            try
            {
                std::thread([state = m_writer]()
                {
                    bool receipt = false;
                    try { receipt = writeTrace(state); }
                    catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->closed = true;
                        state->finished = true;
                        state->completionReceipt = receipt;
                    }
                    state->finishedReady.notify_all();
                }).detach();
            }
            catch (...)
            {
                m_writer->closed = true; // Diagnostics must not fail a product operation.
                m_writer->finished = true;
            }
            m_heartbeat.setParent(this);
            if (QCoreApplication* application = QCoreApplication::instance())
            {
                if (thread() != application->thread())
                    moveToThread(application->thread());
                if (QThread::currentThread() == thread())
                    startGuiProbes();
                else
                    QMetaObject::invokeMethod(this, [this]() { startGuiProbes(); }, Qt::QueuedConnection);
            }
        }

        ~PerfTrace() override { closeWriter(); }

        void startGuiProbes()
        {
            QCoreApplication* application = QCoreApplication::instance();
            if (!application)
                return;
            application->installEventFilter(this);
            connect(application, &QCoreApplication::aboutToQuit, this, [this]()
            {
                m_heartbeat.stop();
                closeWriter();
            });
            m_previousHeartbeatNs = referenceClock().nsecsElapsed();
            m_heartbeat.setTimerType(Qt::PreciseTimer);
            m_heartbeat.setInterval(kHeartbeatIntervalMs);
            connect(&m_heartbeat, &QTimer::timeout, this, [this]() { onHeartbeat(); });
            m_heartbeat.start();
        }

        void closeWriter()
        {
            notifyTraceReady.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(m_writer->mutex);
                m_writer->closed = true;
            }
            m_writer->ready.notify_one();
        }

        void onHeartbeat()
        {
            const qint64 endedAtNs = referenceClock().nsecsElapsed();
            const qint64 startedAtNs = std::exchange(m_previousHeartbeatNs, endedAtNs);
            const qint64 overrunNs = endedAtNs - startedAtNs - kHeartbeatIntervalMs * 1000000LL;
            if (overrunNs < kStallThresholdNs || overrunNs > kStallIgnoreNs)
                return;
            TraceRecord record = makeRecord("stall", "perf_ui_stall", QString());
            record.beginNs = startedAtNs;
            record.endNs = endedAtNs;
            record.durationNs = overrunNs;
            enqueue(std::move(record));
        }

        TraceRecord makeRecord(const char* event, const char* operation, const QString& target)
        {
            TraceRecord record;
            record.atNs = referenceClock().nsecsElapsed();
            record.event = QString::fromLatin1(event);
            record.operation = QString::fromUtf8(operation);
            record.target = target;
            {
                std::lock_guard<std::mutex> lock(m_contextMutex);
                record.context = m_cachedContext;
            }
            return record;
        }

        void enqueue(TraceRecord record)
        {
            {
                std::lock_guard<std::mutex> lock(m_writer->mutex);
                if (m_writer->closed)
                    return;
                if (m_writer->records.size() >= kPendingRecordLimit)
                    ++m_writer->dropped;
                else
                {
                    m_writer->records.push_back(std::move(record));
                    ++m_writer->accepted;
                    auto& queued = m_writer->records.back();
                    if (queued.eventType >= 0)
                        queued.probeUntilEnqueueNs = queued.probeSetupNs
                            + referenceClock().nsecsElapsed() - queued.endNs;
                }
            }
            m_writer->ready.notify_one();
        }

        QString tabBarLabelFor(QObject* watched)
        {
            QObject* object = watched;
            for (int depth = 0; object && depth < kMaxAncestorWalk; ++depth)
            {
                const QByteArray className = object->metaObject()->className();
                if (className == QByteArrayLiteral("QTabBar") ||
                    className.startsWith(QByteArrayLiteral("ads::CDockAreaTabBar")))
                {
                    QString label = QString::fromUtf8(className);
                    if (!object->objectName().isEmpty())
                        label += QLatin1Char('/') + object->objectName();
                    return label;
                }
                object = object->parent();
            }
            return QString();
        }

        const std::shared_ptr<TraceWriterState> m_writer;
        std::mutex m_contextMutex;
        QString m_cachedContext;
        QTimer m_heartbeat;
        qint64 m_previousHeartbeatNs = 0;
        std::atomic<quint64> m_nextScopeId{1};
    };

    void RecordPerfSpan(const char* operation, const QString& target, qint64 durationMs)
    {
        PerfTrace::instance().recordSpan(operation, target, durationMs * 1000000, -1);
    }

    void RecordPerfSpanWithCount(const char* operation, const QString& target, qint64 durationMs, qint64 count)
    {
        PerfTrace::instance().recordSpan(operation, target, durationMs * 1000000, count);
    }

    void RecordPerfSpanNs(const char* operation, const QString& target, qint64 durationNs, qint64 count)
    {
        PerfTrace::instance().recordSpan(operation, target, durationNs, count);
    }

    void RecordPerfMark(const char* operation, const QString& target)
    {
        PerfTrace::instance().recordMark(operation, target);
    }

    qint64 PerfTraceNowMs() { return referenceClock().elapsed(); }
    qint64 PerfTraceNowNs() { return referenceClock().nsecsElapsed(); }

    void InitializePerfTrace()
    {
        (void)PerfTrace::instance();
        notifyTraceReady.store(true, std::memory_order_relaxed);
    }

    bool FinishPerfTrace(int maxWaitMs) noexcept
    {
        try { return PerfTrace::instance().finishWriter(maxWaitMs); }
        catch (...) { return false; }
    }

    PerfNotifyScope::PerfNotifyScope(QObject* receiver, const int eventType) noexcept
    {
        try
        {
            auto* app = QCoreApplication::instance();
            if (!notifyTraceReady.load(std::memory_order_relaxed) || !app || !receiver
                || eventType < 0 || QThread::currentThread() != app->thread()) return;
            const auto setupStarted = referenceClock().nsecsElapsed();
            // Copy identity before dispatch: the event may destroy its receiver.
            m_target = QString::fromUtf8(receiver->metaObject()->className());
            if (!receiver->objectName().isEmpty()) m_target += QLatin1Char('/') + receiver->objectName();
            m_scopeId = PerfTrace::instance().nextNotifyId();
            m_parent = currentNotifyScope;
            m_eventType = eventType;
            const auto cpu = sampleNotifyCpu();
            m_startedCycles = cpu.cycles;
            m_startedCpu100ns = cpu.time100ns;
            m_cyclesValid = cpu.cyclesValid;
            m_cpuValid = cpu.timeValid;
            m_startedAtNs = referenceClock().nsecsElapsed();
            m_setupNs = m_startedAtNs - setupStarted;
            currentNotifyScope = this;
            m_active = true;
        }
        catch (...) {} // Diagnostics must not change event delivery.
    }

    PerfNotifyScope::~PerfNotifyScope() noexcept
    {
        if (!m_active) return;
        currentNotifyScope = m_parent;
        try
        {
            const auto endedAt = referenceClock().nsecsElapsed();
            const auto cpu = sampleNotifyCpu();
            const auto wall = std::max<qint64>(0, endedAt - m_startedAtNs);
            const bool cyclesValid = m_cyclesValid && cpu.cyclesValid && cpu.cycles >= m_startedCycles
                && cpu.cycles - m_startedCycles <= static_cast<quint64>(std::numeric_limits<qint64>::max());
            const bool timeValid = m_cpuValid && cpu.timeValid && cpu.time100ns >= m_startedCpu100ns
                && cpu.time100ns - m_startedCpu100ns <= static_cast<quint64>(std::numeric_limits<qint64>::max());
            const auto cycles = cyclesValid ? cpu.cycles - m_startedCycles : 0;
            const auto time = timeValid ? cpu.time100ns - m_startedCpu100ns : 0;
            if (m_parent)
            {
                m_parent->m_childWallNs += wall;
                if (cyclesValid && std::numeric_limits<quint64>::max() - m_parent->m_childCycles >= cycles)
                    m_parent->m_childCycles += cycles;
                else m_parent->m_childCyclesValid = false;
                if (timeValid && std::numeric_limits<quint64>::max() - m_parent->m_childCpu100ns >= time)
                    m_parent->m_childCpu100ns += time;
                else m_parent->m_childCpuValid = false;
            }
            if (!notifyTraceReady.load(std::memory_order_relaxed)) return;
            TraceRecord record;
            record.atNs = record.endNs = endedAt;
            record.beginNs = m_startedAtNs;
            record.durationNs = wall;
            record.scopeId = m_scopeId;
            record.parentScopeId = m_parent ? m_parent->m_scopeId : 0;
            record.eventType = m_eventType;
            record.target = std::move(m_target);
            record.selfWallNs = std::max<qint64>(0, wall - m_childWallNs);
            if (cyclesValid)
            {
                record.threadCycles = static_cast<qint64>(cycles);
                if (m_childCyclesValid && m_childCycles <= cycles)
                    record.selfThreadCycles = static_cast<qint64>(cycles - m_childCycles);
            }
            if (timeValid)
            {
                record.threadCpu100ns = static_cast<qint64>(time);
                if (m_childCpuValid && m_childCpu100ns <= time)
                    record.selfThreadCpu100ns = static_cast<qint64>(time - m_childCpu100ns);
            }
            record.probeSetupNs = m_setupNs;
            PerfTrace::instance().recordNotify(std::move(record));
        }
        catch (...) {}
    }

    PerfScope::PerfScope(const char* operation, QString target)
        : m_operationToken(operation), m_targetLabel(std::move(target))
    {
        m_startedAtNs = referenceClock().nsecsElapsed();
        m_scopeId = PerfTrace::instance().beginScope(m_operationToken, m_targetLabel, m_startedAtNs);
    }

    PerfScope::~PerfScope() { finish(); }

    void PerfScope::finish()
    {
        if (m_finished)
            return;
        m_finished = true;
        PerfTrace::instance().recordSpan(m_operationToken, m_targetLabel,
            referenceClock().nsecsElapsed() - m_startedAtNs, m_itemCount, m_scopeId, m_startedAtNs);
    }
}
#endif
