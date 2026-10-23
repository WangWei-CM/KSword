#include "../UI/AsyncOperation.h"
#include "DriverDock.Internal.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableFreezeSupport.h"
#include <QItemSelectionModel>
#include <QPersistentModelIndex>
#include <QSet>

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
    // - 处理：只渲染消息本身。**采集状态不再从这里的子串猜**——那由
    //   describeDriverCollection 走 F-05 归一层判定，见下。
    // - 返回：适合状态栏、详情区和表格末列展示的短文本。
    QString friendlyDriverIoMessage(const std::string& rawMessage)
    {
        const QString rawText = QString::fromUtf8(rawMessage.data(), static_cast<int>(rawMessage.size())).trimmed();
        if (rawText.isEmpty())
        {
            return driverText("driver.io.empty", QStringLiteral("无额外驱动消息"));
        }
        // capability/DynData 是"缺什么能力"的内容提示，不是采集状态，保留。
        if (rawText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive))
        {
            return driverText(
                "driver.io.capability_missing",
                QStringLiteral("动态偏移能力未满足，请查看内核 DynData/Capability 状态"));
        }
        return rawText;
    }

    // describeDriverCollection（F-05 采集状态与结论分离）：
    // - 输入：ArkDriverClient 的 IoResult，以及调用方已知的 unsupported / partial；
    // - 处理：交给 ArkDriverEvidence.h 的 toCollectionOutcome 归一。此前这里靠
    //   rawMessage.contains("DeviceIoControl") / contains("unsupported") 猜状态，
    //   既区分不出拒绝访问与超时，也会丢掉原始错误码；
    // - 返回："<采集状态>（<域> <原始码>）：<原始消息>"。原始码一律保留，
    //   任何分支都不产出"正常"结论——结论由分析侧另行给出。
    QString describeDriverCollection(const ksword::ark::IoResult& ioResult,
                                     const bool unsupported,
                                     const bool partial)
    {
        namespace ev = Ksword::Evidence;

        ksword::ark::DriverCallShape shape;
        shape.unsupported = unsupported;
        shape.partial = partial;
        const ev::CollectionOutcome outcome = ksword::ark::toCollectionOutcome(ioResult, shape);

        QString stateText;
        switch (outcome.status)
        {
        case ev::CollectionStatus::Success:
            stateText = driverText("driver.collect.success", QStringLiteral("采集成功"));
            break;
        case ev::CollectionStatus::Partial:
            stateText = driverText(
                "driver.collect.partial",
                QStringLiteral("采集不完整：结果被截断或未覆盖请求范围"));
            break;
        case ev::CollectionStatus::NotCollected:
            stateText = driverText("driver.collect.not_collected", QStringLiteral("未采集"));
            break;
        case ev::CollectionStatus::Unsupported:
            stateText = driverText(
                "driver.io.unsupported",
                QStringLiteral("当前驱动不支持该只读查询入口"));
            break;
        case ev::CollectionStatus::AccessDenied:
            stateText = driverText("driver.collect.access_denied", QStringLiteral("权限不足，访问被拒绝"));
            break;
        case ev::CollectionStatus::Timeout:
            stateText = driverText("driver.collect.timeout", QStringLiteral("驱动调用超时"));
            break;
        case ev::CollectionStatus::Error:
            stateText = driverText(
                "driver.io.device_control_failed",
                QStringLiteral("驱动接口调用失败或当前驱动版本不匹配"));
            break;
        }

        QString text = stateText;
        if (outcome.nativeCode.present)
        {
            // NTSTATUS 用十六进制、Win32 用十进制，各自领域的惯用读法。
            // 两者都直接取 64 位原值，不经过浮点。
            const QString codeText = (outcome.nativeCodeDomain == "NTSTATUS")
                ? QStringLiteral("0x%1")
                      .arg(static_cast<qulonglong>(outcome.nativeCode.value), 8, 16, QChar('0'))
                      .toUpper()
                : QString::fromStdString(
                      ev::FormatU64(outcome.nativeCode.value, ev::U64Format::Decimal));
            text += QStringLiteral(" (%1 %2)")
                        .arg(QString::fromStdString(outcome.nativeCodeDomain), codeText);
        }

        const QString detail = friendlyDriverIoMessage(outcome.message);
        if (!detail.isEmpty() && detail != stateText)
        {
            text += QStringLiteral("：") + detail;
        }
        return text;
    }

    // isDriverSignatureLoadError：
    // - 输入：StartServiceW 返回后的 Win32 错误码；
    // - 处理：识别“镜像哈希/签名/策略阻止”类加载失败；
    // - 返回：true 表示应提示用户检查测试签名，而不只提示普通 SCM 错误。
    bool isDriverSignatureLoadError(const DWORD errorCode)
    {
        return errorCode == ERROR_INVALID_IMAGE_HASH ||
            errorCode == ERROR_DRIVER_BLOCKED ||
            errorCode == ERROR_ACCESS_DISABLED_BY_POLICY;
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

    // WindowsReleaseInfo：
    // - 作用：保存 Windows 面向用户的版本标签和内核构建号；
    // - displayVersion 用于区分共享同一服务分支的 25H2/26H2，不能只看 build。
    struct WindowsReleaseInfo
    {
        QString displayVersion; // displayVersion：注册表中的 DisplayVersion，例如 26H2。
        QString buildNumber;    // buildNumber：注册表中的 CurrentBuildNumber，仅用于诊断展示。
        bool is26H2OrNewer = false; // is26H2OrNewer：版本标签是否达到 26H2。
    };

    // readWindowsVersionRegistryText：
    // - 输入：CurrentVersion 注册表中的字符串值名；
    // - 处理：固定读取 64 位系统视图，避免 32 位重定向造成版本误判；
    // - 返回：读取成功后的去空白文本，失败返回空字符串。
    QString readWindowsVersionRegistryText(const wchar_t* valueName)
    {
        HKEY versionKey = nullptr;
        const LSTATUS openStatus = ::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY,
            &versionKey);
        if (openStatus != ERROR_SUCCESS || versionKey == nullptr)
        {
            return QString();
        }

        std::array<wchar_t, 64> valueBuffer{};
        DWORD valueType = 0;
        DWORD valueBytes = static_cast<DWORD>(valueBuffer.size() * sizeof(wchar_t));
        const LSTATUS queryStatus = ::RegQueryValueExW(
            versionKey,
            valueName,
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(valueBuffer.data()),
            &valueBytes);
        ::RegCloseKey(versionKey);
        if (queryStatus != ERROR_SUCCESS ||
            (valueType != REG_SZ && valueType != REG_EXPAND_SZ))
        {
            return QString();
        }

        valueBuffer.back() = L'\0';
        return QString::fromWCharArray(valueBuffer.data()).trimmed();
    }

    // queryWindowsReleaseInfo：
    // - 作用：按 DisplayVersion 解析 Windows 年度/半年度版本；
    // - 26H2 与 25H2 共享服务分支，故 build 只展示、不参与版本阈值判断；
    // - 返回：供驱动加载错误分流和日志展示使用的稳定快照。
    WindowsReleaseInfo queryWindowsReleaseInfo()
    {
        WindowsReleaseInfo releaseInfo;
        releaseInfo.displayVersion =
            readWindowsVersionRegistryText(L"DisplayVersion").toUpper();
        releaseInfo.buildNumber =
            readWindowsVersionRegistryText(L"CurrentBuildNumber");

        const int halfMarkerIndex =
            releaseInfo.displayVersion.indexOf(QLatin1Char('H'));
        bool yearValid = false;
        bool halfValid = false;
        const int releaseYear = halfMarkerIndex > 0
            ? releaseInfo.displayVersion.left(halfMarkerIndex).toInt(&yearValid)
            : 0;
        const int releaseHalf = halfMarkerIndex > 0
            ? releaseInfo.displayVersion.mid(halfMarkerIndex + 1).toInt(&halfValid)
            : 0;
        releaseInfo.is26H2OrNewer =
            yearValid &&
            halfValid &&
            (releaseYear > 26 || (releaseYear == 26 && releaseHalf >= 2));
        return releaseInfo;
    }

    // buildDriverSignatureLoadAdvice：
    // - 输入：错误码、服务名、驱动路径；
    // - 处理：26H2 及以上先进入新内核信任策略专用分支，其余版本保留通用说明；
    // - 返回：可直接追加到 DriverDock 操作日志中的多行文本。
    QString buildDriverSignatureLoadAdvice(
        const DWORD errorCode,
        const QString& serviceNameText,
        const QString& binaryPathText)
    {
        const QString resolvedBinaryPath = binaryPathText.trimmed().isEmpty()
            ? driverText(
                "driver.load_advice.path_missing",
                QStringLiteral("<未从表单读取到路径>"))
            : binaryPathText;
        const WindowsReleaseInfo releaseInfo = queryWindowsReleaseInfo();
        if (releaseInfo.is26H2OrNewer)
        {
            return driverText(
                "driver.load_advice.windows_26h2_policy",
                QStringLiteral(
                    "挂载失败：Windows 拒绝加载驱动（%1）。\n"
                    "系统：Windows %2（Build %3，已按 26H2 或更高版本处理）\n"
                    "服务：%4\n"
                    "驱动路径：%5\n"
                    "此版本使用更严格的内核驱动信任策略。正式环境请换用经 Microsoft Hardware Dev Center "
                    "签名（证明签名/WHQL）的 KswordARK.sys；开发环境也必须给二进制写入有效测试签名，"
                    "启用 TESTSIGNING 后重启。若仍失败，请检查“代码完整性/操作”事件日志以及 "
                    "Secure Boot、内存完整性或 WDAC 策略，不要把策略阻止误判为普通 SCM 故障。"))
                .arg(formatWin32ErrorTextForAdvice(errorCode))
                .arg(releaseInfo.displayVersion)
                .arg(releaseInfo.buildNumber.isEmpty()
                    ? driverText(
                        "driver.load_advice.build_unknown",
                        QStringLiteral("<未知>"))
                    : releaseInfo.buildNumber)
                .arg(serviceNameText)
                .arg(resolvedBinaryPath);
        }

        QString adviceText = driverText(
            "driver.load_advice.signature_error",
            QStringLiteral(
                "挂载失败：Windows 拒绝加载驱动（%1）。\n"
                "服务：%2\n"
                "驱动路径：%3\n"
                "请使用可信签名的 KswordARK.sys；开发或测试环境可使用测试签名并启用测试模式，随后重启系统。"))
            .arg(formatWin32ErrorTextForAdvice(errorCode))
            .arg(serviceNameText)
            .arg(resolvedBinaryPath);
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
        case KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_CALLED:
            return driverText("driver.unload.status.routine_called", QStringLiteral("已直接调用 DriverUnload"));
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
        const bool resolvedModule = (flags & KSWORD_ARK_DRIVER_DISPATCH_FLAG_MODULE_RESOLVED) != 0U;
        const bool insideOwnImage = (flags & KSWORD_ARK_DRIVER_DISPATCH_FLAG_OWN_IMAGE) != 0U;
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

    QString driverStartIoStateText(const ksword::ark::DriverStartIoEntry& entry, const bool queryOk)
    {
        switch (entry.state)
        {
        case KSWORD_ARK_DRIVER_START_IO_STATE_NULL:
            return driverText("driver.object.start_io.null", QStringLiteral("空值（未使用 StartIo）"));
        case KSWORD_ARK_DRIVER_START_IO_STATE_READ_FAILED:
            return driverText("driver.object.start_io.read_failed", QStringLiteral("读取失败"));
        case KSWORD_ARK_DRIVER_START_IO_STATE_PRESENT:
            return driverDispatchLocationText(entry.flags);
        default:
            // NOT_QUERIED 有两个来源，必须分开说：查询本身失败时怪到"驱动协议较旧"
            // 会把人送去查驱动版本，而真因是设备没打开/驱动没加载/拒绝访问。
            return queryOk
                ? driverText("driver.object.start_io.not_queried", QStringLiteral("未查询（驱动协议较旧）"))
                : driverText("driver.object.start_io.query_failed", QStringLiteral("未查询（DriverObject 查询未成功）"));
        }
    }

}


#include "../UI/AsyncOperation.h"

using namespace ksword::driver_dock_internal;

DriverDock::DriverDock(QWidget* parent)
    : QWidget(parent)
    , m_debugCaptureDispatcher(std::make_shared<ks::ui::AsyncUiDispatcher>(this))
{
    // initEvent 用途：贯穿整个构造流程，便于按同一 GUID 追踪初始化链路。
    kLogEvent initEvent;
    info << initEvent
        << driverText("driver.log.initializing", QStringLiteral("[DriverDock] 开始初始化驱动页。"))
        << eol;

    m_serviceStatusOperation = new ks::ui::AsyncOperation(this);
    m_serviceRegistrationOperation = new ks::ui::AsyncOperation(this);
    initializeUi();
    initializeConnections();
    const auto recoverDelivery = [this]() { recoverServiceFormDeliveryState(); };
    m_serviceStatusOperation->setStateChangedCallback(recoverDelivery);
    m_serviceRegistrationOperation->setStateChangedCallback(recoverDelivery);
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
                QStringLiteral("状态：首次打开，正在加载驱动服务与内核模块...")));
    }

    QTimer::singleShot(0, this, [this]()
        {
            refreshDriverServiceRecords();
            refreshLoadedKernelModuleRecords();
            refreshUnloadedDriversAsync();
        });
}

DriverDock::~DriverDock()
{
    // Close delivery gates before any derived-page member or child disappears.
    m_debugCaptureDispatcher->close();
    if (m_moduleEvidenceOperation != nullptr) { m_moduleEvidenceOperation->close(); }
    if (m_serviceStatusOperation) { m_serviceStatusOperation->close(); }
    if (m_serviceRegistrationOperation) { m_serviceRegistrationOperation->close(); }
    if (m_piDdbPreflightOperation) m_piDdbPreflightOperation->close();
    if (m_piDdbCommitOperation) m_piDdbCommitOperation->close();
    ++*m_serviceFilterTicket;
    ++*m_moduleFilterTicket;
    ++*m_unloadedFilterTicket;
    releaseDebugOutputCapture();

    // 自身驱动页的业务对象仍由 KernelDock 持有；独立销毁 DriverDock 时将页面接回，
    // 避免 KernelDock 的动态偏移/状态成员指向已经被 QTabWidget 删除的控件。
    if (m_kswordSelfDriverPage != nullptr && m_tabWidget != nullptr)
    {
        const int pageIndex = m_tabWidget->indexOf(m_kswordSelfDriverPage);
        if (pageIndex >= 0)
        {
            m_tabWidget->removeTab(pageIndex);
        }
        if (!m_kswordSelfDriverFallbackOwner.isNull())
        {
            m_kswordSelfDriverPage->setParent(m_kswordSelfDriverFallbackOwner);
            m_kswordSelfDriverPage->hide();
        }
    }

    kLogEvent destroyEvent;
    info << destroyEvent
        << driverText("driver.log.destroyed", QStringLiteral("[DriverDock] 驱动页已析构。"))
        << eol;
}

void DriverDock::attachKswordSelfDriverPage(QWidget* page, QWidget* fallbackOwner)
{
    if (page == nullptr || m_tabWidget == nullptr)
    {
        return;
    }
    if (m_kswordSelfDriverPage == page)
    {
        return;
    }

    // 若未来调用方替换页面，先把旧页面从当前 Tab 中安全移除并交还原所有者。
    if (m_kswordSelfDriverPage != nullptr)
    {
        const int oldPageIndex = m_tabWidget->indexOf(m_kswordSelfDriverPage);
        if (oldPageIndex >= 0)
        {
            m_tabWidget->removeTab(oldPageIndex);
        }
        if (!m_kswordSelfDriverFallbackOwner.isNull())
        {
            m_kswordSelfDriverPage->setParent(m_kswordSelfDriverFallbackOwner);
            m_kswordSelfDriverPage->hide();
        }
    }

    m_kswordSelfDriverPage = page;
    m_kswordSelfDriverFallbackOwner = fallbackOwner;
    m_kswordSelfDriverTabIndex = addNavigationPage(m_tabWidget,page,"self_driver",QIcon(QStringLiteral(":/Icon/process_priority.svg")));
    m_tabWidget->setTabToolTip(
        m_kswordSelfDriverTabIndex,
        driverText(
            "driver.tab.self_driver.tooltip",
            QStringLiteral("KswordARK 动态偏移与驱动状态")));
}

namespace
{
    struct DriverFilterMatchResult
    {
        std::vector<std::uint8_t> matches;
        QString error;
        bool canceled = false;
    };

    // 只读不可变文本快照；-2 保留服务/模块原来的逐字段包含，-1 是卸载表“所有字段”。
    DriverFilterMatchResult matchDriverFilterRows(const std::vector<QStringList>& rows,
        const QString& pattern, const int field, const bool regex,
        const std::atomic_uint64_t& currentTicket, const std::uint64_t ticket)
    {
        DriverFilterMatchResult result;
        result.matches.resize(rows.size(), 0);
        QRegularExpression expression;
        if (regex && !pattern.isEmpty())
        {
            expression = QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);
            if (!expression.isValid())
            {
                result.error = expression.errorString();
                return result;
            }
        }
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            if (currentTicket.load(std::memory_order_relaxed) != ticket)
            {
                result.canceled = true;
                return result;
            }
            const QStringList& fields = rows[index];
            if (pattern.isEmpty())
            {
                result.matches[index] = 1;
            }
            else if (field == -2)
            {
                result.matches[index] = static_cast<std::uint8_t>(std::any_of(fields.cbegin(), fields.cend(),
                    [&pattern](const QString& value) { return value.contains(pattern, Qt::CaseInsensitive); }));
            }
            else if (!fields.isEmpty())
            {
                const QString& haystack = field >= 0 && field < fields.size() - 1
                    ? fields[field] : fields.back();
                result.matches[index] = static_cast<std::uint8_t>(regex
                    ? expression.match(haystack).hasMatch()
                    : haystack.contains(pattern, Qt::CaseInsensitive));
            }
        }
        return result;
    }
}

void DriverDock::queueDriverTableFilter(const TableFilterTarget target, const bool debounce)
{
    QTableWidget* table = nullptr;
    std::shared_ptr<const TableFilterRows> rows;
    std::shared_ptr<std::atomic_uint64_t> ticketState;
    QString pattern;
    int field = -2;
    bool regex = false;
    switch (target)
    {
    case TableFilterTarget::Services:
        table = m_serviceTable;
        rows = m_serviceFilterRows;
        ticketState = m_serviceFilterTicket;
        pattern = m_serviceFilterEdit != nullptr ? m_serviceFilterEdit->text().trimmed() : QString();
        break;
    case TableFilterTarget::Modules:
        table = m_moduleTable;
        rows = m_moduleFilterRows;
        ticketState = m_moduleFilterTicket;
        pattern = m_moduleFilterEdit != nullptr ? m_moduleFilterEdit->text().trimmed() : QString();
        break;
    case TableFilterTarget::Unloaded:
        table = m_unloadedPiddbTable;
        rows = m_unloadedFilterRows;
        ticketState = m_unloadedFilterTicket;
        pattern = m_unloadedPiddbFilterEdit != nullptr ? m_unloadedPiddbFilterEdit->text().trimmed() : QString();
        field = m_unloadedPiddbFieldCombo != nullptr ? m_unloadedPiddbFieldCombo->currentData().toInt() : -1;
        regex = m_unloadedPiddbRegexCheck != nullptr && m_unloadedPiddbRegexCheck->isChecked();
        break;
    }
    const std::uint64_t ticket = ++*ticketState;
    // 新缓存的行与过滤结果同一轮发布前先关闭可操作窗口。
    // 输入中的旧投影保持可读；只有实际换数据时进入此待发布状态。
    if (!debounce && table != nullptr)
    {
        if (target == TableFilterTarget::Services)
        {
            m_visibleServiceCount = 0;
        }
        else if (target == TableFilterTarget::Modules)
        {
            m_visibleModuleCount = 0;
        }
        else if (m_unloadedPiddbCountLabel != nullptr)
        {
            m_unloadedPiddbCountLabel->setText(driverText("driver.unloaded.count", QStringLiteral("数量：%1")).arg(0));
        }
        if (target == TableFilterTarget::Modules)
        {
            m_moduleFilterRestoreIndex = QPersistentModelIndex(table->model()->index(table->currentRow(), 0));
        }
        QSet<QPersistentModelIndex> pendingRows;
        for (int row = 0; row < table->rowCount(); ++row)
        {
            pendingRows.insert(QPersistentModelIndex(table->model()->index(row, 0)));
        }
        ks::ui::ApplyTableRowFilter(table, pendingRows);
        table->clearSelection();
        if (table->selectionModel() != nullptr)
        {
            table->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
        }
    }
    if (!rows)
    {
        rows = std::make_shared<const TableFilterRows>();
    }
    const QPointer<DriverDock> guard(this);
    // 同一输入串只启动最后一次工作；旧任务逐行核对票号，关闭页面也会使其失效。
    QTimer::singleShot(debounce ? 80 : 0, this, [guard, target, rows, ticketState, ticket, pattern, field, regex]()
    {
        if (guard.isNull() || ticketState->load(std::memory_order_relaxed) != ticket)
        {
            return;
        }
        auto* task = QRunnable::create([guard, target, rows, ticketState, ticket, pattern, field, regex]()
        {
            DriverFilterMatchResult result = matchDriverFilterRows(*rows, pattern, field, regex, *ticketState, ticket);
            if (result.canceled || ticketState->load(std::memory_order_relaxed) != ticket)
            {
                return;
            }
            auto matches = std::make_shared<const std::vector<std::uint8_t>>(std::move(result.matches));
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [guard, target, ticket, matches, error = std::move(result.error)]()
            {
                if (!guard.isNull())
                {
                    guard->applyDriverTableFilterResult(target, ticket, matches, error);
                }
            }, Qt::QueuedConnection);
        });
        task->setAutoDelete(true);
        QThreadPool::globalInstance()->start(task);
    });
}

void DriverDock::applyDriverTableFilterResult(const TableFilterTarget target, const std::uint64_t ticket,
    std::shared_ptr<const std::vector<std::uint8_t>> matches, const QString& error)
{
    QTableWidget* table = nullptr;
    std::shared_ptr<std::atomic_uint64_t> ticketState;
    int sourceRole = Qt::UserRole + 42;
    switch (target)
    {
    case TableFilterTarget::Services:
        table = m_serviceTable;
        ticketState = m_serviceFilterTicket;
        break;
    case TableFilterTarget::Modules:
        table = m_moduleTable;
        ticketState = m_moduleFilterTicket;
        sourceRole = ModuleRecordIndexRole;
        break;
    case TableFilterTarget::Unloaded:
        table = m_unloadedPiddbTable;
        ticketState = m_unloadedFilterTicket;
        sourceRole = Qt::UserRole + 57;
        break;
    }
    if (table == nullptr || ticketState->load(std::memory_order_relaxed) != ticket)
    {
        return;
    }
    const QPointer<DriverDock> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("driver-table-filter-") + QString::number(static_cast<int>(target)),
        { table }, [guard, target, ticket, matches, error]()
        {
            if (!guard.isNull())
            {
                guard->applyDriverTableFilterResult(target, ticket, matches, error);
            }
        }))
    {
        return;
    }
    QSet<QPersistentModelIndex> excludedRows;
    int acceptedCount = 0;
    int firstAcceptedRow = -1;
    for (int row = 0; row < table->rowCount(); ++row)
    {
        const QTableWidgetItem* item = table->item(row, 0);
        const auto sourceIndex = item != nullptr
            ? static_cast<std::size_t>(item->data(sourceRole).toULongLong()) : matches->size();
        const bool accepted = sourceIndex < matches->size() && (*matches)[sourceIndex] != 0;
        if (!accepted)
        {
            excludedRows.insert(QPersistentModelIndex(table->model()->index(row, 0)));
        }
        else
        {
            ++acceptedCount;
            if (firstAcceptedRow < 0)
            {
                firstAcceptedRow = row;
            }
        }
    }
    const int currentRow = table->currentRow();
    const bool clearCurrent = currentRow >= 0 &&
        excludedRows.contains(QPersistentModelIndex(table->model()->index(currentRow, 0)));
    // 共享入口保留冻结身份，筛选/复制/快照/搜索使用同一业务可见性。
    ks::ui::ApplyTableRowFilter(table, excludedRows);
    if (table->selectionModel() != nullptr)
    {
        const QModelIndexList selectedRows = table->selectionModel()->selectedRows(0);
        for (const QModelIndex& index : selectedRows)
        {
            if (excludedRows.contains(QPersistentModelIndex(index)))
            {
                table->selectionModel()->select(index, QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
            }
        }
    }
    if (clearCurrent)
    {
        table->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
    }
    if (target == TableFilterTarget::Modules)
    {
        if (m_moduleFilterRestoreIndex.isValid() && !excludedRows.contains(m_moduleFilterRestoreIndex))
        {
            table->setCurrentCell(m_moduleFilterRestoreIndex.row(), 0);
        }
        m_moduleFilterRestoreIndex = QPersistentModelIndex();
        if (m_selectFirstFilteredModule && table->currentRow() < 0 && firstAcceptedRow >= 0)
        {
            table->setCurrentCell(firstAcceptedRow, 0);
        }
        m_selectFirstFilteredModule = false;
        m_visibleModuleCount = acceptedCount;
        updateDriverOverviewFilteredCounts();
    }
    else if (target == TableFilterTarget::Services)
    {
        m_visibleServiceCount = acceptedCount;
        updateDriverOverviewFilteredCounts();
    }
    else
    {
        if (m_unloadedPiddbCountLabel != nullptr)
        {
            m_unloadedPiddbCountLabel->setText(driverText("driver.unloaded.count", QStringLiteral("数量：%1")).arg(acceptedCount));
        }
        if (m_unloadedDriverQuerying)
        {
            return;
        }
        if (!error.isEmpty() && m_unloadedPiddbStatusLabel != nullptr)
        {
            m_unloadedPiddbStatusLabel->setText(driverText("driver.unloaded.status.regex_invalid",
                QStringLiteral("状态：正则表达式无效：%1")).arg(error));
            m_unloadedPiddbStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:700;")
                .arg(KswordTheme::WarningColor().name(QColor::HexRgb)));
        }
        else
        {
            // updateQueryStatusLabel 为当前来源的原生状态所有者，实现在 Unloaded.cpp。
            refreshUnloadedFilterStatus();
        }
    }
}

void DriverDock::updateDriverOverviewFilteredCounts()
{
    if (m_overviewStatusLabel != nullptr && !m_serviceEnumerationFailed && !m_moduleEnumerationFailed)
    {
        m_overviewStatusLabel->setText(driverText("driver.overview.count.filtered",
            QStringLiteral("状态：驱动服务 %1 条（显示 %2 条），模块 %3 条（显示 %4 条）"))
            .arg(m_driverServiceCache.size()).arg(m_visibleServiceCount)
            .arg(m_loadedModuleCache.size()).arg(m_visibleModuleCount));
    }
}
