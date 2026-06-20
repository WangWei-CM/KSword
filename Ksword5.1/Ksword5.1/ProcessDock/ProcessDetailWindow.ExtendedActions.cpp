#include "ProcessDetailWindow.InternalCommon.h"

using namespace process_detail_window_internal;

// ============================================================
// ProcessDetailWindow.ExtendedActions.cpp
// 作用：
// - 承载详情页“右键菜单同步能力”的补充动作；
// - 让基础操作文件继续聚焦原有终止/挂起/注入逻辑；
// - R0 调用统一通过 ArkDriverClient，UI 文件不直接 DeviceIoControl。
// ============================================================

namespace
{
    // detailExtendedProcessStillPresent 作用：
    // - 使用 Toolhelp 快照检查目标 PID 是否仍存在；
    // - 输入 targetPid 为进程 ID，queryOkOut 接收快照是否成功；
    // - 返回 true 表示进程仍存在，false 表示未找到或查询失败。
    bool detailExtendedProcessStillPresent(const std::uint32_t targetPid, bool* const queryOkOut)
    {
        if (queryOkOut != nullptr)
        {
            *queryOkOut = false;
        }

        const HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return true;
        }

        PROCESSENTRY32W processEntry{};
        processEntry.dwSize = sizeof(processEntry);
        bool foundTarget = false;
        BOOL walkOk = ::Process32FirstW(snapshotHandle, &processEntry);
        while (walkOk != FALSE)
        {
            if (processEntry.th32ProcessID == targetPid)
            {
                foundTarget = true;
                break;
            }
            walkOk = ::Process32NextW(snapshotHandle, &processEntry);
        }

        ::CloseHandle(snapshotHandle);
        if (queryOkOut != nullptr)
        {
            *queryOkOut = true;
        }
        return foundTarget;
    }

    // appendExtendedIoResultDetail 作用：
    // - 把 ArkDriverClient 通用 I/O 结果格式化进输出流；
    // - 输入 result 为驱动客户端返回值，detailStream 为追加目标；
    // - 返回值：无。
    void appendExtendedIoResultDetail(
        const ksword::ark::IoResult& result,
        std::ostringstream& detailStream)
    {
        detailStream
            << "io.ok=" << (result.ok ? "true" : "false")
            << ", win32=" << result.win32Error
            << ", nt=0x" << std::hex << static_cast<unsigned long>(result.ntStatus) << std::dec
            << ", bytes=" << result.bytesReturned
            << ", message=" << result.message;
    }

    // processPriorityLevelFromActionId 作用：
    // - 把 UI 菜单 ID 转成 ks::process 的优先级枚举；
    // - 输入 priorityActionId 为 0~5；
    // - 返回对应优先级，非法值返回 Normal。
    ks::process::ProcessPriorityLevel processPriorityLevelFromActionId(const int priorityActionId)
    {
        switch (priorityActionId)
        {
        case 0:
            return ks::process::ProcessPriorityLevel::Idle;
        case 1:
            return ks::process::ProcessPriorityLevel::BelowNormal;
        case 2:
            return ks::process::ProcessPriorityLevel::Normal;
        case 3:
            return ks::process::ProcessPriorityLevel::AboveNormal;
        case 4:
            return ks::process::ProcessPriorityLevel::High;
        case 5:
            return ks::process::ProcessPriorityLevel::Realtime;
        default:
            return ks::process::ProcessPriorityLevel::Normal;
        }
    }
}

void ProcessDetailWindow::executeTerminateProcessComboAction()
{
    // 组合结束动作：
    // - 与进程列表右键菜单保持同一方法顺序；
    // - 每个方法执行后检查目标是否退出，避免无意义继续破坏现场。
    kLogEvent actionEvent;
    const std::uint32_t targetPid = m_baseRecord.pid;
    warn << actionEvent
        << "[ProcessDetailWindow] executeTerminateProcessComboAction: pid="
        << targetPid
        << eol;

    struct TerminateMethodEntry
    {
        const char* methodName = nullptr; // methodName：日志和结果里的方法名称。
        std::function<bool(std::string*)> invokeMethod; // invokeMethod：实际结束方法。
    };

    const std::vector<TerminateMethodEntry> terminateMethodList =
    {
        { "TerminateProcess(Kernel32)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByWin32(targetPid, detailOut); } },
        { "NtTerminateProcess/ZwTerminateProcess", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByNtNative(targetPid, detailOut); } },
        { "WTSTerminateProcess(WTS API)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByWtsApi(targetPid, detailOut); } },
        { "WinStationTerminateProcess(winsta)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByWinStationApi(targetPid, detailOut); } },
        { "TerminateJobObject(Job)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByJobObject(targetPid, detailOut); } },
        { "NtTerminateJobObject/ZwTerminateJobObject", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByNtJobObject(targetPid, detailOut); } },
        { "RmShutdown(Restart Manager)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByRestartManager(targetPid, false, detailOut); } },
        { "RmShutdown(Restart Manager, force)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByRestartManager(targetPid, true, detailOut); } },
        { "DuplicateHandle(-1)+TerminateProcess", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByDuplicateHandlePseudo(targetPid, detailOut); } },
        { "TerminateThread(全部线程)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateAllThreadsByPid(targetPid, detailOut); } },
        { "NtTerminateThread/ZwTerminateThread(全部线程)", [targetPid](std::string* detailOut)
            { return ks::process::TerminateAllThreadsByPidNtNative(targetPid, detailOut); } },
        { "DebugActiveProcess 调试附加", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByDebugAttach(targetPid, detailOut); } },
        { "ntsd -c q -p <pid>", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByNtsdCommand(targetPid, detailOut); } },
        { "NtUnmapViewOfSection 卸载 ntdll.dll", [targetPid](std::string* detailOut)
            { return ks::process::TerminateProcessByNtUnmapNtdll(targetPid, detailOut); } }
    };

    bool processExited = false;
    std::ostringstream actionDetailStream;
    actionDetailStream << "pid=" << targetPid;
    constexpr int kTerminateRoundLimit = 2;
    for (int roundIndex = 0; roundIndex < kTerminateRoundLimit && !processExited; ++roundIndex)
    {
        const int roundNumber = roundIndex + 1;
        for (const TerminateMethodEntry& methodEntry : terminateMethodList)
        {
            if (methodEntry.methodName == nullptr || !methodEntry.invokeMethod)
            {
                continue;
            }

            std::string methodDetailText;
            const bool methodOk = methodEntry.invokeMethod(&methodDetailText);
            const std::string normalizedDetail = methodDetailText.empty() ? "无附加信息" : methodDetailText;
            (methodOk ? info : warn) << actionEvent
                << "[ProcessDetailWindow] 组合结束方法执行, pid="
                << targetPid
                << ", round="
                << roundNumber
                << ", method="
                << methodEntry.methodName
                << ", ok="
                << (methodOk ? "true" : "false")
                << ", detail="
                << normalizedDetail
                << eol;

            actionDetailStream
                << " | round" << roundNumber
                << ":" << methodEntry.methodName
                << "=" << (methodOk ? "ok" : "fail")
                << "(" << normalizedDetail << ")";

            bool queryOk = false;
            processExited = !detailExtendedProcessStillPresent(targetPid, &queryOk);
            if (!queryOk)
            {
                warn << actionEvent
                    << "[ProcessDetailWindow] 组合结束后存在性检查失败，继续下一方法, pid="
                    << targetPid
                    << eol;
            }
            if (processExited)
            {
                break;
            }
        }
    }

    showActionResultMessage(
        QStringLiteral("结束进程"),
        processExited,
        actionDetailStream.str(),
        actionEvent);
}

void ProcessDetailWindow::executeSetPriorityActionById(const int priorityActionId)
{
    // 优先级菜单动作：
    // - 允许新增按钮/菜单直接指定优先级，而不依赖下拉框当前状态；
    // - 结果仍复用统一动作反馈日志。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeSetPriorityActionById: pid="
        << m_baseRecord.pid
        << ", actionId="
        << priorityActionId
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::SetProcessPriority(
        m_baseRecord.pid,
        processPriorityLevelFromActionId(priorityActionId),
        &detailText);
    showActionResultMessage(QStringLiteral("设置进程优先级"), actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeSetEfficiencyModeAction(const bool enableEfficiencyMode)
{
    // 效率模式动作：
    // - 通过 Windows ProcessPowerThrottling 控制；
    // - 成功后只更新详情窗口缓存，进程列表会在下一轮刷新同步。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeSetEfficiencyModeAction: pid="
        << m_baseRecord.pid
        << ", enable="
        << (enableEfficiencyMode ? "true" : "false")
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::SetProcessEfficiencyMode(
        m_baseRecord.pid,
        enableEfficiencyMode,
        &detailText);
    if (actionOk)
    {
        m_baseRecord.efficiencyModeSupported = true;
        m_baseRecord.efficiencyModeEnabled = enableEfficiencyMode;
    }
    showActionResultMessage(
        enableEfficiencyMode ? QStringLiteral("开启效率模式") : QStringLiteral("关闭效率模式"),
        actionOk,
        detailText,
        actionEvent);
}

void ProcessDetailWindow::executeOpenProcessFolderAction()
{
    // 打开目录动作：
    // - 优先用 PID 查询当前镜像路径，避免详情页旧缓存路径为空；
    // - 结果统一记录日志，不额外弹窗。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeOpenProcessFolderAction: pid="
        << m_baseRecord.pid
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::OpenProcessFolder(m_baseRecord.pid, &detailText);
    showActionResultMessage(QStringLiteral("打开所在目录"), actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeRefreshPplProtectionLevelAction()
{
    // PPL 手动刷新：
    // - R3 查询 ProcessProtectionLevelInfo；
    // - 只更新当前详情页记录和文本，不写入其它 Dock 的跨轮缓存。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeRefreshPplProtectionLevelAction: pid="
        << m_baseRecord.pid
        << eol;

    std::uint32_t protectionLevel = 0;
    std::string displayText;
    std::string errorText;
    const bool queryOk = ks::process::QueryProcessProtectionLevelByPid(
        m_baseRecord.pid,
        &protectionLevel,
        &displayText,
        &errorText);
    if (queryOk)
    {
        m_baseRecord.protectionLevel = protectionLevel;
        m_baseRecord.protectionLevelKnown = true;
        m_baseRecord.protectionLevelText = displayText;
        refreshDetailTabTexts();
    }
    showActionResultMessage(
        QStringLiteral("手动刷新PPL保护级别"),
        queryOk,
        queryOk ? displayText : errorText,
        actionEvent);
}

void ProcessDetailWindow::executeR0TerminateProcessAction()
{
    // R0 结束进程：
    // - 通过 ArkDriverClient 封装控制设备访问；
    // - exitStatus 固定为 1，与列表右键入口保持一致。
    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0TerminateProcessAction: pid="
        << m_baseRecord.pid
        << eol;

    ksword::ark::DriverClient driverClient;
    const ksword::ark::IoResult result = driverClient.terminateProcess(m_baseRecord.pid, 1L);
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result, detailStream);
    showActionResultMessage(QStringLiteral("R0结束进程"), result.ok, detailStream.str(), actionEvent);
}

void ProcessDetailWindow::executeR0SuspendProcessAction()
{
    // R0 挂起进程：
    // - 通过 ArkDriverClient::suspendProcess 调用驱动；
    // - UI 只负责展示结果，不直接发送 IOCTL。
    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0SuspendProcessAction: pid="
        << m_baseRecord.pid
        << eol;

    ksword::ark::DriverClient driverClient;
    const ksword::ark::IoResult result = driverClient.suspendProcess(m_baseRecord.pid);
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result, detailStream);
    showActionResultMessage(QStringLiteral("R0挂起进程"), result.ok, detailStream.str(), actionEvent);
}

void ProcessDetailWindow::executeR0SetPplProtectionAction(
    const std::uint8_t protectionLevel,
    const QString& levelDisplayText)
{
    // R0 PPL 设置：
    // - protectionLevel 为 PS_PROTECTION 原始字节；
    // - 动作前弹出确认，避免误把普通进程设置成高保护级别。
    const int confirmResult = QMessageBox::question(
        this,
        QStringLiteral("确认 R0 设置 PPL 层级"),
        QStringLiteral("将通过 R0 驱动修改当前进程 PPL/Protection 字段。\n\n进程: %1 (PID %2)\n目标: %3\n\n错误偏移或系统版本差异可能导致系统不稳定。是否继续？")
            .arg(QString::fromStdString(m_baseRecord.processName.empty() ? std::string("Unknown") : m_baseRecord.processName))
            .arg(m_baseRecord.pid)
            .arg(levelDisplayText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0SetPplProtectionAction: pid="
        << m_baseRecord.pid
        << ", protectionLevel="
        << static_cast<unsigned int>(protectionLevel)
        << eol;

    ksword::ark::DriverClient driverClient;
    const ksword::ark::IoResult result = driverClient.setProcessProtection(m_baseRecord.pid, protectionLevel);
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result, detailStream);
    showActionResultMessage(QStringLiteral("R0设置PPL层级"), result.ok, detailStream.str(), actionEvent);
}

void ProcessDetailWindow::executeR0SetProcessHiddenAction(
    const bool hidden,
    const unsigned long visibilityFlags)
{
    // R0 可恢复隐藏：
    // - hidden=true 时根据 flags 选择只断链、只改 PID 或旧版双操作；
    // - hidden=false 时由驱动按记录恢复当前 PID。
    const QString confirmTitle = hidden
        ? QStringLiteral("确认 R0 隐藏当前进程")
        : QStringLiteral("确认 R0 取消隐藏当前进程");
    const QString confirmText = hidden
        ? QStringLiteral("将修改当前进程可见性，错误偏移或竞态可能导致系统不稳定。是否继续？")
        : QStringLiteral("将按 Ksword 驱动记录恢复当前进程可见性。是否继续？");
    const int confirmResult = QMessageBox::question(
        this,
        confirmTitle,
        confirmText,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0SetProcessHiddenAction: pid="
        << m_baseRecord.pid
        << ", hidden="
        << (hidden ? "true" : "false")
        << ", flags="
        << visibilityFlags
        << eol;

    ksword::ark::DriverClient driverClient;
    const unsigned long action = hidden
        ? KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE
        : KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE;
    const ksword::ark::ProcessVisibilityResult result =
        driverClient.setProcessVisibility(m_baseRecord.pid, action, visibilityFlags);
    const bool actionOk = result.io.ok &&
        (result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN ||
            result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_VISIBLE ||
            result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED);

    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result.io, detailStream);
    detailStream
        << ", status=" << result.status
        << ", hiddenCount=" << result.hiddenCount
        << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus) << std::dec;
    showActionResultMessage(
        hidden ? QStringLiteral("R0隐藏进程") : QStringLiteral("R0取消隐藏进程"),
        actionOk,
        detailStream.str(),
        actionEvent);
}

void ProcessDetailWindow::executeR0ClearProcessHiddenAction()
{
    // 清空隐藏标记：
    // - 这是全局驱动状态操作，不只影响当前详情页 PID；
    // - 因此必须二次确认。
    const int confirmResult = QMessageBox::question(
        this,
        QStringLiteral("确认清空 R0 隐藏标记"),
        QStringLiteral("将恢复并清空 Ksword 驱动内全部可恢复进程隐藏标记。是否继续？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0ClearProcessHiddenAction"
        << eol;

    ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessVisibilityResult result =
        driverClient.setProcessVisibility(0, KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL, 0UL);
    const bool actionOk = result.io.ok &&
        result.status == KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED;
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result.io, detailStream);
    detailStream
        << ", status=" << result.status
        << ", hiddenCount=" << result.hiddenCount
        << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus) << std::dec;
    showActionResultMessage(QStringLiteral("R0清空隐藏标记"), actionOk, detailStream.str(), actionEvent);
}

void ProcessDetailWindow::executeR0SetBreakOnTerminationAction(const bool enabled)
{
    // BreakOnTermination：
    // - 启用后目标进程退出可能触发系统崩溃保护；
    // - 禁用时同样通过驱动路径清理标志。
    const int confirmResult = QMessageBox::question(
        this,
        enabled ? QStringLiteral("确认启用 BreakOnTermination") : QStringLiteral("确认关闭 BreakOnTermination"),
        enabled
            ? QStringLiteral("将把当前进程设为关键进程，目标退出可能触发系统崩溃保护。是否继续？")
            : QStringLiteral("将清除当前进程的 BreakOnTermination。是否继续？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0SetBreakOnTerminationAction: pid="
        << m_baseRecord.pid
        << ", enabled="
        << (enabled ? "true" : "false")
        << eol;

    const unsigned long action = enabled
        ? KSWORD_ARK_PROCESS_SPECIAL_ACTION_ENABLE_BREAK_ON_TERMINATION
        : KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_BREAK_ON_TERMINATION;
    ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessSpecialFlagsResult result =
        driverClient.setProcessSpecialFlags(m_baseRecord.pid, action);
    const bool actionOk = result.io.ok &&
        result.status == KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED;
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result.io, detailStream);
    detailStream
        << ", status=" << result.status
        << ", appliedFlags=" << result.appliedFlags
        << ", touchedThreadCount=" << result.touchedThreadCount
        << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus) << std::dec;
    showActionResultMessage(
        enabled ? QStringLiteral("R0启用BreakOnTermination") : QStringLiteral("R0关闭BreakOnTermination"),
        actionOk,
        detailStream.str(),
        actionEvent);
}

void ProcessDetailWindow::executeR0DisableApcInsertionAction()
{
    // 禁止 APC 插入：
    // - 只处理当前已有线程的 ApcQueueable 位；
    // - 新建线程不自动继承该状态。
    const int confirmResult = QMessageBox::question(
        this,
        QStringLiteral("确认 R0 禁止 APC 插入"),
        QStringLiteral("将清除当前进程现有线程的 ApcQueueable 位。新建线程不自动继承。是否继续？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0DisableApcInsertionAction: pid="
        << m_baseRecord.pid
        << eol;

    ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessSpecialFlagsResult result =
        driverClient.setProcessSpecialFlags(
            m_baseRecord.pid,
            KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_APC_INSERTION);
    const bool actionOk = result.io.ok &&
        result.status == KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED;
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result.io, detailStream);
    detailStream
        << ", status=" << result.status
        << ", appliedFlags=" << result.appliedFlags
        << ", touchedThreadCount=" << result.touchedThreadCount
        << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus) << std::dec;
    showActionResultMessage(QStringLiteral("R0禁止APC插入"), actionOk, detailStream.str(), actionEvent);
}

void ProcessDetailWindow::executeR0DkomRemoveFromCidTableAction()
{
    // DKOM CID 删除：
    // - 该动作不可通过当前菜单恢复；
    // - 操作前强制确认，结果只写日志。
    const int confirmResult = QMessageBox::question(
        this,
        QStringLiteral("确认 DKOM 删除 PspCidTable"),
        QStringLiteral("将从 PspCidTable 删除当前进程 CID 表项。\n\n该动作不可通过当前菜单恢复，可能破坏句柄/PID 查询语义或导致蓝屏。是否继续？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmResult != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeR0DkomRemoveFromCidTableAction: pid="
        << m_baseRecord.pid
        << eol;

    ksword::ark::DriverClient driverClient;
    const ksword::ark::ProcessDkomResult result =
        driverClient.dkomProcess(
            m_baseRecord.pid,
            KSWORD_ARK_PROCESS_DKOM_ACTION_REMOVE_FROM_PSP_CID_TABLE);
    const bool actionOk = result.io.ok &&
        result.status == KSWORD_ARK_PROCESS_DKOM_STATUS_REMOVED;
    std::ostringstream detailStream;
    appendExtendedIoResultDetail(result.io, detailStream);
    detailStream
        << ", status=" << result.status
        << ", removedEntries=" << result.removedEntries
        << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus) << std::dec
        << ", pspCidTable=0x" << std::hex << result.pspCidTableAddress
        << ", eprocess=0x" << result.processObjectAddress << std::dec;
    showActionResultMessage(QStringLiteral("R0 DKOM PspCidTable删除"), actionOk, detailStream.str(), actionEvent);
}
