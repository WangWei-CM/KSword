#include "WinAPIDock.h"
#include "MonitorEventFilterProjection.h"
#include "../UI/TableSearchSupport.h"
#include <QItemSelectionModel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QHeaderView>
#include "../UI/ThemeItemForeground.h"
#include "../theme.h"
#include "../Framework/PrivilegeElevationPrompt.h"
#include "../UI/TableInteractionSupport.h"

// ============================================================
// WinAPIDock.Pipe.cpp
// 作用：
// 1) 实现与 APIMonitor_x64 命名管道服务端的连接与读取；
// 2) 把后台收到的固定长度事件包转换成 UI 可显示的 EventRow；
// 3) 统一处理后台线程退出、连接失败和事件表批量刷新。
// ============================================================

#include <QApplication>
#include <QAbstractItemModel>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QStringConverter>
#include "../../../shared/ApiMonitorInjection.h"
#include <QCryptographicHash>
#include <QMetaObject>
#include <QPointer>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QLineEdit>
#include <QMessageBox>
#include <tuple>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>

namespace
{
    // kApiMonitorPacketSize：
    // - 作用：统一 UI 侧命名管道每次读取的固定包大小；
    // - 调用：主目标管道与自动注入子进程管道复用同一协议结构。
    constexpr DWORD kApiMonitorPacketSize = static_cast<DWORD>(sizeof(ks::winapi_monitor::ApiMonitorEventPacket));

    // packetWideText：
    // - 作用：把固定长度 wchar_t 缓冲转成 QString；
    // - 调用：解析 Agent 回传的 moduleName/apiName/detailText 时复用。
    template <std::size_t kCount>
    QString packetWideText(const wchar_t(&bufferValue)[kCount])
    {
        QString textValue = QString::fromWCharArray(bufferValue, static_cast<int>(kCount));
        const int nullIndex = textValue.indexOf(QChar(u'\0'));
        if (nullIndex >= 0)
        {
            textValue.truncate(nullIndex);
        }
        return textValue.trimmed();
    }

    // packetEventCategoryText：
    // - 作用：把协议分类码转换为 UI 文本；
    // - 调用：Pipe.cpp 本地解析主进程/子进程事件包时使用；
    // - 返回：分类中文名，未知分类返回“未知”。
    QString packetEventCategoryText(const std::uint32_t categoryValue)
    {
        switch (static_cast<ks::winapi_monitor::EventCategory>(categoryValue))
        {
        case ks::winapi_monitor::EventCategory::File:
            return QStringLiteral("文件");
        case ks::winapi_monitor::EventCategory::Registry:
            return QStringLiteral("注册表");
        case ks::winapi_monitor::EventCategory::Network:
            return QStringLiteral("网络");
        case ks::winapi_monitor::EventCategory::Process:
            return QStringLiteral("进程");
        case ks::winapi_monitor::EventCategory::Loader:
            return QStringLiteral("加载器");
        case ks::winapi_monitor::EventCategory::Clipboard:
            return QStringLiteral("剪贴板");
        case ks::winapi_monitor::EventCategory::Internal:
            return QStringLiteral("内部");
        default:
            break;
        }
        return QStringLiteral("未知");
    }

    // packetResultCodeText：
    // - 作用：把 Agent resultCode 转成表格文本；
    // - 调用：Pipe.cpp 本地解析事件包时使用，避免访问 WinAPIDock 私有静态函数；
    // - 返回：0 返回 OK，非 0 返回十进制错误码。
    QString packetResultCodeText(const std::int32_t resultCodeValue)
    {
        return resultCodeValue == 0
            ? QStringLiteral("OK")
            : QString::number(resultCodeValue);
    }

    // packetToEventRow：
    // - 作用：把 Agent 固定包转换成 WinAPIDock::EventRow；
    // - 调用：主进程管道和子进程管道读取线程共用；
    // - 返回：转换后的事件行，调用者负责入队或显示。
    WinAPIDock::EventRow packetToEventRow(const ks::winapi_monitor::ApiMonitorEventPacket& packetValue)
    {
        WinAPIDock::EventRow rowValue;
        rowValue.time100nsText = QString::number(static_cast<qulonglong>(packetValue.timestamp100ns));
        rowValue.categoryText = packetEventCategoryText(packetValue.category);

        const QString moduleNameText = packetWideText(packetValue.moduleName);
        const QString apiNameText = packetWideText(packetValue.apiName);
        rowValue.apiText = moduleNameText.trimmed().isEmpty()
            ? apiNameText
            : QStringLiteral("%1!%2").arg(moduleNameText, apiNameText);
        rowValue.resultKnown = packetValue.resultKind == static_cast<std::uint32_t>(ks::winapi_monitor::EventResultKind::StatusCode);
        rowValue.resultText = rowValue.resultKnown ? packetResultCodeText(packetValue.resultCode) : QStringLiteral("未采集");
        rowValue.pidTidText = QStringLiteral("%1 / %2").arg(packetValue.pid).arg(packetValue.tid);
        rowValue.detailText = packetWideText(packetValue.detailText);
        QString phase;
        switch (static_cast<ks::winapi_monitor::EventKind>(packetValue.eventKind)) {
        case ks::winapi_monitor::EventKind::IoSubmit: phase = QObject::tr("提交"); break;
        case ks::winapi_monitor::EventKind::IoWait: phase = QObject::tr("等待完成"); break;
        case ks::winapi_monitor::EventKind::IoComplete: phase = QObject::tr("完成"); break;
        case ks::winapi_monitor::EventKind::IoCancel: phase = QObject::tr("取消结果"); break;
        default: break;
        }
        if (!phase.isEmpty()) rowValue.detailText.prepend(QStringLiteral("[%1 #%2] ").arg(phase).arg(packetValue.operationId));
        rowValue.sourcePid = packetValue.pid;
        if (packetValue.category == static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal)
            && moduleNameText == QStringLiteral("Agent")) rowValue.agentStatus = apiNameText;
        rowValue.internalEvent =
            packetValue.category == static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal);
        return rowValue;
    }

    // tryExtractAutoInjectChildPid：
    // - 作用：从 Agent 内部 AutoInjectChild 事件详情中提取 childPid；
    // - 调用：主目标管道收到子进程注入成功事件后，启动对应子管道读取线程；
    // - 返回：成功解析返回 true，并通过 childPidOut 输出 PID。
    bool tryExtractAutoInjectChildPid(
        const ks::winapi_monitor::ApiMonitorEventPacket& packetValue,
        std::uint32_t* const childPidOut)
    {
        if (childPidOut == nullptr)
        {
            return false;
        }
        *childPidOut = 0;

        const QString apiNameText = packetWideText(packetValue.apiName);
        if (packetValue.category != static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal)
            || apiNameText != QStringLiteral("AutoInjectChild"))
        {
            return false;
        }

        const QString detailText = packetWideText(packetValue.detailText);
        const QString markerText = QStringLiteral("childPid=");
        const int markerIndex = detailText.indexOf(markerText);
        if (markerIndex < 0)
        {
            return false;
        }

        int valueStart = markerIndex + markerText.size();
        int valueEnd = valueStart;
        while (valueEnd < detailText.size() && detailText.at(valueEnd).isDigit())
        {
            ++valueEnd;
        }

        bool parseOk = false;
        const std::uint32_t pidValue = detailText.mid(valueStart, valueEnd - valueStart).toUInt(&parseOk, 10);
        if (!parseOk || pidValue == 0)
        {
            return false;
        }

        *childPidOut = pidValue;
        return true;
    }
}

WinAPISessionBackend::~WinAPISessionBackend()
{
    // RetainedCleanupJob releases its value owner only after all readers joined.
    if (cancelEvent) ::CloseHandle(cancelEvent);
    if (targetProcess) ::CloseHandle(targetProcess);
}

void WinAPISessionBackend::requestStop() noexcept
{
    stop.store(true);
    std::lock_guard<std::mutex> lock(cancelMutex);
    if (cancelEvent) (void)::SetEvent(cancelEvent);
}

bool WinAPISessionBackend::start()
{
    const std::lock_guard<std::mutex> operation(operationGate);
    struct Terminal { std::atomic_bool& flag; ~Terminal() { flag.store(true); } } terminal{startupTerminal};
    if (stop.load()) return false;
    try
    {
        {
            const std::lock_guard<std::mutex> lock(cancelMutex);
            cancelEvent = ::CreateEventW(nullptr, TRUE, stop.load(), nullptr);
        }
        if (!cancelEvent || stop.load()) return false;
        targetProcess = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, config.pid);
        creation = targetProcess ? ks::winapi_monitor::processCreationIdentity(targetProcess) : 0;
        if (!creation) { startupError = QString::number(::GetLastError()); return false; }
        std::wstring path, error;
        if (!ks::winapi_monitor::resolveAgentPath(config.pid, config.requestedAgentPath.toStdWString(), &path, &error))
        {
            startupError = QStringLiteral("无法选择匹配目标进程位数的 Agent DLL：%1").arg(QString::fromStdWString(error));
            return false;
        }
        selectedAgentPath = QString::fromStdWString(path);
        DWORD leaseError = 0;
        if (!lease.acquire(config.pid, &leaseError))
        {
            startupError = QStringLiteral("无法占用 PID=%1 的 Agent 会话（错误码 %2）；该进程可能已由 API 监控或剪贴板保护占用。")
                .arg(config.pid).arg(leaseError);
            return false;
        }
        if (!ks::winapi_monitor::addSessionDirectoryReader(config.pid, &error))
        {
            startupError = QStringLiteral("无法准备跨账户会话目录：%1").arg(QString::fromStdWString(error));
            return false;
        }
        configPath = QString::fromStdWString(ks::winapi_monitor::buildConfigPathForPid(config.pid));
        stopPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(config.pid));
        if (stop.load() || ::WaitForSingleObject(targetProcess, 0) != WAIT_TIMEOUT) return false;
        if (config.autoInjectChildren)
        {
            broker = std::make_unique<ks::winapi_monitor::InjectionBroker>();
            if (!broker->start(config.pid, path, config.sessionId.toStdWString(), stopPath.toStdWString(), &error))
            { startupError = QString::fromStdWString(error); return false; }
        }
        QSaveFile file(configPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        { startupError = QStringLiteral("无法写入会话配置：%1").arg(configPath); return false; }
        std::wstring controllerSid;
        if (!ks::winapi_monitor::currentProcessUserSidString(&controllerSid))
        { startupError = QStringLiteral("无法读取当前控制端 SID。"); return false; }
        const auto endpoint = broker ? broker->endpoint() : ks::winapi_monitor::InjectionEndpoint{};
        QTextStream stream(&file);
        stream.setEncoding(QStringConverter::Utf16LE);
        stream.setGenerateByteOrderMark(true);
        stream << "[monitor]\n";
        stream << "pipe_name=" << QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(config.pid)) << '\n';
        stream << "root_stop_flag_path=" << stopPath << '\n' << "stop_flag_path=" << stopPath << '\n';
        stream << "session_id=" << config.sessionId << '\n';
        stream << "controller_sid=" << QString::fromStdWString(controllerSid) << '\n';
        stream << "injection_broker_pipe=" << QString::fromStdWString(endpoint.pipeName) << '\n';
        stream << "injection_broker_token=" << QString::fromStdWString(endpoint.token) << '\n';
        stream << "injection_broker_pid=" << endpoint.hostPid << '\n';
        stream << "injection_broker_creation=" << endpoint.hostCreation << '\n';
        stream << "agent_dll_path=" << selectedAgentPath << '\n' << config.options;
        stream.flush();
        if (stream.status() != QTextStream::Ok || !file.commit())
        { startupError = QStringLiteral("无法原子提交会话配置：%1").arg(configPath); return false; }
        configPublished = true; // From this point even failed/canceled startup owes STOP.
        if (!ks::winapi_monitor::grantSessionFileRead(configPath.toStdWString(), config.pid, &error))
        { startupError = QStringLiteral("无法授予目标进程读取会话配置的权限：%1").arg(QString::fromStdWString(error)); return false; }
        if (stop.load() || ::WaitForSingleObject(targetProcess, 0) != WAIT_TIMEOUT) return false;
        if (QFile::exists(stopPath) && !QFile::remove(stopPath))
        { startupError = QStringLiteral("会话配置已写入，但无法撤销停止标记：%1").arg(stopPath); return false; }
        if (stop.load()) return false;
        const bool reuse = config.residentPid == config.pid && config.residentCreation == creation;
        reusedResident.store(reuse);
        startReader(config.pid, config.sessionId.toStdWString(), true);
        if (stop.load()) return false;
        if (!reuse && !ks::winapi_monitor::injectAgent(config.pid, path, &error, cancelEvent, creation))
        { startupError = QString::fromStdWString(error); return false; }
        if (stop.load()) return false;
        startupSucceeded.store(true);
        return true;
    }
    catch (...)
    {
        startupError = QStringLiteral("DLL 注入失败：%1").arg(QString::number(ERROR_NOT_ENOUGH_MEMORY));
        return false;
    }
}

void WinAPISessionBackend::enqueue(EventRow row)
{
    auto record = std::make_shared<WinAPIEventRecord>();
    record->searchText = WinAPIEventFields(row).join(QStringLiteral(" | "));
    record->value = std::move(row);
    record->generation = config.generation;
    const std::lock_guard<std::mutex> lock(pendingMutex);
    record->sequence = ++nextEventSequence;
    const auto& value = record->value;
    if (value.agentStatus == QStringLiteral("EventsDropped"))
    {
        bool ok = false;
        const auto count = value.detailText.toULongLong(&ok);
        if (ok) agentDroppedRows[value.sourcePid] = std::max<std::uint64_t>(agentDroppedRows[value.sourcePid], count);
    }
    if (pendingRows.size() >= pendingCapacity)
    {
        const auto ordinary = std::find_if(pendingRows.begin(), pendingRows.end(), [](const auto& value) { return !value->value.internalEvent; });
        if (ordinary != pendingRows.end()) pendingRows.erase(ordinary); else pendingRows.pop_front();
        ++pendingDroppedRows;
    }
    pendingRows.push_back(std::move(record));
}

void WinAPISessionBackend::notice(const QString& api, const QString& detail, std::uint32_t pid)
{
    FILETIME now{}; ::GetSystemTimeAsFileTime(&now);
    EventRow row;
    row.time100nsText = QString::number((static_cast<qulonglong>(now.dwHighDateTime) << 32) | now.dwLowDateTime);
    row.categoryText = QStringLiteral("内部"); row.apiText = api; row.resultText = QStringLiteral("OK");
    row.sourcePid = pid ? pid : config.pid; row.pidTidText = QStringLiteral("%1 / -").arg(row.sourcePid);
    row.detailText = detail; row.internalEvent = true;
    enqueue(std::move(row));
}

void WinAPISessionBackend::startReader(std::uint32_t pid, const std::wstring& session, bool root)
{
    if (!pid || stop.load()) return;
    auto reader = std::make_shared<Reader>();
    reader->pid = pid;
    reader->session = ks::winapi_monitor::sessionIdentity(session);
    if (root) reader->creation = creation;
    else
    {
        if (pid == config.pid || session.rfind(config.sessionId.toStdWString() + L"_", 0) != 0) return;
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        reader->creation = process ? ks::winapi_monitor::processCreationIdentity(process) : 0;
        if (process) ::CloseHandle(process);
        DWORD error = 0;
        if (!reader->creation || !reader->lease.acquire(pid, &error))
        {
            notice(QStringLiteral("子进程管道连接失败"),
                QStringLiteral("无法占用 PID=%1 的 Agent 会话（错误码 %2）；该进程可能已由 API 监控或剪贴板保护占用。").arg(pid).arg(error), pid);
            return;
        }
    }
    const std::lock_guard<std::mutex> lock(readerMutex);
    if (stop.load() || std::any_of(readers.begin(), readers.end(), [pid](const auto& value) { return value->pid == pid; })) return;
    readers.push_back(reader); // Publish ownership before any thread can discover descendants.
    const auto self = shared_from_this();
    try { reader->thread = std::make_unique<std::thread>([self, reader, root]() { self->readPipe(reader, root); }); }
    catch (...) { readers.pop_back(); throw; }
}

void WinAPISessionBackend::readPipe(const std::shared_ptr<Reader>& reader, bool root)
{
    try
    {
        const auto pipeName = ks::winapi_monitor::buildPipeNameForPid(reader->pid);
        for (unsigned reconnect = 0; !stop.load() && reconnect <= 3; ++reconnect)
        {
            HANDLE pipe = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 120 && !stop.load(); ++attempt)
            {
                pipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (pipe != INVALID_HANDLE_VALUE) break;
                const auto error = ::GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
                {
                    { const std::lock_guard<std::mutex> lock(pendingMutex); pipeErrors.push_back({reader->pid, error}); }
                    notice(root ? QStringLiteral("命名管道连接失败") : QStringLiteral("子进程管道连接失败"),
                        QStringLiteral("PID=%1 CreateFileW 错误码 %2。").arg(reader->pid).arg(error), reader->pid);
                    break;
                }
                (void)::WaitNamedPipeW(pipeName.c_str(), 250);
                if (::WaitForSingleObject(cancelEvent, 120) == WAIT_OBJECT_0) break;
            }
            if (pipe == INVALID_HANDLE_VALUE)
            {
                if (!stop.load()) notice(root ? QStringLiteral("命名管道连接超时") : QStringLiteral("子进程管道连接超时"),
                    QStringLiteral("PID=%1 的 Agent 管道未在限定时间内建立。").arg(reader->pid), reader->pid);
                break;
            }
            // Only this reader ever closes its pipe; cleanup cancels synchronous IO.
            struct PipeOwner { HANDLE value; ~PipeOwner() { ::CloseHandle(value); } } pipeOwner{pipe};
            if (root) connected.store(true);
            notice(root ? QStringLiteral("管道已连接") : QStringLiteral("子进程管道已连接"),
                QStringLiteral("已与 PID=%1 的 Agent 建立命名管道连接。").arg(reader->pid), reader->pid);
            ks::winapi_monitor::CoverageSnapshotReceiver receiver(reader->pid, reader->session);
            while (!stop.load())
            {
                ks::winapi_monitor::ApiMonitorEventPacket packet{};
                if (!ks::winapi_monitor::readEventPacket(pipe, &packet, stop)) break;
                if (packet.size != sizeof(packet) || packet.version != ks::winapi_monitor::kProtocolVersion
                    || packet.sessionIdentity != reader->session) continue;
                if (ks::winapi_monitor::isCoverageEvent(packet.eventKind))
                {
                    if (receiver.consume(packet)) queueCoverageSnapshot(reader->pid, reader->session, receiver.revision(), receiver.items(), false, config.generation);
                    else if (receiver.stale()) queueCoverageSnapshot(reader->pid, reader->session, receiver.revision(), {}, true, config.generation);
                    continue;
                }
                std::uint32_t childPid = 0;
                if (tryExtractAutoInjectChildPid(packet, &childPid) && !stop.load())
                {
                    wchar_t childSession[1024]{};
                    const auto path = ks::winapi_monitor::buildConfigPathForPid(childPid);
                    ::GetPrivateProfileStringW(L"monitor", L"session_id", L"", childSession, 1024, path.c_str());
                    startReader(childPid, childSession, false);
                }
                enqueue(packetToEventRow(packet));
            }
            queueCoverageSnapshot(reader->pid, reader->session, receiver.revision(), {}, true, config.generation);
            if (root) connected.store(false);
            if (!root || stop.load() || reconnect == 3) break;
            notice(QStringLiteral("管道短暂断开"), QStringLiteral("Agent 正在切换会话，正在重新连接命名管道。"));
        }
    }
    catch (...)
    {
        try { notice(QStringLiteral("命名管道连接失败"), QString::number(ERROR_NOT_ENOUGH_MEMORY), reader->pid); } catch (...) {}
    }
    if (root) { connected.store(false); rootEnded.store(true); }
}

bool WinAPISessionBackend::cleanup()
{
    requestStop();
    const std::lock_guard<std::mutex> operation(operationGate);
    if (config.stopOnly && !configPublished)
    {
        if (!targetProcess) targetProcess = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, config.pid);
        if (!targetProcess && ::GetLastError() == ERROR_INVALID_PARAMETER)
        { lease.reset(); cleanupComplete.store(true); return true; }
        creation = targetProcess ? ks::winapi_monitor::processCreationIdentity(targetProcess) : 0;
        DWORD leaseError = 0;
        std::wstring preparationError;
        if (!creation || !lease.acquire(config.pid, &leaseError)
            || !ks::winapi_monitor::addSessionDirectoryReader(config.pid, &preparationError))
        {
            const std::lock_guard<std::mutex> lock(pendingMutex);
            cleanupError = preparationError.empty()
                ? QStringLiteral("无法占用 PID=%1 的 Agent 会话（错误码 %2）；该进程可能已由 API 监控或剪贴板保护占用。")
                    .arg(config.pid).arg(leaseError ? leaseError : ::GetLastError())
                : QStringLiteral("无法准备会话目录：%1").arg(QString::fromStdWString(preparationError));
            // No config/marker or hook was accepted; this is a failed action,
            // not a resource that can keep shutdown pending indefinitely.
            lease.reset();
            if (targetProcess) { ::CloseHandle(targetProcess); targetProcess = nullptr; }
            cleanupComplete.store(true);
            return true;
        }
        stopPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(config.pid));
        configPublished = true;
    }
    // Startup that lost this gate cannot execute after cleanup and release a lease.
    bool flagsOk = true;
    QString error;
    const auto writeStop = [&](std::uint32_t pid, std::uint64_t expected, bool root) -> bool
    {
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process) return ::GetLastError() == ERROR_INVALID_PARAMETER;
        const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        const bool same = ks::winapi_monitor::processCreationIdentity(process) == expected;
        ::CloseHandle(process);
        if (!alive || !same) return true; // The original instance cannot retain hooks.
        const auto path = root ? stopPath : QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pid));
        if (!root)
        {
            wchar_t session[1024]{};
            const auto configFile = ks::winapi_monitor::buildConfigPathForPid(pid);
            ::GetPrivateProfileStringW(L"monitor", L"session_id", L"", session, 1024, configFile.c_str());
            if (std::wstring(session).rfind(config.sessionId.toStdWString() + L"_", 0) != 0)
            { error = QStringLiteral("无法写入停止标记：%1").arg(path); return false; }
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write("stop", 4) != 4 || !file.flush())
        { error = QStringLiteral("无法写入停止标记：%1").arg(path); return false; }
        file.close();
        std::wstring aclError;
        if (!ks::winapi_monitor::grantSessionFileRead(path.toStdWString(), pid, &aclError)
            || (!root && QFile::exists(stopPath)
                && !ks::winapi_monitor::grantSessionFileRead(stopPath.toStdWString(), pid, &aclError)))
        { error = QStringLiteral("无法授予目标进程读取停止标记的权限：%1").arg(QString::fromStdWString(aclError)); return false; }
        return true;
    };
    if (configPublished) flagsOk = writeStop(config.pid, creation, true);
    if (broker)
    {
        participants = broker->stopAndTakeParticipants(); // noexcept value transfer after broker joins
        broker.reset();
    }
    std::vector<std::shared_ptr<Reader>> current;
    { const std::lock_guard<std::mutex> lock(readerMutex); current = readers; }
    for (const auto& reader : current)
    {
        if (reader->pid != config.pid) participants.push_back({reader->pid, reader->creation});
        if (reader->thread && reader->thread->joinable()) (void)::CancelSynchronousIo(reader->thread->native_handle());
    }
    // Every known descendant is notified, including accepted broker loads that
    // never reached the event inbox. No page or UI queue is required to discover it.
    std::sort(participants.begin(), participants.end(), [](const auto& x, const auto& y) { return std::tie(x.pid, x.creation) < std::tie(y.pid, y.creation); });
    participants.erase(std::unique(participants.begin(), participants.end(), [](const auto& x, const auto& y) { return x.pid == y.pid && x.creation == y.creation; }), participants.end());
    if (configPublished) for (const auto& child : participants)
        if (!writeStop(child.pid, child.creation, false)) flagsOk = false;
    for (const auto& reader : current)
        if (reader->thread && reader->thread->joinable()) reader->thread->join();
    connected.store(false);
    if (!flagsOk)
    {
        { const std::lock_guard<std::mutex> lock(pendingMutex); cleanupError = std::move(error); }
        return false; // Retained value owner keeps target + child leases for explicit Retry.
    }
    { const std::lock_guard<std::mutex> lock(readerMutex); readers.clear(); }
    if (targetProcess) { ::CloseHandle(targetProcess); targetProcess = nullptr; }
    {
        const std::lock_guard<std::mutex> lock(cancelMutex);
        if (cancelEvent) { ::CloseHandle(cancelEvent); cancelEvent = nullptr; }
    }
    { const std::lock_guard<std::mutex> lock(pendingMutex); cleanupError.clear(); }
    lease.reset();
    cleanupComplete.store(true);
    return true;
}

void WinAPIDock::pollSessionBackend()
{
    const auto backend = m_sessionBackend;
    if (!backend || backend->config.generation != m_sessionGeneration.load()) return;
    std::vector<std::pair<std::uint32_t, DWORD>> pipeErrors;
    {
        const std::lock_guard<std::mutex> source(backend->pendingMutex);
        const std::lock_guard<std::mutex> target(m_pendingMutex);
        while (!backend->pendingRows.empty())
        { m_pendingRows.push_back(std::move(backend->pendingRows.front())); backend->pendingRows.pop_front(); }
        for (auto& [pid, update] : backend->pendingCoverage)
        {
            auto& pending = m_pendingCoverage[pid];
            if (pending.session && (pending.session != update.session || pending.revision > update.revision)) continue;
            // A stale marker preserves the last accepted complete snapshot.
            if (!update.complete && pending.complete)
            { pending.stale = true; continue; }
            pending = std::move(update);
        }
        while (m_pendingRows.size() > kPendingRowCapacity)
        {
            const auto ordinary = std::find_if(m_pendingRows.begin(), m_pendingRows.end(), [](const auto& row) { return !row->value.internalEvent; });
            if (ordinary != m_pendingRows.end()) m_pendingRows.erase(ordinary); else m_pendingRows.pop_front();
            ++m_pendingDroppedRows;
        }
        backend->pendingCoverage.clear();
        m_agentDroppedRows = backend->agentDroppedRows;
        m_pendingDroppedRows += backend->pendingDroppedRows;
        backend->pendingDroppedRows = 0;
        pipeErrors.swap(backend->pipeErrors);
    }
    m_pipeConnected.store(backend->connected.load());
    for (const auto& [pid, error] : pipeErrors)
        (void)ks::ui::promptForPrivilegeFailure(this, pid == m_currentSessionPid
            ? QStringLiteral("连接 API 监控命名管道") : QStringLiteral("连接子进程 API 监控命名管道"), error);
    if (backend->startupTerminal.load() && !m_startupReported)
    {
        m_startupReported = true;
        if (backend->startupSucceeded.load())
        {
            m_residentAgentPid = backend->config.pid;
            m_residentAgentCreationTime100ns = backend->creation;
            if (m_agentDllPathEdit) m_agentDllPathEdit->setText(backend->selectedAgentPath);
            appendInternalEvent(QStringLiteral("内部"), QStringLiteral("会话已启动"), backend->reusedResident.load()
                ? QStringLiteral("已原子写入配置并复用常驻 Agent，等待其重新创建命名管道。")
                : QStringLiteral("已原子写入配置并完成 DLL 注入，等待 Agent 创建命名管道。"));
            kPro.set(m_sessionProgressPid, "DLL 已注入，等待 Agent 握手", 0, 45.0f);
        }
        else if (!m_stopping)
        {
            const auto detail = backend->startupError;
            if (!detail.isEmpty())
            {
                appendInternalEvent(QStringLiteral("内部"), QStringLiteral("InjectDllByPath"), detail);
                if (!ks::ui::promptForPrivilegeFailure(this, QStringLiteral("注入 API 监控 DLL"), detail))
                    QMessageBox::warning(this, QStringLiteral("WinAPI 监控"), QStringLiteral("DLL 注入失败：%1").arg(detail));
            }
            m_stopping = true;
            backend->requestStop();
            if (!m_sessionCleanup->isCleanupRequested()) m_sessionCleanup->requestCleanup();
        }
    }
    if (backend->rootEnded.load() && !m_stopping)
    {
        m_stopping = true;
        backend->requestStop();
        if (!m_sessionCleanup->isCleanupRequested()) m_sessionCleanup->requestCleanup();
    }
    if (m_sessionCleanup && m_sessionCleanup->isCleanupRequested())
    {
        m_stopping = true;
        if (m_sessionCleanup->hasFailed() && m_reportedCleanupAttempt != m_sessionCleanup->attemptCount())
        {
            m_reportedCleanupAttempt = m_sessionCleanup->attemptCount();
            QString error;
            { const std::lock_guard<std::mutex> lock(backend->pendingMutex); error = backend->cleanupError; }
            if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("终止 Hook"), error);
        }
        if (m_sessionCleanup->isComplete() && !m_stopReported)
        {
            m_stopReported = true;
            m_pipeRunning.store(false);
            m_pipeConnected.store(false);
            markCoverageStale(); m_hookState = HookState::Waiting; m_definitionMismatch = false;
            kPro.set(m_sessionProgressPid, "WinAPI 监控已停止", 0, 100.0f);
            QString terminalError;
            { const std::lock_guard<std::mutex> lock(backend->pendingMutex); terminalError = backend->cleanupError; }
            if (backend->config.stopOnly && !terminalError.isEmpty())
            {
                appendInternalEvent(QStringLiteral("内部"), QStringLiteral("手动终止 Hook"), terminalError);
                QMessageBox::warning(this, QStringLiteral("终止 Hook"), terminalError);
            }
            else appendInternalEvent(QStringLiteral("内部"), QStringLiteral("停止监控"), QStringLiteral("已发出停止标记并回收本地管道线程。"));
        }
    }
    updateActionState();
}

void WinAPIDock::acceptPendingEventRows()
{
    std::deque<WinAPIEventRef> accepted;
    { const std::lock_guard<std::mutex> lock(m_pendingMutex); accepted.swap(m_pendingRows); }
    const bool visible = m_eventTable && m_eventTable->isVisible();
    if (!visible) { m_eventProjectionStale = true; m_eventProjectionQueue.clear(); m_expiredEventProjection.clear(); }
    for (auto& record : accepted)
    {
        applyEventState(record->value); // Hook status/definition state remains current while hidden.
        auto evicted = m_eventArchive.append(record);
        if (evicted)
        {
            ++m_evictedRows;
            if (visible && !m_eventProjectionStale && m_projectedEvents.contains(evicted.get()))
                m_expiredEventProjection.push_back(std::move(evicted));
        }
        if (visible && !m_eventProjectionStale) m_eventProjectionQueue.push_back(std::move(record));
    }
    if (m_eventProjectionQueue.size() > WinAPIEventArchive::capacity)
    { m_eventProjectionStale = true; m_eventProjectionQueue.clear(); m_expiredEventProjection.clear(); }
}

WinAPIEventRef WinAPIDock::eventRecordAt(int row) const
{
    if (!m_eventTable || row < 0) return {};
    const auto item = m_eventTable->item(row, EventColumnTime100ns);
    if (!item) return {};
    const auto key = reinterpret_cast<const WinAPIEventRecord*>(item->data(Qt::UserRole + 1).value<quintptr>());
    const auto found = m_projectedEvents.find(key);
    return found != m_projectedEvents.end() ? found->second.record : WinAPIEventRef{};
}

void WinAPIDock::flushPendingRows()
{
    pollSessionBackend();
    acceptPendingEventRows(); // Always archive before menu/visibility projection barriers.
    flushCoverageUpdates();
    const bool visible = m_eventTable && m_eventTable->isVisible();
    if (m_eventFilterProjection) m_eventFilterProjection->setSuspended(!visible);
    if (!visible && m_eventProjectionWasVisible)
    {
        m_restoreSelectedEvent = eventRecordAt(m_eventTable->currentRow());
        m_restoreTopEvent = eventRecordAt(m_eventTable->indexAt(QPoint(0, 0)).row());
    }
    m_eventProjectionWasVisible = visible;
    if (!visible) { updateActionState(); updateStatusLabel(); return; }
    const QPointer<WinAPIDock> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("winapi-event-flush"), {m_eventTable},
        [guard]() { if (guard) guard->flushPendingRows(); }))
    { m_eventProjectionStale = true; m_eventProjectionQueue.clear(); m_expiredEventProjection.clear(); return; }
    projectEventRows();
    applyEventFilter(); updateActionState(); updateStatusLabel();
}

void WinAPIDock::projectEventRows()
{
    if (!m_eventTable || !m_eventTable->isVisible()) return;
    if (m_eventProjectionStale)
    {
        m_eventProjectionQueue.clear(); m_expiredEventProjection.clear();
        for (const auto& [key, projected] : m_projectedEvents)
            if (!m_eventArchive.contains(projected.record)) m_expiredEventProjection.push_back(projected.record);
        for (const auto& record : m_eventArchive.snapshot())
            if (!m_projectedEvents.contains(record.get())) m_eventProjectionQueue.push_back(record);
        m_eventProjectionStale = false;
    }
    if (m_expiredEventProjection.empty() && m_eventProjectionQueue.empty()
        && !m_restoreSelectedEvent && !m_restoreTopEvent) return;
    QElapsedTimer budget; budget.start();
    const bool updates = m_eventTable->updatesEnabled();
    m_eventTable->setUpdatesEnabled(false);
    const bool sorting = m_eventTable->isSortingEnabled();
    if (sorting) m_eventTable->setSortingEnabled(false);
    const auto selected = m_restoreSelectedEvent ? m_restoreSelectedEvent : eventRecordAt(m_eventTable->currentRow());
    const auto top = m_restoreTopEvent ? m_restoreTopEvent : eventRecordAt(m_eventTable->indexAt(QPoint(0, 0)).row());
    std::vector<int> removed;
    std::size_t mutations = 0;
    while (!m_expiredEventProjection.empty() && mutations < kUiFlushRowLimit && budget.elapsed() < kUiFlushBudgetMs)
    {
        auto record = std::move(m_expiredEventProjection.front()); m_expiredEventProjection.pop_front();
        const auto found = m_projectedEvents.find(record.get());
        if (found == m_projectedEvents.end()) continue;
        if (found->second.index.isValid()) { removed.push_back(found->second.index.row()); ++mutations; }
        m_projectedEvents.erase(found);
    }
    std::sort(removed.begin(), removed.end(), std::greater<int>());
    removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
    for (std::size_t i = 0; i < removed.size();)
    {
        const int last = removed[i]; int first = last;
        while (++i < removed.size() && removed[i] == first - 1) first = removed[i];
        m_eventTable->model()->removeRows(first, last - first + 1);
    }
    std::size_t appended = 0;
    while (!m_eventProjectionQueue.empty() && mutations < kUiFlushRowLimit && budget.elapsed() < kUiFlushBudgetMs)
    {
        auto record = std::move(m_eventProjectionQueue.front()); m_eventProjectionQueue.pop_front();
        if (!m_eventArchive.contains(record) || m_projectedEvents.contains(record.get())) continue;
        materializeEventRow(record); ++appended; ++mutations;
    }
    if (sorting) m_eventTable->setSortingEnabled(true); // One sort per bounded projection batch.
    if (m_restoreSelectedEvent && selected && m_eventArchive.contains(selected))
    {
        const auto found = m_projectedEvents.find(selected.get());
        if (found != m_projectedEvents.end() && found->second.index.isValid())
        {
            if (m_eventTable->selectionModel())
                m_eventTable->selectionModel()->setCurrentIndex(found->second.index, QItemSelectionModel::NoUpdate);
            m_restoreSelectedEvent.reset();
        }
    }
    if (m_eventKeepBottomCheck && m_eventKeepBottomCheck->isChecked())
        m_eventTable->scrollToBottom();
    else if ((m_restoreTopEvent || !removed.empty()) && top && m_eventArchive.contains(top))
    {
        const auto found = m_projectedEvents.find(top.get());
        if (found != m_projectedEvents.end() && found->second.index.isValid())
        { m_eventTable->scrollTo(found->second.index, QAbstractItemView::PositionAtTop); m_restoreTopEvent.reset(); }
    }
    if (selected && !m_eventArchive.contains(selected)) m_restoreSelectedEvent.reset();
    if (top && !m_eventArchive.contains(top)) m_restoreTopEvent.reset();
    m_eventTable->setUpdatesEnabled(updates);
    if (updates && (appended || !removed.empty())) m_eventTable->viewport()->update();
    if ((!m_eventProjectionQueue.empty() || !m_expiredEventProjection.empty()) && !m_eventProjectionPumpScheduled)
    {
        m_eventProjectionPumpScheduled = true;
        QTimer::singleShot(0, this, [this]() { m_eventProjectionPumpScheduled = false; flushPendingRows(); });
    }
}

void WinAPIDock::clearCanonicalEvents()
{
    m_eventArchive.clear(); m_eventProjectionQueue.clear(); m_expiredEventProjection.clear(); m_projectedEvents.clear();
    m_restoreSelectedEvent.reset(); m_restoreTopEvent.reset(); m_eventProjectionStale = true;
    if (m_eventTable) { m_eventTable->clearContents(); m_eventTable->setRowCount(0); }
    m_visibleEventCount = 0;
}

WinAPIDock::EventExportSnapshot WinAPIDock::freezeVisibleEventRowsForExport()
{
    pollSessionBackend(); acceptPendingEventRows(); // Never materializes a hidden table.
    EventExportSnapshot snapshot;
    // Preserve current sort/order through stable identities, then append arrivals
    // that have not been materialized. This reads no item texts or hidden bits.
    const auto canonical = m_eventArchive.snapshot();
    std::vector<WinAPIEventRef> projected(m_eventTable ? m_eventTable->rowCount() : 0);
    std::unordered_set<const WinAPIEventRecord*> represented;
    for (const auto& [key, row] : m_projectedEvents)
    {
        if (!row.index.isValid() || !m_eventArchive.contains(row.record)) continue;
        const int index = row.index.row();
        if (index >= 0 && static_cast<std::size_t>(index) < projected.size())
        { projected[static_cast<std::size_t>(index)] = row.record; represented.insert(key); }
    }
    snapshot.rows.reserve(canonical.size());
    for (auto& row : projected) if (row) snapshot.rows.push_back(std::move(row));
    for (const auto& row : canonical) if (!represented.contains(row.get())) snapshot.rows.push_back(row);
    snapshot.businessKeyword = m_eventFilterEdit ? m_eventFilterEdit->text().trimmed() : m_eventFilterKeyword;
    snapshot.search = ks::ui::CaptureTableSearchResultFilterSpec(m_eventTable);
    snapshot.language = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
    snapshot.sessionGeneration = m_sessionGeneration.load(); snapshot.targetPid = m_currentSessionPid;
    snapshot.archiveRevision = m_eventArchive.revision(); snapshot.lossSummary = eventLossSummary();
    if (m_eventTable && m_eventTable->horizontalHeader())
    {
        snapshot.continuousSort = m_eventTable->isSortingEnabled();
        snapshot.sortColumn = m_eventTable->horizontalHeader()->sortIndicatorSection();
        snapshot.sortOrder = m_eventTable->horizontalHeader()->sortIndicatorOrder();
    }
    for (int column = 0; column < EventColumnCount; ++column)
    {
        const auto header = m_eventTable ? m_eventTable->horizontalHeaderItem(column) : nullptr;
        snapshot.headers.push_back(header ? snapshot.language.displayText(header->text()) : QString());
    }
    return snapshot;
}

void WinAPIDock::applyEventState(const EventRow& rowValue)
{
    if (rowValue.sourcePid == m_currentSessionPid && !rowValue.agentStatus.isEmpty())
    {
        if (rowValue.agentStatus == QStringLiteral("SessionReady"))
        {
            m_hookState = HookState::Installing;
            const QString marker = QString::fromLatin1("definitionSha256=");
            const QString remoteHash = rowValue.detailText.startsWith(marker) ? rowValue.detailText.mid(marker.size(), 64) : QString();
            m_definitionMismatch = m_coverageDefinitionHash.isEmpty() || remoteHash.size() != 64
                || remoteHash.toLatin1() != m_coverageDefinitionHash;
        }
        else if (rowValue.agentStatus == QStringLiteral("InstallHooksFailed")) m_hookState = HookState::Failed;
        else if (rowValue.agentStatus == QStringLiteral("HooksPartial")) m_hookState = HookState::Partial;
        else if (rowValue.agentStatus == QStringLiteral("HooksInstalled"))
        {
            if (m_hookState != HookState::Partial) m_hookState = HookState::Active;
            kPro.set(m_sessionProgressPid, "WinAPI Hook 安装完成", 0, 100.0f);
        }
        else if (rowValue.agentStatus == QStringLiteral("HooksRemoved")) m_hookState = HookState::Removed;
        if (m_hookState == HookState::Failed) kPro.set(m_sessionProgressPid, "WinAPI Hook 安装失败", 0, 100.0f);
    }
 }

void WinAPIDock::materializeEventRow(const WinAPIEventRef& record)
{
    const auto& rowValue = record->value;
    if (m_eventTable == nullptr)
    {
        return;
    }

    const bool sorting = m_eventTable->isSortingEnabled();
    if (sorting) m_eventTable->setSortingEnabled(false);
    const int row = m_eventTable->rowCount();
    m_eventTable->insertRow(row);

    QTableWidgetItem* timeItem = createReadOnlyItem(rowValue.time100nsText);
    QTableWidgetItem* categoryItem = createReadOnlyItem(rowValue.categoryText);
    QTableWidgetItem* apiItem = createReadOnlyItem(rowValue.apiText);
    QTableWidgetItem* resultItem = createReadOnlyItem(rowValue.resultText);
    QTableWidgetItem* pidTidItem = createReadOnlyItem(rowValue.pidTidText);
    QTableWidgetItem* detailItem = createReadOnlyItem(rowValue.detailText);

    if (rowValue.internalEvent)
    {
        ks::ui::ApplyThemeItemForeground(categoryItem, ks::ui::ItemForegroundRole::Info);
        ks::ui::ApplyThemeItemForeground(apiItem, ks::ui::ItemForegroundRole::Info);
    }
    else if (rowValue.resultKnown && rowValue.resultText != QStringLiteral("OK"))
    {
        ks::ui::ApplyThemeItemForeground(resultItem, ks::ui::ItemForegroundRole::Error);
    }

    m_eventTable->setItem(row, EventColumnTime100ns, timeItem);
    m_eventTable->setItem(row, EventColumnCategory, categoryItem);
    m_eventTable->setItem(row, EventColumnApi, apiItem);
    m_eventTable->setItem(row, EventColumnResult, resultItem);
    m_eventTable->setItem(row, EventColumnPidTid, pidTidItem);
    m_eventTable->setItem(row, EventColumnDetail, detailItem);
    timeItem->setData(Qt::UserRole, record->searchText);
    timeItem->setData(Qt::UserRole + 1, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(record.get())));
    const QPersistentModelIndex identity(m_eventTable->model()->index(row, 0));
    m_projectedEvents.emplace(record.get(), ProjectedEvent{record, identity});
    if (sorting) m_eventTable->setSortingEnabled(true);
    if (m_eventFilterProjection && identity.isValid()) m_eventFilterProjection->trackRow(identity.row());
}
