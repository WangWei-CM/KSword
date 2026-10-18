#include "./PerfTrace.h"

// 采集实现整体只在开启时参与编译；关闭时头文件提供内联空实现，本翻译单元为空。
#if KSWORD_PERF_TRACE_ENABLED

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
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
#include <utility>

namespace
{
    // 全局单调时钟：日志时间轴、分片计时与停顿记录共用同一基准，保证跨模块时间可比。
    QElapsedTimer& referenceClock()
    {
        static QElapsedTimer clock;
        if (!clock.isValid())
        {
            clock.start();
        }
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
            qint64 durationMs, qint64 itemCount)
        {
            QJsonObject record;
            fillCommon(record, operationToken, targetLabel);
            record.insert(QStringLiteral("event"), QStringLiteral("span"));
            record.insert(QStringLiteral("ms"), static_cast<double>(durationMs));
            if (itemCount >= 0)
            {
                record.insert(QStringLiteral("count"), static_cast<double>(itemCount));
            }
            enqueue(record);
        }

        // 记录一次标记，不计时；停顿记录会带上最近一次标记，便于对齐用户操作。
        void recordMark(const char* operationToken, const QString& targetLabel)
        {
            {
                QMutexLocker locker(&m_mutex);
                m_lastMark = QString::fromUtf8(operationToken) + QLatin1Char(':') + targetLabel;
            }
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

            m_heartbeatClock.start();
            m_heartbeat.setTimerType(Qt::PreciseTimer);
            m_heartbeat.setInterval(kHeartbeatIntervalMs);
            QObject::connect(&m_heartbeat, &QTimer::timeout, this, [this]() { onHeartbeat(); });
            m_heartbeat.start();

            // 会话头：把进程版本和采集起点写进文件，便于把多份日志区分开。
            QJsonObject session;
            session.insert(QStringLiteral("t"), 0.0);
            session.insert(QStringLiteral("event"), QStringLiteral("session"));
            session.insert(QStringLiteral("op"), QStringLiteral("perf_session_start"));
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
            const qint64 elapsed = m_heartbeatClock.restart();
            const qint64 overrun = elapsed - kHeartbeatIntervalMs;
            if (overrun < kStallThresholdMs || overrun > kStallIgnoreMs)
            {
                return;
            }
            QJsonObject record;
            fillCommon(record, "perf_ui_stall", QString());
            record.insert(QStringLiteral("event"), QStringLiteral("stall"));
            record.insert(QStringLiteral("ms"), static_cast<double>(overrun));
            enqueue(record);
        }

        // 填充公共字段：时间轴、操作记号、目标与现场上下文。
        void fillCommon(QJsonObject& record, const char* operationToken, const QString& targetLabel)
        {
            record.insert(QStringLiteral("t"), static_cast<double>(referenceClock().elapsed()));
            record.insert(QStringLiteral("op"), QString::fromUtf8(operationToken));
            if (!targetLabel.isEmpty())
            {
                record.insert(QStringLiteral("target"), targetLabel);
            }

            QString lastMark;
            std::function<QString()> provider;
            {
                QMutexLocker locker(&m_mutex);
                lastMark = m_lastMark;
                provider = m_contextProvider;
            }
            // 上下文提供者可能读取界面状态，必须在释放锁之后再调用，避免反向加锁。
            if (provider)
            {
                const QString context = provider();
                if (!context.isEmpty())
                {
                    record.insert(QStringLiteral("ctx"), context);
                }
            }
            if (!lastMark.isEmpty())
            {
                record.insert(QStringLiteral("last"), lastMark);
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
        QElapsedTimer m_heartbeatClock; // 上一次心跳到现在的实际经过时间。
        QString m_lastMark;             // 最近一次用户操作标记。
        std::function<QString()> m_contextProvider; // 现场上下文提供者。
    };

    void RecordPerfSpan(const char* operationToken, const QString& targetLabel, qint64 durationMs)
    {
        PerfTrace::instance().recordSpan(operationToken, targetLabel, durationMs, -1);
    }

    void RecordPerfSpanWithCount(const char* operationToken, const QString& targetLabel,
        qint64 durationMs, qint64 itemCount)
    {
        PerfTrace::instance().recordSpan(operationToken, targetLabel, durationMs, itemCount);
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
        m_clock.start();
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
            m_operationToken, m_targetLabel, m_clock.elapsed(), m_itemCount);
    }
}

#endif
