#include "DriverDock.Internal.h"

// 说明：由原聚合式实现迁移为独立 .cpp，成员函数实现保持原样。
using namespace ksword::driver_dock_internal;

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

    // ============================================================
    // DBWIN 句柄初始化策略：
    // 1) 先尝试 Global\ 前缀（服务/内核侧更常用）；
    // 2) 失败再回退无前缀名称（兼容旧程序）；
    // 3) 事件与共享内存都优先 Open，再 Create，减少“重复实例冲突”。
    // ============================================================
    const auto openOrCreateEventByName = [](const wchar_t* objectName) -> HANDLE
        {
            if (objectName == nullptr)
            {
                return nullptr;
            }

            // 先尝试打开已存在对象，便于附着到同机已有 DBWIN 会话。
            HANDLE eventHandle = ::OpenEventW(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE,
                objectName);
            if (eventHandle != nullptr)
            {
                return eventHandle;
            }

            // 创建安全描述符：空 DACL 允许 SYSTEM/服务进程写入，降低“内核输出丢失”概率。
            SECURITY_ATTRIBUTES securityAttr{};
            SECURITY_DESCRIPTOR securityDesc{};
            securityAttr.nLength = static_cast<DWORD>(sizeof(securityAttr));
            securityAttr.bInheritHandle = FALSE;
            securityAttr.lpSecurityDescriptor = nullptr;
            if (::InitializeSecurityDescriptor(&securityDesc, SECURITY_DESCRIPTOR_REVISION) != FALSE &&
                ::SetSecurityDescriptorDacl(&securityDesc, TRUE, nullptr, FALSE) != FALSE)
            {
                securityAttr.lpSecurityDescriptor = &securityDesc;
            }

            eventHandle = ::CreateEventW(
                &securityAttr,
                FALSE,
                FALSE,
                objectName);
            return eventHandle;
        };

    // openOrCreateMappingByName 作用：
    // - 先打开现有 DBWIN 共享内存，再按需创建；
    // - 复用已存在缓冲可减少与其它捕获器的抢占冲突。
    const auto openOrCreateMappingByName = [](const wchar_t* objectName) -> HANDLE
        {
            if (objectName == nullptr)
            {
                return nullptr;
            }

            HANDLE mappingHandle = ::OpenFileMappingW(
                FILE_MAP_READ | FILE_MAP_WRITE,
                FALSE,
                objectName);
            if (mappingHandle != nullptr)
            {
                return mappingHandle;
            }

            SECURITY_ATTRIBUTES securityAttr{};
            SECURITY_DESCRIPTOR securityDesc{};
            securityAttr.nLength = static_cast<DWORD>(sizeof(securityAttr));
            securityAttr.bInheritHandle = FALSE;
            securityAttr.lpSecurityDescriptor = nullptr;
            if (::InitializeSecurityDescriptor(&securityDesc, SECURITY_DESCRIPTOR_REVISION) != FALSE &&
                ::SetSecurityDescriptorDacl(&securityDesc, TRUE, nullptr, FALSE) != FALSE)
            {
                securityAttr.lpSecurityDescriptor = &securityDesc;
            }

            mappingHandle = ::CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                &securityAttr,
                PAGE_READWRITE,
                0,
                static_cast<DWORD>(sizeof(DbwinBufferPacket)),
                objectName);
            return mappingHandle;
        };

    HANDLE bufferReadyEventHandle = nullptr; // bufferReadyEventHandle：通知写入方“缓冲可写”事件句柄。
    HANDLE dataReadyEventHandle = nullptr;   // dataReadyEventHandle：通知读取方“数据可读”事件句柄。
    HANDLE sharedBufferHandle = nullptr;     // sharedBufferHandle：DBWIN 共享内存句柄。
    QString objectScopeText = QStringLiteral("Global"); // objectScopeText：记录当前成功附着的命名空间。

    // 第一轮：Global\ 前缀（优先覆盖系统服务/会话隔离场景）。
    bufferReadyEventHandle = openOrCreateEventByName(L"Global\\DBWIN_BUFFER_READY");
    dataReadyEventHandle = openOrCreateEventByName(L"Global\\DBWIN_DATA_READY");
    sharedBufferHandle = openOrCreateMappingByName(L"Global\\DBWIN_BUFFER");

    // 第二轮：无前缀回退（兼容旧工具或权限不允许创建 Global 对象的环境）。
    if (bufferReadyEventHandle == nullptr || dataReadyEventHandle == nullptr || sharedBufferHandle == nullptr)
    {
        if (sharedBufferHandle != nullptr)
        {
            ::CloseHandle(sharedBufferHandle);
            sharedBufferHandle = nullptr;
        }
        if (dataReadyEventHandle != nullptr)
        {
            ::CloseHandle(dataReadyEventHandle);
            dataReadyEventHandle = nullptr;
        }
        if (bufferReadyEventHandle != nullptr)
        {
            ::CloseHandle(bufferReadyEventHandle);
            bufferReadyEventHandle = nullptr;
        }

        objectScopeText = QStringLiteral("Local");
        bufferReadyEventHandle = openOrCreateEventByName(L"DBWIN_BUFFER_READY");
        dataReadyEventHandle = openOrCreateEventByName(L"DBWIN_DATA_READY");
        sharedBufferHandle = openOrCreateMappingByName(L"DBWIN_BUFFER");
    }

    if (bufferReadyEventHandle == nullptr || dataReadyEventHandle == nullptr || sharedBufferHandle == nullptr)
    {
        QMetaObject::invokeMethod(this, [guardThis]()
            {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->appendDebugOutputLine(QStringLiteral("启动失败：DBWIN 句柄初始化失败（Global/Local均不可用）。"));
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

    QMetaObject::invokeMethod(this, [guardThis, objectScopeText]()
        {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->appendDebugOutputLine(
                QStringLiteral("捕获线程已启动（命名空间=%1），等待调试输出...").arg(objectScopeText));
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
    // DriverDock now delegates raw SCM enumeration/config reads to ks::service:
    // - input: no UI widgets are passed into the reusable layer;
    // - processing: this wrapper only converts std::wstring records into QString rows;
    // - return: true when SCM enumeration completed, false with UTF-8 error text.
    recordListOut.clear();
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    std::vector<ks::service::ServiceRecord> serviceRecordList;
    if (!ks::service::EnumerateServiceRecords(
        SERVICE_DRIVER,
        SERVICE_STATE_ALL,
        &serviceRecordList,
        errorTextOut))
    {
        return false;
    }

    recordListOut.reserve(serviceRecordList.size());
    for (const ks::service::ServiceRecord& serviceRecord : serviceRecordList)
    {
        DriverServiceRecord driverRecord;
        driverRecord.serviceName = QString::fromStdWString(serviceRecord.serviceName);
        driverRecord.displayName = QString::fromStdWString(serviceRecord.displayName);
        driverRecord.currentState = serviceRecord.status.currentState;
        driverRecord.serviceType = serviceRecord.status.serviceType;
        if (serviceRecord.hasConfig)
        {
            driverRecord.startType = serviceRecord.config.startType;
            driverRecord.errorControl = serviceRecord.config.errorControl;
            driverRecord.binaryPath = QString::fromStdWString(serviceRecord.config.binaryPath);
            if (driverRecord.displayName.trimmed().isEmpty())
            {
                driverRecord.displayName = QString::fromStdWString(serviceRecord.config.displayName);
            }
        }
        driverRecord.description = QString::fromStdWString(serviceRecord.description);
        recordListOut.push_back(std::move(driverRecord));
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
    // Keep DriverDock formatting as a UI adapter only:
    // - input: raw Win32 error code;
    // - processing: ks::service owns FormatMessageW and UTF-8 formatting;
    // - return: QString text suitable for existing log lines.
    return QString::fromUtf8(ks::service::FormatWin32ErrorText(win32ErrorCode).c_str());
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
