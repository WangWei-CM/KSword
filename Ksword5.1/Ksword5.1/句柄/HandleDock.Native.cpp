#include "HandleDock.h"

#include "HandleObjectTypeWorker.h"
#include "../ksword/file/file_handle_tools.h"

#include <QChar>
#include <QStringList>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

HandleDock::HandleRefreshResult HandleDock::buildHandleRefreshResult(const HandleRefreshOptions& options)
{
    HandleRefreshResult result{};

    // Translate the UI-facing refresh options into the shared ks::file backend contract.
    // Keyword, only-named, and diff filters remain local UI filters in HandleDock.Filter.cpp.
    ks::file::HandleSnapshotOptions backendOptions{};
    backendOptions.hasPidFilter = options.hasPidFilter;
    backendOptions.pidFilter = options.pidFilter;
    backendOptions.typeFilterText = options.typeFilterText.toStdWString();
    backendOptions.resolveObjectName = options.resolveObjectName;
    backendOptions.nameResolveBudget = options.nameResolveBudget;
    backendOptions.typeNameCacheByIndex = options.typeNameCacheByIndex;
    backendOptions.typeNameMapFromObjectTab = options.typeNameMapFromObjectTab;

    auto toBackendMode = [](const HandleEnumMode mode) -> ks::file::HandleEnumMode
    {
        switch (mode)
        {
        case HandleEnumMode::UserSnapshot: return ks::file::HandleEnumMode::UserSnapshot;
        case HandleEnumMode::DuplicateHandle: return ks::file::HandleEnumMode::DuplicateHandle;
        case HandleEnumMode::KernelHandleTable: return ks::file::HandleEnumMode::KernelHandleTable;
        default: return ks::file::HandleEnumMode::DuplicateHandle;
        }
    };
    auto fromBackendMode = [](const ks::file::HandleEnumMode mode) -> HandleEnumMode
    {
        switch (mode)
        {
        case ks::file::HandleEnumMode::UserSnapshot: return HandleEnumMode::UserSnapshot;
        case ks::file::HandleEnumMode::DuplicateHandle: return HandleEnumMode::DuplicateHandle;
        case ks::file::HandleEnumMode::KernelHandleTable: return HandleEnumMode::KernelHandleTable;
        default: return HandleEnumMode::DuplicateHandle;
        }
    };
    auto fromBackendDiff = [](const ks::file::HandleDiffStatus status) -> HandleDiffStatus
    {
        switch (status)
        {
        case ks::file::HandleDiffStatus::NotCompared: return HandleDiffStatus::NotCompared;
        case ks::file::HandleDiffStatus::UserOnly: return HandleDiffStatus::UserOnly;
        case ks::file::HandleDiffStatus::KernelOnly: return HandleDiffStatus::KernelOnly;
        case ks::file::HandleDiffStatus::Both: return HandleDiffStatus::Both;
        default: return HandleDiffStatus::NotCompared;
        }
    };
    backendOptions.enumMode = toBackendMode(options.enumMode);

    const ks::file::HandleSnapshotResult backendResult = ks::file::BuildHandleSnapshot(backendOptions);

    // Copy aggregate statistics verbatim so existing status-bar and log text remain stable.
    result.totalHandleCount = backendResult.totalHandleCount;
    result.visibleHandleCount = backendResult.visibleHandleCount;
    result.basicInfoResolvedCount = backendResult.basicInfoResolvedCount;
    result.resolvedNameCount = backendResult.resolvedNameCount;
    result.fallbackNameCount = backendResult.fallbackNameCount;
    result.objectTypeMappedCount = backendResult.objectTypeMappedCount;
    result.kernelHandleCount = backendResult.kernelHandleCount;
    result.userOnlyCount = backendResult.userOnlyCount;
    result.kernelOnlyCount = backendResult.kernelOnlyCount;
    result.bothCount = backendResult.bothCount;
    result.elapsedMs = backendResult.elapsedMs;
    result.diagnosticText = QString::fromStdWString(backendResult.diagnosticText);
    result.updatedTypeNameCacheByIndex = backendResult.updatedTypeNameCacheByIndex;

    result.availableTypeList.reserve(backendResult.availableTypeList.size());
    for (const std::wstring& typeNameText : backendResult.availableTypeList)
    {
        result.availableTypeList.push_back(QString::fromStdWString(typeNameText));
    }

    result.rows.reserve(backendResult.rows.size());
    for (const ks::file::HandleSnapshotRow& backendRow : backendResult.rows)
    {
        HandleRow row{};
        row.processId = backendRow.processId;
        row.processName = QString::fromStdWString(backendRow.processName);
        row.handleValue = backendRow.handleValue;
        row.typeIndex = backendRow.typeIndex;
        row.typeName = QString::fromStdWString(backendRow.typeName);
        row.objectName = QString::fromStdWString(backendRow.objectName);
        row.objectAddress = backendRow.objectAddress;
        row.grantedAccess = backendRow.grantedAccess;
        row.attributes = backendRow.attributes;
        row.handleCount = backendRow.handleCount;
        row.pointerCount = backendRow.pointerCount;
        row.basicInfoAvailable = backendRow.basicInfoAvailable;
        row.objectNameAvailable = backendRow.objectNameAvailable;
        row.objectNameFailed = backendRow.objectNameFailed;
        row.objectNameFromFallback = backendRow.objectNameFromFallback;
        row.sourceMode = fromBackendMode(backendRow.sourceMode);
        row.diffStatus = fromBackendDiff(backendRow.diffStatus);
        row.decodeStatus = backendRow.decodeStatus;
        row.r0FieldFlags = backendRow.r0FieldFlags;
        row.r0DynDataCapabilityMask = backendRow.r0DynDataCapabilityMask;
        row.epObjectTableOffset = backendRow.epObjectTableOffset;
        row.htHandleContentionEventOffset = backendRow.htHandleContentionEventOffset;
        row.obDecodeShift = backendRow.obDecodeShift;
        row.obAttributesShift = backendRow.obAttributesShift;
        row.otNameOffset = backendRow.otNameOffset;
        row.otIndexOffset = backendRow.otIndexOffset;
        result.rows.push_back(std::move(row));
    }

    return result;
}

HandleDock::ObjectTypeRefreshResult HandleDock::buildObjectTypeRefreshResult()
{
    ObjectTypeRefreshResult result{};
    const auto beginTime = std::chrono::steady_clock::now();

    QString errorText;
    std::vector<HandleObjectTypeEntry> rows;
    runHandleObjectTypeSnapshotTask(rows, errorText);
    result.rows = std::move(rows);
    result.typeNameMapByIndex = buildTypeNameMapFromObjectTypeRows(result.rows);
    result.diagnosticText = errorText;
    result.elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
    return result;
}

bool HandleDock::closeRemoteHandle(
    const std::uint32_t processId,
    const std::uint64_t handleValue,
    std::string& detailTextOut)
{
    // Remote handle closing is a reusable backend operation; the UI only forwards the
    // selected PID/handle pair and displays the returned detail text.
    return ks::file::CloseRemoteHandle(processId, handleValue, detailTextOut);
}

QString HandleDock::formatHex(const std::uint64_t value, const int width)
{
    if (width > 0)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), width, 16, QChar('0'))
            .toUpper();
    }
    return QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(value), 0, 16)
        .toUpper();
}

QString HandleDock::formatOptionalObjectCount(
    const std::uint32_t countValue,
    const bool countAvailable)
{
    if (!countAvailable)
    {
        return QStringLiteral("未查到");
    }
    return QString::number(countValue);
}

QString HandleDock::formatObjectNameDisplayText(const HandleRow& row)
{
    if (row.objectNameAvailable)
    {
        if (row.objectName.trimmed().isEmpty())
        {
            return QStringLiteral("无名称");
        }
        return row.objectName;
    }

    if (row.objectNameFailed)
    {
        return QStringLiteral("未查到");
    }

    return QStringLiteral("未查询");
}

QString HandleDock::formatHandleSourceText(const HandleEnumMode mode)
{
    switch (mode)
    {
    case HandleEnumMode::UserSnapshot:
        return QStringLiteral("User Snapshot");
    case HandleEnumMode::DuplicateHandle:
        return QStringLiteral("DuplicateHandle");
    case HandleEnumMode::KernelHandleTable:
        return QStringLiteral("Kernel HandleTable");
    default:
        return QStringLiteral("Unknown");
    }
}

QString HandleDock::formatHandleDecodeStatusText(const std::uint32_t status)
{
    switch (status)
    {
    case KSWORD_ARK_HANDLE_DECODE_STATUS_OK:
        return QStringLiteral("OK");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_PARTIAL:
        return QStringLiteral("Partial");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_DYNDATA_MISSING:
        return QStringLiteral("DynData Missing");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_LOOKUP_FAILED:
        return QStringLiteral("Process Lookup Failed");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_EXITING:
        return QStringLiteral("Process Exiting");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_HANDLE_TABLE_MISSING:
        return QStringLiteral("HandleTable Missing");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_OBJECT_DECODE_FAILED:
        return QStringLiteral("Object Decode Failed");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_TYPE_DECODE_FAILED:
        return QStringLiteral("Type Decode Failed");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_READ_FAILED:
        return QStringLiteral("Read Failed");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_BUFFER_TOO_SMALL:
        return QStringLiteral("Buffer Too Small");
    case KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE:
    default:
        return QStringLiteral("Unavailable");
    }
}

QString HandleDock::formatHandleDiffStatusText(const HandleDiffStatus status)
{
    switch (status)
    {
    case HandleDiffStatus::UserOnly:
        return QStringLiteral("仅用户态可见");
    case HandleDiffStatus::KernelOnly:
        return QStringLiteral("仅内核可见");
    case HandleDiffStatus::Both:
        return QStringLiteral("两者均可见");
    case HandleDiffStatus::NotCompared:
    default:
        return QStringLiteral("未对比");
    }
}

QString HandleDock::formatTypeIndexDisplayText(
    const std::uint16_t typeIndex,
    const QString& typeName)
{
    const QString trimmedTypeName = typeName.trimmed();
    const QString fallbackTypeText = QStringLiteral("Type#%1").arg(typeIndex);
    if (trimmedTypeName.isEmpty() ||
        trimmedTypeName.compare(fallbackTypeText, Qt::CaseInsensitive) == 0 ||
        trimmedTypeName.startsWith(QStringLiteral("<UnknownType_"), Qt::CaseInsensitive))
    {
        return QString::number(typeIndex);
    }

    return QStringLiteral("%1 (%2)")
        .arg(trimmedTypeName)
        .arg(typeIndex);
}

QString HandleDock::formatHandleAttributes(const std::uint32_t attributes)
{
    QStringList flagTextList;
    if ((attributes & 0x00000001U) != 0)
    {
        flagTextList.push_back(QStringLiteral("PROTECT"));
    }
    if ((attributes & 0x00000002U) != 0)
    {
        flagTextList.push_back(QStringLiteral("INHERIT"));
    }
    if ((attributes & 0x00000004U) != 0)
    {
        flagTextList.push_back(QStringLiteral("AUDIT"));
    }
    if (flagTextList.isEmpty())
    {
        return QStringLiteral("None");
    }
    return flagTextList.join('|');
}

HandleDock::HandleEnumMode HandleDock::resolveHandleEnumModeFromText(const QString& modeText)
{
    const QString normalizedText = modeText.trimmed().toLower();
    if (normalizedText.contains(QStringLiteral("kernel")))
    {
        return HandleEnumMode::KernelHandleTable;
    }
    if (normalizedText.contains(QStringLiteral("duplicate")))
    {
        return HandleEnumMode::DuplicateHandle;
    }
    return HandleEnumMode::UserSnapshot;
}

HandleDock::HandleDiffStatus HandleDock::resolveHandleDiffFilterFromText(const QString& filterText)
{
    const QString normalizedText = filterText.trimmed().toLower();
    if (normalizedText.contains(QStringLiteral("仅用户")) || normalizedText.contains(QStringLiteral("user")))
    {
        return HandleDiffStatus::UserOnly;
    }
    if (normalizedText.contains(QStringLiteral("仅内核")) || normalizedText.contains(QStringLiteral("kernel")))
    {
        return HandleDiffStatus::KernelOnly;
    }
    if (normalizedText.contains(QStringLiteral("两者")) || normalizedText.contains(QStringLiteral("both")))
    {
        return HandleDiffStatus::Both;
    }
    return HandleDiffStatus::NotCompared;
}
