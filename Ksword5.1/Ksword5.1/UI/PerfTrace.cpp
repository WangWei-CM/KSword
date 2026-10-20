#include "./PerfTrace.h"

// 采集实现整体只在开启时参与编译；关闭时头文件提供内联空实现，本翻译单元为空。
#if KSWORD_PERF_TRACE_ENABLED

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QUuid>
#include <atomic>
#include <utility>

namespace
{
    // 全局单调时钟：日志时间轴、分片计时与停顿记录共用同一基准，保证跨模块时间可比。
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

    // 批量落盘间隔：两秒一次，避免逐条写入把 UI 线程当成 I/O 线程使用。
    constexpr int kFlushIntervalMs = 2000;
    // 缓冲上限：达到即提前落盘，避免长时间没有定时器回调时丢数据。
    constexpr int kBufferFlushThreshold = 256;
    // 心跳间隔：事件循环在空闲时应当达到的响应周期。
    constexpr int kHeartbeatIntervalMs = 25;
    // 停顿阈值：心跳晚于理论间隔超过该值，记一次 UI 停顿。
    constexpr int kStallThresholdMs = 50;
    // 超大“停顿”更像系统休眠或进程挂起，不作为 UI 卡顿记录。
    constexpr qint64 kStallIgnoreMs = 10000;
    // 向上查找页签栏时的最大父级层数，避免在超深控件树里长时间遍历。
    constexpr int kMaxAncestorWalk = 16;
}

namespace ks::ui
{
    // PerfTrace 作用：采集数据的单例宿主，负责缓冲、批量落盘、心跳停顿检测与页签按下探针。
    // 它不做任何界面改动，也不在热路径上做同步文件写入。
    class PerfTrace final : public QObject
    {
    public:
        static PerfTrace& instance()
        {
            static PerfTrace trace;
            return trace;
        }

        // 记录一次操作边界耗时；itemCount 为负表示不写入计数字段。
        void recordSpan(const char* operationToken, const QString& targetLabel,
            qint64 durationNs, qint64 itemCount, quint64 scopeId = 0, qint64 startedAtNs = -1)
        {
            const qint64 endedAtNs = referenceClock().nsecsElapsed();
            QJsonObject record;
            fillCommon(record, operationToken, targetLabel);
            record.insert(QStringLiteral("event"), QStringLiteral("span"));
            record.insert(QStringLiteral("ms"), durationNs / 1000000.0);
            record.insert(QStringLiteral("duration_ns"), static_cast<double>(durationNs));
            record.insert(QStringLiteral("begin_ns"), static_cast<double>(
                startedAtNs >= 0 ? startedAtNs : endedAtNs - durationNs));
            record.insert(QStringLiteral("end_ns"), static_cast<double>(endedAtNs));
            record.insert(QStringLiteral("measurement"), QStringLiteral("elapsed_wall"));
            if (scopeId != 0)
                record.insert(QStringLiteral("scope_id"), QString::number(scopeId));
            if (itemCount >= 0)
            {
                record.insert(QStringLiteral("count"), static_cast<double>(itemCount));
            }
            enqueue(record);
        }

        quint64 beginScope(const char* operationToken, const QString& targetLabel, qint64 atNs)
        {
            const quint64 id = m_nextScopeId.fetch_add(1, std::memory_order_relaxed);
            QJsonObject record;
            fillCommon(record, operationToken, targetLabel);
            record.insert(QStringLiteral("event"), QStringLiteral("scope_begin"));
            record.insert(QStringLiteral("scope_id"), QString::number(id));
            record.insert(QStringLiteral("begin_ns"), static_cast<double>(atNs));
            enqueue(record);
            return id;
        }

        // 标记仅表示该时间点观察到的状态，不能归因此前整个心跳区间。
        void recordMark(const char* operationToken, const QString& targetLabel)
        {
            QJsonObject record;
            fillCommon(record, operationToken, targetLabel);
            record.insert(QStringLiteral("event"), QStringLiteral("mark"));
            enqueue(record);
        }

        void setContextProvider(std::function<QString()> provider)
        {
            QMutexLocker locker(&m_mutex);
            m_contextProvider = std::move(provider);
        }

        void flush()
        {
            QMutexLocker locker(&m_mutex);
            flushLocked();
        }

    protected:
        // 只观察鼠标按下：页签切换的入口边界在这里，其它事件一律立即放行。
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event != nullptr && watched != nullptr &&
                event->type() == QEvent::MouseButtonPress)
            {
                const QString label = tabBarLabelFor(watched);
                if (!label.isEmpty())
                {
                    recordMark("perf_tab_press", label);
                }
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        PerfTrace()
        {
            if (QCoreApplication* application = QCoreApplication::instance())
            {
                application->installEventFilter(this);
                QObject::connect(application, &QCoreApplication::aboutToQuit,
                    this, [this]() { flush(); });
            }

            m_flushTimer.setTimerType(Qt::CoarseTimer);
            m_flushTimer.setInterval(kFlushIntervalMs);
            QObject::connect(&m_flushTimer, &QTimer::timeout, this, [this]() { flush(); });
            m_flushTimer.start();

            m_previousHeartbeatNs = referenceClock().nsecsElapsed();
            m_heartbeat.setTimerType(Qt::PreciseTimer);
            m_heartbeat.setInterval(kHeartbeatIntervalMs);
            QObject::connect(&m_heartbeat, &QTimer::timeout, this, [this]() { onHeartbeat(); });
            m_heartbeat.start();

            // 会话头：把进程版本和采集起点写进文件，便于把多份日志区分开。
            QJsonObject session;
            session.insert(QStringLiteral("t"), 0.0);
            session.insert(QStringLiteral("event"), QStringLiteral("session"));
            session.insert(QStringLiteral("op"), QStringLiteral("perf_session_start"));
            session.insert(QStringLiteral("session_id"), m_sessionId);
            session.insert(QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            session.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
            session.insert(QStringLiteral("build_stamp"), QStringLiteral(__DATE__ " " __TIME__));
            session.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
            if (QCoreApplication::instance() != nullptr)
            {
                session.insert(QStringLiteral("target"),
                    QCoreApplication::applicationVersion());
            }
            enqueue(session);
        }

        // 心跳回调：事件循环被长时间占住时，定时器的实际间隔会明显超出理论值。
        void onHeartbeat()
        {
            const qint64 endedAtNs = referenceClock().nsecsElapsed();
            const qint64 startedAtNs = std::exchange(m_previousHeartbeatNs, endedAtNs);
            const qint64 overrun = (endedAtNs - startedAtNs) / 1000000 - kHeartbeatIntervalMs;
            if (overrun < kStallThresholdMs || overrun > kStallIgnoreMs)
            {
                return;
            }
            QJsonObject record;
            fillCommon(record, "perf_ui_stall", QString());
            record.insert(QStringLiteral("event"), QStringLiteral("stall"));
            record.insert(QStringLiteral("ms"), static_cast<double>(overrun));
            record.insert(QStringLiteral("begin_ns"), static_cast<double>(startedAtNs));
            record.insert(QStringLiteral("end_ns"), static_cast<double>(endedAtNs));
            record.insert(QStringLiteral("interval_ns"), static_cast<double>(endedAtNs - startedAtNs));
            record.insert(QStringLiteral("measurement"), QStringLiteral("heartbeat_overrun"));
            record.insert(QStringLiteral("attribution"), QStringLiteral("unattributed_interval"));
            enqueue(record);
        }

        // 填充公共字段：时间轴、操作记号、目标与现场上下文。
        void fillCommon(QJsonObject& record, const char* operationToken, const QString& targetLabel)
        {
            record.insert(QStringLiteral("t"), static_cast<double>(referenceClock().elapsed()));
            record.insert(QStringLiteral("t_ns"), static_cast<double>(referenceClock().nsecsElapsed()));
            record.insert(QStringLiteral("session_id"), m_sessionId);
            record.insert(QStringLiteral("op"), QString::fromUtf8(operationToken));
            if (!targetLabel.isEmpty())
            {
                record.insert(QStringLiteral("target"), targetLabel);
            }

            std::function<QString()> provider;
            {
                QMutexLocker locker(&m_mutex);
                provider = m_contextProvider;
            }
            // 上下文提供者可能读取界面状态，必须在释放锁之后再调用，避免反向加锁。
            if (provider)
            {
                const QString context = provider();
                if (!context.isEmpty())
                {
                    record.insert(QStringLiteral("context_at_record_end"), context);
                }
            }
        }

        // 把记录转成单行 JSON 并存入缓冲；缓冲达到上限时提前落盘。
        void enqueue(const QJsonObject& record)
        {
            const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
            QMutexLocker locker(&m_mutex);
            m_lines.append(line);
            if (m_lines.size() >= kBufferFlushThreshold)
            {
                flushLocked();
            }
        }

        // 落盘：调用方必须已持有锁。文件只打开一次，持续以追加方式写入。
        void flushLocked()
        {
            if (m_lines.isEmpty())
            {
                return;
            }
            if (!ensureFileOpen())
            {
                m_lines.clear();
                return;
            }
            for (const QByteArray& line : m_lines)
            {
                m_file.write(line);
            }
            m_file.flush();
            m_lines.clear();
        }

        bool ensureFileOpen()
        {
            if (m_file.isOpen())
            {
                return true;
            }
            // 允许用环境变量指定采集文件位置，便于把多次采集分开归档。
            QString path = qEnvironmentVariable("KSWORD_PERF_TRACE");
            if (path.isEmpty())
            {
                path = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/ksword_perf_trace.jsonl");
            }
            m_file.setFileName(path);
            if (m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            {
                return true;
            }
            // 主程序目录不可写时退回临时目录，保证采集不中断。
            m_file.setFileName(QDir::tempPath() + QStringLiteral("/ksword_perf_trace.jsonl"));
            return m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        }

        // 从事件接收者向上寻找页签栏；普通 QTabBar 与 ADS 页签栏都要识别。
        // 用类名比较而不是包含 ADS 头，避免采集模块依赖停靠系统实现。
        QString tabBarLabelFor(QObject* watched)
        {
            QObject* object = watched;
            for (int depth = 0; object != nullptr && depth < kMaxAncestorWalk; ++depth)
            {
                const QByteArray className = object->metaObject()->className();
                const bool isPlainTabBar = (className == QByteArrayLiteral("QTabBar"));
                const bool isDockTabBar = className.startsWith(QByteArrayLiteral("ads::CDockAreaTabBar"));
                if (isPlainTabBar || isDockTabBar)
                {
                    QString label = QString::fromUtf8(className);
                    const QString objectName = object->objectName();
                    if (!objectName.isEmpty())
                    {
                        label += QLatin1Char('/') + objectName;
                    }
                    return label;
                }
                object = object->parent();
            }
            return QString();
        }

        QMutex m_mutex;                 // 保护缓冲、文件与现场字段，允许后台线程记录。
        QVector<QByteArray> m_lines;    // 待落盘的 JSONL 行。
        QFile m_file;                   // 采集文件，首次落盘时打开并持续追加。
        QTimer m_flushTimer;            // 定时批量落盘。
        QTimer m_heartbeat;             // 事件循环心跳，用于检测 UI 停顿。
        qint64 m_previousHeartbeatNs = 0;
        const QString m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::atomic<quint64> m_nextScopeId{1};
        std::function<QString()> m_contextProvider; // 现场上下文提供者。
    };

    void RecordPerfSpan(const char* operationToken, const QString& targetLabel, qint64 durationMs)
    {
        PerfTrace::instance().recordSpan(operationToken, targetLabel, durationMs * 1000000, -1);
    }

    void RecordPerfSpanWithCount(const char* operationToken, const QString& targetLabel,
        qint64 durationMs, qint64 itemCount)
    {
        PerfTrace::instance().recordSpan(operationToken, targetLabel, durationMs * 1000000, itemCount);
    }

    void RecordPerfSpanNs(const char* operationToken, const QString& targetLabel,
        qint64 durationNs, qint64 itemCount)
    {
        PerfTrace::instance().recordSpan(operationToken, targetLabel, durationNs, itemCount);
    }

    qint64 PerfTraceNowNs()
    {
        return referenceClock().nsecsElapsed();
    }

    void RecordPerfMark(const char* operationToken, const QString& targetLabel)
    {
        PerfTrace::instance().recordMark(operationToken, targetLabel);
    }

    qint64 PerfTraceNowMs()
    {
        return referenceClock().elapsed();
    }

    void SetPerfTraceContextProvider(std::function<QString()> provider)
    {
        PerfTrace::instance().setContextProvider(std::move(provider));
    }

    PerfScope::PerfScope(const char* operationToken, QString targetLabel)
        : m_operationToken(operationToken), m_targetLabel(std::move(targetLabel))
    {
        m_startedAtNs = referenceClock().nsecsElapsed();
        m_scopeId = PerfTrace::instance().beginScope(m_operationToken, m_targetLabel, m_startedAtNs);
    }

    PerfScope::~PerfScope()
    {
        finish();
    }

    void PerfScope::finish()
    {
        if (m_finished)
        {
            return;
        }
        m_finished = true;
        PerfTrace::instance().recordSpan(
            m_operationToken, m_targetLabel, referenceClock().nsecsElapsed() - m_startedAtNs,
            m_itemCount, m_scopeId, m_startedAtNs);
    }
}

#endif
