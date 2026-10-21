#include "./PerfTrace.h"

#if KSWORD_PERF_TRACE_ENABLED

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
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
    };

    struct TraceWriterState
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<TraceRecord> records;
        bool closed = false;
        quint64 dropped = 0;
        QString path;
        QString fallbackPath;
        QString sessionId;
        QString executable;
        QString applicationVersion;
        QString utc;
    };

    // The existing trace gets one value-owned writer, not a second UI service.
    // It owns QFile/JSON/hash work, including shutdown drain. GUI never waits for disk.
    void writeTrace(const std::shared_ptr<TraceWriterState>& state)
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
                return;
            const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
            qint64 offset = 0;
            while (offset < line.size())
            {
                const qint64 written = file.write(line.constData() + offset, line.size() - offset);
                if (written <= 0)
                {
                    file.close();
                    return;
                }
                offset += written;
            }
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
        writeRecord(session);

        for (;;)
        {
            std::deque<TraceRecord> records;
            quint64 dropped = 0;
            bool closed = false;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->ready.wait(lock, [&state]()
                {
                    return state->closed || !state->records.empty() || state->dropped != 0;
                });
                records.swap(state->records);
                dropped = std::exchange(state->dropped, 0);
                closed = state->closed;
            }
            if (dropped != 0)
            {
                QJsonObject loss;
                loss.insert(QStringLiteral("event"), QStringLiteral("dropped"));
                loss.insert(QStringLiteral("session_id"), state->sessionId);
                loss.insert(QStringLiteral("count"), QString::number(dropped));
                writeRecord(loss);
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
                writeRecord(record);
            }
            if (file.isOpen())
                file.flush();
            if (closed)
                return;
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
                    try { writeTrace(state); }
                    catch (...)
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->closed = true;
                    }
                }).detach();
            }
            catch (...)
            {
                m_writer->closed = true; // Diagnostics must not fail a product operation.
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
                    m_writer->records.push_back(std::move(record));
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
