// ============================================================
// ProcessDetailWindow.AdvancedInfo.inc
// 作用：
// - 线程细节异步刷新；
// - 令牌详情异步刷新；
// - PEB/内存摘要异步刷新。
// ============================================================

namespace
{
    // NtQueryInformationThread 函数签名：
    // - 第二个参数直接用 ULONG，规避 SDK 枚举差异。
    using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    // NtQueryInformationProcess 函数签名：
    // - 这里统一使用 ULONG 信息类，避免 SDK 版本差异导致的编译问题。
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    // PROCESSINFOCLASS 关键常量：
    // - 这些值用于补齐令牌页/PEB页的深度信息读取；
    // - 若目标系统不支持对应信息类，查询会返回失败并自动降级。
    constexpr ULONG kProcessInfoClassDebugPort = 7;
    constexpr ULONG kProcessInfoClassBreakOnTermination = 29;
    constexpr ULONG kProcessInfoClassExecuteFlags = 34;
    constexpr ULONG kProcessInfoClassCommandLine = 60;
    constexpr ULONG kProcessInfoClassProtection = 61;
    constexpr ULONG kProcessInfoClassSubsystem = 75;

    // GetProcessInformation(ProcessPowerThrottling) 所需结构：
    // - 采用本地定义，避免依赖高版本 SDK 才可见的类型声明。
    struct ProcessPowerThrottlingStateNative
    {
        ULONG version = 0;      // 结构版本号。
        ULONG controlMask = 0;  // 可控位掩码。
        ULONG stateMask = 0;    // 当前状态位掩码。
    };

    // GetProcessInformation 动态函数签名：
    // - 第二个参数用 ULONG 表示信息类，兼容不同头文件环境。
    using GetProcessInformationFn = BOOL(WINAPI*)(HANDLE, ULONG, LPVOID, DWORD);

    // queryNtProcessInfoFixed：
    // - 读取“固定大小”的 NtQueryInformationProcess 输出结构；
    // - 成功返回 true，失败返回 false。
    template<typename T>
    bool queryNtProcessInfoFixed(
        const NtQueryInformationProcessFn queryFunction,
        HANDLE processHandle,
        const ULONG infoClass,
        T& outValue)
    {
        if (queryFunction == nullptr || processHandle == nullptr)
        {
            return false;
        }

        std::memset(&outValue, 0, sizeof(T));
        const NTSTATUS status = queryFunction(
            processHandle,
            infoClass,
            &outValue,
            static_cast<ULONG>(sizeof(T)),
            nullptr);
        return NT_SUCCESS(status);
    }

    // queryNtProcessInfoBuffer：
    // - 读取“变长缓冲区”类型的 NtQueryInformationProcess 输出；
    // - 例如 ProcessCommandLineInformation 等场景。
    bool queryNtProcessInfoBuffer(
        const NtQueryInformationProcessFn queryFunction,
        HANDLE processHandle,
        const ULONG infoClass,
        std::vector<std::uint8_t>& bufferOut)
    {
        bufferOut.clear();
        if (queryFunction == nullptr || processHandle == nullptr)
        {
            return false;
        }

        ULONG requiredLength = 0;
        NTSTATUS firstStatus = queryFunction(
            processHandle,
            infoClass,
            nullptr,
            0,
            &requiredLength);

        if (!NT_SUCCESS(firstStatus) && requiredLength == 0)
        {
            return false;
        }

        if (requiredLength < sizeof(UNICODE_STRING))
        {
            requiredLength = static_cast<ULONG>(sizeof(UNICODE_STRING) + 512);
        }

        bufferOut.resize(requiredLength + sizeof(wchar_t), 0);
        NTSTATUS secondStatus = queryFunction(
            processHandle,
            infoClass,
            bufferOut.data(),
            static_cast<ULONG>(bufferOut.size()),
            &requiredLength);
        if (!NT_SUCCESS(secondStatus))
        {
            bufferOut.clear();
            return false;
        }
        return true;
    }

    // queryCommandLineTextByNt：
    // - 通过 ProcessCommandLineInformation 读取命令行；
    // - 同时兼容“返回内嵌字符串”与“返回远程指针”两种实现。
    QString queryCommandLineTextByNt(
        const NtQueryInformationProcessFn queryFunction,
        HANDLE processHandle)
    {
        std::vector<std::uint8_t> commandBuffer;
        if (!queryNtProcessInfoBuffer(
            queryFunction,
            processHandle,
            kProcessInfoClassCommandLine,
            commandBuffer))
        {
            return QString();
        }

        const auto* commandUnicode = reinterpret_cast<const UNICODE_STRING*>(commandBuffer.data());
        if (commandUnicode == nullptr || commandUnicode->Length == 0 || commandUnicode->Buffer == nullptr)
        {
            return QString();
        }

        const std::uintptr_t bufferBegin = reinterpret_cast<std::uintptr_t>(commandBuffer.data());
        const std::uintptr_t bufferEnd = bufferBegin + commandBuffer.size();
        const std::uintptr_t stringPtr = reinterpret_cast<std::uintptr_t>(commandUnicode->Buffer);
        const std::size_t stringLengthBytes = static_cast<std::size_t>(commandUnicode->Length);

        // 情况A：字符串直接位于返回缓冲区中。
        if (stringPtr >= bufferBegin && stringPtr + stringLengthBytes <= bufferEnd)
        {
            return QString::fromWCharArray(
                reinterpret_cast<const wchar_t*>(stringPtr),
                static_cast<int>(commandUnicode->Length / sizeof(wchar_t)));
        }

        // 情况B：返回的是远程指针，需二次 ReadProcessMemory。
        std::vector<wchar_t> remoteChars(
            static_cast<std::size_t>(commandUnicode->Length / sizeof(wchar_t)) + 1,
            L'\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(
            processHandle,
            commandUnicode->Buffer,
            remoteChars.data(),
            commandUnicode->Length,
            &bytesRead);
        if (readOk == FALSE || bytesRead == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(remoteChars.data());
    }

    // memoryStateToText：
    // - 内存区域 State 字段文本化。
    QString memoryStateToText(const DWORD stateValue)
    {
        switch (stateValue)
        {
        case MEM_COMMIT: return QStringLiteral("Commit");
        case MEM_RESERVE: return QStringLiteral("Reserve");
        case MEM_FREE: return QStringLiteral("Free");
        default: return QString("State(0x%1)").arg(stateValue, 0, 16).toUpper();
        }
    }

    // memoryTypeToText：
    // - 内存区域 Type 字段文本化。
    QString memoryTypeToText(const DWORD typeValue)
    {
        switch (typeValue)
        {
        case MEM_IMAGE: return QStringLiteral("Image");
        case MEM_MAPPED: return QStringLiteral("Mapped");
        case MEM_PRIVATE: return QStringLiteral("Private");
        default: return QString("Type(0x%1)").arg(typeValue, 0, 16).toUpper();
        }
    }

    // memoryProtectToText：
    // - 内存区域保护属性文本化，便于列表审计。
    QString memoryProtectToText(const DWORD protectValue)
    {
        if (protectValue == 0)
        {
            return QStringLiteral("-");
        }

        QStringList flags;
        const DWORD baseProtect = protectValue & 0xFF;
        switch (baseProtect)
        {
        case PAGE_NOACCESS: flags << QStringLiteral("NOACCESS"); break;
        case PAGE_READONLY: flags << QStringLiteral("R"); break;
        case PAGE_READWRITE: flags << QStringLiteral("RW"); break;
        case PAGE_WRITECOPY: flags << QStringLiteral("WC"); break;
        case PAGE_EXECUTE: flags << QStringLiteral("X"); break;
        case PAGE_EXECUTE_READ: flags << QStringLiteral("XR"); break;
        case PAGE_EXECUTE_READWRITE: flags << QStringLiteral("XRW"); break;
        case PAGE_EXECUTE_WRITECOPY: flags << QStringLiteral("XWC"); break;
        default: flags << QString("0x%1").arg(baseProtect, 0, 16).toUpper(); break;
        }

        if ((protectValue & PAGE_GUARD) != 0) flags << QStringLiteral("GUARD");
        if ((protectValue & PAGE_NOCACHE) != 0) flags << QStringLiteral("NOCACHE");
        if ((protectValue & PAGE_WRITECOMBINE) != 0) flags << QStringLiteral("WRITECOMBINE");
        return flags.join('|');
    }

    // protectionLevelToText：
    // - ProcessProtectionInformation 的单字节级别文本化。
    QString protectionLevelToText(const std::uint8_t protectionLevel)
    {
        if (protectionLevel == 0)
        {
            return QStringLiteral("None");
        }

        const std::uint8_t signer = protectionLevel >> 4;
        const std::uint8_t type = protectionLevel & 0x07;
        return QStringLiteral("Level=0x%1 (Signer=%2, Type=%3)")
            .arg(protectionLevel, 2, 16, QChar('0'))
            .arg(signer)
            .arg(type);
    }

    // countTopLevelWindowsByPid：
    // - 枚举所有顶层窗口并统计目标 PID 持有数量。
    std::uint32_t countTopLevelWindowsByPid(const std::uint32_t pid)
    {
        struct EnumContext final
        {
            std::uint32_t targetPid = 0;   // 目标进程 PID。
            std::uint32_t windowCount = 0; // 统计结果。
        };

        EnumContext context{};
        context.targetPid = pid;

        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL
            {
                auto* ctx = reinterpret_cast<EnumContext*>(param);
                if (ctx == nullptr)
                {
                    return FALSE;
                }

                DWORD ownerPid = 0;
                GetWindowThreadProcessId(hwnd, &ownerPid);
                if (ownerPid == ctx->targetPid)
                {
                    ++ctx->windowCount;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));

        return context.windowCount;
    }

    // queryDesktopNameByProcessThreads：
    // - 通过目标进程线程 ID 获取其桌面对象名称（可用时）。
    QString queryDesktopNameByProcessThreads(const std::uint32_t pid)
    {
        HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return QString();
        }

        QString desktopName;
        THREADENTRY32 threadEntry{};
        threadEntry.dwSize = sizeof(threadEntry);
        BOOL hasThread = Thread32First(snapshotHandle, &threadEntry);
        while (hasThread != FALSE)
        {
            if (threadEntry.th32OwnerProcessID == pid)
            {
                HDESK desktopHandle = GetThreadDesktop(threadEntry.th32ThreadID);
                if (desktopHandle != nullptr)
                {
                    wchar_t desktopBuffer[256] = {};
                    DWORD bytesNeeded = 0;
                    const BOOL queryOk = GetUserObjectInformationW(
                        desktopHandle,
                        UOI_NAME,
                        desktopBuffer,
                        static_cast<DWORD>(sizeof(desktopBuffer)),
                        &bytesNeeded);
                    if (queryOk != FALSE)
                    {
                        desktopName = QString::fromWCharArray(desktopBuffer);
                    }
                }
                break;
            }
            hasThread = Thread32Next(snapshotHandle, &threadEntry);
        }

        CloseHandle(snapshotHandle);
        return desktopName;
    }

    // ThreadBasicInformationNative：
    // - 对应线程基础信息结构；
    // - 只保留当前页面渲染需要的字段。
    struct ThreadBasicInformationNative
    {
        NTSTATUS exitStatus = 0;           // 线程退出状态。
        PVOID tebBaseAddress = nullptr;    // TEB 地址。
        CLIENT_ID clientId{};              // 客户端 ID。
        ULONG_PTR affinityMask = 0;        // 亲和性掩码。
        LONG priority = 0;                 // 当前优先级。
        LONG basePriority = 0;             // 基础优先级。
    };

    // queryTokenInfoBuffer：
    // - 统一读取令牌信息；
    // - 成功时把字节内容写入 bufferOut。
    bool queryTokenInfoBuffer(
        HANDLE tokenHandle,
        TOKEN_INFORMATION_CLASS infoClass,
        std::vector<std::uint8_t>& bufferOut)
    {
        bufferOut.clear();
        DWORD requiredLength = 0;
        GetTokenInformation(tokenHandle, infoClass, nullptr, 0, &requiredLength);
        if (requiredLength == 0)
        {
            return false;
        }

        bufferOut.resize(requiredLength);
        const BOOL queryOk = GetTokenInformation(
            tokenHandle,
            infoClass,
            bufferOut.data(),
            requiredLength,
            &requiredLength);
        return queryOk != FALSE;
    }

    // describeIntegrityLevel：
    // - 完整性 RID 转文本。
    QString describeIntegrityLevel(const DWORD integrityRid)
    {
        switch (integrityRid)
        {
        case SECURITY_MANDATORY_UNTRUSTED_RID: return QStringLiteral("Untrusted");
        case SECURITY_MANDATORY_LOW_RID: return QStringLiteral("Low");
        case SECURITY_MANDATORY_MEDIUM_RID: return QStringLiteral("Medium");
        case SECURITY_MANDATORY_HIGH_RID: return QStringLiteral("High");
        case SECURITY_MANDATORY_SYSTEM_RID: return QStringLiteral("System");
        case SECURITY_MANDATORY_PROTECTED_PROCESS_RID: return QStringLiteral("ProtectedProcess");
        default: return QString("RID=%1").arg(integrityRid);
        }
    }

    // describePriorityClass：
    // - 优先级类常量转文本。
    QString describePriorityClass(const DWORD priorityClass)
    {
        switch (priorityClass)
        {
        case IDLE_PRIORITY_CLASS: return QStringLiteral("IDLE");
        case BELOW_NORMAL_PRIORITY_CLASS: return QStringLiteral("BELOW_NORMAL");
        case NORMAL_PRIORITY_CLASS: return QStringLiteral("NORMAL");
        case ABOVE_NORMAL_PRIORITY_CLASS: return QStringLiteral("ABOVE_NORMAL");
        case HIGH_PRIORITY_CLASS: return QStringLiteral("HIGH");
        case REALTIME_PRIORITY_CLASS: return QStringLiteral("REALTIME");
        default: return QString("UNKNOWN(%1)").arg(priorityClass);
        }
    }
}

void ProcessDetailWindow::updateThreadInspectStatusLabel(const QString& statusText, const bool refreshing)
{
    // 状态标签刷新：
    // - refreshing=true 显示蓝色进行中；
    // - false 显示灰色完成态。
    if (m_threadInspectStatusLabel == nullptr)
    {
        return;
    }

    m_threadInspectStatusLabel->setText(statusText);
    m_threadInspectStatusLabel->setStyleSheet(
        refreshing
        ? buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700)
        : buildStateLabelStyle(statusSecondaryColor(), 600));
}

void ProcessDetailWindow::requestAsyncThreadInspectRefresh()
{
    // 防重入与非法 PID 保护。
    if (m_threadInspectRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    m_threadInspectRefreshing = true;
    const std::uint64_t ticketValue = ++m_threadInspectRefreshTicket;
    updateThreadInspectStatusLabel(QStringLiteral("● 正在刷新线程细节..."), true);
    if (m_refreshThreadInspectButton != nullptr)
    {
        m_refreshThreadInspectButton->setEnabled(false);
    }

    if (m_threadInspectRefreshProgressPid == 0)
    {
        m_threadInspectRefreshProgressPid = kPro.add("进程详情", "刷新线程细节");
    }
    kPro.set(m_threadInspectRefreshProgressPid, "扫描线程信息", 0, 20.0f);

    const std::uint32_t pidValue = m_baseRecord.pid;
    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, pidValue, ticketValue]()
        {
            ThreadInspectRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();

            HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshotHandle == INVALID_HANDLE_VALUE)
            {
                refreshResult.diagnosticText = QString("CreateToolhelp32Snapshot失败(%1)").arg(GetLastError());
            }
            else
            {
                HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
                NtQueryInformationThreadFn ntQueryThread = nullptr;
                if (ntdllModule != nullptr)
                {
                    ntQueryThread = reinterpret_cast<NtQueryInformationThreadFn>(
                        GetProcAddress(ntdllModule, "NtQueryInformationThread"));
                }

                THREADENTRY32 threadEntry{};
                threadEntry.dwSize = sizeof(threadEntry);
                BOOL hasThread = Thread32First(snapshotHandle, &threadEntry);
                while (hasThread != FALSE)
                {
                    if (threadEntry.th32OwnerProcessID == pidValue)
                    {
                        ThreadInspectItem rowItem{};
                        rowItem.threadId = threadEntry.th32ThreadID;
                        rowItem.stateText = QStringLiteral("Unknown");
                        rowItem.priorityValue = 0;
                        rowItem.switchCount = 0;
                        rowItem.startAddressText = QStringLiteral("-");
                        rowItem.tebAddressText = QStringLiteral("-");
                        rowItem.affinityText = QStringLiteral("-");
                        rowItem.registerSummaryText = QStringLiteral("-");

                        HANDLE threadHandle = OpenThread(
                            THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                            FALSE,
                            threadEntry.th32ThreadID);
                        if (threadHandle != nullptr)
                        {
                            rowItem.priorityValue = GetThreadPriority(threadHandle);
                            if (ntQueryThread != nullptr)
                            {
                                ThreadBasicInformationNative basicInfo{};
                                NTSTATUS basicStatus = ntQueryThread(
                                    threadHandle,
                                    0,
                                    &basicInfo,
                                    static_cast<ULONG>(sizeof(basicInfo)),
                                    nullptr);
                                if (NT_SUCCESS(basicStatus))
                                {
                                    rowItem.tebAddressText = uint64ToHex(
                                        reinterpret_cast<std::uint64_t>(basicInfo.tebBaseAddress));
                                    rowItem.affinityText = uint64ToHex(basicInfo.affinityMask);
                                    rowItem.stateText = (basicInfo.exitStatus == STATUS_PENDING)
                                        ? QStringLiteral("Running")
                                        : QStringLiteral("Exited");
                                }

                                PVOID startAddress = nullptr;
                                NTSTATUS startStatus = ntQueryThread(
                                    threadHandle,
                                    9,
                                    &startAddress,
                                    static_cast<ULONG>(sizeof(startAddress)),
                                    nullptr);
                                if (NT_SUCCESS(startStatus))
                                {
                                    rowItem.startAddressText = uint64ToHex(
                                        reinterpret_cast<std::uint64_t>(startAddress));
                                }
                            }

                            CONTEXT threadContext{};
                            threadContext.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                            const DWORD suspendCount = SuspendThread(threadHandle);
                            if (suspendCount != static_cast<DWORD>(-1))
                            {
                                const BOOL contextOk = GetThreadContext(threadHandle, &threadContext);
                                ResumeThread(threadHandle);
                                if (contextOk != FALSE)
                                {
#if defined(_M_X64)
                                    rowItem.registerSummaryText = QString("RIP=%1 RSP=%2")
                                        .arg(uint64ToHex(threadContext.Rip))
                                        .arg(uint64ToHex(threadContext.Rsp));
#elif defined(_M_IX86)
                                    rowItem.registerSummaryText = QString("EIP=%1 ESP=%2")
                                        .arg(uint64ToHex(threadContext.Eip))
                                        .arg(uint64ToHex(threadContext.Esp));
#else
                                    rowItem.registerSummaryText = QStringLiteral("当前架构未实现");
#endif
                                }
                            }
                            CloseHandle(threadHandle);
                        }
                        else
                        {
                            rowItem.stateText = QStringLiteral("AccessDenied");
                        }

                        refreshResult.rows.push_back(std::move(rowItem));
                    }

                    hasThread = Thread32Next(snapshotHandle, &threadEntry);
                }
                CloseHandle(snapshotHandle);
            }

            std::sort(
                refreshResult.rows.begin(),
                refreshResult.rows.end(),
                [](const ThreadInspectItem& left, const ThreadInspectItem& right)
                {
                    return left.threadId < right.threadId;
                });

            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, refreshResult, ticketValue]()
                {
                    if (guardThis == nullptr || guardThis->m_threadInspectRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyThreadInspectResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyThreadInspectResult(const ThreadInspectRefreshResult& refreshResult)
{
    m_threadInspectRefreshing = false;
    if (m_refreshThreadInspectButton != nullptr)
    {
        m_refreshThreadInspectButton->setEnabled(true);
    }

    if (m_threadInspectTable != nullptr)
    {
        m_threadInspectTable->setSortingEnabled(false);
        m_threadInspectTable->setRowCount(0);
        for (const ThreadInspectItem& rowItem : refreshResult.rows)
        {
            const int row = m_threadInspectTable->rowCount();
            m_threadInspectTable->insertRow(row);
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::ThreadId), new QTableWidgetItem(QString::number(rowItem.threadId)));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::State), new QTableWidgetItem(rowItem.stateText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::Priority), new QTableWidgetItem(QString::number(rowItem.priorityValue)));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::SwitchCount), new QTableWidgetItem(QString::number(rowItem.switchCount)));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::StartAddress), new QTableWidgetItem(rowItem.startAddressText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::TebAddress), new QTableWidgetItem(rowItem.tebAddressText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::Affinity), new QTableWidgetItem(rowItem.affinityText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::RegisterSummary), new QTableWidgetItem(rowItem.registerSummaryText));
        }
        m_threadInspectTable->setSortingEnabled(true);
    }

    QString statusText = QString("● 刷新完成 %1 ms | 线程数 %2")
        .arg(refreshResult.elapsedMs)
        .arg(refreshResult.rows.size());
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QString(" | %1").arg(refreshResult.diagnosticText);
    }
    updateThreadInspectStatusLabel(statusText, false);
    kPro.set(m_threadInspectRefreshProgressPid, "线程细节刷新完成", 0, 100.0f);
}

void ProcessDetailWindow::requestAsyncTokenRefresh()
{
    // 令牌页异步刷新：
    // - 解析用户 SID、完整性级别、组和特权；
    // - 全流程在后台线程执行，避免阻塞窗口交互。
    if (m_tokenRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    m_tokenRefreshing = true;
    const std::uint64_t ticketValue = ++m_tokenRefreshTicket;
    if (m_refreshTokenButton != nullptr)
    {
        m_refreshTokenButton->setEnabled(false);
    }
    if (m_tokenStatusLabel != nullptr)
    {
        m_tokenStatusLabel->setText(QStringLiteral("● 正在刷新令牌..."));
        m_tokenStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
    }

    if (m_tokenRefreshProgressPid == 0)
    {
        m_tokenRefreshProgressPid = kPro.add("进程详情", "刷新令牌信息");
    }
    kPro.set(m_tokenRefreshProgressPid, "读取令牌字段", 0, 20.0f);

    const std::uint32_t pidValue = m_baseRecord.pid;
    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, pidValue, ticketValue]()
        {
            TextRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();
            std::wostringstream textBuilder;
            textBuilder << L"[Token / Security Information]\n";
            textBuilder << L"PID: " << pidValue << L"\n";

            // 打开进程句柄：
            // - 令牌信息需要 PROCESS_QUERY_LIMITED_INFORMATION；
            // - 安全扩展字段与虚拟内存摘要需要 PROCESS_QUERY_INFORMATION / VM_READ。
            HANDLE processHandle = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                pidValue);
            if (processHandle == nullptr)
            {
                processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pidValue);
            }
            if (processHandle == nullptr)
            {
                refreshResult.diagnosticText = QString("OpenProcess失败(%1)").arg(GetLastError());
            }
            else
            {
                // 动态加载 NtQueryInformationProcess：
                // - 用于读取 DebugPort/保护级别/DEP/关键进程等原生字段。
                HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
                NtQueryInformationProcessFn ntQueryProcess = reinterpret_cast<NtQueryInformationProcessFn>(
                    ntdllModule != nullptr ? GetProcAddress(ntdllModule, "NtQueryInformationProcess") : nullptr);

                HANDLE tokenHandle = nullptr;
                if (OpenProcessToken(processHandle, TOKEN_QUERY, &tokenHandle) == FALSE)
                {
                    refreshResult.diagnosticText = QString("OpenProcessToken失败(%1)").arg(GetLastError());
                }
                else
                {
                    std::vector<std::uint8_t> tokenBuffer;

                    // TokenUser：输出用户 SID 与账户名。
                    if (queryTokenInfoBuffer(tokenHandle, TokenUser, tokenBuffer))
                    {
                        const auto* tokenUserInfo = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
                        textBuilder << L"User: " << convertSidToText(tokenUserInfo->User.Sid).toStdWString() << L"\n";
                    }

                    // TokenIntegrityLevel：输出完整性级别。
                    if (queryTokenInfoBuffer(tokenHandle, TokenIntegrityLevel, tokenBuffer))
                    {
                        const auto* mandatoryLabel = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(tokenBuffer.data());
                        DWORD integrityRid = 0;
                        if (mandatoryLabel->Label.Sid != nullptr &&
                            *GetSidSubAuthorityCount(mandatoryLabel->Label.Sid) > 0)
                        {
                            integrityRid = *GetSidSubAuthority(
                                mandatoryLabel->Label.Sid,
                                *GetSidSubAuthorityCount(mandatoryLabel->Label.Sid) - 1);
                        }
                        textBuilder << L"Integrity: " << describeIntegrityLevel(integrityRid).toStdWString() << L"\n";
                    }

                    // TokenElevationType：输出提升类型（Default/Full/Limited）。
                    TOKEN_ELEVATION_TYPE elevationType = TokenElevationTypeDefault;
                    if (queryTokenInfoBuffer(tokenHandle, TokenElevationType, tokenBuffer) &&
                        tokenBuffer.size() >= sizeof(TOKEN_ELEVATION_TYPE))
                    {
                        elevationType = *reinterpret_cast<const TOKEN_ELEVATION_TYPE*>(tokenBuffer.data());
                    }
                    const wchar_t* elevationTypeText = L"Default";
                    if (elevationType == TokenElevationTypeFull)
                    {
                        elevationTypeText = L"Full";
                    }
                    else if (elevationType == TokenElevationTypeLimited)
                    {
                        elevationTypeText = L"Limited";
                    }
                    textBuilder << L"ElevationType: " << elevationTypeText << L"\n";

                    // TokenElevation：输出是否已提升。
                    if (queryTokenInfoBuffer(tokenHandle, TokenElevation, tokenBuffer) &&
                        tokenBuffer.size() >= sizeof(TOKEN_ELEVATION))
                    {
                        const auto* elevation = reinterpret_cast<const TOKEN_ELEVATION*>(tokenBuffer.data());
                        textBuilder << L"IsElevated: " << (elevation->TokenIsElevated != 0 ? L"true" : L"false") << L"\n";
                    }

                    // TokenLinkedToken：输出链接令牌是否存在（管理员拆分令牌场景）。
                    if (queryTokenInfoBuffer(tokenHandle, TokenLinkedToken, tokenBuffer) &&
                        tokenBuffer.size() >= sizeof(TOKEN_LINKED_TOKEN))
                    {
                        const auto* linkedTokenInfo = reinterpret_cast<const TOKEN_LINKED_TOKEN*>(tokenBuffer.data());
                        const bool hasLinkedToken =
                            linkedTokenInfo->LinkedToken != nullptr &&
                            linkedTokenInfo->LinkedToken != INVALID_HANDLE_VALUE;
                        textBuilder << L"LinkedToken: " << (hasLinkedToken ? L"Present" : L"None") << L"\n";
                        if (hasLinkedToken)
                        {
                            CloseHandle(linkedTokenInfo->LinkedToken);
                        }
                    }
                    else
                    {
                        textBuilder << L"LinkedToken: Unavailable\n";
                    }

                    // TokenRestrictedSids：输出限制 SID 数量。
                    if (queryTokenInfoBuffer(tokenHandle, TokenRestrictedSids, tokenBuffer))
                    {
                        const auto* restrictedSids = reinterpret_cast<const TOKEN_GROUPS*>(tokenBuffer.data());
                        textBuilder << L"RestrictedSidCount: " << restrictedSids->GroupCount << L"\n";
                    }
                    else
                    {
                        textBuilder << L"RestrictedSidCount: 0\n";
                    }

                    // TokenGroups：输出组数量与预览。
                    if (queryTokenInfoBuffer(tokenHandle, TokenGroups, tokenBuffer))
                    {
                        const auto* groupsInfo = reinterpret_cast<const TOKEN_GROUPS*>(tokenBuffer.data());
                        textBuilder << L"GroupCount: " << groupsInfo->GroupCount << L"\n";
                        const DWORD previewCount = std::min<DWORD>(groupsInfo->GroupCount, 16);
                        for (DWORD index = 0; index < previewCount; ++index)
                        {
                            textBuilder << L"  - "
                                << convertSidToText(groupsInfo->Groups[index].Sid).toStdWString()
                                << L"\n";
                        }
                    }

                    // TokenPrivileges：输出特权数量与启用状态。
                    if (queryTokenInfoBuffer(tokenHandle, TokenPrivileges, tokenBuffer))
                    {
                        const auto* privilegesInfo =
                            reinterpret_cast<const TOKEN_PRIVILEGES*>(tokenBuffer.data());
                        textBuilder << L"PrivilegeCount: " << privilegesInfo->PrivilegeCount << L"\n";
                        const DWORD previewCount = std::min<DWORD>(privilegesInfo->PrivilegeCount, 24);
                        for (DWORD index = 0; index < previewCount; ++index)
                        {
                            const LUID_AND_ATTRIBUTES& privilegeEntry = privilegesInfo->Privileges[index];
                            WCHAR privilegeName[256] = {};
                            DWORD nameLength = static_cast<DWORD>(std::size(privilegeName));
                            const BOOL nameOk = LookupPrivilegeNameW(
                                nullptr,
                                const_cast<LUID*>(&privilegeEntry.Luid),
                                privilegeName,
                                &nameLength);
                            textBuilder << L"  - "
                                << (nameOk != FALSE ? std::wstring(privilegeName) : L"<Unknown>")
                                << ((privilegeEntry.Attributes & SE_PRIVILEGE_ENABLED) != 0 ? L" [Enabled]" : L" [Disabled]")
                                << L"\n";
                        }
                    }

                    CloseHandle(tokenHandle);
                }

                // 原生进程安全字段：
                // - DebugPort / DEP / Critical / Protection / Subsystem。
                if (ntQueryProcess != nullptr)
                {
                    ULONG_PTR debugPort = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassDebugPort, debugPort))
                    {
                        textBuilder << L"DebugPort: " << uint64ToHex(debugPort).toStdWString() << L"\n";
                    }

                    ULONG executeFlags = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassExecuteFlags, executeFlags))
                    {
                        textBuilder << L"DEPFlags: 0x"
                            << QString::number(executeFlags, 16).toUpper().toStdWString()
                            << L"\n";
                    }

                    ULONG breakOnTermination = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassBreakOnTermination, breakOnTermination))
                    {
                        textBuilder << L"BreakOnTermination(Critical): "
                            << (breakOnTermination != 0 ? L"true" : L"false")
                            << L"\n";
                    }

                    std::uint8_t protectionLevel = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassProtection, protectionLevel))
                    {
                        textBuilder << L"Protection: "
                            << protectionLevelToText(protectionLevel).toStdWString()
                            << L"\n";
                    }

                    ULONG subsystemType = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassSubsystem, subsystemType))
                    {
                        textBuilder << L"SubsystemType: " << subsystemType << L"\n";
                    }
                }

                // GUI 资源统计：GDI / USER 对象计数。
                const DWORD gdiObjectCount = GetGuiResources(processHandle, GR_GDIOBJECTS);
                const DWORD userObjectCount = GetGuiResources(processHandle, GR_USEROBJECTS);
                textBuilder << L"GDIObjectCount: " << gdiObjectCount << L"\n";
                textBuilder << L"USERObjectCount: " << userObjectCount << L"\n";

                // IO 计数器：读写次数与字节数。
                IO_COUNTERS ioCounters{};
                if (GetProcessIoCounters(processHandle, &ioCounters) != FALSE)
                {
                    textBuilder << L"IoReadOps: " << ioCounters.ReadOperationCount << L"\n";
                    textBuilder << L"IoWriteOps: " << ioCounters.WriteOperationCount << L"\n";
                    textBuilder << L"IoReadBytes: " << ioCounters.ReadTransferCount << L"\n";
                    textBuilder << L"IoWriteBytes: " << ioCounters.WriteTransferCount << L"\n";
                }

                // Job 关联信息：是否在 Job 对象中。
                BOOL inJobObject = FALSE;
                if (IsProcessInJob(processHandle, nullptr, &inJobObject) != FALSE)
                {
                    textBuilder << L"InJobObject: " << (inJobObject != FALSE ? L"true" : L"false") << L"\n";
                }

                // 节能状态：动态查询 GetProcessInformation(ProcessPowerThrottling=4)。
                HMODULE kernel32Module = GetModuleHandleW(L"kernel32.dll");
                GetProcessInformationFn getProcessInformation = reinterpret_cast<GetProcessInformationFn>(
                    kernel32Module != nullptr ? GetProcAddress(kernel32Module, "GetProcessInformation") : nullptr);
                if (getProcessInformation != nullptr)
                {
                    ProcessPowerThrottlingStateNative powerState{};
                    powerState.version = 1;
                    const BOOL powerOk = getProcessInformation(
                        processHandle,
                        4,
                        &powerState,
                        static_cast<DWORD>(sizeof(powerState)));
                    if (powerOk != FALSE)
                    {
                        textBuilder << L"PowerThrottlingControlMask: 0x"
                            << QString::number(powerState.controlMask, 16).toUpper().toStdWString()
                            << L"\n";
                        textBuilder << L"PowerThrottlingStateMask: 0x"
                            << QString::number(powerState.stateMask, 16).toUpper().toStdWString()
                            << L"\n";
                    }
                }

                // 窗口相关信息：窗口数量 + 线程桌面名称 + 当前窗口站名称。
                const std::uint32_t windowCount = countTopLevelWindowsByPid(pidValue);
                textBuilder << L"TopLevelWindowCount: " << windowCount << L"\n";

                const QString desktopName = queryDesktopNameByProcessThreads(pidValue);
                if (!desktopName.trimmed().isEmpty())
                {
                    textBuilder << L"ThreadDesktop: " << desktopName.toStdWString() << L"\n";
                }

                HWINSTA processWindowStation = GetProcessWindowStation();
                if (processWindowStation != nullptr)
                {
                    wchar_t stationNameBuffer[256] = {};
                    DWORD stationNameBytes = 0;
                    const BOOL stationOk = GetUserObjectInformationW(
                        processWindowStation,
                        UOI_NAME,
                        stationNameBuffer,
                        static_cast<DWORD>(sizeof(stationNameBuffer)),
                        &stationNameBytes);
                    if (stationOk != FALSE)
                    {
                        textBuilder << L"ProcessWindowStation: " << stationNameBuffer << L"\n";
                    }
                }

                // 会话 ID 与句柄数补充输出。
                DWORD sessionId = 0;
                if (ProcessIdToSessionId(pidValue, &sessionId) != FALSE)
                {
                    textBuilder << L"SessionId: " << sessionId << L"\n";
                }
                DWORD handleCount = 0;
                if (GetProcessHandleCount(processHandle, &handleCount) != FALSE)
                {
                    textBuilder << L"HandleCount: " << handleCount << L"\n";
                }

                CloseHandle(processHandle);
            }

            refreshResult.detailText = QString::fromStdWString(textBuilder.str());
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, refreshResult, ticketValue]()
                {
                    if (guardThis == nullptr || guardThis->m_tokenRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyTokenRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::requestAsyncPebRefresh()
{
    // PEB页异步刷新：
    // - 展示 PEB 地址、优先级、亲和性、内存与 IO 计数；
    // - 额外输出虚拟地址空间统计摘要。
    if (m_pebRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[ProcessDetailWindow] requestAsyncPebRefresh: 开始异步刷新, pid="
            << m_baseRecord.pid
            << eol;
    }

    m_pebRefreshing = true;
    const std::uint64_t ticketValue = ++m_pebRefreshTicket;
    if (m_refreshPebButton != nullptr)
    {
        m_refreshPebButton->setEnabled(false);
    }
    if (m_pebStatusLabel != nullptr)
    {
        m_pebStatusLabel->setText(QStringLiteral("● 正在刷新PEB..."));
        m_pebStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
    }

    if (m_pebRefreshProgressPid == 0)
    {
        m_pebRefreshProgressPid = kPro.add("进程详情", "刷新PEB信息");
    }
    kPro.set(m_pebRefreshProgressPid, "读取PEB与地址空间", 0, 20.0f);

    const std::uint32_t pidValue = m_baseRecord.pid;
    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, pidValue, ticketValue]()
        {
            TextRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();
            const auto progressDispatcher = [guardThis, ticketValue](const QString& stepText, const float progressValue)
                {
                    QMetaObject::invokeMethod(
                        guardThis,
                        [guardThis, ticketValue, stepText, progressValue]()
                        {
                            if (guardThis == nullptr || guardThis->m_pebRefreshTicket != ticketValue)
                            {
                                return;
                            }
                            if (guardThis->m_pebRefreshProgressPid != 0)
                            {
                                kPro.set(
                                    guardThis->m_pebRefreshProgressPid,
                                    stepText.toStdString(),
                                    0,
                                    progressValue);
                            }
                        },
                        Qt::QueuedConnection);
                };

            progressDispatcher(QStringLiteral("打开目标进程"), 28.0f);
            std::wostringstream textBuilder;
            textBuilder << L"[PEB / Process Summary]\n";
            textBuilder << L"PID: " << pidValue << L"\n";

            HANDLE processHandle = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                pidValue);
            if (processHandle == nullptr)
            {
                processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pidValue);
            }
            if (processHandle == nullptr)
            {
                refreshResult.diagnosticText = QString("OpenProcess失败(%1)").arg(GetLastError());
                textBuilder << L"OpenProcess: <failed>\n";
                progressDispatcher(QStringLiteral("打开进程失败"), 100.0f);
            }
            else
            {
                progressDispatcher(QStringLiteral("读取PEB与基础信息"), 40.0f);
                PROCESS_BASIC_INFORMATION basicInfo{};
                HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
                NtQueryInformationProcessFn ntQueryProcess = reinterpret_cast<NtQueryInformationProcessFn>(
                    ntdllModule != nullptr ? GetProcAddress(ntdllModule, "NtQueryInformationProcess") : nullptr);
                if (ntQueryProcess != nullptr)
                {
                    NTSTATUS basicStatus = ntQueryProcess(
                        processHandle,
                        static_cast<ULONG>(ProcessBasicInformation),
                        &basicInfo,
                        static_cast<ULONG>(sizeof(basicInfo)),
                        nullptr);
                    if (NT_SUCCESS(basicStatus))
                    {
                        textBuilder << L"PEB Address: "
                            << uint64ToHex(reinterpret_cast<std::uint64_t>(basicInfo.PebBaseAddress)).toStdWString()
                            << L"\n";
                    }
                }

                // 命令行读取：
                // - 优先使用 ProcessCommandLineInformation；
                // - 失败时回退到已缓存的 m_baseRecord.commandLine。
                const QString commandLineText = queryCommandLineTextByNt(ntQueryProcess, processHandle);
                textBuilder << L"CommandLine: "
                    << (commandLineText.trimmed().isEmpty()
                        ? L"-"
                        : commandLineText.toStdWString())
                    << L"\n";

                // 当前目录：
                // - Nt 层当前目录在不同系统结构布局不稳定；
                // - 这里先给出“映像目录”作为稳定可得的近似值。
                QString imagePathText = QString::fromStdString(ks::process::QueryProcessPathByPid(pidValue));
                QString currentDirectoryText;
                if (!imagePathText.trimmed().isEmpty())
                {
                    currentDirectoryText = QFileInfo(imagePathText).absolutePath();
                }
                textBuilder << L"CurrentDirectory(approx): "
                    << (currentDirectoryText.trimmed().isEmpty()
                        ? L"-"
                        : currentDirectoryText.toStdWString())
                    << L"\n";

                ULONG_PTR processAffinityMask = 0;
                ULONG_PTR systemAffinityMask = 0;
                if (GetProcessAffinityMask(processHandle, &processAffinityMask, &systemAffinityMask) != FALSE)
                {
                    textBuilder << L"ProcessAffinity: " << uint64ToHex(processAffinityMask).toStdWString() << L"\n";
                    std::wostringstream coreTextBuilder;
                    bool firstCore = true;
                    for (int bitIndex = 0; bitIndex < static_cast<int>(sizeof(ULONG_PTR) * 8); ++bitIndex)
                    {
                        const ULONG_PTR mask = static_cast<ULONG_PTR>(1ULL) << bitIndex;
                        if ((processAffinityMask & mask) == 0)
                        {
                            continue;
                        }
                        if (!firstCore)
                        {
                            coreTextBuilder << L",";
                        }
                        coreTextBuilder << bitIndex;
                        firstCore = false;
                    }
                    textBuilder << L"CpuCoreAffinity: " << coreTextBuilder.str() << L"\n";
                }

                const DWORD priorityClass = GetPriorityClass(processHandle);
                textBuilder << L"PriorityClass: " << describePriorityClass(priorityClass).toStdWString() << L"\n";

                // Wow64 状态：
                // - 输出当前进程机器架构与 Wow64 来宾架构。
                USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
                USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
                if (IsWow64Process2(processHandle, &processMachine, &nativeMachine) != FALSE)
                {
                    textBuilder << L"Wow64ProcessMachine: 0x"
                        << QString::number(processMachine, 16).toUpper().toStdWString()
                        << L"\n";
                    textBuilder << L"Wow64NativeMachine: 0x"
                        << QString::number(nativeMachine, 16).toUpper().toStdWString()
                        << L"\n";
                }

                // 启动时间与 CPU 时间（内核 + 用户）。
                FILETIME creationTime{};
                FILETIME exitTime{};
                FILETIME kernelTime{};
                FILETIME userTime{};
                if (GetProcessTimes(
                    processHandle,
                    &creationTime,
                    &exitTime,
                    &kernelTime,
                    &userTime) != FALSE)
                {
                    ULARGE_INTEGER kernelValue{};
                    kernelValue.LowPart = kernelTime.dwLowDateTime;
                    kernelValue.HighPart = kernelTime.dwHighDateTime;
                    ULARGE_INTEGER userValue{};
                    userValue.LowPart = userTime.dwLowDateTime;
                    userValue.HighPart = userTime.dwHighDateTime;
                    const double kernelMs = static_cast<double>(kernelValue.QuadPart) / 10000.0;
                    const double userMs = static_cast<double>(userValue.QuadPart) / 10000.0;
                    textBuilder << L"KernelCpuMs: " << kernelMs << L"\n";
                    textBuilder << L"UserCpuMs: " << userMs << L"\n";
                }

                PROCESS_MEMORY_COUNTERS_EX memoryCounters{};
                if (GetProcessMemoryInfo(
                    processHandle,
                    reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memoryCounters),
                    sizeof(memoryCounters)) != FALSE)
                {
                    textBuilder << L"WorkingSet: " << memoryCounters.WorkingSetSize << L" bytes\n";
                    textBuilder << L"PrivateUsage: " << memoryCounters.PrivateUsage << L" bytes\n";
                    textBuilder << L"PeakWorkingSet: " << memoryCounters.PeakWorkingSetSize << L" bytes\n";
                    textBuilder << L"QuotaPagedPool: " << memoryCounters.QuotaPagedPoolUsage << L" bytes\n";
                    textBuilder << L"QuotaNonPagedPool: " << memoryCounters.QuotaNonPagedPoolUsage << L" bytes\n";
                    textBuilder << L"PageFaultCount: " << memoryCounters.PageFaultCount << L"\n";
                }

                IO_COUNTERS ioCounters{};
                if (GetProcessIoCounters(processHandle, &ioCounters) != FALSE)
                {
                    textBuilder << L"ReadOps: " << ioCounters.ReadOperationCount << L"\n";
                    textBuilder << L"WriteOps: " << ioCounters.WriteOperationCount << L"\n";
                    textBuilder << L"ReadBytes: " << ioCounters.ReadTransferCount << L"\n";
                    textBuilder << L"WriteBytes: " << ioCounters.WriteTransferCount << L"\n";
                }

                progressDispatcher(QStringLiteral("解析映像与环境块"), 55.0f);

                // 子系统信息：ProcessSubsystemInformation（可用时）。
                if (ntQueryProcess != nullptr)
                {
                    ULONG subsystemInfo = 0;
                    if (queryNtProcessInfoFixed(
                        ntQueryProcess,
                        processHandle,
                        kProcessInfoClassSubsystem,
                        subsystemInfo))
                    {
                        textBuilder << L"SubsystemInformation: " << subsystemInfo << L"\n";
                    }
                }

                // 映像基址与入口点：
                // - 通过模块快照获取主模块基址；
                // - 读取 DOS/NT 头解析 EntryPoint RVA。
                std::uint64_t imageBaseAddress = 0;
                HANDLE moduleSnapshot = CreateToolhelp32Snapshot(
                    TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                    pidValue);
                if (moduleSnapshot != INVALID_HANDLE_VALUE)
                {
                    MODULEENTRY32 moduleEntry{};
                    moduleEntry.dwSize = sizeof(moduleEntry);
                    if (Module32First(moduleSnapshot, &moduleEntry) != FALSE)
                    {
                        imageBaseAddress = reinterpret_cast<std::uint64_t>(moduleEntry.modBaseAddr);
                        imagePathText = QString::fromWCharArray(moduleEntry.szExePath);
                    }
                    CloseHandle(moduleSnapshot);
                }
                if (imageBaseAddress != 0)
                {
                    textBuilder << L"ImageBaseAddress: "
                        << uint64ToHex(imageBaseAddress).toStdWString()
                        << L"\n";
                    IMAGE_DOS_HEADER dosHeader{};
                    SIZE_T bytesRead = 0;
                    if (ReadProcessMemory(
                        processHandle,
                        reinterpret_cast<LPCVOID>(imageBaseAddress),
                        &dosHeader,
                        sizeof(dosHeader),
                        &bytesRead) != FALSE &&
                        bytesRead == sizeof(dosHeader) &&
                        dosHeader.e_magic == IMAGE_DOS_SIGNATURE)
                    {
                        IMAGE_NT_HEADERS64 ntHeader64{};
                        if (ReadProcessMemory(
                            processHandle,
                            reinterpret_cast<LPCVOID>(imageBaseAddress + static_cast<std::uint64_t>(dosHeader.e_lfanew)),
                            &ntHeader64,
                            sizeof(ntHeader64),
                            &bytesRead) != FALSE &&
                            bytesRead >= sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) &&
                            ntHeader64.Signature == IMAGE_NT_SIGNATURE)
                        {
                            const std::uint32_t entryRva = ntHeader64.OptionalHeader.AddressOfEntryPoint;
                            textBuilder << L"EntryPointRva: 0x"
                                << QString::number(entryRva, 16).toUpper().toStdWString()
                                << L"\n";
                            textBuilder << L"EntryPointAddress: "
                                << uint64ToHex(imageBaseAddress + entryRva).toStdWString()
                                << L"\n";
                        }
                    }
                }

                // 环境变量块预览：
                // - 通过 PEB->ProcessParameters->Environment 指针读取前若干项；
                // - 若读取失败则给出提示文本。
                struct RtlUserProcessParametersLite final
                {
                    BYTE reserved1[16];
                    PVOID reserved2[10];
                    UNICODE_STRING imagePathName;
                    UNICODE_STRING commandLine;
                    PVOID environment;
                };

                bool environmentPreviewOk = false;
                if (basicInfo.PebBaseAddress != nullptr)
                {
                    PEB pebSnapshot{};
                    SIZE_T bytesRead = 0;
                    if (ReadProcessMemory(
                        processHandle,
                        basicInfo.PebBaseAddress,
                        &pebSnapshot,
                        sizeof(pebSnapshot),
                        &bytesRead) != FALSE &&
                        bytesRead == sizeof(pebSnapshot) &&
                        pebSnapshot.ProcessParameters != nullptr)
                    {
                        RtlUserProcessParametersLite processParameters{};
                        if (ReadProcessMemory(
                            processHandle,
                            pebSnapshot.ProcessParameters,
                            &processParameters,
                            sizeof(processParameters),
                            &bytesRead) != FALSE &&
                            bytesRead == sizeof(processParameters))
                        {
                            const QString commandFromPeb = readRemoteUnicodeString(
                                processHandle,
                                processParameters.commandLine);
                            if (!commandFromPeb.trimmed().isEmpty())
                            {
                                textBuilder << L"CommandLine(PEB): " << commandFromPeb.toStdWString() << L"\n";
                            }
                            const QString imagePathFromPeb = readRemoteUnicodeString(
                                processHandle,
                                processParameters.imagePathName);
                            if (!imagePathFromPeb.trimmed().isEmpty())
                            {
                                textBuilder << L"ImagePath(PEB): " << imagePathFromPeb.toStdWString() << L"\n";
                            }

                            if (processParameters.environment != nullptr)
                            {
                                std::vector<wchar_t> envBuffer(32768, L'\0');
                                SIZE_T envBytesRead = 0;
                                const BOOL envReadOk = ReadProcessMemory(
                                    processHandle,
                                    processParameters.environment,
                                    envBuffer.data(),
                                    static_cast<SIZE_T>(envBuffer.size() * sizeof(wchar_t)),
                                    &envBytesRead);
                                if (envReadOk != FALSE && envBytesRead > sizeof(wchar_t))
                                {
                                    environmentPreviewOk = true;
                                    textBuilder << L"[EnvironmentPreview]\n";
                                    std::size_t previewLines = 0;
                                    const std::size_t maxPreviewLines = 20;
                                    const std::size_t totalChars = envBytesRead / sizeof(wchar_t);
                                    std::size_t cursorIndex = 0;
                                    while (cursorIndex < totalChars && previewLines < maxPreviewLines)
                                    {
                                        const wchar_t* currentLine = envBuffer.data() + cursorIndex;
                                        std::size_t currentLength = 0;
                                        while (cursorIndex + currentLength < totalChars)
                                        {
                                            if (envBuffer[cursorIndex + currentLength] == L'\0')
                                            {
                                                break;
                                            }
                                            ++currentLength;
                                        }
                                        if (currentLength == 0)
                                        {
                                            break;
                                        }
                                        textBuilder << L"  " << currentLine << L"\n";
                                        cursorIndex += currentLength + 1;
                                        ++previewLines;
                                    }
                                }
                            }
                        }
                    }
                }
                if (!environmentPreviewOk)
                {
                    textBuilder << L"[EnvironmentPreview]\n";
                    textBuilder << L"  <unavailable>\n";
                }

                SYSTEM_INFO systemInfo{};
                GetSystemInfo(&systemInfo);
                MEMORY_BASIC_INFORMATION memoryInfo{};
                std::uint64_t commitBytes = 0;
                std::uint64_t mappedBytes = 0;
                std::uint64_t imageBytes = 0;
                std::uint64_t privateBytes = 0;
                std::uint64_t regionCount = 0;
                std::uint64_t previewRegionCount = 0;
                constexpr std::uint64_t kMaxRegionScanCount = 60000;
                const std::uintptr_t minAddress =
                    reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
                const std::uintptr_t maxAddress =
                    reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
                std::uintptr_t cursorAddress = minAddress;
                bool regionScanTruncated = false;
                bool regionScanTimeout = false;
                const auto regionScanDeadline = beginTime + std::chrono::seconds(8);

                textBuilder << L"[VirtualAddressRegionPreview]\n";
                while (cursorAddress < maxAddress)
                {
                    if (std::chrono::steady_clock::now() > regionScanDeadline)
                    {
                        regionScanTimeout = true;
                        break;
                    }

                    const SIZE_T querySize = VirtualQueryEx(
                        processHandle,
                        reinterpret_cast<LPCVOID>(cursorAddress),
                        &memoryInfo,
                        sizeof(memoryInfo));
                    if (querySize == 0)
                    {
                        break;
                    }
                    if (memoryInfo.RegionSize == 0)
                    {
                        refreshResult.diagnosticText = QStringLiteral("虚拟内存枚举遇到零长度区域，已提前终止。");
                        break;
                    }

                    ++regionCount;
                    if (regionCount >= kMaxRegionScanCount)
                    {
                        regionScanTruncated = true;
                        break;
                    }

                    if ((regionCount % 2000) == 0)
                    {
                        const float progressValue = std::min(
                            90.0f,
                            60.0f + static_cast<float>(regionCount) * 0.0005f);
                        progressDispatcher(QStringLiteral("扫描虚拟地址空间"), progressValue);
                    }

                    if (memoryInfo.State == MEM_COMMIT)
                    {
                        commitBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }
                    if (memoryInfo.Type == MEM_MAPPED)
                    {
                        mappedBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }
                    if (memoryInfo.Type == MEM_IMAGE)
                    {
                        imageBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }
                    if (memoryInfo.Type == MEM_PRIVATE)
                    {
                        privateBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }

                    // 预览输出：最多列出 40 个 committed 区域，包含状态/保护/类型/映射文件。
                    if (memoryInfo.State == MEM_COMMIT && previewRegionCount < 40)
                    {
                        QString mappedPathText;
                        wchar_t mappedPathBuffer[1024] = {};
                        if ((memoryInfo.Type == MEM_MAPPED || memoryInfo.Type == MEM_IMAGE) &&
                            GetMappedFileNameW(
                                processHandle,
                                memoryInfo.BaseAddress,
                                mappedPathBuffer,
                                static_cast<DWORD>(std::size(mappedPathBuffer))) > 0)
                        {
                            mappedPathText = QString::fromWCharArray(mappedPathBuffer);
                        }

                        const std::uint64_t baseAddress = reinterpret_cast<std::uint64_t>(memoryInfo.BaseAddress);
                        const std::uint64_t endAddress = baseAddress + static_cast<std::uint64_t>(memoryInfo.RegionSize);
                        textBuilder << L"  "
                            << uint64ToHex(baseAddress).toStdWString()
                            << L"-"
                            << uint64ToHex(endAddress).toStdWString()
                            << L" | "
                            << memoryStateToText(memoryInfo.State).toStdWString()
                            << L" | "
                            << memoryProtectToText(memoryInfo.Protect).toStdWString()
                            << L" | "
                            << memoryTypeToText(memoryInfo.Type).toStdWString();
                        if (!mappedPathText.trimmed().isEmpty())
                        {
                            textBuilder << L" | " << mappedPathText.toStdWString();
                        }
                        textBuilder << L"\n";
                        ++previewRegionCount;
                    }

                    const std::uintptr_t nextAddress =
                        cursorAddress + static_cast<std::uintptr_t>(memoryInfo.RegionSize);
                    if (nextAddress <= cursorAddress)
                    {
                        refreshResult.diagnosticText = QStringLiteral("虚拟内存枚举地址发生回绕，已提前终止。");
                        break;
                    }
                    cursorAddress = nextAddress;
                }
                if (regionScanTruncated)
                {
                    refreshResult.diagnosticText = QString("虚拟内存枚举达到上限(%1)，结果为部分数据。")
                        .arg(kMaxRegionScanCount);
                }
                if (regionScanTimeout)
                {
                    if (!refreshResult.diagnosticText.trimmed().isEmpty())
                    {
                        refreshResult.diagnosticText += QStringLiteral(" | ");
                    }
                    refreshResult.diagnosticText += QStringLiteral("虚拟内存枚举超过8秒，已返回部分结果。");
                }
                textBuilder << L"RegionCount: " << regionCount << L"\n";
                textBuilder << L"CommitBytes: " << commitBytes << L"\n";
                textBuilder << L"MappedBytes: " << mappedBytes << L"\n";
                textBuilder << L"ImageBytes: " << imageBytes << L"\n";
                textBuilder << L"PrivateBytes: " << privateBytes << L"\n";

                // 堆信息：Heap32List + Heap32 遍历统计。
                HANDLE heapSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, pidValue);
                if (heapSnapshot != INVALID_HANDLE_VALUE)
                {
                    HEAPLIST32 heapList{};
                    heapList.dwSize = sizeof(heapList);
                    DWORD heapCount = 0;
                    std::uint64_t heapBlockCount = 0;
                    BOOL hasHeap = Heap32ListFirst(heapSnapshot, &heapList);
                    while (hasHeap != FALSE)
                    {
                        ++heapCount;
                        HEAPENTRY32 heapEntry{};
                        heapEntry.dwSize = sizeof(heapEntry);
                        BOOL hasBlock = Heap32First(
                            &heapEntry,
                            pidValue,
                            static_cast<ULONG_PTR>(heapList.th32HeapID));
                        while (hasBlock != FALSE)
                        {
                            ++heapBlockCount;
                            hasBlock = Heap32Next(&heapEntry);
                        }
                        hasHeap = Heap32ListNext(heapSnapshot, &heapList);
                    }
                    CloseHandle(heapSnapshot);
                    textBuilder << L"HeapCount: " << heapCount << L"\n";
                    textBuilder << L"HeapBlockCount: " << heapBlockCount << L"\n";
                }
                else
                {
                    textBuilder << L"HeapCount: <unavailable>\n";
                }

                progressDispatcher(QStringLiteral("汇总PEB结果"), 95.0f);

                CloseHandle(processHandle);
            }

            if (!refreshResult.diagnosticText.trimmed().isEmpty())
            {
                textBuilder << L"[Diagnostic]\n";
                textBuilder << L"  " << refreshResult.diagnosticText.toStdWString() << L"\n";
            }

            refreshResult.detailText = QString::fromStdWString(textBuilder.str());
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, refreshResult, ticketValue]()
                {
                    if (guardThis == nullptr || guardThis->m_pebRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyPebRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyTokenRefreshResult(const TextRefreshResult& refreshResult)
{
    m_tokenRefreshing = false;
    if (m_refreshTokenButton != nullptr)
    {
        m_refreshTokenButton->setEnabled(true);
    }
    if (m_tokenDetailOutput != nullptr)
    {
        m_tokenDetailOutput->setText(refreshResult.detailText);
    }
    if (m_tokenStatusLabel != nullptr)
    {
        m_tokenStatusLabel->setText(QString("● 刷新完成 %1 ms").arg(refreshResult.elapsedMs));
        m_tokenStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
    }
    kPro.set(m_tokenRefreshProgressPid, "令牌信息刷新完成", 0, 100.0f);
}

void ProcessDetailWindow::applyPebRefreshResult(const TextRefreshResult& refreshResult)
{
    m_pebRefreshing = false;
    if (m_refreshPebButton != nullptr)
    {
        m_refreshPebButton->setEnabled(true);
    }
    if (m_pebDetailOutput != nullptr)
    {
        m_pebDetailOutput->setText(refreshResult.detailText);
    }
    if (m_pebStatusLabel != nullptr)
    {
        QString statusText = QString("● 刷新完成 %1 ms").arg(refreshResult.elapsedMs);
        QString statusStyle = buildStateLabelStyle(statusIdleColor(), 600);
        if (!refreshResult.diagnosticText.trimmed().isEmpty())
        {
            statusText += QString(" | %1").arg(refreshResult.diagnosticText);
            statusStyle = buildStateLabelStyle(statusWarningColor(), 700);
        }
        m_pebStatusLabel->setText(statusText);
        m_pebStatusLabel->setStyleSheet(statusStyle);
    }
    kPro.set(m_pebRefreshProgressPid, "PEB信息刷新完成", 0, 100.0f);

    kLogEvent event;
    info << event
        << "[ProcessDetailWindow] applyPebRefreshResult: 完成, pid="
        << m_baseRecord.pid
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << ", diagnostic="
        << refreshResult.diagnosticText.toStdString()
        << eol;
}
