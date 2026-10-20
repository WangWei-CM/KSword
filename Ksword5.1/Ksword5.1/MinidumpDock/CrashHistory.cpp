#include "CrashHistory.h"

#include <QHash>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
#include <vector>

#include <windows.h>
#include <winevt.h>

#pragma comment(lib, "wevtapi.lib")

namespace ks::minidump
{
    namespace
    {
        // kMaxEvents：单次查询最多取多少条。崩溃是低频事件，这个上限足够，
        // 同时挡住日志异常膨胀时把界面塞满。
        constexpr int kMaxEvents = 200;
        constexpr DWORD kEventWaitMs = 100;

        struct ScopedEventHandle
        {
            EVT_HANDLE value = nullptr;
            ~ScopedEventHandle() { if (value != nullptr) { ::EvtClose(value); } }
        };

        struct ScopedEventBatch
        {
            std::array<EVT_HANDLE, 16> values{};
            ~ScopedEventBatch()
            {
                for (EVT_HANDLE value : values)
                {
                    if (value != nullptr) { ::EvtClose(value); }
                }
            }
        };

        void SetQueryError(QString* errorOut, const DWORD error)
        {
            if (errorOut != nullptr && errorOut->isEmpty())
            {
                *errorOut = QStringLiteral("查询系统事件日志失败，错误码 %1。").arg(error);
            }
        }

        // EventFields：一条事件里我们关心的内容。
        struct EventFields
        {
            QDateTime time;              // time：事件时间（已转本地时区）。
            QHash<QString, QString> data; // data：EventData 里具名字段。
            QStringList unnamed;         // unnamed：没有 Name 属性的 Data 值，按顺序。
        };

        // ParseEventXml 作用：从渲染出的事件 XML 里取出时间与 EventData。
        // 传入 xml 事件 XML 文本；返回解析结果，时间无效表示这条不可用。
        EventFields ParseEventXml(const QString& xml)
        {
            EventFields fields; // fields：待返回的字段集合。
            QXmlStreamReader reader(xml);
            bool inEventData = false; // inEventData：当前是否位于 EventData 元素内。

            while (!reader.atEnd())
            {
                CheckDumpCancellation();
                reader.readNext();
                if (reader.isStartElement())
                {
                    const QStringView name = reader.name();
                    if (name == QLatin1String("TimeCreated"))
                    {
                        const QString raw =
                            reader.attributes().value(QLatin1String("SystemTime")).toString();
                        // 事件时间是 UTC 的 ISO8601，转本地时区后才和用户的记忆对得上。
                        QDateTime parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
                        if (!parsed.isValid())
                        {
                            parsed = QDateTime::fromString(raw, Qt::ISODate);
                        }
                        if (parsed.isValid())
                        {
                            parsed.setTimeZone(QTimeZone::UTC);
                            fields.time = parsed.toLocalTime();
                        }
                    }
                    else if (name == QLatin1String("EventData"))
                    {
                        inEventData = true;
                    }
                    else if (inEventData && name == QLatin1String("Data"))
                    {
                        const QString key =
                            reader.attributes().value(QLatin1String("Name")).toString();
                        const QString value = reader.readElementText();
                        if (key.isEmpty())
                        {
                            fields.unnamed.append(value);
                        }
                        else
                        {
                            fields.data.insert(key, value);
                        }
                    }
                }
                else if (reader.isEndElement() &&
                         reader.name() == QLatin1String("EventData"))
                {
                    inEventData = false;
                }
            }
            return fields;
        }

        // QueryEvents 作用：按 XPath 查询系统日志并逐条渲染成 XML 后解析。
        // 传入 query XPath 查询串与 errorOut 失败原因输出；返回解析出的事件字段。
        std::vector<EventFields> QueryEvents(const QString& query, QString* const errorOut)
        {
            std::vector<EventFields> events; // events：解析结果。
            CheckDumpCancellation();

            const std::wstring queryBuffer = query.toStdWString();
            // EvtQueryReverseDirection：从最新往回读，配合 kMaxEvents 才能拿到最近的记录。
            const ScopedEventHandle handle{ ::EvtQuery(
                nullptr,
                L"System",
                queryBuffer.c_str(),
                EvtQueryChannelPath | EvtQueryReverseDirection) };
            if (handle.value == nullptr)
            {
                SetQueryError(errorOut, ::GetLastError());
                return events;
            }
            CheckDumpCancellation();

            std::vector<wchar_t> buffer(4096); // buffer：EvtRender 的输出缓冲，按需扩容。
            while (static_cast<int>(events.size()) < kMaxEvents)
            {
                CheckDumpCancellation();
                ScopedEventBatch batch;
                DWORD returned = 0;
                if (::EvtNext(handle.value, 16, batch.values.data(), kEventWaitMs, 0, &returned) == FALSE)
                {
                    const DWORD error = ::GetLastError();
                    CheckDumpCancellation();
                    if (error == ERROR_TIMEOUT)
                    {
                        continue;
                    }
                    if (error != ERROR_NO_MORE_ITEMS)
                    {
                        SetQueryError(errorOut, error);
                    }
                    break;
                }
                CheckDumpCancellation();
                for (DWORD index = 0; index < returned; ++index)
                {
                    CheckDumpCancellation();
                    DWORD used = 0;
                    DWORD properties = 0;
                    BOOL rendered = ::EvtRender(
                        nullptr,
                        batch.values[index],
                        EvtRenderEventXml,
                        static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                        buffer.data(),
                        &used,
                        &properties);
                    if (rendered == FALSE &&
                        ::GetLastError() == ERROR_INSUFFICIENT_BUFFER)
                    {
                        CheckDumpCancellation();
                        buffer.resize((used / sizeof(wchar_t)) + 1);
                        rendered = ::EvtRender(
                            nullptr,
                            batch.values[index],
                            EvtRenderEventXml,
                            static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                            buffer.data(),
                            &used,
                            &properties);
                    }
                    CheckDumpCancellation();
                    if (rendered != FALSE)
                    {
                        EventFields fields =
                            ParseEventXml(QString::fromWCharArray(buffer.data()));
                        if (fields.time.isValid())
                        {
                            events.push_back(std::move(fields));
                        }
                    }
                    CheckDumpCancellation();
                }
                if (returned == 0)
                {
                    break;
                }
            }
            CheckDumpCancellation();
            return events;
        }

        // BuildQuery 作用：拼出带时间窗口的 XPath 查询串。
        // 传入 provider 提供程序名、eventId 事件 ID、windowMs 时间窗口毫秒数。
        QString BuildQuery(
            const QString& provider,
            const int eventId,
            const qint64 windowMs)
        {
            return QStringLiteral(
                       "*[System[Provider[@Name='%1'] and (EventID=%2) and "
                       "TimeCreated[timediff(@SystemTime) <= %3]]]")
                .arg(provider)
                .arg(eventId)
                .arg(windowMs);
        }
    }

    QString CrashEventKindText(const CrashEventKind kind)
    {
        switch (kind)
        {
        case CrashEventKind::BugCheck:
            return QStringLiteral("蓝屏");
        case CrashEventKind::HardHang:
            return QStringLiteral("硬挂死（无转储）");
        case CrashEventKind::BugCheckReboot:
            return QStringLiteral("蓝屏后重启");
        case CrashEventKind::FilterLoad:
        default:
            return QStringLiteral("筛选器加载");
        }
    }

    std::vector<CrashHistoryEntry> CollectCrashHistory(
        const int maxDays,
        const QString& filterNameFilter,
        QString* const errorOut, const DumpCancellationToken& cancellation)
    {
        const ScopedDumpCancellation cancellationScope(cancellation);
        CheckDumpCancellation();
        std::vector<CrashHistoryEntry> entries; // entries：时间线记录。
        if (errorOut != nullptr)
        {
            errorOut->clear();
        }

        const int days = (maxDays <= 0) ? 30 : maxDays;
        const qint64 windowMs = static_cast<qint64>(days) * 24LL * 3600LL * 1000LL;

        // 一、蓝屏记录：param1 是停止码与四个参数，param2 是转储文件路径。
        for (const EventFields& fields : QueryEvents(
                 BuildQuery(
                     QStringLiteral("Microsoft-Windows-WER-SystemErrorReporting"),
                     1001,
                     windowMs),
                 errorOut))
        {
            CrashHistoryEntry entry;
            entry.time = fields.time;
            entry.kind = CrashEventKind::BugCheck;
            entry.kindText = CrashEventKindText(entry.kind);
            const QString parameters = fields.data.value(QStringLiteral("param1"));
            entry.dumpPath = fields.data.value(QStringLiteral("param2"));
            entry.summary = parameters;
            entry.detail = entry.dumpPath;
            // 从 "0x00000050 (...)" 里取出停止码本身，便于与转储对账。
            const int spaceAt = parameters.indexOf(QLatin1Char(' '));
            const QString codeText =
                (spaceAt > 0) ? parameters.left(spaceAt) : parameters;
            bool converted = false;
            const std::uint32_t code =
                codeText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                    ? codeText.mid(2).toUInt(&converted, 16)
                    : codeText.toUInt(&converted, 16);
            entry.bugCheckCode = converted ? code : 0;
            entries.push_back(entry);
        }

        // 二、非正常关机：BugcheckCode 为 0 就是没有转储的硬挂死。
        // 这一条是本模块最有价值的判据——没有它，硬挂死会被误当成"蓝屏但没存下转储"，
        // 于是有人会一直去找一个根本不存在的转储文件。
        for (const EventFields& fields : QueryEvents(
                 BuildQuery(
                     QStringLiteral("Microsoft-Windows-Kernel-Power"),
                     41,
                     windowMs),
                 errorOut))
        {
            CrashHistoryEntry entry;
            entry.time = fields.time;
            const QString codeText = fields.data.value(QStringLiteral("BugcheckCode"));
            const std::uint32_t code = codeText.toUInt();
            entry.bugCheckCode = code;
            if (code == 0)
            {
                entry.kind = CrashEventKind::HardHang;
                entry.summary = QStringLiteral(
                    "系统失去响应后被复位，未产生停止码，因此不会有转储文件。");
            }
            else
            {
                entry.kind = CrashEventKind::BugCheckReboot;
                entry.summary = QStringLiteral("非正常关机，停止码 0x%1。")
                                    .arg(code, 8, 16, QLatin1Char('0'));
            }
            entry.kindText = CrashEventKindText(entry.kind);
            // 电源键时间戳与长按标志能区分"人为断电"和"系统自己挂死后复位"。
            entry.detail =
                QStringLiteral("电源键时间戳 %1；长按电源键 %2")
                    .arg(fields.data.value(QStringLiteral("PowerButtonTimestamp")),
                         fields.data.value(QStringLiteral("LongPowerButtonPressDetected")));
            entries.push_back(entry);
        }

        // 三、筛选器加载记录：DeviceTime 就是驱动映像的时间戳。
        // 崩溃后改了代码重新编译加载，靠它才能确认"崩的到底是不是修复版"。
        if (!filterNameFilter.trimmed().isEmpty())
        {
            for (const EventFields& fields : QueryEvents(
                     BuildQuery(
                         QStringLiteral("Microsoft-Windows-FilterManager"),
                         6,
                         windowMs),
                     errorOut))
            {
                const QString deviceName = fields.data.value(QStringLiteral("DeviceName"));
                if (!deviceName.contains(filterNameFilter.trimmed(), Qt::CaseInsensitive))
                {
                    continue;
                }
                CrashHistoryEntry entry;
                entry.time = fields.time;
                entry.kind = CrashEventKind::FilterLoad;
                entry.kindText = CrashEventKindText(entry.kind);
                entry.summary = QStringLiteral("筛选器 %1 已加载。").arg(deviceName);
                entry.detail = QStringLiteral("映像时间戳 %1")
                                   .arg(fields.data.value(QStringLiteral("DeviceTime")));
                entries.push_back(entry);
            }
        }

        std::sort(
            entries.begin(),
            entries.end(),
            [](const CrashHistoryEntry& left, const CrashHistoryEntry& right)
            { return left.time > right.time; });
        return entries;
    }
}
