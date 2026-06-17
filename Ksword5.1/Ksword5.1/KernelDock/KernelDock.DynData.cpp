#include "KernelDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>

namespace
{
    // DynDataColumn：
    // - 作用：定义动态偏移字段表列索引；
    // - 使用 enum class 避免魔法数字扩散到渲染与读取逻辑。
    enum class DynDataColumn : int
    {
        Field = 0,
        Offset,
        Status,
        Source,
        Feature,
        Capability,
        Count
    };

    // SummaryColumn：
    // - 作用：定义摘要表两列布局；
    // - Field/Value 模式便于添加 R0 诊断项。
    enum class SummaryColumn : int
    {
        Name = 0,
        Value,
        Count
    };

    // CapabilityDisplay：
    // - 作用：把 capability bit 与 UI 文案绑定；
    // - 处理逻辑：刷新摘要和字段详情时复用同一张表。
    struct CapabilityDisplay
    {
        std::uint64_t mask = 0;      // mask：KSW_CAP_* 单 bit。
        const char* name = nullptr;  // name：英文稳定名称。
        const wchar_t* title = nullptr; // title：中文功能说明。
    };

    // LocalPdbProfile：
    // - 作用：保存一个本地 JSON profile 解析结果；
    // - 输入来源：profiles/ark_dyndata/*.json；
    // - 返回行为：作为 DriverClient::applyDynDataProfile 的输入，不持有 PDB 文件。
    struct LocalPdbProfile
    {
        bool valid = false;                              // valid：R3 语法和范围校验是否通过。
        bool matched = false;                            // matched：module identity 是否精确匹配当前 ntoskrnl。
        std::uint32_t ignoredUnknownFields = 0;          // ignoredUnknownFields：JSON 中 R3 不认识的字段数。
        QString sourceText;                              // sourceText：profile 来源，区分 pack 与散落 JSON。
        QString pathText;                                // pathText：profile 文件路径。
        QString diagnosticsText;                         // diagnosticsText：解析/校验诊断。
        ksword::ark::DynDataProfileApplyInput applyInput; // applyInput：可直接打包给 R0 的 v1 profile。
        ksword::ark::DynDataProfileApplyExInput applyExInput; // applyExInput：可直接打包给 R0 的 v2/v3 typed-item profile。
        std::uint32_t exAppliedCount = 0;               // exAppliedCount：v2/v3 typed items 数量。
        std::uint32_t callbackItemCount = 0;            // callbackItemCount：原始 callbackItems 条目数。
        std::uint32_t typedItemCount = 0;               // typedItemCount：v3 items/typedItems 条目数。
    };

    // kCapabilities：
    // - 作用：枚举 Phase 0 暴露的全部 capability；
    // - 返回行为：由 helper 函数格式化为摘要、详情或缺失列表。
    constexpr std::array<CapabilityDisplay, 21> kCapabilities{ {
        { KSW_CAP_DYN_NTOS_ACTIVE, "KSW_CAP_DYN_NTOS_ACTIVE", L"ntoskrnl profile 已激活" },
        { KSW_CAP_DYN_LXCORE_ACTIVE, "KSW_CAP_DYN_LXCORE_ACTIVE", L"lxcore profile 已激活" },
        { KSW_CAP_OBJECT_TYPE_FIELDS, "KSW_CAP_OBJECT_TYPE_FIELDS", L"对象类型字段" },
        { KSW_CAP_HANDLE_TABLE_DECODE, "KSW_CAP_HANDLE_TABLE_DECODE", L"句柄表解码" },
        { KSW_CAP_PROCESS_OBJECT_TABLE, "KSW_CAP_PROCESS_OBJECT_TABLE", L"进程 ObjectTable" },
        { KSW_CAP_THREAD_STACK_FIELDS, "KSW_CAP_THREAD_STACK_FIELDS", L"线程栈字段" },
        { KSW_CAP_THREAD_IO_COUNTERS, "KSW_CAP_THREAD_IO_COUNTERS", L"线程 I/O 计数" },
        { KSW_CAP_ALPC_FIELDS, "KSW_CAP_ALPC_FIELDS", L"ALPC 字段" },
        { KSW_CAP_SECTION_CONTROL_AREA, "KSW_CAP_SECTION_CONTROL_AREA", L"Section/ControlArea" },
        { KSW_CAP_PROCESS_PROTECTION_PATCH, "KSW_CAP_PROCESS_PROTECTION_PATCH", L"进程保护修改" },
        { KSW_CAP_WSL_LXCORE_FIELDS, "KSW_CAP_WSL_LXCORE_FIELDS", L"WSL/lxcore 字段" },
        { KSW_CAP_ETW_GUID_FIELDS, "KSW_CAP_ETW_GUID_FIELDS", L"ETW GUID/Registration 字段" },
        { KSW_CAP_CALLBACK_NOTIFY_GLOBALS, "KSW_CAP_CALLBACK_NOTIFY_GLOBALS", L"Callback Notify 全局 RVA" },
        { KSW_CAP_CALLBACK_REGISTRY_GLOBALS, "KSW_CAP_CALLBACK_REGISTRY_GLOBALS", L"Registry Callback 全局 RVA" },
        { KSW_CAP_CALLBACK_OBJECT_FIELDS, "KSW_CAP_CALLBACK_OBJECT_FIELDS", L"Object Callback 结构偏移" },
        { KSW_CAP_PROCESS_LIST_FIELDS, "KSW_CAP_PROCESS_LIST_FIELDS", L"进程链表字段" },
        { KSW_CAP_THREAD_LIST_FIELDS, "KSW_CAP_THREAD_LIST_FIELDS", L"线程链表字段" },
        { KSW_CAP_CID_TABLE_WALK, "KSW_CAP_CID_TABLE_WALK", L"CID 表遍历" },
        { KSW_CAP_KERNEL_MODULE_LIST_FIELDS, "KSW_CAP_KERNEL_MODULE_LIST_FIELDS", L"内核模块链表字段" },
        { KSW_CAP_DRIVER_OBJECT_FIELDS, "KSW_CAP_DRIVER_OBJECT_FIELDS", L"驱动对象字段" },
        { KSW_CAP_KERNEL_GLOBALS, "KSW_CAP_KERNEL_GLOBALS", L"内核全局 RVA" }
    } };

    // blueButtonStyle：
    // - 输入：无；
    // - 处理：读取全局主题按钮样式；
    // - 返回：可直接 setStyleSheet 的按钮样式文本。
    QString blueButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    // blueInputStyle：
    // - 输入：无；
    // - 处理：使用主题色拼接 QLineEdit 样式；
    // - 返回：筛选框样式文本。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // headerStyle：
    // - 输入：无；
    // - 处理：使用主题色拼接表头样式；
    // - 返回：可直接应用到 QHeaderView 的样式文本。
    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // itemSelectionStyle：
    // - 输入：无；
    // - 处理：统一表格选中态颜色；
    // - 返回：表格 selection 样式文本。
    QString itemSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // statusLabelStyle：
    // - 输入 colorHex：目标文字颜色；
    // - 处理：拼接 QLabel 样式；
    // - 返回：加粗状态文本样式。
    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    // safeText：
    // - 输入 valueText/fallbackText：待展示文本和兜底文本；
    // - 处理：去除首尾空白后判断是否为空；
    // - 返回：非空原文或兜底占位符。
    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    // stringToQString：
    // - 输入 valueText：ArkDriverClient 返回的 UTF-8/ANSI 小字符串；
    // - 处理：按 UTF-8 转换，兼容 ASCII 字段名；
    // - 返回：Qt 展示字符串。
    QString stringToQString(const std::string& valueText)
    {
        return QString::fromUtf8(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    // wideStringToQString：
    // - 输入 valueText：ArkDriverClient 返回的宽字符串；
    // - 处理：按 wchar_t 数组转换；
    // - 返回：Qt 展示字符串。
    QString wideStringToQString(const std::wstring& valueText)
    {
        return QString::fromWCharArray(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    // formatHex32：
    // - 输入 value：32 位数值；
    // - 处理：补零并使用大写十六进制；
    // - 返回：0xXXXXXXXX 格式文本。
    QString formatHex32(const std::uint32_t value)
    {
        return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper();
    }

    // formatHex64：
    // - 输入 value：64 位数值；
    // - 处理：补零并使用大写十六进制；
    // - 返回：0xXXXXXXXXXXXXXXXX 格式文本。
    QString formatHex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1").arg(value, 16, 16, QChar('0')).toUpper();
    }

    // formatNtStatus：
    // - 输入 statusValue：NTSTATUS signed long；
    // - 处理：保留底层 32 bit 原样展示；
    // - 返回：0xXXXXXXXX 格式文本。
    QString formatNtStatus(const long statusValue)
    {
        return formatHex32(static_cast<std::uint32_t>(statusValue));
    }

    // formatOffset：
    // - 输入 offsetValue：字段偏移；
    // - 处理：识别 DynData 不可用哨兵；
    // - 返回：可读偏移文本或 <不可用>。
    QString formatOffset(const std::uint32_t offsetValue)
    {
        if (offsetValue == 0xFFFFFFFFU || offsetValue == 0x0000FFFFU)
        {
            return QStringLiteral("<不可用>");
        }
        return QStringLiteral("0x%1").arg(offsetValue, 4, 16, QChar('0')).toUpper();
    }

    // fieldPresent：
    // - 输入 flags/offset：R0 字段 flags 和偏移值；
    // - 处理：同时检查 PRESENT bit 与不可用哨兵；
    // - 返回：true 表示字段可用。
    bool fieldPresent(const std::uint32_t flags, const std::uint32_t offset)
    {
        return (flags & KSW_DYN_FIELD_FLAG_PRESENT) != 0U &&
            offset != 0xFFFFFFFFU &&
            offset != 0x0000FFFFU;
    }

    // statusFlagEnabled：
    // - 输入 flags/flag：状态位图和目标 bit；
    // - 处理：按位检测；
    // - 返回：true 表示目标状态启用。
    bool statusFlagEnabled(const std::uint32_t flags, const std::uint32_t flag)
    {
        return (flags & flag) == flag;
    }

    // boolText：
    // - 输入 enabled：布尔状态；
    // - 处理：转换为中文；
    // - 返回：“是”或“否”。
    QString boolText(const bool enabled)
    {
        return enabled ? QStringLiteral("是") : QStringLiteral("否");
    }

    // moduleClassText：
    // - 输入 classId：KSW_DYN_PROFILE_CLASS_*；
    // - 处理：转换为 UI 可读文本；
    // - 返回：profile class 文案。
    QString moduleClassText(const std::uint32_t classId)
    {
        switch (classId)
        {
        case KSW_DYN_PROFILE_CLASS_NTOSKRNL:
            return QStringLiteral("ntoskrnl");
        case KSW_DYN_PROFILE_CLASS_NTKRLA57:
            return QStringLiteral("ntkrla57");
        case KSW_DYN_PROFILE_CLASS_LXCORE:
            return QStringLiteral("lxcore");
        default:
            return QStringLiteral("unknown(%1)").arg(classId);
        }
    }

    // sourceText：
    // - 输入 source：KSW_DYN_FIELD_SOURCE_*；
    // - 处理：转换为 UI 可读文本；
    // - 返回：字段来源文案。
    QString sourceText(const std::uint32_t source)
    {
        switch (source)
        {
        case KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER:
            return QStringLiteral("System Informer");
        case KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN:
            return QStringLiteral("Ksword runtime pattern");
        case KSW_DYN_FIELD_SOURCE_KSWORD_EXTRA_TABLE:
            return QStringLiteral("Ksword extra table");
        case KSW_DYN_FIELD_SOURCE_PDB_PROFILE:
            return QStringLiteral("PDB profile");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // profileSourceDisplayText：
    // - 输入 sourceTextValue：LocalPdbProfile 记录的来源标识；
    // - 处理：把内部来源标识转换为 DynData 摘要页可读文本；
    // - 返回：PDB profile pack、scattered JSON 或兜底来源文本。
    QString profileSourceDisplayText(const QString& sourceTextValue)
    {
        if (sourceTextValue == QStringLiteral("pack"))
        {
            return QStringLiteral("PDB profile pack");
        }
        if (sourceTextValue == QStringLiteral("scattered-json"))
        {
            return QStringLiteral("PDB profile scattered JSON");
        }
        return sourceTextValue.trimmed().isEmpty() ? QStringLiteral("<空>") : sourceTextValue;
    }

    // fieldIdForProfileName：
    // - 输入 fieldName：JSON profile 中的字段名；
    // - 处理：映射到 shared/driver/KswordArkDynDataIoctl.h 中的字段 ID；
    // - 返回：命中返回 true，否则 false，由调用方记录未知字段诊断。
    bool fieldIdForProfileName(const QString& fieldName, std::uint32_t& fieldIdOut)
    {
        static const std::unordered_map<std::string, std::uint32_t> kFieldIds = {
            { "EpObjectTable", KSW_DYN_FIELD_ID_EP_OBJECT_TABLE },
            { "EpSectionObject", KSW_DYN_FIELD_ID_EP_SECTION_OBJECT },
            { "EpUniqueProcessId", KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID },
            { "_EPROCESS.UniqueProcessId", KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID },
            { "EpActiveProcessLinks", KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS },
            { "_EPROCESS.ActiveProcessLinks", KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS },
            { "EpThreadListHead", KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD },
            { "_EPROCESS.ThreadListHead", KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD },
            { "EpImageFileName", KSW_DYN_FIELD_ID_EP_IMAGE_FILE_NAME },
            { "_EPROCESS.ImageFileName", KSW_DYN_FIELD_ID_EP_IMAGE_FILE_NAME },
            { "EpToken", KSW_DYN_FIELD_ID_EP_TOKEN },
            { "_EPROCESS.Token", KSW_DYN_FIELD_ID_EP_TOKEN },
            { "HtHandleContentionEvent", KSW_DYN_FIELD_ID_HT_HANDLE_CONTENTION_EVENT },
            { "HtTableCode", KSW_DYN_FIELD_ID_HT_TABLE_CODE },
            { "_HANDLE_TABLE.TableCode", KSW_DYN_FIELD_ID_HT_TABLE_CODE },
            { "HtHandleCount", KSW_DYN_FIELD_ID_HT_HANDLE_COUNT },
            { "_HANDLE_TABLE.HandleCount", KSW_DYN_FIELD_ID_HT_HANDLE_COUNT },
            { "HteLowValue", KSW_DYN_FIELD_ID_HTE_LOW_VALUE },
            { "_HANDLE_TABLE_ENTRY.LowValue", KSW_DYN_FIELD_ID_HTE_LOW_VALUE },
            { "OtName", KSW_DYN_FIELD_ID_OT_NAME },
            { "OtIndex", KSW_DYN_FIELD_ID_OT_INDEX },
            { "ObDecodeShift", KSW_DYN_FIELD_ID_OB_DECODE_SHIFT },
            { "ObAttributesShift", KSW_DYN_FIELD_ID_OB_ATTRIBUTES_SHIFT },
            { "KtInitialStack", KSW_DYN_FIELD_ID_KT_INITIAL_STACK },
            { "KtStackLimit", KSW_DYN_FIELD_ID_KT_STACK_LIMIT },
            { "KtStackBase", KSW_DYN_FIELD_ID_KT_STACK_BASE },
            { "KtKernelStack", KSW_DYN_FIELD_ID_KT_KERNEL_STACK },
            { "KtProcess", KSW_DYN_FIELD_ID_KT_PROCESS },
            { "_KTHREAD.Process", KSW_DYN_FIELD_ID_KT_PROCESS },
            { "EtCid", KSW_DYN_FIELD_ID_ET_CID },
            { "_ETHREAD.Cid", KSW_DYN_FIELD_ID_ET_CID },
            { "EtThreadListEntry", KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY },
            { "_ETHREAD.ThreadListEntry", KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY },
            { "EtStartAddress", KSW_DYN_FIELD_ID_ET_START_ADDRESS },
            { "_ETHREAD.StartAddress", KSW_DYN_FIELD_ID_ET_START_ADDRESS },
            { "EtWin32StartAddress", KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS },
            { "_ETHREAD.Win32StartAddress", KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS },
            { "KtReadOperationCount", KSW_DYN_FIELD_ID_KT_READ_OPERATION_COUNT },
            { "KtWriteOperationCount", KSW_DYN_FIELD_ID_KT_WRITE_OPERATION_COUNT },
            { "KtOtherOperationCount", KSW_DYN_FIELD_ID_KT_OTHER_OPERATION_COUNT },
            { "KtReadTransferCount", KSW_DYN_FIELD_ID_KT_READ_TRANSFER_COUNT },
            { "KtWriteTransferCount", KSW_DYN_FIELD_ID_KT_WRITE_TRANSFER_COUNT },
            { "KtOtherTransferCount", KSW_DYN_FIELD_ID_KT_OTHER_TRANSFER_COUNT },
            { "MmSectionControlArea", KSW_DYN_FIELD_ID_MM_SECTION_CONTROL_AREA },
            { "MmControlAreaListHead", KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LIST_HEAD },
            { "MmControlAreaLock", KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LOCK },
            { "AlpcCommunicationInfo", KSW_DYN_FIELD_ID_ALPC_COMMUNICATION_INFO },
            { "AlpcOwnerProcess", KSW_DYN_FIELD_ID_ALPC_OWNER_PROCESS },
            { "AlpcConnectionPort", KSW_DYN_FIELD_ID_ALPC_CONNECTION_PORT },
            { "AlpcServerCommunicationPort", KSW_DYN_FIELD_ID_ALPC_SERVER_COMMUNICATION_PORT },
            { "AlpcClientCommunicationPort", KSW_DYN_FIELD_ID_ALPC_CLIENT_COMMUNICATION_PORT },
            { "AlpcHandleTable", KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE },
            { "AlpcHandleTableLock", KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE_LOCK },
            { "AlpcAttributes", KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES },
            { "AlpcAttributesFlags", KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES_FLAGS },
            { "AlpcPortContext", KSW_DYN_FIELD_ID_ALPC_PORT_CONTEXT },
            { "AlpcPortObjectLock", KSW_DYN_FIELD_ID_ALPC_PORT_OBJECT_LOCK },
            { "AlpcSequenceNo", KSW_DYN_FIELD_ID_ALPC_SEQUENCE_NO },
            { "AlpcState", KSW_DYN_FIELD_ID_ALPC_STATE },
            { "LxPicoProc", KSW_DYN_FIELD_ID_LX_PICO_PROC },
            { "LxPicoProcInfo", KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO },
            { "LxPicoProcInfoPID", KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO_PID },
            { "LxPicoThrdInfo", KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO },
            { "LxPicoThrdInfoTID", KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO_TID },
            { "EpProtection", KSW_DYN_FIELD_ID_EP_PROTECTION },
            { "EpSignatureLevel", KSW_DYN_FIELD_ID_EP_SIGNATURE_LEVEL },
            { "EpSectionSignatureLevel", KSW_DYN_FIELD_ID_EP_SECTION_SIGNATURE_LEVEL },
            { "EgeGuid", KSW_DYN_FIELD_ID_EGE_GUID },
            { "EreGuidEntry", KSW_DYN_FIELD_ID_ERE_GUID_ENTRY },
            { "KldrInLoadOrderLinks", KSW_DYN_FIELD_ID_KLDR_IN_LOAD_ORDER_LINKS },
            { "_KLDR_DATA_TABLE_ENTRY.InLoadOrderLinks", KSW_DYN_FIELD_ID_KLDR_IN_LOAD_ORDER_LINKS },
            { "KldrDllBase", KSW_DYN_FIELD_ID_KLDR_DLL_BASE },
            { "_KLDR_DATA_TABLE_ENTRY.DllBase", KSW_DYN_FIELD_ID_KLDR_DLL_BASE },
            { "KldrSizeOfImage", KSW_DYN_FIELD_ID_KLDR_SIZE_OF_IMAGE },
            { "_KLDR_DATA_TABLE_ENTRY.SizeOfImage", KSW_DYN_FIELD_ID_KLDR_SIZE_OF_IMAGE },
            { "KldrFullDllName", KSW_DYN_FIELD_ID_KLDR_FULL_DLL_NAME },
            { "_KLDR_DATA_TABLE_ENTRY.FullDllName", KSW_DYN_FIELD_ID_KLDR_FULL_DLL_NAME },
            { "KldrBaseDllName", KSW_DYN_FIELD_ID_KLDR_BASE_DLL_NAME },
            { "_KLDR_DATA_TABLE_ENTRY.BaseDllName", KSW_DYN_FIELD_ID_KLDR_BASE_DLL_NAME },
            { "KldrFlags", KSW_DYN_FIELD_ID_KLDR_FLAGS },
            { "_KLDR_DATA_TABLE_ENTRY.Flags", KSW_DYN_FIELD_ID_KLDR_FLAGS },
            { "DoDriverStart", KSW_DYN_FIELD_ID_DO_DRIVER_START },
            { "_DRIVER_OBJECT.DriverStart", KSW_DYN_FIELD_ID_DO_DRIVER_START },
            { "DoDriverSize", KSW_DYN_FIELD_ID_DO_DRIVER_SIZE },
            { "_DRIVER_OBJECT.DriverSize", KSW_DYN_FIELD_ID_DO_DRIVER_SIZE },
            { "DoDriverSection", KSW_DYN_FIELD_ID_DO_DRIVER_SECTION },
            { "_DRIVER_OBJECT.DriverSection", KSW_DYN_FIELD_ID_DO_DRIVER_SECTION },
            { "DoMajorFunction", KSW_DYN_FIELD_ID_DO_MAJOR_FUNCTION },
            { "_DRIVER_OBJECT.MajorFunction", KSW_DYN_FIELD_ID_DO_MAJOR_FUNCTION },
            { "DoFastIoDispatch", KSW_DYN_FIELD_ID_DO_FAST_IO_DISPATCH },
            { "_DRIVER_OBJECT.FastIoDispatch", KSW_DYN_FIELD_ID_DO_FAST_IO_DISPATCH },
            { "DoDriverUnload", KSW_DYN_FIELD_ID_DO_DRIVER_UNLOAD },
            { "_DRIVER_OBJECT.DriverUnload", KSW_DYN_FIELD_ID_DO_DRIVER_UNLOAD },
            { "PspCidTable", KSW_DYN_FIELD_ID_KG_PSP_CID_TABLE },
            { "PsLoadedModuleList", KSW_DYN_FIELD_ID_KG_PS_LOADED_MODULE_LIST },
            { "MmUnloadedDrivers", KSW_DYN_FIELD_ID_KG_MM_UNLOADED_DRIVERS },
            { "PiDDBCacheTable", KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE },
            { "PspCreateProcessNotifyRoutine", KSW_DYN_FIELD_ID_CB_PSP_CREATE_PROCESS_NOTIFY_ROUTINE },
            { "PspCreateThreadNotifyRoutine", KSW_DYN_FIELD_ID_CB_PSP_CREATE_THREAD_NOTIFY_ROUTINE },
            { "PspLoadImageNotifyRoutine", KSW_DYN_FIELD_ID_CB_PSP_LOAD_IMAGE_NOTIFY_ROUTINE },
            { "PspNotifyEnableMask", KSW_DYN_FIELD_ID_CB_PSP_NOTIFY_ENABLE_MASK },
            { "CmCallbackListHead", KSW_DYN_FIELD_ID_CB_CM_CALLBACK_LIST_HEAD },
            { "_OBJECT_TYPE.CallbackList", KSW_DYN_FIELD_ID_CB_OBJECT_TYPE_CALLBACK_LIST },
            { "_CALLBACK_ENTRY_ITEM.EntryList", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_ENTRY_LIST },
            { "_CALLBACK_ENTRY_ITEM.EntryItemList", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_ENTRY_LIST },
            { "_CALLBACK_ENTRY_ITEM.PreOperation", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_PRE_OPERATION },
            { "_CALLBACK_ENTRY_ITEM.PostOperation", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_POST_OPERATION },
            { "_CALLBACK_ENTRY_ITEM.Operations", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_OPERATIONS },
            { "_CALLBACK_ENTRY_ITEM.CallbackEntry", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_CALLBACK_ENTRY },
            { "_CALLBACK_ENTRY.Altitude", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ALTITUDE },
            { "_CALLBACK_ENTRY.RegistrationContext", KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_REGISTRATION_CONTEXT }
        };

        const auto iterator = kFieldIds.find(fieldName.toStdString());
        if (iterator == kFieldIds.end())
        {
            fieldIdOut = 0U;
            return false;
        }

        fieldIdOut = iterator->second;
        return true;
    }


    // fieldIdIsCallbackGlobal：
    // - 输入 fieldId：共享协议字段 ID；
    // - 处理：识别 callbackItems/v2 使用的 callback 全局 RVA；
    // - 返回：true 表示该字段应作为 callback GlobalRva 发送给 EX IOCTL。
    bool fieldIdIsCallbackGlobal(const std::uint32_t fieldId)
    {
        switch (fieldId)
        {
        case KSW_DYN_FIELD_ID_CB_PSP_CREATE_PROCESS_NOTIFY_ROUTINE:
        case KSW_DYN_FIELD_ID_CB_PSP_CREATE_THREAD_NOTIFY_ROUTINE:
        case KSW_DYN_FIELD_ID_CB_PSP_LOAD_IMAGE_NOTIFY_ROUTINE:
        case KSW_DYN_FIELD_ID_CB_PSP_NOTIFY_ENABLE_MASK:
        case KSW_DYN_FIELD_ID_CB_CM_CALLBACK_LIST_HEAD:
            return true;
        default:
            return false;
        }
    }

    // fieldIdIsKernelGlobal：
    // - 输入 fieldId：共享协议字段 ID；
    // - 处理：识别 v3 typed items 使用的内核全局 RVA；
    // - 返回：true 表示该字段应作为非 callback GlobalRva 发送给 EX IOCTL。
    bool fieldIdIsKernelGlobal(const std::uint32_t fieldId)
    {
        switch (fieldId)
        {
        case KSW_DYN_FIELD_ID_KG_PSP_CID_TABLE:
        case KSW_DYN_FIELD_ID_KG_PS_LOADED_MODULE_LIST:
        case KSW_DYN_FIELD_ID_KG_MM_UNLOADED_DRIVERS:
        case KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE:
            return true;
        default:
            return false;
        }
    }

    // fieldIdIsGlobalRva：
    // - 输入 fieldId：共享协议字段 ID；
    // - 处理：汇总 callback 和 kernel global 两类 GlobalRva 项；
    // - 返回：true 表示字段值应按 RVA 校验和提交。
    bool fieldIdIsGlobalRva(const std::uint32_t fieldId)
    {
        return fieldIdIsCallbackGlobal(fieldId) || fieldIdIsKernelGlobal(fieldId);
    }

    // fieldIdIsCallbackOffset：
    // - 输入 fieldId：共享协议字段 ID；
    // - 处理：识别 callbackItems/v2 使用的 callback 结构偏移；
    // - 返回：true 表示该字段应作为 callback StructOffset 发送给 EX IOCTL。
    bool fieldIdIsCallbackOffset(const std::uint32_t fieldId)
    {
        switch (fieldId)
        {
        case KSW_DYN_FIELD_ID_CB_OBJECT_TYPE_CALLBACK_LIST:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_ENTRY_LIST:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_PRE_OPERATION:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_POST_OPERATION:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_OPERATIONS:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_CALLBACK_ENTRY:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ALTITUDE:
        case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_REGISTRATION_CONTEXT:
            return true;
        default:
            return false;
        }
    }

    // fieldIdIsLxcoreOffset：
    // - 输入 fieldId：共享协议字段 ID；
    // - 处理：识别 System Informer lxcore profile 字段，当前 ntoskrnl PDB EX apply 不承载这些字段；
    // - 返回：true 表示该字段不能放入 ntoskrnl v3 StructOffset typed item。
    bool fieldIdIsLxcoreOffset(const std::uint32_t fieldId)
    {
        switch (fieldId)
        {
        case KSW_DYN_FIELD_ID_LX_PICO_PROC:
        case KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO:
        case KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO_PID:
        case KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO:
        case KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO_TID:
            return true;
        default:
            return false;
        }
    }

    // fieldIdIsStructOffset：
    // - 输入 fieldId：共享协议字段 ID；
    // - 处理：排除 GlobalRva 和 lxcore-only 字段后，允许已知字段通过 StructOffset 路径应用；
    // - 返回：true 表示字段值应按结构偏移校验和提交。
    bool fieldIdIsStructOffset(const std::uint32_t fieldId)
    {
        return fieldId != 0U &&
            fieldId <= KSW_DYN_FIELD_ID_MAX &&
            !fieldIdIsGlobalRva(fieldId) &&
            !fieldIdIsLxcoreOffset(fieldId);
    }

    QString callbackItemKindText(const QString& kindText)
    {
        const QString normalized = kindText.trimmed();
        if (normalized.compare(QStringLiteral("GlobalRva"), Qt::CaseInsensitive) == 0)
        {
            return QStringLiteral("GlobalRva");
        }
        if (normalized.compare(QStringLiteral("StructOffset"), Qt::CaseInsensitive) == 0)
        {
            return QStringLiteral("StructOffset");
        }
        return normalized;
    }

    // profileContainsExItem：
    // - 输入 profile/fieldId：当前本地 profile 和候选字段 ID；
    // - 处理：扫描已展开的 EX item，避免 callbackItems 与 v3 items 重复提交；
    // - 返回：true 表示字段 ID 已存在。
    bool profileContainsExItem(const LocalPdbProfile& profile, const std::uint32_t fieldId)
    {
        return std::any_of(
            profile.applyExInput.items.begin(),
            profile.applyExInput.items.end(),
            [fieldId](const ksword::ark::DynDataProfileExItem& item) {
                return item.itemId == fieldId;
            });
    }

    // appendProfileExItem：
    // - 输入 profile/fieldId/itemKind/value/required：待追加的 typed item 数据；
    // - 处理：设置 EX IOCTL 所需 itemKind、value 和 callback/required 标志；
    // - 返回：无返回值，profile.applyExInput.items 会按需追加一项。
    void appendProfileExItem(
        LocalPdbProfile& profile,
        const std::uint32_t fieldId,
        const std::uint32_t itemKind,
        const std::uint32_t value,
        const bool required)
    {
        ksword::ark::DynDataProfileExItem exItem{};
        exItem.itemId = fieldId;
        exItem.itemKind = itemKind;
        exItem.value = value;
        exItem.flags = required ? KSW_DYN_PROFILE_EX_ITEM_FLAG_REQUIRED : 0U;
        if (fieldIdIsCallbackGlobal(fieldId) || fieldIdIsCallbackOffset(fieldId))
        {
            exItem.flags |= KSW_DYN_PROFILE_EX_ITEM_FLAG_CALLBACK;
        }
        profile.applyExInput.items.push_back(exItem);
        profile.exAppliedCount = static_cast<std::uint32_t>(profile.applyExInput.items.size());
    }

    // parseProfileUInt32 前置声明：
    // - 输入 value：JSON 数值或字符串；
    // - 处理/返回：实际解析逻辑位于下方定义，此处只允许 callbackItems 解析 helper 提前调用。
    bool parseProfileUInt32(const QJsonValue& value, std::uint32_t& valueOut);

    bool loadCallbackItemsFromJson(
        const QJsonArray& callbackItemsArray,
        LocalPdbProfile& profile,
        QStringList& diagnostics,
        bool& hasCallbackItemsOut)
    {
        hasCallbackItemsOut = false;
        if (callbackItemsArray.isEmpty())
        {
            return true;
        }

        hasCallbackItemsOut = true;
        std::array<bool, KSW_DYN_FIELD_ID_MAX + 1U> seenFieldIds{};
        for (const QJsonValue& itemValue : callbackItemsArray)
        {
            if (!itemValue.isObject())
            {
                diagnostics << QStringLiteral("callbackItems 包含非对象项，已忽略。");
                continue;
            }

            const QJsonObject itemObject = itemValue.toObject();
            const QString itemName = itemObject.value(QStringLiteral("name")).toString().trimmed();
            const QString itemKind = callbackItemKindText(itemObject.value(QStringLiteral("kind")).toString());
            std::uint32_t fieldId = 0U;
            if (!fieldIdForProfileName(itemName, fieldId))
            {
                profile.ignoredUnknownFields += 1U;
                continue;
            }

            std::uint32_t value = 0U;
            if (!parseProfileUInt32(itemObject.value(QStringLiteral("value")), value))
            {
                diagnostics << QStringLiteral("callbackItems 解析失败: %1").arg(itemName);
                return false;
            }

            if (fieldId >= seenFieldIds.size() || seenFieldIds[fieldId])
            {
                diagnostics << QStringLiteral("callbackItems 含重复字段: %1").arg(itemName);
                return false;
            }
            seenFieldIds[fieldId] = true;

            if (itemKind == QStringLiteral("GlobalRva"))
            {
                if (!fieldIdIsCallbackGlobal(fieldId))
                {
                    diagnostics << QStringLiteral("callbackItems 语义不匹配: %1 不是 global").arg(itemName);
                    return false;
                }
                if (value == 0U || value > KSW_DYN_PROFILE_GLOBAL_RVA_MAX)
                {
                    diagnostics << QStringLiteral("callbackItems global RVA 越界: %1").arg(itemName);
                    return false;
                }
                ksword::ark::DynDataProfileExItem exItem{};
                exItem.itemId = fieldId;
                exItem.itemKind = KSW_DYN_PROFILE_EX_ITEM_KIND_GLOBAL_RVA;
                exItem.value = value;
                exItem.flags = KSW_DYN_PROFILE_EX_ITEM_FLAG_REQUIRED | KSW_DYN_PROFILE_EX_ITEM_FLAG_CALLBACK;
                profile.applyExInput.items.push_back(exItem);
            }
            else if (itemKind == QStringLiteral("StructOffset"))
            {
                if (!fieldIdIsCallbackOffset(fieldId))
                {
                    diagnostics << QStringLiteral("callbackItems 语义不匹配: %1 不是 struct offset").arg(itemName);
                    return false;
                }
                ksword::ark::DynDataProfileExItem exItem{};
                exItem.itemId = fieldId;
                exItem.itemKind = KSW_DYN_PROFILE_EX_ITEM_KIND_STRUCT_OFFSET;
                exItem.value = value;
                exItem.flags = KSW_DYN_PROFILE_EX_ITEM_FLAG_REQUIRED | KSW_DYN_PROFILE_EX_ITEM_FLAG_CALLBACK;
                profile.applyExInput.items.push_back(exItem);
            }
            else
            {
                diagnostics << QStringLiteral("callbackItems kind 不支持: %1").arg(itemName);
                return false;
            }

            profile.callbackItemCount += 1U;
        }

        profile.exAppliedCount = static_cast<std::uint32_t>(profile.applyExInput.items.size());
        return true;
    }

    // loadTypedItemsFromJson：
    // - 输入 typedItemsArray/profile/diagnostics/hasTypedItemsOut：v3 items 数组、输出 profile 和诊断容器；
    // - 处理：解析 name/kind/value，把 StructOffset 与 GlobalRva 统一展开为 EX IOCTL item；
    // - 返回：true 表示 typed items 可用或为空；false 表示命中项语义/范围错误。
    bool loadTypedItemsFromJson(
        const QJsonArray& typedItemsArray,
        LocalPdbProfile& profile,
        QStringList& diagnostics,
        bool& hasTypedItemsOut)
    {
        hasTypedItemsOut = false;
        if (typedItemsArray.isEmpty())
        {
            return true;
        }

        hasTypedItemsOut = true;
        std::array<bool, KSW_DYN_FIELD_ID_MAX + 1U> seenFieldIds{};
        for (const QJsonValue& itemValue : typedItemsArray)
        {
            if (!itemValue.isObject())
            {
                diagnostics << QStringLiteral("typed items 包含非对象项，已忽略。");
                continue;
            }

            const QJsonObject itemObject = itemValue.toObject();
            const QString itemName = itemObject.value(QStringLiteral("name")).toString().trimmed();
            const QString itemKind = callbackItemKindText(itemObject.value(QStringLiteral("kind")).toString());
            const bool required = itemObject.value(QStringLiteral("required")).toBool(false);

            std::uint32_t fieldId = 0U;
            if (!fieldIdForProfileName(itemName, fieldId))
            {
                profile.ignoredUnknownFields += 1U;
                continue;
            }

            std::uint32_t value = 0U;
            if (!parseProfileUInt32(itemObject.value(QStringLiteral("value")), value))
            {
                diagnostics << QStringLiteral("typed items 解析失败: %1").arg(itemName);
                return false;
            }

            if (fieldId >= seenFieldIds.size() || seenFieldIds[fieldId] || profileContainsExItem(profile, fieldId))
            {
                diagnostics << QStringLiteral("typed items 含重复字段: %1").arg(itemName);
                return false;
            }
            seenFieldIds[fieldId] = true;

            if (itemKind == QStringLiteral("GlobalRva"))
            {
                if (!fieldIdIsGlobalRva(fieldId))
                {
                    diagnostics << QStringLiteral("typed items 语义不匹配: %1 不是 global RVA").arg(itemName);
                    return false;
                }
                if (value == 0U || value > KSW_DYN_PROFILE_GLOBAL_RVA_MAX)
                {
                    diagnostics << QStringLiteral("typed items global RVA 越界: %1").arg(itemName);
                    return false;
                }
                appendProfileExItem(profile, fieldId, KSW_DYN_PROFILE_EX_ITEM_KIND_GLOBAL_RVA, value, required);
            }
            else if (itemKind == QStringLiteral("StructOffset"))
            {
                if (!fieldIdIsStructOffset(fieldId))
                {
                    diagnostics << QStringLiteral("typed items 语义不匹配: %1 不是 struct offset").arg(itemName);
                    return false;
                }
                if (value == 0xFFFFFFFFU || value > KSW_DYN_PROFILE_OFFSET_MAX)
                {
                    diagnostics << QStringLiteral("typed items struct offset 越界: %1").arg(itemName);
                    return false;
                }
                appendProfileExItem(profile, fieldId, KSW_DYN_PROFILE_EX_ITEM_KIND_STRUCT_OFFSET, value, required);
            }
            else
            {
                diagnostics << QStringLiteral("typed items kind 不支持: %1").arg(itemName);
                return false;
            }
            profile.typedItemCount += 1U;
        }

        return true;
    }

    // parseProfileUInt32：
    // - 输入 value：JSON 数值或 0x 前缀字符串；
    // - 处理：进行 32-bit 无符号范围校验；
    // - 返回：成功解析 true，失败 false。
    bool parseProfileUInt32(const QJsonValue& value, std::uint32_t& valueOut)
    {
        bool ok = false;
        qulonglong parsedValue = 0ULL;

        valueOut = 0U;
        if (value.isString())
        {
            QString text = value.toString().trimmed();
            int base = 10;
            if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            {
                text = text.mid(2);
                base = 16;
            }
            parsedValue = text.toULongLong(&ok, base);
        }
        else if (value.isDouble())
        {
            const double numericValue = value.toDouble();
            if (numericValue >= 0.0 && numericValue <= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
            {
                parsedValue = static_cast<qulonglong>(numericValue);
                ok = true;
            }
        }

        if (!ok || parsedValue > static_cast<qulonglong>(std::numeric_limits<std::uint32_t>::max()))
        {
            return false;
        }

        valueOut = static_cast<std::uint32_t>(parsedValue);
        return true;
    }

    // profileClassIdFromText：
    // - 输入 classText：JSON module.class；
    // - 处理：转换到 R0 profile class id；
    // - 返回：成功 true，未知 class false。
    bool profileClassIdFromText(const QString& classText, std::uint32_t& classIdOut)
    {
        const QString normalized = classText.trimmed().toLower();
        if (normalized == QStringLiteral("ntoskrnl") ||
            normalized == QStringLiteral("ntoskrnl.exe") ||
            normalized == QStringLiteral("ntkrnlmp") ||
            normalized == QStringLiteral("ntkrnlmp.exe"))
        {
            classIdOut = KSW_DYN_PROFILE_CLASS_NTOSKRNL;
            return true;
        }
        if (normalized == QStringLiteral("ntkrla57") || normalized == QStringLiteral("ntkrla57.exe"))
        {
            classIdOut = KSW_DYN_PROFILE_CLASS_NTKRLA57;
            return true;
        }

        classIdOut = 0U;
        return false;
    }

    // profileClassIdFromJsonValue：
    // - 输入 value：pack profile 中的 moduleClassId；
    // - 处理：解析 uint32 并限制到已知 class；
    // - 返回：成功 true，失败 false。
    bool profileClassIdFromJsonValue(const QJsonValue& value, std::uint32_t& classIdOut)
    {
        std::uint32_t parsedValue = 0U;
        if (!parseProfileUInt32(value, parsedValue))
        {
            classIdOut = 0U;
            return false;
        }

        switch (parsedValue)
        {
        case KSW_DYN_PROFILE_CLASS_NTOSKRNL:
        case KSW_DYN_PROFILE_CLASS_NTKRLA57:
        case KSW_DYN_PROFILE_CLASS_LXCORE:
            classIdOut = parsedValue;
            return true;
        default:
            classIdOut = 0U;
            return false;
        }
    }

    // appendUniquePath：
    // - 输入 paths/pathText：待维护列表和候选路径；
    // - 处理：清理路径并进行大小写不敏感去重；
    // - 返回：无，paths 按需追加。
    void appendUniquePath(QStringList& paths, const QString& pathText)
    {
        const QString trimmed = pathText.trimmed();
        if (trimmed.isEmpty())
        {
            return;
        }

        const QString cleaned = QDir::cleanPath(trimmed);
        if (!cleaned.isEmpty() && !paths.contains(cleaned, Qt::CaseInsensitive))
        {
            paths << cleaned;
        }
    }

    // profilePackSearchPaths：
    // - 输入：无；
    // - 处理：优先读取程序目录 pack，并允许环境变量指定调试 pack；
    // - 返回：候选 pack 文件路径列表，不保证存在。
    QStringList profilePackSearchPaths()
    {
        QStringList paths;
        appendUniquePath(paths, qEnvironmentVariable("KSWORD_ARK_PROFILE_PACK"));
        appendUniquePath(paths, QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles/ark_dyndata_pack_v3.json")));
        appendUniquePath(paths, QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles/ark_dyndata_pack_v2.json")));
        appendUniquePath(paths, QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles/ark_dyndata_pack_v1.json")));
        appendUniquePath(paths, QDir::current().filePath(QStringLiteral("profiles/ark_dyndata_pack_v3.json")));
        appendUniquePath(paths, QDir::current().filePath(QStringLiteral("profiles/ark_dyndata_pack_v2.json")));
        appendUniquePath(paths, QDir::current().filePath(QStringLiteral("profiles/ark_dyndata_pack_v1.json")));
        return paths;
    }

    // profileSearchDirectories：
    // - 输入：无；
    // - 处理：仅当 KSWORD_ARK_PROFILE_DIR 显式设置时启用散落 JSON 调试 fallback；
    // - 返回：候选目录列表，不保证目录存在。
    QStringList profileSearchDirectories()
    {
        QStringList directories;
        appendUniquePath(directories, qEnvironmentVariable("KSWORD_ARK_PROFILE_DIR"));
        return directories;
    }

    // loadPdbProfileFile：
    // - 输入 filePath/currentIdentity：候选 JSON 文件和当前 R0 ntoskrnl identity；
    // - 处理：解析 module identity、字段表和 offset 范围；
    // - 返回：LocalPdbProfile；matched=false 表示不是当前内核 profile。
    LocalPdbProfile loadPdbProfileFile(const QString& filePath, const ksword::ark::ArkDynModuleIdentity& currentIdentity)
    {
        LocalPdbProfile profile;
        profile.sourceText = QStringLiteral("scattered-json");
        profile.pathText = QDir::toNativeSeparators(filePath);

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            profile.diagnosticsText = QStringLiteral("无法打开 profile: %1").arg(file.errorString());
            return profile;
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            profile.diagnosticsText = QStringLiteral("JSON 解析失败: %1").arg(parseError.errorString());
            return profile;
        }

        const QJsonObject rootObject = document.object();
        const QJsonObject moduleObject = rootObject.value(QStringLiteral("module")).toObject();
        std::uint32_t profileClass = 0U;
        std::uint32_t machine = 0U;
        std::uint32_t timeDateStamp = 0U;
        std::uint32_t sizeOfImage = 0U;
        if (!profileClassIdFromText(moduleObject.value(QStringLiteral("class")).toString(), profileClass) ||
            !parseProfileUInt32(moduleObject.value(QStringLiteral("machine")), machine) ||
            !parseProfileUInt32(moduleObject.value(QStringLiteral("timeDateStamp")), timeDateStamp) ||
            !parseProfileUInt32(moduleObject.value(QStringLiteral("sizeOfImage")), sizeOfImage))
        {
            profile.diagnosticsText = QStringLiteral("profile module identity 字段缺失或格式无效。");
            return profile;
        }

        profile.matched = currentIdentity.present &&
            currentIdentity.classId == profileClass &&
            currentIdentity.machine == machine &&
            currentIdentity.timeDateStamp == timeDateStamp &&
            currentIdentity.sizeOfImage == sizeOfImage;
        if (!profile.matched)
        {
            profile.diagnosticsText = QStringLiteral("profile identity 不匹配当前内核。");
            return profile;
        }

        const QJsonObject fieldsObject = rootObject.value(QStringLiteral("fields")).toObject();
        profile.applyInput.profileName = rootObject.value(QStringLiteral("profileName")).toString(QFileInfo(filePath).baseName()).toStdString();
        profile.applyInput.pdbName = moduleObject.value(QStringLiteral("pdbName")).toString().toStdString();
        profile.applyInput.pdbGuid = moduleObject.value(QStringLiteral("pdbGuid")).toString().toStdString();
        std::uint32_t pdbAge = 0U;
        if (parseProfileUInt32(moduleObject.value(QStringLiteral("pdbAge")), pdbAge))
        {
            profile.applyInput.pdbAge = pdbAge;
        }
        profile.applyInput.ntoskrnl = currentIdentity;

        std::uint32_t invalidOffsetCount = 0U;
        for (auto iterator = fieldsObject.constBegin(); iterator != fieldsObject.constEnd(); ++iterator)
        {
            std::uint32_t fieldId = 0U;
            std::uint32_t offset = 0U;
            if (!fieldIdForProfileName(iterator.key(), fieldId))
            {
                profile.ignoredUnknownFields += 1U;
                continue;
            }
            if (!parseProfileUInt32(iterator.value(), offset) ||
                offset == 0xFFFFFFFFU ||
                offset > KSW_DYN_PROFILE_OFFSET_MAX)
            {
                invalidOffsetCount += 1U;
                continue;
            }

            ksword::ark::DynDataProfileField field{};
            field.fieldId = fieldId;
            field.offset = offset;
            profile.applyInput.fields.push_back(field);
        }

        QJsonArray typedItemsArray = rootObject.value(QStringLiteral("items")).toArray();
        if (typedItemsArray.isEmpty())
        {
            typedItemsArray = rootObject.value(QStringLiteral("typedItems")).toArray();
        }
        const QJsonArray callbackItemsArray = typedItemsArray.isEmpty()
            ? rootObject.value(QStringLiteral("callbackItems")).toArray()
            : QJsonArray();
        QStringList callbackDiagnostics;
        bool hasCallbackItems = false;
        if (!callbackItemsArray.isEmpty() && !loadCallbackItemsFromJson(callbackItemsArray, profile, callbackDiagnostics, hasCallbackItems))
        {
            profile.diagnosticsText = callbackDiagnostics.join(QStringLiteral(" | "));
            return profile;
        }
        if (hasCallbackItems)
        {
            profile.applyExInput.profileName = profile.applyInput.profileName;
            profile.applyExInput.pdbName = profile.applyInput.pdbName;
            profile.applyExInput.pdbGuid = profile.applyInput.pdbGuid;
            profile.applyExInput.pdbAge = profile.applyInput.pdbAge;
            profile.applyExInput.ntoskrnl = currentIdentity;
        }
        QStringList typedDiagnostics;
        bool hasTypedItems = false;
        if (!loadTypedItemsFromJson(typedItemsArray, profile, typedDiagnostics, hasTypedItems))
        {
            profile.diagnosticsText = typedDiagnostics.join(QStringLiteral(" | "));
            return profile;
        }
        if (hasTypedItems)
        {
            profile.applyExInput.profileName = profile.applyInput.profileName;
            profile.applyExInput.pdbName = profile.applyInput.pdbName;
            profile.applyExInput.pdbGuid = profile.applyInput.pdbGuid;
            profile.applyExInput.pdbAge = profile.applyInput.pdbAge;
            profile.applyExInput.ntoskrnl = currentIdentity;
            profile.applyInput.fields.clear();
        }
        if (fieldsObject.isEmpty() && !hasTypedItems)
        {
            profile.diagnosticsText = QStringLiteral("profile fields 为空。");
            return profile;
        }

        if (invalidOffsetCount != 0U && !hasTypedItems)
        {
            profile.diagnosticsText = QStringLiteral("profile 含 %1 个越界或无效 offset，R3 已拒绝应用。").arg(invalidOffsetCount);
            return profile;
        }
        if ((profile.applyInput.fields.empty() && profile.applyExInput.items.empty()) ||
            profile.applyInput.fields.size() > KSW_DYN_PROFILE_MAX_FIELDS ||
            profile.applyExInput.items.size() > KSW_DYN_PROFILE_EX_MAX_ITEMS)
        {
            profile.diagnosticsText = QStringLiteral("profile 有效字段/item 数量异常: fields=%1 items=%2。")
                .arg(static_cast<qulonglong>(profile.applyInput.fields.size()))
                .arg(static_cast<qulonglong>(profile.applyExInput.items.size()));
            return profile;
        }

        profile.valid = true;
        profile.diagnosticsText = QStringLiteral("profile 匹配，字段 %1 个，typed 项 %2 个，callback 项 %3 个，忽略未知字段 %4 个。")
            .arg(static_cast<qulonglong>(profile.applyInput.fields.size()))
            .arg(static_cast<qulonglong>(profile.typedItemCount))
            .arg(static_cast<qulonglong>(profile.callbackItemCount))
            .arg(profile.ignoredUnknownFields);
        return profile;
    }

    // loadPdbProfilePackEntry：
    // - 输入 pack 记录/currentIdentity/fieldDictionary/packVersion：pack profile 条目、当前内核身份、字段字典和版本；
    // - 处理：把 v1/v2 紧凑字段和 v3 typed items 展开为 apply input；
    // - 返回：LocalPdbProfile；matched=false 表示不是当前内核 profile。
    LocalPdbProfile loadPdbProfilePackEntry(
        const QJsonObject& packEntry,
        const QJsonArray& fieldDictionary,
        const std::uint32_t packVersion,
        const ksword::ark::ArkDynModuleIdentity& currentIdentity)
    {
        LocalPdbProfile profile;
        profile.sourceText = QStringLiteral("pack");

        std::uint32_t profileClass = 0U;
        std::uint32_t machine = 0U;
        std::uint32_t timeDateStamp = 0U;
        std::uint32_t sizeOfImage = 0U;
        if (!profileClassIdFromJsonValue(packEntry.value(QStringLiteral("moduleClassId")), profileClass) ||
            !parseProfileUInt32(packEntry.value(QStringLiteral("machine")), machine) ||
            !parseProfileUInt32(packEntry.value(QStringLiteral("timeDateStamp")), timeDateStamp) ||
            !parseProfileUInt32(packEntry.value(QStringLiteral("sizeOfImage")), sizeOfImage))
        {
            profile.diagnosticsText = QStringLiteral("pack profile identity 字段缺失或格式无效。");
            return profile;
        }

        profile.matched = currentIdentity.present &&
            currentIdentity.classId == profileClass &&
            currentIdentity.machine == machine &&
            currentIdentity.timeDateStamp == timeDateStamp &&
            currentIdentity.sizeOfImage == sizeOfImage;
        if (!profile.matched)
        {
            profile.diagnosticsText = QStringLiteral("pack profile identity 不匹配当前内核。");
            return profile;
        }

        const QJsonArray fieldsArray = packEntry.value(QStringLiteral("fields")).toArray();
        QJsonArray typedItemsArray = packEntry.value(QStringLiteral("items")).toArray();
        if (typedItemsArray.isEmpty())
        {
            typedItemsArray = packEntry.value(QStringLiteral("typedItems")).toArray();
        }
        if (fieldsArray.isEmpty() && typedItemsArray.isEmpty())
        {
            profile.diagnosticsText = QStringLiteral("pack profile fields/items 均为空。");
            return profile;
        }

        const QJsonValue profileNameValue = packEntry.value(QStringLiteral("profileName"));
        const QJsonValue pdbNameValue = packEntry.value(QStringLiteral("pdbName"));
        const QJsonValue pdbGuidValue = packEntry.value(QStringLiteral("pdbGuid"));
        QString profileNameText = profileNameValue.toString().trimmed();
        if (profileNameText.isEmpty())
        {
            profileNameText = QStringLiteral("pack-profile");
        }
        profile.applyInput.profileName = profileNameText.toStdString();
        profile.applyInput.pdbName = pdbNameValue.toString().toStdString();
        profile.applyInput.pdbGuid = pdbGuidValue.toString().toStdString();
        std::uint32_t pdbAge = 0U;
        if (parseProfileUInt32(packEntry.value(QStringLiteral("pdbAge")), pdbAge))
        {
            profile.applyInput.pdbAge = pdbAge;
        }
        profile.applyInput.ntoskrnl = currentIdentity;

        std::uint32_t invalidFieldCount = 0U;
        std::array<bool, KSW_DYN_FIELD_ID_MAX + 1U> seenFieldIds{};
        for (const QJsonValue& entryValue : fieldsArray)
        {
            const QJsonArray pairArray = entryValue.toArray();
            if (pairArray.size() != 2)
            {
                invalidFieldCount += 1U;
                continue;
            }

            std::uint32_t fieldIndex = 0U;
            std::uint32_t offset = 0U;
            if (!parseProfileUInt32(pairArray.at(0), fieldIndex) ||
                !parseProfileUInt32(pairArray.at(1), offset) ||
                fieldIndex >= static_cast<std::uint32_t>(fieldDictionary.size()))
            {
                invalidFieldCount += 1U;
                continue;
            }

            const QString fieldName = fieldDictionary.at(static_cast<int>(fieldIndex)).toString();
            std::uint32_t fieldId = 0U;
            if (!fieldIdForProfileName(fieldName, fieldId))
            {
                profile.ignoredUnknownFields += 1U;
                continue;
            }
            if (offset == 0xFFFFFFFFU || offset > KSW_DYN_PROFILE_OFFSET_MAX)
            {
                invalidFieldCount += 1U;
                continue;
            }
            if (fieldId >= seenFieldIds.size() || seenFieldIds[fieldId])
            {
                invalidFieldCount += 1U;
                continue;
            }
            seenFieldIds[fieldId] = true;

            ksword::ark::DynDataProfileField field{};
            field.fieldId = fieldId;
            field.offset = offset;
            profile.applyInput.fields.push_back(field);
        }

        const QJsonArray callbackItemsArray = typedItemsArray.isEmpty()
            ? packEntry.value(QStringLiteral("callbackItems")).toArray()
            : QJsonArray();
        QStringList callbackDiagnostics;
        bool hasCallbackItems = false;
        if (!callbackItemsArray.isEmpty() && !loadCallbackItemsFromJson(callbackItemsArray, profile, callbackDiagnostics, hasCallbackItems))
        {
            profile.diagnosticsText = callbackDiagnostics.join(QStringLiteral(" | "));
            return profile;
        }
        if (hasCallbackItems)
        {
            profile.applyExInput.profileName = profile.applyInput.profileName;
            profile.applyExInput.pdbName = profile.applyInput.pdbName;
            profile.applyExInput.pdbGuid = profile.applyInput.pdbGuid;
            profile.applyExInput.pdbAge = profile.applyInput.pdbAge;
            profile.applyExInput.ntoskrnl = currentIdentity;
        }

        QStringList typedDiagnostics;
        bool hasTypedItems = false;
        if (!loadTypedItemsFromJson(typedItemsArray, profile, typedDiagnostics, hasTypedItems))
        {
            profile.diagnosticsText = typedDiagnostics.join(QStringLiteral(" | "));
            return profile;
        }
        if (hasTypedItems)
        {
            profile.applyExInput.profileName = profile.applyInput.profileName;
            profile.applyExInput.pdbName = profile.applyInput.pdbName;
            profile.applyExInput.pdbGuid = profile.applyInput.pdbGuid;
            profile.applyExInput.pdbAge = profile.applyInput.pdbAge;
            profile.applyExInput.ntoskrnl = currentIdentity;
            profile.applyInput.fields.clear();
        }
        if (invalidFieldCount != 0U && !(packVersion == 3U && hasTypedItems))
        {
            profile.diagnosticsText = QStringLiteral("pack profile 含 %1 个越界或无效字段，R3 已拒绝应用。").arg(invalidFieldCount);
            return profile;
        }
        if ((profile.applyInput.fields.empty() && profile.applyExInput.items.empty()) ||
            profile.applyInput.fields.size() > KSW_DYN_PROFILE_MAX_FIELDS ||
            profile.applyExInput.items.size() > KSW_DYN_PROFILE_EX_MAX_ITEMS)
        {
            profile.diagnosticsText = QStringLiteral("pack profile 有效字段/item 数量异常: fields=%1 items=%2。")
                .arg(static_cast<qulonglong>(profile.applyInput.fields.size()))
                .arg(static_cast<qulonglong>(profile.applyExInput.items.size()));
            return profile;
        }

        profile.valid = true;
        profile.diagnosticsText = QStringLiteral("pack profile 匹配，字段 %1 个，typed 项 %2 个，callback 项 %3 个，忽略未知字段 %4 个。")
            .arg(static_cast<qulonglong>(profile.applyInput.fields.size()))
            .arg(static_cast<qulonglong>(profile.typedItemCount))
            .arg(static_cast<qulonglong>(profile.callbackItemCount))
            .arg(profile.ignoredUnknownFields);
        return profile;
    }

    // loadPdbProfilePackFile：
    // - 输入 filePath/currentIdentity/diagnosticsOut：候选 pack、当前内核身份和诊断输出；
    // - 处理：校验 v1/v2/v3 pack schema、字段字典和 profiles 数组，寻找精确匹配条目；
    // - 返回：匹配 profile；未命中或 pack 无效时 valid=false/matched=false。
    LocalPdbProfile loadPdbProfilePackFile(const QString& filePath, const ksword::ark::ArkDynModuleIdentity& currentIdentity, QString& diagnosticsOut)
    {
        LocalPdbProfile bestProfile;
        bestProfile.sourceText = QStringLiteral("pack");
        bestProfile.pathText = QDir::toNativeSeparators(filePath);

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            diagnosticsOut = QStringLiteral("无法打开 PDB profile pack: %1").arg(file.errorString());
            return bestProfile;
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            diagnosticsOut = QStringLiteral("PDB profile pack JSON 解析失败: %1").arg(parseError.errorString());
            return bestProfile;
        }

        const QJsonObject rootObject = document.object();
        std::uint32_t schemaVersion = 0U;
        std::uint32_t packVersion = 0U;
        if (!parseProfileUInt32(rootObject.value(QStringLiteral("schemaVersion")), schemaVersion) ||
            !parseProfileUInt32(rootObject.value(QStringLiteral("packVersion")), packVersion) ||
            schemaVersion != 1U ||
            (packVersion != 1U && packVersion != 2U && packVersion != 3U))
        {
            diagnosticsOut = QStringLiteral("PDB profile pack schemaVersion/packVersion 不支持。");
            return bestProfile;
        }

        const QJsonArray fieldDictionary = rootObject.value(QStringLiteral("fieldDictionary")).toArray();
        const QJsonArray profilesArray = rootObject.value(QStringLiteral("profiles")).toArray();
        if ((fieldDictionary.isEmpty() && packVersion != 3U) || profilesArray.isEmpty())
        {
            diagnosticsOut = QStringLiteral("PDB profile pack 字段字典或 profile 列表为空。");
            return bestProfile;
        }

        std::uint32_t invalidDictionaryFields = 0U;
        std::array<bool, KSW_DYN_PROFILE_MAX_FIELDS + 1U> seenDictionaryFieldIds{};
        for (const QJsonValue& fieldNameValue : fieldDictionary)
        {
            std::uint32_t ignoredFieldId = 0U;
            if (!fieldNameValue.isString() || !fieldIdForProfileName(fieldNameValue.toString(), ignoredFieldId))
            {
                invalidDictionaryFields += 1U;
                continue;
            }
            if (ignoredFieldId >= seenDictionaryFieldIds.size() || seenDictionaryFieldIds[ignoredFieldId])
            {
                invalidDictionaryFields += 1U;
                continue;
            }
            seenDictionaryFieldIds[ignoredFieldId] = true;
        }
        if (invalidDictionaryFields != 0U)
        {
            diagnosticsOut = QStringLiteral("PDB profile pack 字段字典包含 %1 个未知字段，已拒绝。").arg(invalidDictionaryFields);
            return bestProfile;
        }

        std::uint32_t scannedCount = 0U;
        std::uint32_t invalidMatchedCount = 0U;
        for (const QJsonValue& profileValue : profilesArray)
        {
            if (!profileValue.isObject())
            {
                continue;
            }

            scannedCount += 1U;
            LocalPdbProfile profile = loadPdbProfilePackEntry(profileValue.toObject(), fieldDictionary, packVersion, currentIdentity);
            profile.pathText = QDir::toNativeSeparators(filePath);
            if (profile.matched)
            {
                if (profile.valid)
                {
                    diagnosticsOut = QStringLiteral("PDB profile pack 命中；路径=%1，profiles=%2，扫描=%3，%4")
                        .arg(QDir::toNativeSeparators(filePath))
                        .arg(static_cast<qulonglong>(profilesArray.size()))
                        .arg(scannedCount)
                        .arg(profile.diagnosticsText);
                    return profile;
                }

                invalidMatchedCount += 1U;
                if (!bestProfile.matched)
                {
                    bestProfile = profile;
                    bestProfile.pathText = QDir::toNativeSeparators(filePath);
                }
            }
        }

        diagnosticsOut = QStringLiteral("PDB profile pack 未命中；路径=%1，profiles=%2，扫描=%3，无效命中=%4。")
            .arg(QDir::toNativeSeparators(filePath))
            .arg(static_cast<qulonglong>(profilesArray.size()))
            .arg(scannedCount)
            .arg(invalidMatchedCount);
        return bestProfile;
    }

    // findMatchingPdbProfilePack：
    // - 输入 currentIdentity/diagnosticsOut：当前内核身份和诊断输出；
    // - 处理：按默认 pack 路径和环境变量路径查找，返回第一条有效命中；
    // - 返回：匹配 profile；未命中时 valid=false/matched=false。
    LocalPdbProfile findMatchingPdbProfilePack(const ksword::ark::ArkDynModuleIdentity& currentIdentity, QString& diagnosticsOut)
    {
        LocalPdbProfile bestProfile;
        QStringList diagnostics;
        std::uint32_t existingPackCount = 0U;

        for (const QString& packPath : profilePackSearchPaths())
        {
            QFileInfo fileInfo(packPath);
            if (!fileInfo.exists() || !fileInfo.isFile())
            {
                diagnostics << QStringLiteral("pack 不存在: %1").arg(QDir::toNativeSeparators(packPath));
                continue;
            }

            existingPackCount += 1U;
            QString packDiagnostics;
            LocalPdbProfile profile = loadPdbProfilePackFile(fileInfo.absoluteFilePath(), currentIdentity, packDiagnostics);
            diagnostics << packDiagnostics;
            if (profile.matched)
            {
                if (profile.valid)
                {
                    diagnosticsOut = diagnostics.join(QStringLiteral(" | "));
                    return profile;
                }
                if (!bestProfile.matched)
                {
                    bestProfile = profile;
                }
            }
        }

        diagnosticsOut = QStringLiteral("未找到匹配 PDB profile pack；检查 %1 个存在的 pack。%2")
            .arg(existingPackCount)
            .arg(diagnostics.join(QStringLiteral(" | ")));
        return bestProfile;
    }

    // findMatchingPdbProfile：
    // - 输入 currentIdentity/diagnosticsOut：当前 ntoskrnl identity 和诊断输出；
    // - 处理：优先扫描 compact pack；仅当 KSWORD_ARK_PROFILE_DIR 显式设置时扫描散落 JSON；
    // - 返回：匹配 profile；未命中时 valid=false/matched=false。
    LocalPdbProfile findMatchingPdbProfile(const ksword::ark::ArkDynModuleIdentity& currentIdentity, QString& diagnosticsOut)
    {
        LocalPdbProfile bestProfile;
        QStringList diagnostics;
        std::uint32_t scannedCount = 0U;
        std::uint32_t parseErrorCount = 0U;

        if (!currentIdentity.present)
        {
            diagnosticsOut = QStringLiteral("当前 ntoskrnl identity 不可用，跳过 PDB profile 扫描。");
            return bestProfile;
        }

        QString packDiagnostics;
        LocalPdbProfile packProfile = findMatchingPdbProfilePack(currentIdentity, packDiagnostics);
        diagnostics << packDiagnostics;
        if (packProfile.valid)
        {
            diagnosticsOut = packDiagnostics;
            return packProfile;
        }
        if (packProfile.matched)
        {
            bestProfile = packProfile;
        }

        for (const QString& directoryPath : profileSearchDirectories())
        {
            QDir directory(directoryPath);
            if (!directory.exists())
            {
                diagnostics << QStringLiteral("目录不存在: %1").arg(QDir::toNativeSeparators(directoryPath));
                continue;
            }

            const QFileInfoList files = directory.entryInfoList(
                QStringList{ QStringLiteral("*.json") },
                QDir::Files | QDir::Readable,
                QDir::Name);
            for (const QFileInfo& fileInfo : files)
            {
                scannedCount += 1U;
                LocalPdbProfile profile = loadPdbProfileFile(fileInfo.absoluteFilePath(), currentIdentity);
                if (profile.matched)
                {
                    diagnostics << profile.diagnosticsText;
                    if (profile.valid)
                    {
                        diagnosticsOut = QStringLiteral("散落 JSON fallback 扫描 %1 个 profile。%2").arg(scannedCount).arg(diagnostics.join(QStringLiteral(" | ")));
                        return profile;
                    }
                    if (!bestProfile.matched)
                    {
                        bestProfile = profile;
                    }
                    parseErrorCount += 1U;
                    continue;
                }
                if (!profile.diagnosticsText.isEmpty() && !profile.diagnosticsText.contains(QStringLiteral("identity 不匹配")))
                {
                    parseErrorCount += 1U;
                }
            }
        }

        diagnosticsOut = QStringLiteral("未找到匹配 PDB profile；散落 JSON fallback 扫描 %1 个 JSON，解析/格式异常 %2 个。%3")
            .arg(scannedCount)
            .arg(parseErrorCount)
            .arg(diagnostics.join(QStringLiteral(" | ")));
        return bestProfile;
    }

    // capabilityNames：
    // - 输入 mask：能力位图；
    // - 处理：遍历能力表并拼接命中名称；
    // - 返回：逗号分隔名称；无命中返回 None。
    QString capabilityNames(const std::uint64_t mask)
    {
        QStringList names;
        for (const CapabilityDisplay& capability : kCapabilities)
        {
            if ((mask & capability.mask) == capability.mask)
            {
                names << QString::fromLatin1(capability.name);
            }
        }
        return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
    }

    // capabilityReport：
    // - 输入 mask：能力位图；
    // - 处理：逐项列出启用/禁用；
    // - 返回：多行报告文本。
    QString capabilityReport(const std::uint64_t mask)
    {
        QStringList lines;
        for (const CapabilityDisplay& capability : kCapabilities)
        {
            const bool enabled = (mask & capability.mask) == capability.mask;
            lines << QStringLiteral("%1 [%2] %3")
                .arg(enabled ? QStringLiteral("[ON]") : QStringLiteral("[OFF]"))
                .arg(QString::fromLatin1(capability.name))
                .arg(QString::fromWCharArray(capability.title));
        }
        return lines.join(QStringLiteral("\n"));
    }

    // disabledCapabilitySummary：
    // - 输入 mask：能力位图；
    // - 处理：收集未启用能力中文名；
    // - 返回：缺失能力摘要。
    QString disabledCapabilitySummary(const std::uint64_t mask)
    {
        QStringList disabledItems;
        for (const CapabilityDisplay& capability : kCapabilities)
        {
            if ((mask & capability.mask) != capability.mask)
            {
                disabledItems << QString::fromWCharArray(capability.title);
            }
        }
        return disabledItems.isEmpty() ? QStringLiteral("无") : disabledItems.join(QStringLiteral("、"));
    }

    // convertModuleIdentity：
    // - 输入 source：ArkDriverClient 模块身份；
    // - 处理：转换到 KernelDock 内部模型；
    // - 返回：KernelDynDataModuleIdentity 值对象。
    KernelDynDataModuleIdentity convertModuleIdentity(const ksword::ark::ArkDynModuleIdentity& source)
    {
        KernelDynDataModuleIdentity result{};
        result.present = source.present;
        result.classId = source.classId;
        result.machine = source.machine;
        result.timeDateStamp = source.timeDateStamp;
        result.sizeOfImage = source.sizeOfImage;
        result.imageBase = source.imageBase;
        result.moduleNameText = wideStringToQString(source.moduleName);
        return result;
    }

    // moduleDetailText：
    // - 输入 title/source：模块标题和身份结构；
    // - 处理：格式化模块 identity；
    // - 返回：多行诊断文本。
    QString moduleDetailText(const QString& title, const KernelDynDataModuleIdentity& source)
    {
        if (!source.present)
        {
            return QStringLiteral("%1: <未加载或未识别>").arg(title);
        }

        return QStringLiteral(
            "%1:\n"
            "  ModuleName: %2\n"
            "  Class: %3 (%4)\n"
            "  Machine: %5\n"
            "  TimeDateStamp: %6\n"
            "  SizeOfImage: %7\n"
            "  ImageBase: %8")
            .arg(title)
            .arg(safeText(source.moduleNameText))
            .arg(moduleClassText(source.classId))
            .arg(source.classId)
            .arg(formatHex32(source.machine))
            .arg(formatHex32(source.timeDateStamp))
            .arg(formatHex32(source.sizeOfImage))
            .arg(formatHex64(source.imageBase));
    }

    // appendSummaryRow：
    // - 输入 table/name/value：摘要表、字段名和值；
    // - 处理：追加只读行；
    // - 返回：无。
    void appendSummaryRow(QTableWidget* table, const QString& nameText, const QString& valueText)
    {
        if (table == nullptr)
        {
            return;
        }

        const int rowIndex = table->rowCount();
        table->insertRow(rowIndex);

        auto* nameItem = new QTableWidgetItem(nameText);
        auto* valueItem = new QTableWidgetItem(valueText);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);

        table->setItem(rowIndex, static_cast<int>(SummaryColumn::Name), nameItem);
        table->setItem(rowIndex, static_cast<int>(SummaryColumn::Value), valueItem);
    }

    // setReadonlyItem：
    // - 输入 table/row/column/item：目标表、行列和 item；
    // - 处理：去掉可编辑 flag 后放入表格；
    // - 返回：无。
    void setReadonlyItem(QTableWidget* table, const int rowIndex, const DynDataColumn column, QTableWidgetItem* item)
    {
        if (table == nullptr || item == nullptr)
        {
            delete item;
            return;
        }

        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(rowIndex, static_cast<int>(column), item);
    }

    // buildFieldDetail：
    // - 输入 entry/summary：字段行和当前摘要；
    // - 处理：生成详情面板文本，包含能力依赖和全局状态；
    // - 返回：多行详情文本。
    QString buildFieldDetail(const KernelDynDataFieldEntry& entry, const KernelDynDataSummary& summary)
    {
        return QStringLiteral(
            "字段名: %1\n"
            "字段ID: %2\n"
            "偏移: %3\n"
            "状态: %4\n"
            "来源: %5\n"
            "功能: %6\n"
            "字段标志: %7\n"
            "字段能力位: %8\n"
            "字段能力名: %9\n\n"
            "当前全局能力位: %10\n"
            "当前未启用能力: %11\n\n"
            "R0不可用原因: %12")
            .arg(safeText(entry.fieldNameText))
            .arg(entry.fieldId)
            .arg(formatOffset(entry.offset))
            .arg(safeText(entry.statusText))
            .arg(safeText(entry.sourceNameText))
            .arg(safeText(entry.featureNameText))
            .arg(formatHex32(entry.flags))
            .arg(formatHex64(entry.capabilityMask))
            .arg(capabilityNames(entry.capabilityMask))
            .arg(formatHex64(summary.capabilityMask))
            .arg(disabledCapabilitySummary(summary.capabilityMask))
            .arg(safeText(summary.unavailableReasonText));
    }

    // buildDynDataReport：
    // - 输入 summary/rows：摘要和字段行；
    // - 处理：拼出可复制的完整诊断报告；
    // - 返回：多行报告文本。
    QString buildDynDataReport(const KernelDynDataSummary& summary, const std::vector<KernelDynDataFieldEntry>& rows)
    {
        QStringList lines;
        lines << QStringLiteral("Ksword DynData Diagnostic Report");
        lines << QStringLiteral("StatusQueryOk: %1").arg(boolText(summary.statusQueryOk));
        lines << QStringLiteral("FieldsQueryOk: %1").arg(boolText(summary.fieldsQueryOk));
        lines << QStringLiteral("StatusFlags: %1").arg(formatHex32(summary.statusFlags));
        lines << QStringLiteral("CapabilityMask: %1").arg(formatHex64(summary.capabilityMask));
        lines << QStringLiteral("SystemInformerDataVersion: %1").arg(summary.systemInformerDataVersion);
        lines << QStringLiteral("SystemInformerDataLength: %1").arg(summary.systemInformerDataLength);
        lines << QStringLiteral("LastStatus: %1").arg(formatNtStatus(summary.lastStatus));
        lines << QStringLiteral("MatchedProfileClass: %1").arg(moduleClassText(summary.matchedProfileClass));
        lines << QStringLiteral("MatchedProfileOffset: %1").arg(formatHex32(summary.matchedProfileOffset));
        lines << QStringLiteral("MatchedFieldsId: %1").arg(summary.matchedFieldsId);
        lines << QStringLiteral("UnavailableReason: %1").arg(safeText(summary.unavailableReasonText));
        lines << QStringLiteral("PdbProfileActive: %1").arg(boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE)));
        lines << QStringLiteral("PdbProfileScanAttempted: %1").arg(boolText(summary.pdbProfileScanAttempted));
        lines << QStringLiteral("PdbProfileFound: %1").arg(boolText(summary.pdbProfileFound));
        lines << QStringLiteral("PdbProfileAppliedThisRefresh: %1").arg(boolText(summary.pdbProfileApplied));
        lines << QStringLiteral("PdbProfileSource: %1").arg(safeText(summary.pdbProfileSourceText));
        lines << QStringLiteral("PdbProfileName: %1").arg(safeText(summary.pdbProfileNameText));
        lines << QStringLiteral("PdbProfilePath: %1").arg(safeText(summary.pdbProfilePathText));
        lines << QStringLiteral("PdbProfileStatus: %1").arg(formatNtStatus(summary.pdbProfileStatus));
        lines << QStringLiteral("PdbProfileAppliedFields: %1").arg(summary.pdbProfileAppliedFields);
        lines << QStringLiteral("PdbProfileRejectedFields: %1").arg(summary.pdbProfileRejectedFields);
        lines << QStringLiteral("PdbProfileUnknownFields: %1").arg(summary.pdbProfileUnknownFields);
        lines << QStringLiteral("PdbProfileIgnoredJsonFields: %1").arg(summary.pdbProfileIgnoredJsonFields);
        lines << QStringLiteral("PdbProfileMessage: %1").arg(safeText(summary.pdbProfileMessageText));
        lines << QStringLiteral("PdbProfileIo: %1").arg(safeText(summary.pdbProfileIoMessageText));
        lines << moduleDetailText(QStringLiteral("ntoskrnl"), summary.ntoskrnl);
        lines << moduleDetailText(QStringLiteral("lxcore"), summary.lxcore);
        lines << QStringLiteral("");
        lines << QStringLiteral("Capabilities:");
        lines << capabilityReport(summary.capabilityMask);
        lines << QStringLiteral("");
        lines << QStringLiteral("Fields:");
        for (const KernelDynDataFieldEntry& entry : rows)
        {
            lines << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6")
                .arg(safeText(entry.fieldNameText))
                .arg(formatOffset(entry.offset))
                .arg(safeText(entry.statusText))
                .arg(safeText(entry.sourceNameText))
                .arg(safeText(entry.featureNameText))
                .arg(formatHex64(entry.capabilityMask));
        }
        return lines.join(QStringLiteral("\n"));
    }

    // populateSummaryTable：
    // - 输入 table/summary/visibleRows：摘要表、摘要数据和当前字段总数；
    // - 处理：重建两列表格；
    // - 返回：无。
    void populateSummaryTable(QTableWidget* table, const KernelDynDataSummary& summary, const std::size_t visibleRows)
    {
        if (table == nullptr)
        {
            return;
        }

        table->setSortingEnabled(false);
        table->setRowCount(0);
        appendSummaryRow(table, QStringLiteral("DynData 初始化"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_INITIALIZED)));
        appendSummaryRow(table, QStringLiteral("ntoskrnl profile"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("lxcore profile"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("Ksword runtime offset"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("PDB profile active"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("PDB profile 扫描"), boolText(summary.pdbProfileScanAttempted));
        appendSummaryRow(table, QStringLiteral("PDB profile 命中"), boolText(summary.pdbProfileFound));
        appendSummaryRow(table, QStringLiteral("PDB profile 本次应用"), boolText(summary.pdbProfileApplied));
        appendSummaryRow(table, QStringLiteral("PDB profile 来源"), safeText(summary.pdbProfileSourceText));
        appendSummaryRow(table, QStringLiteral("PDB profile 名称"), safeText(summary.pdbProfileNameText));
        appendSummaryRow(table, QStringLiteral("PDB profile 路径"), safeText(summary.pdbProfilePathText));
        appendSummaryRow(table, QStringLiteral("PDB profile 状态"), formatNtStatus(summary.pdbProfileStatus));
        appendSummaryRow(table, QStringLiteral("PDB profile 字段"), QStringLiteral("applied=%1 rejected=%2 unknown=%3 ignoredJson=%4")
            .arg(summary.pdbProfileAppliedFields)
            .arg(summary.pdbProfileRejectedFields)
            .arg(summary.pdbProfileUnknownFields)
            .arg(summary.pdbProfileIgnoredJsonFields));
        appendSummaryRow(table, QStringLiteral("PDB profile 消息"), safeText(summary.pdbProfileMessageText));
        appendSummaryRow(table, QStringLiteral("PDB profile IO"), safeText(summary.pdbProfileIoMessageText));
        appendSummaryRow(table, QStringLiteral("System Informer 版本"), QString::number(summary.systemInformerDataVersion));
        appendSummaryRow(table, QStringLiteral("System Informer 数据长度"), QString::number(summary.systemInformerDataLength));
        appendSummaryRow(table, QStringLiteral("LastStatus"), formatNtStatus(summary.lastStatus));
        appendSummaryRow(table, QStringLiteral("MatchedProfileClass"), QStringLiteral("%1 (%2)").arg(moduleClassText(summary.matchedProfileClass)).arg(summary.matchedProfileClass));
        appendSummaryRow(table, QStringLiteral("MatchedProfileOffset"), formatHex32(summary.matchedProfileOffset));
        appendSummaryRow(table, QStringLiteral("MatchedFieldsId"), QString::number(summary.matchedFieldsId));
        appendSummaryRow(table, QStringLiteral("CapabilityMask"), formatHex64(summary.capabilityMask));
        appendSummaryRow(table, QStringLiteral("字段总数/当前返回"), QStringLiteral("%1 / %2")
            .arg(summary.fieldCount)
            .arg(static_cast<qulonglong>(visibleRows)));
        appendSummaryRow(table, QStringLiteral("禁用能力"), disabledCapabilitySummary(summary.capabilityMask));
        appendSummaryRow(table, QStringLiteral("不可用原因"), safeText(summary.unavailableReasonText));
        appendSummaryRow(table, QStringLiteral("Status IO"), safeText(summary.statusIoMessageText));
        appendSummaryRow(table, QStringLiteral("Fields IO"), safeText(summary.fieldsIoMessageText));
        appendSummaryRow(table, QStringLiteral("ntoskrnl"), moduleDetailText(QStringLiteral("ntoskrnl"), summary.ntoskrnl).replace(QStringLiteral("\n"), QStringLiteral("; ")));
        appendSummaryRow(table, QStringLiteral("lxcore"), moduleDetailText(QStringLiteral("lxcore"), summary.lxcore).replace(QStringLiteral("\n"), QStringLiteral("; ")));
        table->setSortingEnabled(false);
    }

    // shouldShowField：
    // - 输入 entry/filterKeyword：字段行和筛选关键字；
    // - 处理：在字段名、偏移、来源、功能、状态和能力名中匹配；
    // - 返回：true 表示该行应显示。
    bool shouldShowField(const KernelDynDataFieldEntry& entry, const QString& filterKeyword)
    {
        if (filterKeyword.isEmpty())
        {
            return true;
        }

        return entry.fieldNameText.contains(filterKeyword, Qt::CaseInsensitive) ||
            formatOffset(entry.offset).contains(filterKeyword, Qt::CaseInsensitive) ||
            entry.statusText.contains(filterKeyword, Qt::CaseInsensitive) ||
            entry.sourceNameText.contains(filterKeyword, Qt::CaseInsensitive) ||
            entry.featureNameText.contains(filterKeyword, Qt::CaseInsensitive) ||
            capabilityNames(entry.capabilityMask).contains(filterKeyword, Qt::CaseInsensitive);
    }

    // queryDynDataSnapshot：
    // - 输入 summaryOut/rowsOut：输出摘要和字段缓存；
    // - 处理：通过 ArkDriverClient 查询三个 DynData IOCTL 并转换模型；
    // - 返回：true 表示至少 status 和 fields 查询都成功。
    bool queryDynDataSnapshot(KernelDynDataSummary& summaryOut, std::vector<KernelDynDataFieldEntry>& rowsOut)
    {
        ksword::ark::DriverClient client;
        const ksword::ark::DynDataStatusResult initialStatusResult = client.queryDynDataStatus();

        bool pdbProfileScanAttempted = false;
        bool pdbProfileFound = false;
        bool pdbProfileApplied = false;
        bool requeryAfterProfileApply = false;
        long pdbProfileStatus = 0;
        std::uint32_t pdbProfileAppliedFields = 0U;
        std::uint32_t pdbProfileRejectedFields = 0U;
        std::uint32_t pdbProfileUnknownFields = 0U;
        std::uint32_t pdbProfileIgnoredJsonFields = 0U;
        QString pdbProfileNameText;
        QString pdbProfilePathText;
        QString pdbProfileSourceText;
        QString pdbProfileMessageText;
        QString pdbProfileIoMessageText;

        if (initialStatusResult.io.ok)
        {
            const bool pdbProfileAlreadyActive =
                statusFlagEnabled(initialStatusResult.statusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE);
            const bool callbackProfileAlreadyActive =
                statusFlagEnabled(initialStatusResult.statusFlags, KSW_DYN_STATUS_FLAG_CALLBACK_PROFILE_ACTIVE);
            const bool v3ProfileAlreadyActive =
                (initialStatusResult.capabilityMask &
                    (KSW_CAP_PROCESS_LIST_FIELDS |
                        KSW_CAP_THREAD_LIST_FIELDS |
                        KSW_CAP_CID_TABLE_WALK |
                        KSW_CAP_KERNEL_MODULE_LIST_FIELDS |
                        KSW_CAP_DRIVER_OBJECT_FIELDS |
                        KSW_CAP_KERNEL_GLOBALS)) != 0ULL;
            if (pdbProfileAlreadyActive && callbackProfileAlreadyActive && v3ProfileAlreadyActive)
            {
                pdbProfileMessageText = QStringLiteral("R0 已经启用 PDB/callback/v3 DynData profile，本次刷新跳过重复 apply。");
            }
            else
            {
                pdbProfileScanAttempted = true;
                QString scanDiagnostics;
                const LocalPdbProfile profile = findMatchingPdbProfile(initialStatusResult.ntoskrnl, scanDiagnostics);
                pdbProfileMessageText = scanDiagnostics;
                if (profile.matched)
                {
                    pdbProfileFound = true;
                    pdbProfileSourceText = profileSourceDisplayText(profile.sourceText);
                    pdbProfileNameText = stringToQString(profile.applyInput.profileName);
                    pdbProfilePathText = profile.pathText;
                    pdbProfileIgnoredJsonFields = profile.ignoredUnknownFields;

                    if (!profile.valid)
                    {
                        pdbProfileMessageText = profile.diagnosticsText;
                    }
                    else if (pdbProfileAlreadyActive &&
                        callbackProfileAlreadyActive &&
                        !v3ProfileAlreadyActive &&
                        profile.typedItemCount == 0U)
                    {
                        pdbProfileMessageText = QStringLiteral("R0 已启用 v1/v2 PDB profile，当前匹配 profile 未提供 v3 typed items，本次刷新跳过重复 apply。");
                    }
                    else
                    {
                        QStringList applyMessages;
                        bool anyApplySucceeded = false;

                        if (!profile.applyInput.fields.empty())
                        {
                            const ksword::ark::DynDataProfileApplyResult applyResult =
                                client.applyDynDataProfile(profile.applyInput);
                            pdbProfileIoMessageText = QString::fromStdString(applyResult.io.message);
                            pdbProfileStatus = applyResult.status;
                            pdbProfileAppliedFields = applyResult.appliedFieldCount;
                            pdbProfileRejectedFields = applyResult.rejectedFieldCount;
                            pdbProfileUnknownFields = applyResult.unknownFieldCount;
                            if (!applyResult.message.empty())
                            {
                                applyMessages << wideStringToQString(applyResult.message);
                            }
                            anyApplySucceeded = applyResult.io.ok && applyResult.status == 0;
                        }

                        if (!profile.applyExInput.items.empty())
                        {
                            const ksword::ark::DynDataProfileApplyExResult applyExResult =
                                client.applyDynDataProfileEx(profile.applyExInput);
                            if (!pdbProfileIoMessageText.isEmpty())
                            {
                                pdbProfileIoMessageText += QStringLiteral(" | ");
                            }
                            pdbProfileIoMessageText += QString::fromStdString(applyExResult.io.message);
                            pdbProfileStatus = applyExResult.status;
                            pdbProfileAppliedFields += applyExResult.appliedItemCount;
                            pdbProfileRejectedFields += applyExResult.rejectedItemCount;
                            pdbProfileUnknownFields += applyExResult.unknownItemCount;
                            if (!applyExResult.message.empty())
                            {
                                applyMessages << wideStringToQString(applyExResult.message);
                            }
                            anyApplySucceeded = anyApplySucceeded || (applyExResult.io.ok && applyExResult.status == 0);
                        }

                        if (!applyMessages.isEmpty())
                        {
                            pdbProfileMessageText = applyMessages.join(QStringLiteral(" | "));
                        }
                        pdbProfileApplied = anyApplySucceeded;
                        requeryAfterProfileApply = anyApplySucceeded;
                    }
                }
            }
        }
        else
        {
            pdbProfileMessageText = QStringLiteral("DynData status 查询失败，无法确认 ntoskrnl identity，跳过 PDB profile 扫描。");
            pdbProfileIoMessageText = QString::fromStdString(initialStatusResult.io.message);
        }

        ksword::ark::DynDataStatusResult statusResult = initialStatusResult;
        if (requeryAfterProfileApply)
        {
            const ksword::ark::DynDataStatusResult refreshedStatusResult = client.queryDynDataStatus();
            if (refreshedStatusResult.io.ok)
            {
                statusResult = refreshedStatusResult;
            }
            else if (pdbProfileIoMessageText.isEmpty())
            {
                pdbProfileIoMessageText = QString::fromStdString(refreshedStatusResult.io.message);
            }
        }

        const ksword::ark::DynDataFieldsResult fieldsResult = client.queryDynDataFields();
        const ksword::ark::DynDataCapabilitiesResult capabilitiesResult = client.queryDynDataCapabilities();

        summaryOut = KernelDynDataSummary{};
        rowsOut.clear();

        summaryOut.statusQueryOk = statusResult.io.ok;
        summaryOut.fieldsQueryOk = fieldsResult.io.ok;
        summaryOut.statusIoMessageText = QString::fromStdString(statusResult.io.message);
        summaryOut.fieldsIoMessageText = QString::fromStdString(fieldsResult.io.message);
        summaryOut.pdbProfileScanAttempted = pdbProfileScanAttempted;
        summaryOut.pdbProfileFound = pdbProfileFound;
        summaryOut.pdbProfileApplied = pdbProfileApplied;
        summaryOut.pdbProfileStatus = pdbProfileStatus;
        summaryOut.pdbProfileAppliedFields = pdbProfileAppliedFields;
        summaryOut.pdbProfileRejectedFields = pdbProfileRejectedFields;
        summaryOut.pdbProfileUnknownFields = pdbProfileUnknownFields;
        summaryOut.pdbProfileIgnoredJsonFields = pdbProfileIgnoredJsonFields;
        summaryOut.pdbProfileSourceText = pdbProfileSourceText;
        summaryOut.pdbProfileNameText = pdbProfileNameText;
        summaryOut.pdbProfilePathText = pdbProfilePathText;
        summaryOut.pdbProfileMessageText = pdbProfileMessageText;
        summaryOut.pdbProfileIoMessageText = pdbProfileIoMessageText;

        if (statusResult.io.ok)
        {
            summaryOut.statusFlags = statusResult.statusFlags;
            summaryOut.systemInformerDataVersion = statusResult.systemInformerDataVersion;
            summaryOut.systemInformerDataLength = statusResult.systemInformerDataLength;
            summaryOut.lastStatus = statusResult.lastStatus;
            summaryOut.matchedProfileClass = statusResult.matchedProfileClass;
            summaryOut.matchedProfileOffset = statusResult.matchedProfileOffset;
            summaryOut.matchedFieldsId = statusResult.matchedFieldsId;
            summaryOut.fieldCount = statusResult.fieldCount;
            summaryOut.capabilityMask = statusResult.capabilityMask;
            summaryOut.ntoskrnl = convertModuleIdentity(statusResult.ntoskrnl);
            summaryOut.lxcore = convertModuleIdentity(statusResult.lxcore);
            summaryOut.unavailableReasonText = wideStringToQString(statusResult.unavailableReason);
        }

        if (capabilitiesResult.io.ok)
        {
            summaryOut.capabilityMask = capabilitiesResult.capabilityMask;
            summaryOut.statusFlags = capabilitiesResult.statusFlags != 0U ? capabilitiesResult.statusFlags : summaryOut.statusFlags;
        }

        if (fieldsResult.io.ok)
        {
            rowsOut.reserve(fieldsResult.entries.size());
            for (const ksword::ark::DynDataFieldEntry& sourceEntry : fieldsResult.entries)
            {
                KernelDynDataFieldEntry row{};
                row.fieldId = sourceEntry.fieldId;
                row.flags = sourceEntry.flags;
                row.source = sourceEntry.source;
                row.offset = sourceEntry.offset;
                row.capabilityMask = sourceEntry.capabilityMask;
                row.fieldNameText = stringToQString(sourceEntry.fieldName);
                row.sourceNameText = !sourceEntry.sourceName.empty()
                    ? stringToQString(sourceEntry.sourceName)
                    : sourceText(sourceEntry.source);
                row.featureNameText = stringToQString(sourceEntry.featureName);
                row.statusText = fieldPresent(row.flags, row.offset)
                    ? QStringLiteral("可用")
                    : (row.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U
                        ? QStringLiteral("缺失(必需)")
                        : QStringLiteral("缺失(可选)");
                row.detailText = buildFieldDetail(row, summaryOut);
                rowsOut.push_back(row);
            }
        }

        return summaryOut.statusQueryOk && summaryOut.fieldsQueryOk;
    }
}

void KernelDock::initializeDynDataTab()
{
    if (m_dynDataPage == nullptr || m_dynDataLayout != nullptr)
    {
        return;
    }

    // 顶层布局：工具栏 + 上下分割区域。上半部分摘要，下半部分字段表和详情。
    m_dynDataLayout = new QVBoxLayout(m_dynDataPage);
    m_dynDataLayout->setContentsMargins(4, 4, 4, 4);
    m_dynDataLayout->setSpacing(6);

    m_dynDataToolLayout = new QHBoxLayout();
    m_dynDataToolLayout->setContentsMargins(0, 0, 0, 0);
    m_dynDataToolLayout->setSpacing(6);

    m_refreshDynDataButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_dynDataPage);
    m_refreshDynDataButton->setToolTip(QStringLiteral("刷新 R0 DynData 状态和字段表"));
    m_refreshDynDataButton->setStyleSheet(blueButtonStyle());
    m_refreshDynDataButton->setFixedWidth(34);

    m_copyDynDataReportButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制诊断"), m_dynDataPage);
    m_copyDynDataReportButton->setToolTip(QStringLiteral("复制 DynData 状态、能力和字段列表到剪贴板"));
    m_copyDynDataReportButton->setStyleSheet(blueButtonStyle());

    m_dynDataFilterEdit = new QLineEdit(m_dynDataPage);
    m_dynDataFilterEdit->setPlaceholderText(QStringLiteral("按字段名/偏移/状态/来源/功能/capability 筛选"));
    m_dynDataFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤动态偏移字段表"));
    m_dynDataFilterEdit->setClearButtonEnabled(true);
    m_dynDataFilterEdit->setStyleSheet(blueInputStyle());

    m_dynDataStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_dynDataPage);
    m_dynDataStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_dynDataKernelBadge = new QLabel(m_dynDataPage);
    m_dynDataKernelBadge->setToolTip(QStringLiteral("Kernel/R0 数据来源标识"));
    m_dynDataKernelBadge->setPixmap(QPixmap(QStringLiteral(":/Image/kernel_badge.png")).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_dynDataKernelBadge->setFixedSize(24, 24);

    m_dynDataToolLayout->addWidget(m_refreshDynDataButton, 0);
    m_dynDataToolLayout->addWidget(m_copyDynDataReportButton, 0);
    m_dynDataToolLayout->addWidget(m_dynDataFilterEdit, 1);
    m_dynDataToolLayout->addWidget(m_dynDataKernelBadge, 0);
    m_dynDataToolLayout->addWidget(m_dynDataStatusLabel, 0);
    m_dynDataLayout->addLayout(m_dynDataToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_dynDataPage);
    m_dynDataLayout->addWidget(verticalSplitter, 1);

    m_dynDataSummaryTable = new QTableWidget(verticalSplitter);
    m_dynDataSummaryTable->setColumnCount(static_cast<int>(SummaryColumn::Count));
    m_dynDataSummaryTable->setHorizontalHeaderLabels(QStringList{ QStringLiteral("项目"), QStringLiteral("值") });
    m_dynDataSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_dynDataSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dynDataSummaryTable->setAlternatingRowColors(true);
    m_dynDataSummaryTable->setStyleSheet(itemSelectionStyle());
    m_dynDataSummaryTable->setCornerButtonEnabled(false);
    m_dynDataSummaryTable->verticalHeader()->setVisible(false);
    m_dynDataSummaryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_dynDataSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_dynDataSummaryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(SummaryColumn::Value), QHeaderView::Stretch);
    m_dynDataSummaryTable->setColumnWidth(static_cast<int>(SummaryColumn::Name), 220);
    m_dynDataSummaryTable->setToolTip(QStringLiteral("DynData 精确匹配、模块身份和 capability 摘要"));

    QSplitter* lowerSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    m_dynDataFieldTable = new QTableWidget(lowerSplitter);
    m_dynDataFieldTable->setColumnCount(static_cast<int>(DynDataColumn::Count));
    m_dynDataFieldTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("字段"),
        QStringLiteral("偏移"),
        QStringLiteral("状态"),
        QStringLiteral("来源"),
        QStringLiteral("功能"),
        QStringLiteral("Capability")
        });
    m_dynDataFieldTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_dynDataFieldTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_dynDataFieldTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dynDataFieldTable->setAlternatingRowColors(true);
    m_dynDataFieldTable->setStyleSheet(itemSelectionStyle());
    m_dynDataFieldTable->setCornerButtonEnabled(false);
    m_dynDataFieldTable->verticalHeader()->setVisible(false);
    m_dynDataFieldTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_dynDataFieldTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_dynDataFieldTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(DynDataColumn::Field), QHeaderView::Stretch);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Offset), 100);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Status), 110);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Source), 180);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Feature), 180);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Capability), 180);

    m_dynDataDetailEditor = new CodeEditorWidget(lowerSplitter);
    m_dynDataDetailEditor->setReadOnly(true);
    m_dynDataDetailEditor->setText(QStringLiteral("请选择一条动态偏移字段查看详情。"));

    verticalSplitter->setStretchFactor(0, 2);
    verticalSplitter->setStretchFactor(1, 5);
    lowerSplitter->setStretchFactor(0, 3);
    lowerSplitter->setStretchFactor(1, 2);

    // 信号连接：刷新、筛选、当前行详情和报告复制都在本页内部完成。
    connect(m_refreshDynDataButton, &QPushButton::clicked, this, [this]() {
        refreshDynDataAsync();
    });
    connect(m_copyDynDataReportButton, &QPushButton::clicked, this, [this]() {
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard != nullptr)
        {
            clipboard->setText(buildDynDataReport(m_dynDataSummary, m_dynDataRows));
            m_dynDataStatusLabel->setText(QStringLiteral("状态：诊断报告已复制"));
        }
    });
    connect(m_dynDataFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildDynDataFieldTable(filterText.trimmed());
    });
    connect(m_dynDataFieldTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showDynDataDetailByCurrentRow();
    });
}

void KernelDock::requestDynDataRefresh()
{
    if (!m_dynDataTabInitialized)
    {
        ensureTabInitialized(m_dynDataTabIndex);
        return;
    }

    refreshDynDataAsync();
}

void KernelDock::refreshDynDataAsync()
{
    if (m_dynDataRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] DynData 刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshDynDataButton->setEnabled(false);
    m_dynDataStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_dynDataStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        KernelDynDataSummary summary;
        std::vector<KernelDynDataFieldEntry> rows;
        const bool success = queryDynDataSnapshot(summary, rows);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, summary = std::move(summary), rows = std::move(rows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_dynDataRefreshRunning.store(false);
            guardThis->m_refreshDynDataButton->setEnabled(true);
            guardThis->m_dynDataSummary = std::move(summary);
            guardThis->m_dynDataRows = std::move(rows);

            populateSummaryTable(
                guardThis->m_dynDataSummaryTable,
                guardThis->m_dynDataSummary,
                guardThis->m_dynDataRows.size());
            guardThis->rebuildDynDataFieldTable(guardThis->m_dynDataFilterEdit->text().trimmed());

            const std::size_t missingRequiredCount = static_cast<std::size_t>(
                std::count_if(
                    guardThis->m_dynDataRows.begin(),
                    guardThis->m_dynDataRows.end(),
                    [](const KernelDynDataFieldEntry& entry) {
                        return (entry.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U &&
                            !fieldPresent(entry.flags, entry.offset);
                    }));

            if (!success)
            {
                guardThis->m_dynDataStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_dynDataStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_dynDataDetailEditor->setText(buildDynDataReport(guardThis->m_dynDataSummary, guardThis->m_dynDataRows));
            }
            else
            {
                const bool ntosActive = statusFlagEnabled(guardThis->m_dynDataSummary.statusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE);
                const bool pdbProfileActive = statusFlagEnabled(guardThis->m_dynDataSummary.statusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE);
                guardThis->m_dynDataStatusLabel->setText(
                    QStringLiteral("状态：%1%2，字段 %3 项，缺失必需 %4 项")
                    .arg(ntosActive ? QStringLiteral("ntos profile 已命中") : QStringLiteral("ntos profile 未命中"))
                    .arg(pdbProfileActive ? QStringLiteral("，PDB profile 已启用") : QString())
                    .arg(static_cast<qulonglong>(guardThis->m_dynDataRows.size()))
                    .arg(static_cast<qulonglong>(missingRequiredCount)));
                guardThis->m_dynDataStatusLabel->setStyleSheet(
                    statusLabelStyle(ntosActive && pdbProfileActive && missingRequiredCount == 0U ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));

                if (guardThis->m_dynDataFieldTable->rowCount() > 0)
                {
                    guardThis->m_dynDataFieldTable->setCurrentCell(0, 0);
                }
                else
                {
                    guardThis->m_dynDataDetailEditor->setText(QStringLiteral("当前筛选条件下没有动态偏移字段。"));
                }
            }

            kLogEvent doneEvent;
            info << doneEvent
                << "[KernelDock] DynData 刷新完成, success="
                << success
                << ", fields="
                << guardThis->m_dynDataRows.size()
                << ", caps="
                << formatHex64(guardThis->m_dynDataSummary.capabilityMask)
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildDynDataFieldTable(const QString& filterKeyword)
{
    if (m_dynDataFieldTable == nullptr)
    {
        return;
    }

    m_dynDataFieldTable->setSortingEnabled(false);
    m_dynDataFieldTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_dynDataRows.size(); ++sourceIndex)
    {
        const KernelDynDataFieldEntry& entry = m_dynDataRows[sourceIndex];
        if (!shouldShowField(entry, filterKeyword))
        {
            continue;
        }

        const int rowIndex = m_dynDataFieldTable->rowCount();
        m_dynDataFieldTable->insertRow(rowIndex);

        auto* fieldItem = new QTableWidgetItem(safeText(entry.fieldNameText));
        auto* offsetItem = new QTableWidgetItem(formatOffset(entry.offset));
        auto* statusItem = new QTableWidgetItem(safeText(entry.statusText));
        auto* sourceItem = new QTableWidgetItem(safeText(entry.sourceNameText));
        auto* featureItem = new QTableWidgetItem(safeText(entry.featureNameText));
        auto* capabilityItem = new QTableWidgetItem(formatHex64(entry.capabilityMask));

        fieldItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        if (!fieldPresent(entry.flags, entry.offset))
        {
            statusItem->setForeground(QBrush((entry.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U
                ? QColor(QStringLiteral("#B23A3A"))
                : KswordTheme::WarningAccentColor()));
        }
        else
        {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#3A8F3A"))));
        }

        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Field, fieldItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Offset, offsetItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Status, statusItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Source, sourceItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Feature, featureItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Capability, capabilityItem);
    }

    m_dynDataFieldTable->setSortingEnabled(true);
}

bool KernelDock::currentDynDataFieldSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;

    if (m_dynDataFieldTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_dynDataFieldTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* fieldItem = m_dynDataFieldTable->item(currentRow, static_cast<int>(DynDataColumn::Field));
    if (fieldItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(fieldItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_dynDataRows.size();
}

const KernelDynDataFieldEntry* KernelDock::currentDynDataFieldEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentDynDataFieldSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_dynDataRows[sourceIndex];
}

void KernelDock::showDynDataDetailByCurrentRow()
{
    if (m_dynDataDetailEditor == nullptr)
    {
        return;
    }

    const KernelDynDataFieldEntry* entry = currentDynDataFieldEntry();
    if (entry == nullptr)
    {
        m_dynDataDetailEditor->setText(buildDynDataReport(m_dynDataSummary, m_dynDataRows));
        return;
    }

    m_dynDataDetailEditor->setText(QStringLiteral(
        "%1\n\n"
        "模块身份:\n"
        "%2\n\n"
        "%3\n\n"
        "Capability 状态:\n"
        "%4")
        .arg(entry->detailText)
        .arg(moduleDetailText(QStringLiteral("ntoskrnl"), m_dynDataSummary.ntoskrnl))
        .arg(moduleDetailText(QStringLiteral("lxcore"), m_dynDataSummary.lxcore))
        .arg(capabilityReport(m_dynDataSummary.capabilityMask)));
}
