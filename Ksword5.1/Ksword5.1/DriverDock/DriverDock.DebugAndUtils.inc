void DriverDock::startDebugOutputCapture()
{
    if (m_dbwinCaptureRunning.exchange(true))
    {
        appendDebugOutputLine(QStringLiteral("捕获线程已在运行。"));
        return;
    }

    appendDebugOutputLine(QStringLiteral("正在启动调试输出捕获线程..."));
    try
    {
        m_dbwinCaptureThread = std::make_unique<std::thread>([this]()
            {
                runDbwinCaptureLoop();
            });
    }
    catch (...)
    {
        m_dbwinCaptureRunning.store(false);
        m_dbwinCaptureThread.reset();
        appendDebugOutputLine(QStringLiteral("启动失败：无法创建后台线程。"));
    }

    updateDebugCaptureButtonState();
}

void DriverDock::stopDebugOutputCapture()
{
    m_dbwinCaptureRunning.store(false);
    if (m_dbwinCaptureThread != nullptr && m_dbwinCaptureThread->joinable())
    {
        m_dbwinCaptureThread->join();
    }
    m_dbwinCaptureThread.reset();
    updateDebugCaptureButtonState();
}

void DriverDock::runDbwinCaptureLoop()
{
    // guardThis 用途：回投 UI 更新时判空，避免析构后访问悬空对象。
    QPointer<DriverDock> guardThis(this);

    const HANDLE bufferReadyEventHandle = ::CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
    const HANDLE dataReadyEventHandle = ::CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
    const HANDLE sharedBufferHandle = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(DbwinBufferPacket)),
        L"DBWIN_BUFFER");

    if (bufferReadyEventHandle == nullptr || dataReadyEventHandle == nullptr || sharedBufferHandle == nullptr)
    {
        QMetaObject::invokeMethod(this, [guardThis]()
            {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->appendDebugOutputLine(QStringLiteral("启动失败：DBWIN 句柄初始化失败。"));
                guardThis->updateDebugCaptureButtonState();
            }, Qt::QueuedConnection);

        if (sharedBufferHandle != nullptr)
        {
            ::CloseHandle(sharedBufferHandle);
        }
        if (dataReadyEventHandle != nullptr)
        {
            ::CloseHandle(dataReadyEventHandle);
        }
        if (bufferReadyEventHandle != nullptr)
        {
            ::CloseHandle(bufferReadyEventHandle);
        }
        m_dbwinCaptureRunning.store(false);
        return;
    }

    void* mappedView = ::MapViewOfFile(
        sharedBufferHandle,
        FILE_MAP_READ,
        0,
        0,
        static_cast<SIZE_T>(sizeof(DbwinBufferPacket)));
    if (mappedView == nullptr)
    {
        ::CloseHandle(sharedBufferHandle);
        ::CloseHandle(dataReadyEventHandle);
        ::CloseHandle(bufferReadyEventHandle);
        QMetaObject::invokeMethod(this, [guardThis]()
            {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->appendDebugOutputLine(QStringLiteral("启动失败：映射 DBWIN 共享内存失败。"));
                guardThis->updateDebugCaptureButtonState();
            }, Qt::QueuedConnection);
        m_dbwinCaptureRunning.store(false);
        return;
    }

    QMetaObject::invokeMethod(this, [guardThis]()
        {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->appendDebugOutputLine(QStringLiteral("捕获线程已启动，等待调试输出..."));
            guardThis->updateDebugCaptureButtonState();
        }, Qt::QueuedConnection);

    (void)::SetEvent(bufferReadyEventHandle);
    while (m_dbwinCaptureRunning.load())
    {
        const DWORD waitResult = ::WaitForSingleObject(dataReadyEventHandle, 200);
        if (waitResult == WAIT_TIMEOUT)
        {
            (void)::SetEvent(bufferReadyEventHandle);
            continue;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            break;
        }

        DbwinBufferPacket packetSnapshot{};
        std::memcpy(&packetSnapshot, mappedView, sizeof(packetSnapshot));
        packetSnapshot.messageBuffer[sizeof(packetSnapshot.messageBuffer) - 1] = '\0';
        const QString messageText = QString::fromLocal8Bit(packetSnapshot.messageBuffer).trimmed();
        if (!messageText.isEmpty())
        {
            const QString outputLine = QStringLiteral("[PID=%1] %2")
                .arg(packetSnapshot.processId)
                .arg(messageText);
            QMetaObject::invokeMethod(this, [guardThis, outputLine]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->appendDebugOutputLine(outputLine);
                }, Qt::QueuedConnection);
        }
        (void)::SetEvent(bufferReadyEventHandle);
    }

    ::UnmapViewOfFile(mappedView);
    ::CloseHandle(sharedBufferHandle);
    ::CloseHandle(dataReadyEventHandle);
    ::CloseHandle(bufferReadyEventHandle);

    m_dbwinCaptureRunning.store(false);
    QMetaObject::invokeMethod(this, [guardThis]()
        {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->appendDebugOutputLine(QStringLiteral("捕获线程已退出。"));
            guardThis->updateDebugCaptureButtonState();
        }, Qt::QueuedConnection);
}

void DriverDock::updateDebugCaptureButtonState()
{
    const bool running = m_dbwinCaptureRunning.load();
    if (m_startCaptureButton != nullptr)
    {
        m_startCaptureButton->setEnabled(!running);
    }
    if (m_stopCaptureButton != nullptr)
    {
        m_stopCaptureButton->setEnabled(running);
    }
    if (m_debugCaptureStatusLabel != nullptr)
    {
        m_debugCaptureStatusLabel->setText(
            running ? QStringLiteral("状态：捕获运行中") : QStringLiteral("状态：未启动"));
    }
}

void DriverDock::appendOperateLogLine(const QString& logText)
{
    if (m_operateLogOutput == nullptr)
    {
        return;
    }

    const QString timePrefix = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_operateLogOutput->appendPlainText(QStringLiteral("[%1] %2").arg(timePrefix, logText));
}

void DriverDock::appendDebugOutputLine(const QString& debugText)
{
    if (m_debugOutputEdit == nullptr)
    {
        return;
    }

    const QString timePrefix = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_debugOutputEdit->appendPlainText(QStringLiteral("[%1] %2").arg(timePrefix, debugText));
}

bool DriverDock::queryDriverServiceRecords(
    std::vector<DriverServiceRecord>& recordListOut,
    std::string* errorTextOut)
{
    recordListOut.clear();
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    ServiceHandleGuard scmHandle(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scmHandle.valid())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("OpenSCManagerW failed: %1")
                .arg(formatWin32ErrorText(::GetLastError()))
                .toStdString();
        }
        return false;
    }

    // 分页枚举所有驱动服务，避免单次缓冲不足导致漏项。
    DWORD resumeHandle = 0;
    while (true)
    {
        DWORD bytesNeeded = 0;
        DWORD serviceCount = 0;
        const BOOL probeResult = ::EnumServicesStatusExW(
            scmHandle.get(),
            SC_ENUM_PROCESS_INFO,
            SERVICE_DRIVER,
            SERVICE_STATE_ALL,
            nullptr,
            0,
            &bytesNeeded,
            &serviceCount,
            &resumeHandle,
            nullptr);
        if (probeResult != FALSE)
        {
            // 没有可枚举项时会直接成功并返回空结果。
            break;
        }

        const DWORD probeError = ::GetLastError();
        if (probeError != ERROR_MORE_DATA)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("EnumServicesStatusExW probe failed: %1")
                    .arg(formatWin32ErrorText(probeError))
                    .toStdString();
            }
            return false;
        }
        if (bytesNeeded == 0)
        {
            break;
        }

        std::vector<std::uint8_t> buffer(bytesNeeded, 0);
        serviceCount = 0;
        if (!::EnumServicesStatusExW(
            scmHandle.get(),
            SC_ENUM_PROCESS_INFO,
            SERVICE_DRIVER,
            SERVICE_STATE_ALL,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesNeeded,
            &serviceCount,
            &resumeHandle,
            nullptr))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("EnumServicesStatusExW failed: %1")
                    .arg(formatWin32ErrorText(::GetLastError()))
                    .toStdString();
            }
            return false;
        }

        auto* serviceArray = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD index = 0; index < serviceCount; ++index)
        {
            const ENUM_SERVICE_STATUS_PROCESSW& serviceEntry = serviceArray[index];
            DriverServiceRecord serviceRecord;
            serviceRecord.serviceName = QString::fromWCharArray(
                serviceEntry.lpServiceName == nullptr ? L"" : serviceEntry.lpServiceName);
            serviceRecord.displayName = QString::fromWCharArray(
                serviceEntry.lpDisplayName == nullptr ? L"" : serviceEntry.lpDisplayName);
            serviceRecord.currentState = serviceEntry.ServiceStatusProcess.dwCurrentState;
            serviceRecord.serviceType = serviceEntry.ServiceStatusProcess.dwServiceType;

            ServiceHandleGuard serviceHandle(::OpenServiceW(
                scmHandle.get(),
                serviceEntry.lpServiceName,
                SERVICE_QUERY_CONFIG));
            if (serviceHandle.valid())
            {
                DWORD configBytes = 0;
                (void)::QueryServiceConfigW(serviceHandle.get(), nullptr, 0, &configBytes);
                if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && configBytes > 0)
                {
                    std::vector<std::uint8_t> configBuffer(configBytes, 0);
                    QUERY_SERVICE_CONFIGW* configPtr =
                        reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
                    if (::QueryServiceConfigW(serviceHandle.get(), configPtr, configBytes, &configBytes))
                    {
                        serviceRecord.startType = configPtr->dwStartType;
                        serviceRecord.errorControl = configPtr->dwErrorControl;
                        serviceRecord.binaryPath = QString::fromWCharArray(
                            configPtr->lpBinaryPathName == nullptr ? L"" : configPtr->lpBinaryPathName);
                    }
                }

                // 读取服务描述（可选字段），用于概览表的“描述”列展示。
                DWORD descriptionBytes = 0;
                (void)::QueryServiceConfig2W(
                    serviceHandle.get(),
                    SERVICE_CONFIG_DESCRIPTION,
                    nullptr,
                    0,
                    &descriptionBytes);
                if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && descriptionBytes > 0)
                {
                    std::vector<std::uint8_t> descriptionBuffer(descriptionBytes, 0);
                    if (::QueryServiceConfig2W(
                        serviceHandle.get(),
                        SERVICE_CONFIG_DESCRIPTION,
                        descriptionBuffer.data(),
                        static_cast<DWORD>(descriptionBuffer.size()),
                        &descriptionBytes))
                    {
                        auto* descriptionPtr =
                            reinterpret_cast<SERVICE_DESCRIPTIONW*>(descriptionBuffer.data());
                        if (descriptionPtr->lpDescription != nullptr)
                        {
                            serviceRecord.description = QString::fromWCharArray(descriptionPtr->lpDescription);
                        }
                    }
                }
            }

            recordListOut.push_back(std::move(serviceRecord));
        }

        if (resumeHandle == 0)
        {
            break;
        }
    }

    std::sort(
        recordListOut.begin(),
        recordListOut.end(),
        [](const DriverServiceRecord& left, const DriverServiceRecord& right)
        {
            return left.serviceName.compare(right.serviceName, Qt::CaseInsensitive) < 0;
        });
    return true;
}

bool DriverDock::queryLoadedKernelModuleRecords(
    std::vector<LoadedKernelModuleRecord>& recordListOut,
    std::string* errorTextOut)
{
    recordListOut.clear();
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    // 动态扩容驱动基址数组，避免在驱动数量较多时被固定 1024 截断。
    std::vector<LPVOID> moduleBaseList(1024, nullptr);
    DWORD bytesNeeded = 0;
    while (true)
    {
        if (!::EnumDeviceDrivers(
            moduleBaseList.data(),
            static_cast<DWORD>(moduleBaseList.size() * sizeof(LPVOID)),
            &bytesNeeded))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("EnumDeviceDrivers failed: %1")
                    .arg(formatWin32ErrorText(::GetLastError()))
                    .toStdString();
            }
            return false;
        }

        const std::size_t currentCapacityBytes = moduleBaseList.size() * sizeof(LPVOID);
        if (bytesNeeded <= currentCapacityBytes)
        {
            break;
        }

        const std::size_t requiredCount =
            (static_cast<std::size_t>(bytesNeeded) + sizeof(LPVOID) - 1) / sizeof(LPVOID);
        moduleBaseList.assign(requiredCount + 128, nullptr);
    }

    const std::size_t moduleCount = static_cast<std::size_t>(bytesNeeded / sizeof(LPVOID));
    recordListOut.reserve(moduleCount);
    for (std::size_t index = 0; index < moduleCount; ++index)
    {
        LPVOID moduleBase = moduleBaseList[index];
        if (moduleBase == nullptr)
        {
            continue;
        }

        std::array<wchar_t, MAX_PATH> moduleNameBuffer{};
        std::array<wchar_t, 1024> modulePathBuffer{};
        const DWORD nameLength = ::GetDeviceDriverBaseNameW(
            moduleBase,
            moduleNameBuffer.data(),
            static_cast<DWORD>(moduleNameBuffer.size()));
        const DWORD pathLength = ::GetDeviceDriverFileNameW(
            moduleBase,
            modulePathBuffer.data(),
            static_cast<DWORD>(modulePathBuffer.size()));

        LoadedKernelModuleRecord moduleRecord;
        moduleRecord.moduleName = (nameLength == 0)
            ? QStringLiteral("<unknown>")
            : QString::fromWCharArray(moduleNameBuffer.data());
        moduleRecord.imagePath = (pathLength == 0)
            ? QStringLiteral("<unknown>")
            : QString::fromWCharArray(modulePathBuffer.data());
        moduleRecord.baseAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(moduleBase));
        recordListOut.push_back(std::move(moduleRecord));
    }

    std::sort(
        recordListOut.begin(),
        recordListOut.end(),
        [](const LoadedKernelModuleRecord& left, const LoadedKernelModuleRecord& right)
        {
            return left.moduleName.compare(right.moduleName, Qt::CaseInsensitive) < 0;
        });
    return true;
}

QString DriverDock::serviceStateToText(const std::uint32_t stateValue)
{
    switch (stateValue)
    {
    case SERVICE_STOPPED:          return QStringLiteral("已停止");
    case SERVICE_START_PENDING:    return QStringLiteral("启动中");
    case SERVICE_STOP_PENDING:     return QStringLiteral("停止中");
    case SERVICE_RUNNING:          return QStringLiteral("运行中");
    case SERVICE_CONTINUE_PENDING: return QStringLiteral("继续中");
    case SERVICE_PAUSE_PENDING:    return QStringLiteral("暂停中");
    case SERVICE_PAUSED:           return QStringLiteral("已暂停");
    default:                       return QStringLiteral("未知状态(%1)").arg(stateValue);
    }
}

QString DriverDock::startTypeToText(const std::uint32_t startTypeValue)
{
    switch (startTypeValue)
    {
    case SERVICE_BOOT_START:   return QStringLiteral("BOOT");
    case SERVICE_SYSTEM_START: return QStringLiteral("SYSTEM");
    case SERVICE_AUTO_START:   return QStringLiteral("AUTO");
    case SERVICE_DEMAND_START: return QStringLiteral("DEMAND");
    case SERVICE_DISABLED:     return QStringLiteral("DISABLED");
    default:                   return QStringLiteral("UNKNOWN(%1)").arg(startTypeValue);
    }
}

QString DriverDock::errorControlToText(const std::uint32_t errorControlValue)
{
    switch (errorControlValue)
    {
    case SERVICE_ERROR_IGNORE:   return QStringLiteral("IGNORE");
    case SERVICE_ERROR_NORMAL:   return QStringLiteral("NORMAL");
    case SERVICE_ERROR_SEVERE:   return QStringLiteral("SEVERE");
    case SERVICE_ERROR_CRITICAL: return QStringLiteral("CRITICAL");
    default:                     return QStringLiteral("UNKNOWN(%1)").arg(errorControlValue);
    }
}

QString DriverDock::formatWin32ErrorText(const std::uint32_t win32ErrorCode)
{
    LPWSTR messageBuffer = nullptr;
    const DWORD messageLength = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        win32ErrorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    QString messageText;
    if (messageLength > 0 && messageBuffer != nullptr)
    {
        messageText = QString::fromWCharArray(messageBuffer).trimmed();
        ::LocalFree(messageBuffer);
    }
    else
    {
        messageText = QStringLiteral("unknown error");
    }
    return QStringLiteral("%1: %2").arg(win32ErrorCode).arg(messageText);
}

QString DriverDock::trimQuotedText(const QString& textValue)
{
    QString normalizedText = textValue.trimmed();
    if (normalizedText.startsWith('"') && normalizedText.endsWith('"') && normalizedText.size() >= 2)
    {
        normalizedText = normalizedText.mid(1, normalizedText.size() - 2);
    }
    return normalizedText.trimmed();
}

QString DriverDock::normalizeDriverBinaryPath(const QString& pathText)
{
    QString normalizedPath = pathText.trimmed();
    if (normalizedPath.isEmpty())
    {
        return normalizedPath;
    }

    const bool quoted =
        normalizedPath.startsWith('"') &&
        normalizedPath.endsWith('"') &&
        normalizedPath.size() >= 2;
    if (!quoted && normalizedPath.contains(' '))
    {
        normalizedPath = QStringLiteral("\"%1\"").arg(normalizedPath);
    }
    return normalizedPath;
}
