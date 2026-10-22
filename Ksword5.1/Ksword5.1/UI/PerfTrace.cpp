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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTabWidget>
#include <QWidget>
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
    thread_local ks::ui::PerfTraceContext currentTraceContext;

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
        quint64 actionId = 0, requestId = 0, parentRequestId = 0;
        quintptr threadId = 0;
        QString origin;
        ks::ui::PerfNavigationTrace navigation;
        QString navigationStage, readinessBoundary, outcome;
        QJsonObject conditions;
        quintptr moduleExportAddress = 0;
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

    // This is an artifact association, not an attestation. Source identity is
    // accepted only from a bounded sidecar whose main hash matches these bytes.
    QJsonObject readBuildIdentity(const QString& executablePath, const QString& executableSha256)
    {
        if (executableSha256.isEmpty()) return {{QStringLiteral("status"), QStringLiteral("exe_hash_unavailable")}};
        const QFileInfo executable(executablePath);
        const QDir directory = executable.absoluteDir();
        for (const QString& candidate : {directory.filePath(QStringLiteral("build-info.json")),
                 directory.filePath(QStringLiteral("../build-info.json"))})
        {
            QFile sidecar(candidate);
            if (!sidecar.open(QIODevice::ReadOnly) || sidecar.size() > 2 * 1024 * 1024) continue;
            const QJsonObject info = QJsonDocument::fromJson(sidecar.read(2 * 1024 * 1024 + 1)).object();
            const QString commit = info.value(QStringLiteral("source_commit")).toString();
            const bool validCommit = commit.size() == 40 && std::all_of(commit.cbegin(), commit.cend(), [](QChar ch) {
                return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                    || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
                    || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
            });
            if (!validCommit) continue;
            int mainEntries = 0;
            bool matchingHash = false;
            for (const QJsonValue& entry : info.value(QStringLiteral("files")).toArray())
            {
                const QJsonObject file = entry.toObject();
                if (file.value(QStringLiteral("path")).toString() != executable.fileName()) continue;
                ++mainEntries;
                matchingHash = file.value(QStringLiteral("sha256")).toString().compare(executableSha256, Qt::CaseInsensitive) == 0;
            }
            if (mainEntries != 1 || !matchingHash) continue;
            QJsonObject identity{{QStringLiteral("status"), QStringLiteral("artifact_sha256_matched")},
                {QStringLiteral("commit"), commit}, {QStringLiteral("manifest_path"), QFileInfo(candidate).absoluteFilePath()},
                {QStringLiteral("dirty_scope"), QStringLiteral("tracked_source_at_build_boundaries")}};
            const QJsonValue dirty = info.value(QStringLiteral("source_dirty"));
            identity.insert(QStringLiteral("dirty"), dirty.isBool() ? dirty : QJsonValue(QStringLiteral("unavailable")));
            return identity;
        }
        return {{QStringLiteral("status"), QStringLiteral("matching_manifest_unavailable")}};
    }

    QJsonObject readLoadedModuleIdentity(quintptr exportAddress)
    {
        if (!exportAddress)
            return {{QStringLiteral("status"), QStringLiteral("export_address_unavailable")}};
        struct ModuleReference
        {
            HMODULE value = nullptr;
            ~ModuleReference() { if (value) ::FreeLibrary(value); }
        } module;
        // Hold the module through path resolution and file observation. Never
        // load another DLL, guess a basename, or call the captured export.
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(exportAddress), &module.value))
            return {{QStringLiteral("status"), QStringLiteral("module_lookup_failed")},
                {QStringLiteral("win32_error"), static_cast<double>(::GetLastError())}};
        QString path;
        for (int capacity = 512; capacity <= 32768; capacity *= 2)
        {
            path.resize(capacity);
            const DWORD length = ::GetModuleFileNameW(module.value,
                reinterpret_cast<LPWSTR>(path.data()), static_cast<DWORD>(capacity));
            if (!length)
                return {{QStringLiteral("status"), QStringLiteral("module_path_failed")},
                    {QStringLiteral("win32_error"), static_cast<double>(::GetLastError())}};
            if (length < static_cast<DWORD>(capacity)) { path.resize(length); break; }
            if (capacity == 32768)
                return {{QStringLiteral("status"), QStringLiteral("module_path_too_long")}};
        }
        QJsonObject identity{{QStringLiteral("status"), QStringLiteral("loaded_module_path_resolved")},
            {QStringLiteral("lookup"), QStringLiteral("imported_export_address")},
            {QStringLiteral("path"), path},
            {QStringLiteral("hash_scope"), QStringLiteral("file_at_observation_time")},
            {QStringLiteral("mapped_image_hash_measured"), false}};
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            identity.insert(QStringLiteral("file_status"), QStringLiteral("open_failed"));
            return identity;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            identity.insert(QStringLiteral("file_status"), QStringLiteral("read_failed"));
        else
        {
            identity.insert(QStringLiteral("file_status"), QStringLiteral("hashed"));
            identity.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
            identity.insert(QStringLiteral("bytes"), static_cast<double>(file.size()));
        }
        return identity;
    }

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
        session.insert(QStringLiteral("pid"), static_cast<double>(::GetCurrentProcessId()));
        // Bound the clock alignment with external Windows ETW/WPR evidence.
        // This is a calibration, not a measured display/presentation event.
        LARGE_INTEGER frequency{}, qpc{};
        const auto syncBefore = referenceClock().nsecsElapsed();
        if (::QueryPerformanceFrequency(&frequency) && ::QueryPerformanceCounter(&qpc))
        {
            const auto syncAfter = referenceClock().nsecsElapsed();
            session.insert(QStringLiteral("qpc_frequency"), QString::number(frequency.QuadPart));
            session.insert(QStringLiteral("clock_sync_qpc"), QString::number(qpc.QuadPart));
            session.insert(QStringLiteral("clock_sync_before_ns"), static_cast<double>(syncBefore));
            session.insert(QStringLiteral("clock_sync_after_ns"), static_cast<double>(syncAfter));
        }
        session.insert(QStringLiteral("exe"), state->executable);
        session.insert(QStringLiteral("writer_thread"), QString::number(
            reinterpret_cast<quintptr>(QThread::currentThreadId())));
        QString executableSha256;
        QFile executable(state->executable);
        if (executable.open(QIODevice::ReadOnly))
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&executable))
            {
                executableSha256 = QString::fromLatin1(hash.result().toHex());
                session.insert(QStringLiteral("exe_sha256"), executableSha256);
            }
        }
        session.insert(QStringLiteral("source_identity"), readBuildIdentity(state->executable, executableSha256));
        QFile ads(QFileInfo(state->executable).absoluteDir().filePath(QStringLiteral("qtadvanceddocking.dll")));
        if (ads.open(QIODevice::ReadOnly))
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&ads)) session.insert(QStringLiteral("ads_runtime_sha256"), QString::fromLatin1(hash.result().toHex()));
        }
        // Retain the old field for existing readers, with its limited scope
        // explicit. The loaded_module record below is the module-path witness.
        session.insert(QStringLiteral("ads_runtime_sha256_scope"), QStringLiteral("exe_directory_file_only"));
        session.insert(QStringLiteral("physical_present_measured"), false);
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
                record.insert(QStringLiteral("action_id"), QString::number(value.actionId));
                record.insert(QStringLiteral("request_id"), QString::number(value.requestId));
                record.insert(QStringLiteral("thread_id"), QString::number(value.threadId));
                if (value.parentRequestId)
                    record.insert(QStringLiteral("parent_request_id"), QString::number(value.parentRequestId));
                if (!value.origin.isEmpty()) record.insert(QStringLiteral("origin"), value.origin);
                if (value.navigation.id)
                {
                    const auto& navigation = value.navigation;
                    record.insert(QStringLiteral("navigation_id"), QString::number(navigation.id));
                    record.insert(QStringLiteral("navigation_kind"), navigation.kind);
                    record.insert(QStringLiteral("navigation_stage"), value.navigationStage);
                    record.insert(QStringLiteral("target_id"), navigation.target.id);
                    record.insert(QStringLiteral("target_path"), navigation.target.path);
                    record.insert(QStringLiteral("target_index"), navigation.target.index);
                    record.insert(QStringLiteral("target_identity_scope"), navigation.target.identityScope);
                    record.insert(QStringLiteral("target_path_complete"), navigation.target.pathComplete);
                    record.insert(QStringLiteral("readiness_boundary"), value.readinessBoundary);
                    record.insert(QStringLiteral("outcome"), value.outcome);
                    record.insert(QStringLiteral("operation_scope"), QStringLiteral("navigation_shell_only"));
                    record.insert(QStringLiteral("business_ready_proven"), false);
                    record.insert(QStringLiteral("present_proven"), false);
                }
                if (!value.conditions.isEmpty()) record.insert(QStringLiteral("conditions"), value.conditions);
                if (value.event == QStringLiteral("loaded_module"))
                    record.insert(QStringLiteral("module"), readLoadedModuleIdentity(value.moduleExportAddress));
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
                if (value.eventType >= 0) record.insert(QStringLiteral("event_type"), value.eventType);
                if (value.event == QStringLiteral("notify_dispatch"))
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

        void recordConditions(const QJsonObject& conditions)
        {
            auto record = makeRecord("runtime_conditions", "perf_runtime_conditions", QString());
            record.conditions = conditions;
            enqueue(std::move(record));
        }

        void recordModule(const char* role, quintptr exportAddress)
        {
            auto record = makeRecord("loaded_module", "perf_loaded_module", QString::fromLatin1(role));
            record.moduleExportAddress = exportAddress;
            enqueue(std::move(record));
        }

        quint64 nextNotifyId() { return m_nextScopeId.fetch_add(1, std::memory_order_relaxed); }

        void recordNavigation(const PerfNavigationTrace& navigation, const char* stage,
            const char* boundary, const char* outcome)
        {
            auto record = makeRecord("navigation_stage", "perf_navigation", navigation.target.label);
            record.actionId = navigation.context.actionId;
            record.requestId = navigation.context.requestId;
            record.origin = navigation.origin;
            record.navigation = navigation;
            record.navigationStage = QString::fromLatin1(stage);
            record.readinessBoundary = QString::fromLatin1(boundary);
            record.outcome = QString::fromLatin1(outcome);
            record.beginNs = navigation.beganAtNs;
            record.endNs = record.atNs;
            enqueue(std::move(record));
        }

        void recordAsync(const char* stage, PerfTraceContext context, qint64 count,
            qint64 beganAtNs, quint64 parentRequestId = 0)
        {
            auto record = makeRecord("async_stage", stage, QString());
            record.actionId = context.actionId;
            record.requestId = context.requestId;
            record.parentRequestId = parentRequestId;
            record.count = count;
            if (beganAtNs >= 0)
            {
                record.beginNs = beganAtNs;
                record.endNs = record.atNs;
                record.durationNs = std::max<qint64>(0, record.endNs - beganAtNs);
            }
            enqueue(std::move(record));
        }

        void recordInput(bool begin, PerfTraceContext context, const QString& target,
            int eventType, bool spontaneous)
        {
            auto record = makeRecord(begin ? "input_begin" : "input_dispatch_end",
                "perf_input_dispatch", target);
            record.actionId = context.actionId;
            record.requestId = context.requestId;
            record.eventType = eventType;
            record.origin = spontaneous ? QStringLiteral("qt_spontaneous_input")
                : QStringLiteral("qt_synthetic_input");
            enqueue(std::move(record));
        }

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
            record.actionId = record.requestId = 0; // No single action owns the preceding interval.
            enqueue(std::move(record));
        }

        TraceRecord makeRecord(const char* event, const char* operation, const QString& target)
        {
            TraceRecord record;
            record.atNs = referenceClock().nsecsElapsed();
            record.event = QString::fromLatin1(event);
            record.operation = QString::fromUtf8(operation);
            record.target = target;
            record.actionId = currentTraceContext.actionId;
            record.requestId = currentTraceContext.requestId;
            {
                std::lock_guard<std::mutex> lock(m_contextMutex);
                record.context = m_cachedContext;
            }
            return record;
        }

        void enqueue(TraceRecord record)
        {
            record.threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
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
                    if (queued.event == QStringLiteral("notify_dispatch"))
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

    void RecordPerfRuntimeConditions(const QJsonObject& conditions) noexcept
    {
        try { PerfTrace::instance().recordConditions(conditions); }
        catch (...) {} // Observability must not alter the original UI action.
    }

    void RecordPerfLoadedModule(const char* role, quintptr exportAddress) noexcept
    {
        try { PerfTrace::instance().recordModule(role ? role : "", exportAddress); }
        catch (...) {}
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

    PerfTraceContext CapturePerfTraceContext() noexcept { return currentTraceContext; }

    PerfNavigationTarget CapturePerfNavigationTarget(QWidget* container, QWidget* target, int index) noexcept
    {
        PerfNavigationTarget result;
        try
        {
            result.index = index;
            if (!target) { result.identityScope = QStringLiteral("unavailable"); return result; }
            result.label = target->windowTitle();
            if (auto* tabs = qobject_cast<QTabWidget*>(container); tabs && tabs->widget(index) == target)
                result.label = tabs->tabText(index);
            QStringList parts;
            bool structural = false;
            // Only this target's ancestry and bounded direct siblings, never a
            // descendant/tree walk or construction of hidden navigation pages.
            for (QWidget* widget = target; widget && parts.size() < 32; widget = widget->parentWidget())
            {
                QString key = widget->property("ks_lazy_key").toString();
                const bool dockKey = !key.isEmpty();
                if (!dockKey) key = widget->objectName();
                QString component = QString::fromUtf8(widget->metaObject()->className());
                if (!key.isEmpty())
                {
                    key.replace(QLatin1Char('%'), QStringLiteral("%25"));
                    key.replace(QLatin1Char('/'), QStringLiteral("%2F"));
                    component += QLatin1Char(':') + key;
                }
                else
                {
                    structural = true;
                    int ordinal = 0, visited = 0;
                    bool found = !widget->parent();
                    if (QObject* parent = widget->parent())
                        for (QObject* sibling : parent->children())
                        {
                            if (++visited > 256) break;
                            if (sibling == widget) { found = true; break; }
                            if (sibling->metaObject() == widget->metaObject()) ++ordinal;
                        }
                    component += QLatin1Char('#') + QString::number(ordinal);
                    if (!found) { parts.prepend(component); break; }
                }
                parts.prepend(component);
                if (dockKey || widget->isWindow()) { result.pathComplete = true; break; }
            }
            result.path = parts.join(QLatin1Char('/'));
            result.id = result.path;
            result.identityScope = structural ? QStringLiteral("structural_path_same_hierarchy")
                : QStringLiteral("named_object_path");
        }
        catch (...) { result.pathComplete = false; }
        return result;
    }

    PerfNavigationTrace BeginPerfNavigationTrace(const char* kind, PerfNavigationTarget target,
        const char* stage, const char* origin) noexcept
    {
        PerfNavigationTrace navigation;
        try
        {
            if (!notifyTraceReady.load(std::memory_order_relaxed)) return navigation;
            navigation.context = currentTraceContext;
            navigation.id = PerfTrace::instance().nextNotifyId();
            navigation.target = std::move(target);
            navigation.kind = QString::fromLatin1(kind);
            if (origin) navigation.origin = QString::fromLatin1(origin);
            else switch (navigation.context.origin)
            {
            case PerfTraceOrigin::SpontaneousInput: navigation.origin = QStringLiteral("qt_spontaneous_input_context"); break;
            case PerfTraceOrigin::SyntheticInput: navigation.origin = QStringLiteral("qt_synthetic_input_context"); break;
            case PerfTraceOrigin::ProgrammaticNavigation: navigation.origin = QStringLiteral("programmatic_navigation"); break;
            default: navigation.origin = QStringLiteral("unproven"); break;
            }
            navigation.beganAtNs = referenceClock().nsecsElapsed();
            PerfTrace::instance().recordNavigation(navigation, stage, "selection_or_preparation_entry", "observed");
        }
        catch (...) { navigation.id = 0; }
        return navigation;
    }

    void RecordPerfNavigationStage(const PerfNavigationTrace& navigation, const char* stage,
        const char* boundary, const char* outcome) noexcept
    {
        try
        {
            if (navigation.id && notifyTraceReady.load(std::memory_order_relaxed))
                PerfTrace::instance().recordNavigation(navigation, stage, boundary, outcome);
        }
        catch (...) {}
    }

    PerfTraceContext BeginPerfAsyncTrace() noexcept
    {
        try
        {
            if (!notifyTraceReady.load(std::memory_order_relaxed)) return {};
            auto context = currentTraceContext;
            const auto parentRequest = context.requestId;
            context.requestId = PerfTrace::instance().nextNotifyId();
            PerfTrace::instance().recordAsync("perf_async_requested", context, -1, -1, parentRequest);
            return context;
        }
        catch (...) { return {}; }
    }

    void RecordPerfAsyncTrace(const char* stage, PerfTraceContext context,
        qint64 count, qint64 beganAtNs) noexcept
    {
        try
        {
            if (notifyTraceReady.load(std::memory_order_relaxed) && (context.requestId || context.actionId))
                PerfTrace::instance().recordAsync(stage, context, count, beganAtNs);
        }
        catch (...) {}
    }

    PerfTraceContextScope::PerfTraceContextScope(PerfTraceContext context) noexcept
        : m_previous(currentTraceContext) { currentTraceContext = context; }
    PerfTraceContextScope::~PerfTraceContextScope() noexcept { currentTraceContext = m_previous; }

    PerfNotifyScope::PerfNotifyScope(QObject* receiver, const int eventType, const QEvent* event) noexcept
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
            m_previousContext = m_context = currentTraceContext;
            const bool input = eventType == QEvent::MouseButtonPress || eventType == QEvent::MouseMove || eventType == QEvent::MouseButtonRelease
                || eventType == QEvent::MouseButtonDblClick || eventType == QEvent::KeyPress
                || eventType == QEvent::KeyRelease || eventType == QEvent::Wheel
                || eventType == QEvent::TouchBegin || eventType == QEvent::TouchUpdate
                || eventType == QEvent::TouchEnd || eventType == QEvent::TabletPress
                || eventType == QEvent::TabletMove || eventType == QEvent::TabletRelease;
            if (input)
            {
                m_inputEvent = event;
                // Qt can forward the same event through multiple notify calls.
                // A nested event loop's distinct input must get its own ID.
                m_inputAction = !event || !m_parent || m_parent->m_inputEvent != event;
                if (m_inputAction)
                {
                    m_context = {PerfTrace::instance().nextNotifyId(), 0};
                    m_inputSpontaneous = event && event->spontaneous();
                    m_context.origin = m_inputSpontaneous ? PerfTraceOrigin::SpontaneousInput : PerfTraceOrigin::SyntheticInput;
                    PerfTrace::instance().recordInput(true, m_context, m_target, eventType, m_inputSpontaneous);
                }
            }
            const auto cpu = sampleNotifyCpu();
            m_startedCycles = cpu.cycles;
            m_startedCpu100ns = cpu.time100ns;
            m_cyclesValid = cpu.cyclesValid;
            m_cpuValid = cpu.timeValid;
            m_startedAtNs = referenceClock().nsecsElapsed();
            m_setupNs = m_startedAtNs - setupStarted;
            currentNotifyScope = this;
            currentTraceContext = m_context;
            m_active = true;
        }
        catch (...) {} // Diagnostics must not change event delivery.
    }

    PerfNotifyScope::~PerfNotifyScope() noexcept
    {
        if (!m_active) return;
        currentNotifyScope = m_parent;
        currentTraceContext = m_previousContext;
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
            record.actionId = m_context.actionId;
            record.requestId = m_context.requestId;
            if (m_inputAction)
                PerfTrace::instance().recordInput(false, m_context, m_target, m_eventType, m_inputSpontaneous);
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

    PerfScope::PerfScope(const char* operation, QString target) noexcept
        : m_operationToken(operation), m_targetLabel(std::move(target))
    {
        if (!operation) { m_finished = true; return; }
        try
        {
            m_context = currentTraceContext;
            m_startedAtNs = referenceClock().nsecsElapsed();
            m_scopeId = PerfTrace::instance().beginScope(m_operationToken, m_targetLabel, m_startedAtNs);
        }
        catch (...) { m_finished = true; }
    }

    PerfScope::~PerfScope() noexcept { finish(); }

    void PerfScope::finish() noexcept
    {
        if (m_finished)
            return;
        m_finished = true;
        try
        {
            PerfTraceContextScope context(m_context);
            PerfTrace::instance().recordSpan(m_operationToken, m_targetLabel,
                referenceClock().nsecsElapsed() - m_startedAtNs, m_itemCount, m_scopeId, m_startedAtNs);
        }
        catch (...) {}
    }
}
#endif
