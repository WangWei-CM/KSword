#include "DriverDock.Internal.h"

namespace ksword::driver_dock_internal
{
    // toWideString：
    // - 作用：QString 转 std::wstring。
    std::wstring toWideString(const QString& textValue)
    {
        return textValue.toStdWString();
    }

    // createReadOnlyItem：
    // - 作用：创建只读单元格，统一表格交互行为。
    QTableWidgetItem* createReadOnlyItem(const QString& textValue)
    {
        QTableWidgetItem* tableItem = new QTableWidgetItem(textValue);
        tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        return tableItem;
    }

    // formatAddress：
    // - 作用：地址转固定宽度十六进制字符串。
    QString formatAddress(const std::uint64_t addressValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 16, 16, QChar('0'))
            .toUpper();
    }

    // formatCompactAddress：
    // - 作用：地址为 0 时显示 Unavailable；
    // - 适合 DriverObject/DeviceObject 诊断表格中展示可选指针。
    QString formatCompactAddress(const std::uint64_t addressValue)
    {
        if (addressValue == 0)
        {
            return QStringLiteral("Unavailable");
        }
        return formatAddress(addressValue);
    }

    // formatHex32：
    // - 作用：把 32 位 flags/type/status 按固定十六进制展示；
    // - 便于和 WinDbg/WDK 常量对照。
    QString formatHex32(const std::uint32_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(value), 8, 16, QChar('0'))
            .toUpper();
    }

    // formatNtStatusText：
    // - 作用：把 NTSTATUS 以 0xXXXXXXXX 形式展示；
    // - 这里只做原始值展示，不在 UI 中吞掉失败码。
    QString formatNtStatusText(const long statusValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(statusValue), 8, 16, QChar('0'))
            .toUpper();
    }

    // friendlyDriverIoMessage：
    // - 输入：ArkDriverClient 返回的底层 IO 诊断字符串；
    // - 处理：把 DeviceIoControl/unsupported/capability 等机器可读文本转成中文说明；
    // - 返回：适合状态栏、详情区和表格末列展示的短文本。
    QString friendlyDriverIoMessage(const std::string& rawMessage)
    {
        const QString rawText = QString::fromUtf8(rawMessage.data(), static_cast<int>(rawMessage.size())).trimmed();
        if (rawText.isEmpty())
        {
            return driverText("driver.io.empty", QStringLiteral("无额外驱动消息"));
        }
        if (rawText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return driverText(
                "driver.io.device_control_failed",
                QStringLiteral("驱动接口调用失败或当前驱动版本不匹配"));
        }
        if (rawText.contains(QStringLiteral("unsupported"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("not supported"), Qt::CaseInsensitive))
        {
            return driverText(
                "driver.io.unsupported",
                QStringLiteral("当前驱动不支持该只读查询入口"));
        }
        if (rawText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive))
        {
            return driverText(
                "driver.io.capability_missing",
                QStringLiteral("动态偏移能力未满足，请查看内核 DynData/Capability 状态"));
        }
        return rawText;
    }

    // isDriverSignatureLoadError：
    // - 输入：StartServiceW 返回后的 Win32 错误码；
    // - 处理：识别“镜像哈希/签名/策略阻止”类加载失败；
    // - 返回：true 表示应提示用户检查测试签名，而不只提示普通 SCM 错误。
    bool isDriverSignatureLoadError(const DWORD errorCode)
    {
        return errorCode == ERROR_INVALID_IMAGE_HASH ||
            errorCode == ERROR_DRIVER_BLOCKED;
    }

    // formatWin32ErrorTextForAdvice：
    // - 输入：Win32 错误码；
    // - 处理：通过 FormatMessageW 提取系统错误文本；
    // - 返回：带十进制错误码的短文本，供签名修复建议使用。
    QString formatWin32ErrorTextForAdvice(const DWORD errorCode)
    {
        LPWSTR messageBuffer = nullptr;
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
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
        if (messageText.isEmpty())
        {
            messageText = driverText("driver.error.unknown", QStringLiteral("未知错误"));
        }

        return driverText("driver.error.code", QStringLiteral("%1：%2"))
            .arg(static_cast<unsigned long>(errorCode))
            .arg(messageText);
    }

    // buildDriverSignatureLoadAdvice：
    // - 输入：错误码、服务名、驱动路径；
    // - 处理：生成面向开发环境的签名修复说明；
    // - 返回：可直接追加到 DriverDock 操作日志中的多行文本。
    QString buildDriverSignatureLoadAdvice(
        const DWORD errorCode,
        const QString& serviceNameText,
        const QString& binaryPathText)
    {
        QString adviceText = driverText(
            "driver.load_advice.signature_error",
            QStringLiteral(
                "挂载失败：StartServiceW 返回签名/镜像校验错误（%1）。\n"
                "服务：%2\n"
                "驱动路径：%3\n"
                "说明：测试模式并不会放行完全未签名的 x64 .sys；"
                "KswordARK.sys 必须带测试签名，并且测试证书要被本机信任。\n"
                "修复：管理员 PowerShell 进入仓库根目录后执行：\n"
                "powershell -ExecutionPolicy Bypass -File scripts\\Sign-KswordArkDriverTest.ps1 -EnableTestSigning\n"
                "执行后重启，并确认服务 ImagePath 指向刚签名的 KswordARK.sys。"))
            .arg(formatWin32ErrorTextForAdvice(errorCode))
            .arg(serviceNameText)
            .arg(binaryPathText.trimmed().isEmpty()
                ? driverText("driver.load_advice.path_missing", QStringLiteral("<未从表单读取到路径>"))
                : binaryPathText);
        return adviceText;
    }

    // driverObjectQueryStatusText：
    // - 作用：把共享协议查询状态转成用户可读文本；
    // - 仍保留原始 NTSTATUS 由 summary 区展示。
    QString driverObjectQueryStatusText(const std::uint32_t statusValue)
    {
        switch (statusValue)
        {
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NAME_INVALID:
            return QStringLiteral("Name invalid");
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NOT_FOUND:
            return QStringLiteral("Not found");
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_REFERENCE_FAILED:
            return QStringLiteral("Reference failed");
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_BUFFER_TOO_SMALL:
            return QStringLiteral("Buffer too small");
        case KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_QUERY_FAILED:
            return QStringLiteral("Query failed");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // driverForceUnloadStatusText：
    // - 作用：把 R0 强制卸载状态转成 DriverDock 日志文本；
    // - 返回：中文状态描述，原始 NTSTATUS 仍单独展示。
    QString driverForceUnloadStatusText(const std::uint32_t statusValue)
    {
        switch (statusValue)
        {
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED:
            return driverText("driver.unload.status.called", QStringLiteral("已调用 DriverUnload"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_MISSING:
            return driverText("driver.unload.status.missing_routine", QStringLiteral("缺少 DriverUnload"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_REFERENCE_FAILED:
            return driverText("driver.unload.status.reference_failed", QStringLiteral("引用 DriverObject 失败"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_THREAD_FAILED:
            return driverText("driver.unload.status.thread_failed", QStringLiteral("系统线程失败"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT:
            return driverText("driver.unload.status.wait_timeout", QStringLiteral("等待超时"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED:
            return driverText("driver.unload.status.operation_failed", QStringLiteral("操作失败"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP:
            return driverText("driver.unload.status.forced_cleanup", QStringLiteral("已强制清理"));
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_CLEANUP_FAILED:
            return driverText("driver.unload.status.cleanup_failed", QStringLiteral("清理失败"));
        default:
            return driverText("driver.unload.status.unknown", QStringLiteral("未知状态"));
        }
    }

    // driverMajorFunctionName：
    // - 作用：把 IRP_MJ 编号转成稳定名称；
    // - 输出格式保留编号，便于排查自定义过滤栈。
    QString driverMajorFunctionName(const std::uint32_t majorFunction)
    {
        switch (majorFunction)
        {
        case 0x00: return QStringLiteral("IRP_MJ_CREATE");
        case 0x01: return QStringLiteral("IRP_MJ_CREATE_NAMED_PIPE");
        case 0x02: return QStringLiteral("IRP_MJ_CLOSE");
        case 0x03: return QStringLiteral("IRP_MJ_READ");
        case 0x04: return QStringLiteral("IRP_MJ_WRITE");
        case 0x05: return QStringLiteral("IRP_MJ_QUERY_INFORMATION");
        case 0x06: return QStringLiteral("IRP_MJ_SET_INFORMATION");
        case 0x07: return QStringLiteral("IRP_MJ_QUERY_EA");
        case 0x08: return QStringLiteral("IRP_MJ_SET_EA");
        case 0x09: return QStringLiteral("IRP_MJ_FLUSH_BUFFERS");
        case 0x0A: return QStringLiteral("IRP_MJ_QUERY_VOLUME_INFORMATION");
        case 0x0B: return QStringLiteral("IRP_MJ_SET_VOLUME_INFORMATION");
        case 0x0C: return QStringLiteral("IRP_MJ_DIRECTORY_CONTROL");
        case 0x0D: return QStringLiteral("IRP_MJ_FILE_SYSTEM_CONTROL");
        case 0x0E: return QStringLiteral("IRP_MJ_DEVICE_CONTROL");
        case 0x0F: return QStringLiteral("IRP_MJ_INTERNAL_DEVICE_CONTROL");
        case 0x10: return QStringLiteral("IRP_MJ_SHUTDOWN");
        case 0x11: return QStringLiteral("IRP_MJ_LOCK_CONTROL");
        case 0x12: return QStringLiteral("IRP_MJ_CLEANUP");
        case 0x13: return QStringLiteral("IRP_MJ_CREATE_MAILSLOT");
        case 0x14: return QStringLiteral("IRP_MJ_QUERY_SECURITY");
        case 0x15: return QStringLiteral("IRP_MJ_SET_SECURITY");
        case 0x16: return QStringLiteral("IRP_MJ_POWER");
        case 0x17: return QStringLiteral("IRP_MJ_SYSTEM_CONTROL");
        case 0x18: return QStringLiteral("IRP_MJ_DEVICE_CHANGE");
        case 0x19: return QStringLiteral("IRP_MJ_QUERY_QUOTA");
        case 0x1A: return QStringLiteral("IRP_MJ_SET_QUOTA");
        case 0x1B: return QStringLiteral("IRP_MJ_PNP");
        default: return QStringLiteral("IRP_MJ_%1").arg(majorFunction);
        }
    }

    // driverDeviceTypeText：
    // - 作用：对常见 DEVICE_TYPE 做名称提示；
    // - 未知值仍保留十六进制，避免误判。
    QString driverDeviceTypeText(const std::uint32_t deviceType)
    {
        switch (deviceType)
        {
        case FILE_DEVICE_DISK: return QStringLiteral("DISK");
        case FILE_DEVICE_DISK_FILE_SYSTEM: return QStringLiteral("DISK_FILE_SYSTEM");
        case FILE_DEVICE_FILE_SYSTEM: return QStringLiteral("FILE_SYSTEM");
        case FILE_DEVICE_NETWORK: return QStringLiteral("NETWORK");
        case FILE_DEVICE_NETWORK_FILE_SYSTEM: return QStringLiteral("NETWORK_FILE_SYSTEM");
        case FILE_DEVICE_NULL: return QStringLiteral("NULL");
        case FILE_DEVICE_UNKNOWN: return QStringLiteral("UNKNOWN");
        case FILE_DEVICE_KSEC: return QStringLiteral("KSEC");
        default: return formatHex32(deviceType);
        }
    }

    // driverDispatchLocationText：
    // - 作用：标记 dispatch 是否落在 DriverObject 自身镜像内；
    // - “外部模块”用文本明确展示，不只依赖颜色。
    QString driverDispatchLocationText(const std::uint32_t flags)
    {
        const bool resolvedModule = (flags & 0x00000001U) != 0U;
        const bool insideOwnImage = (flags & 0x00000002U) != 0U;
        if (insideOwnImage)
        {
            return driverText("driver.location.inside_image", QStringLiteral("自身镜像内"));
        }
        if (resolvedModule)
        {
            return driverText("driver.location.external_module", QStringLiteral("外部模块"));
        }
        return driverText("driver.location.unresolved_module", QStringLiteral("未解析模块"));
    }

}


using namespace ksword::driver_dock_internal;

DriverDock::DriverDock(QWidget* parent)
    : QWidget(parent)
{
    // initEvent 用途：贯穿整个构造流程，便于按同一 GUID 追踪初始化链路。
    kLogEvent initEvent;
    info << initEvent
        << driverText("driver.log.initializing", QStringLiteral("[DriverDock] 开始初始化驱动页。"))
        << eol;

    initializeUi();
    initializeConnections();
    updateDebugCaptureButtonState();

    info << initEvent
        << driverText("driver.log.initialized", QStringLiteral("[DriverDock] 驱动页初始化完成。"))
        << eol;
}

void DriverDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (m_initialRefreshDone)
    {
        return;
    }

    m_initialRefreshDone = true;
    if (m_overviewStatusLabel != nullptr)
    {
        m_overviewStatusLabel->setText(
            driverText(
                "driver.status.first_open",
                QStringLiteral("状态：首次打开，正在加载驱动服务与模块...")));
    }

    QTimer::singleShot(0, this, [this]()
        {
            refreshDriverServiceRecords();
            refreshLoadedKernelModuleRecords();
        });
}

DriverDock::~DriverDock()
{
    stopDebugOutputCapture();

    kLogEvent destroyEvent;
    info << destroyEvent
        << driverText("driver.log.destroyed", QStringLiteral("[DriverDock] 驱动页已析构。"))
        << eol;
}
