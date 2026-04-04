#include "HandleObjectTypeWorker.h"

// ============================================================
// HandleObjectTypeWorker.cpp
// 作用：
// - 实现 NtQueryObject(ObjectTypesInformation) 的对象类型解析；
// - 输出稳定的 typeIndex/typeName 映射，供句柄模块复用；
// - 兼容不同系统结构差异（对齐、TypeIndex 缺失、名称指针越界）。
// ============================================================

#include <QChar>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace
{
    // NTSTATUS 常量：统一本地定义，避免环境宏缺失。
    constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);
    constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
    constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);

    // NtQueryObject 信息类常量：避免魔法数字散落。
    constexpr ULONG kObjectTypesInformationClass = 3;

    // NtQueryObject 函数签名。
    using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    // NtApi：缓存 ntdll 句柄与 NtQueryObject 地址。
    struct NtApi final
    {
        HMODULE ntdllModule = nullptr;      // ntdllModule：ntdll 模块句柄。
        NtQueryObjectFn queryObject = nullptr; // queryObject：NtQueryObject 地址。
    };

    // RawObjectTypesHeader：ObjectTypesInformation 返回头。
    struct RawObjectTypesHeader
    {
        ULONG numberOfTypes = 0; // numberOfTypes：类型记录总数。
    };

    // RawObjectTypeInformation：对象类型原始结构体（本模块所需字段子集）。
    struct RawObjectTypeInformation
    {
        UNICODE_STRING typeName{};
        ULONG totalNumberOfObjects = 0;
        ULONG totalNumberOfHandles = 0;
        ULONG totalPagedPoolUsage = 0;
        ULONG totalNonPagedPoolUsage = 0;
        ULONG totalNamePoolUsage = 0;
        ULONG totalHandleTableUsage = 0;
        ULONG highWaterNumberOfObjects = 0;
        ULONG highWaterNumberOfHandles = 0;
        ULONG highWaterPagedPoolUsage = 0;
        ULONG highWaterNonPagedPoolUsage = 0;
        ULONG highWaterNamePoolUsage = 0;
        ULONG highWaterHandleTableUsage = 0;
        ULONG invalidAttributes = 0;
        GENERIC_MAPPING genericMapping{};
        ULONG validAccessMask = 0;
        BOOLEAN securityRequired = FALSE;
        BOOLEAN maintainHandleCount = FALSE;
        UCHAR typeIndex = 0;
        CHAR reservedByte = 0;
        ULONG poolType = 0;
        ULONG defaultPagedPoolCharge = 0;
        ULONG defaultNonPagedPoolCharge = 0;
    };

    // loadNtApi：
    // - 作用：加载 ntdll 并解析 NtQueryObject 地址；
    // - 失败时返回 false，并写入错误文本。
    bool loadNtApi(NtApi& apiOut, QString& errorTextOut)
    {
        errorTextOut.clear();
        apiOut.ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (apiOut.ntdllModule == nullptr)
        {
            apiOut.ntdllModule = ::LoadLibraryW(L"ntdll.dll");
        }
        if (apiOut.ntdllModule == nullptr)
        {
            errorTextOut = QStringLiteral("加载 ntdll.dll 失败。");
            return false;
        }

        apiOut.queryObject = reinterpret_cast<NtQueryObjectFn>(
            ::GetProcAddress(apiOut.ntdllModule, "NtQueryObject"));
        if (apiOut.queryObject == nullptr)
        {
            errorTextOut = QStringLiteral("解析 NtQueryObject 入口失败。");
            return false;
        }
        return true;
    }

    // isNeedGrowBufferStatus：
    // - 作用：判断 NTSTATUS 是否表示“缓冲区不足，需要扩容”。
    bool isNeedGrowBufferStatus(const NTSTATUS statusCode)
    {
        return statusCode == kStatusInfoLengthMismatch
            || statusCode == kStatusBufferTooSmall
            || statusCode == kStatusBufferOverflow;
    }

    // ntStatusToText：
    // - 作用：把 NTSTATUS 转换成“十六进制 + 文本”。
    QString ntStatusToText(const HMODULE ntdllModule, const NTSTATUS statusCode)
    {
        const QString hexText = QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(statusCode), 8, 16, QChar('0'))
            .toUpper();
        const QString successText = NT_SUCCESS(statusCode)
            ? QStringLiteral("SUCCESS")
            : QStringLiteral("FAILED");

        wchar_t messageBuffer[256] = {};
        const DWORD textLength = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            ntdllModule,
            static_cast<DWORD>(statusCode),
            0,
            messageBuffer,
            static_cast<DWORD>(std::size(messageBuffer)),
            nullptr);
        if (textLength == 0)
        {
            return QStringLiteral("%1 (%2)").arg(hexText, successText);
        }
        return QStringLiteral("%1 (%2) %3")
            .arg(hexText, successText, QString::fromWCharArray(messageBuffer).trimmed());
    }

    // alignUp：
    // - 作用：把偏移量按指定对齐值向上对齐。
    std::size_t alignUp(const std::size_t value, const std::size_t alignment)
    {
        if (alignment == 0)
        {
            return value;
        }
        const std::size_t mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    // isReadableMemoryRange：
    // - 作用：探测一段内存是否可读；
    // - 用于防止直接解引用异常指针导致崩溃。
    bool isReadableMemoryRange(const void* beginAddress, const std::size_t length)
    {
        if (beginAddress == nullptr || length == 0)
        {
            return false;
        }

        std::uintptr_t currentAddress = reinterpret_cast<std::uintptr_t>(beginAddress);
        const std::uintptr_t endAddress = currentAddress + length;
        if (endAddress < currentAddress)
        {
            return false;
        }

        while (currentAddress < endAddress)
        {
            MEMORY_BASIC_INFORMATION memoryInfo{};
            const SIZE_T querySize = ::VirtualQuery(
                reinterpret_cast<LPCVOID>(currentAddress),
                &memoryInfo,
                sizeof(memoryInfo));
            if (querySize != sizeof(memoryInfo))
            {
                return false;
            }
            if (memoryInfo.State != MEM_COMMIT)
            {
                return false;
            }
            const DWORD protect = (memoryInfo.Protect & 0xFFU);
            if (protect == PAGE_NOACCESS || protect == PAGE_GUARD)
            {
                return false;
            }

            const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
            const std::uintptr_t regionEnd = regionBegin + memoryInfo.RegionSize;
            if (regionEnd <= currentAddress)
            {
                return false;
            }
            currentAddress = std::min(regionEnd, endAddress);
        }
        return true;
    }

    // queryAutoBuffer：
    // - 作用：自动扩容并重试 Nt 查询，直到成功或失败；
    // - 返回 true 时 outputBuffer 内保存有效响应。
    bool queryAutoBuffer(
        const std::function<NTSTATUS(void*, ULONG, ULONG*)>& queryFunction,
        std::vector<std::uint8_t>& outputBuffer,
        NTSTATUS& statusCodeOut,
        const ULONG initialSize = 4096U)
    {
        statusCodeOut = kStatusUnsuccessful;
        ULONG bufferSize = std::max<ULONG>(initialSize, 256U);

        for (int retryIndex = 0; retryIndex < 8; ++retryIndex)
        {
            outputBuffer.assign(bufferSize, 0);
            ULONG returnLength = 0;
            const NTSTATUS statusCode = queryFunction(outputBuffer.data(), bufferSize, &returnLength);
            statusCodeOut = statusCode;

            if (NT_SUCCESS(statusCode))
            {
                if (returnLength > 0 && returnLength <= outputBuffer.size())
                {
                    outputBuffer.resize(returnLength);
                }
                return true;
            }
            if (!isNeedGrowBufferStatus(statusCode))
            {
                return false;
            }
            bufferSize = std::max(bufferSize * 2U, returnLength + 512U);
        }
        return false;
    }
}

bool runHandleObjectTypeSnapshotTask(
    std::vector<HandleObjectTypeEntry>& rowsOut,
    QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    // 采集开始日志：用于审计对象类型刷新链路。
    kLogEvent beginEvent;
    info << beginEvent << "[HandleObjectTypeWorker] 开始采集对象类型快照。" << eol;

    NtApi api{};
    if (!loadNtApi(api, errorTextOut))
    {
        kLogEvent loadFailEvent;
        err << loadFailEvent
            << "[HandleObjectTypeWorker] 加载 NtApi 失败: "
            << errorTextOut.toStdString()
            << eol;
        return false;
    }

    std::vector<std::uint8_t> buffer;
    NTSTATUS statusCode = kStatusUnsuccessful;
    const bool queryOk = queryAutoBuffer(
        [&api](void* outBuffer, ULONG outLength, ULONG* returnLengthOut) -> NTSTATUS
        {
            return api.queryObject(
                nullptr,
                kObjectTypesInformationClass,
                outBuffer,
                outLength,
                returnLengthOut);
        },
        buffer,
        statusCode,
        64U * 1024U);
    if (!queryOk || !NT_SUCCESS(statusCode))
    {
        errorTextOut = QStringLiteral("NtQueryObject(ObjectTypesInformation) 失败: %1")
            .arg(ntStatusToText(api.ntdllModule, statusCode));
        kLogEvent queryFailEvent;
        err << queryFailEvent
            << "[HandleObjectTypeWorker] 查询对象类型失败: "
            << errorTextOut.toStdString()
            << eol;
        return false;
    }
    if (buffer.size() < sizeof(RawObjectTypesHeader))
    {
        errorTextOut = QStringLiteral("对象类型返回缓冲区长度不足。");
        return false;
    }

    std::vector<HandleObjectTypeEntry> snapshotRows;
    const auto* header = reinterpret_cast<const RawObjectTypesHeader*>(buffer.data());
    snapshotRows.reserve(header->numberOfTypes);

    // 结构遍历策略：头部后按指针对齐，逐条读取并推进。
    const std::uintptr_t bufferBase = reinterpret_cast<std::uintptr_t>(buffer.data());
    const std::uintptr_t bufferEnd = bufferBase + buffer.size();
    std::uintptr_t entryAddress = alignUp(
        bufferBase + sizeof(RawObjectTypesHeader),
        sizeof(void*));

    // usedTypeIndex：处理 typeIndex 缺失或重复时的编号兜底。
    std::unordered_set<std::uint32_t> usedTypeIndex;
    usedTypeIndex.reserve(header->numberOfTypes);

    for (ULONG index = 0; index < header->numberOfTypes; ++index)
    {
        if (entryAddress + sizeof(RawObjectTypeInformation) > bufferEnd)
        {
            break;
        }

        const auto* rawInfo = reinterpret_cast<const RawObjectTypeInformation*>(entryAddress);
        QString typeNameText;

        // 路径A：优先读取 UNICODE_STRING 的 Buffer 指针。
        if (rawInfo->typeName.Buffer != nullptr && rawInfo->typeName.Length > 0)
        {
            const std::size_t byteLength = static_cast<std::size_t>(rawInfo->typeName.Length);
            const std::uintptr_t ptrAddress = reinterpret_cast<std::uintptr_t>(rawInfo->typeName.Buffer);
            const bool inReplyBuffer =
                ptrAddress >= bufferBase &&
                ptrAddress <= bufferEnd &&
                byteLength <= (bufferEnd - ptrAddress);
            const bool externalReadable =
                !inReplyBuffer &&
                isReadableMemoryRange(rawInfo->typeName.Buffer, byteLength);
            if (inReplyBuffer || externalReadable)
            {
                typeNameText = QString::fromWCharArray(
                    rawInfo->typeName.Buffer,
                    rawInfo->typeName.Length / static_cast<USHORT>(sizeof(wchar_t)));
            }
        }

        // 路径B：兜底读取结构体后紧邻字符串（兼容旧系统布局）。
        if (typeNameText.trimmed().isEmpty() && rawInfo->typeName.Length > 0)
        {
            const std::uintptr_t inlineNameAddress = entryAddress + sizeof(RawObjectTypeInformation);
            if (inlineNameAddress + rawInfo->typeName.Length <= bufferEnd)
            {
                typeNameText = QString::fromWCharArray(
                    reinterpret_cast<const wchar_t*>(inlineNameAddress),
                    rawInfo->typeName.Length / static_cast<USHORT>(sizeof(wchar_t)));
            }
        }
        if (typeNameText.trimmed().isEmpty())
        {
            typeNameText = QStringLiteral("<UnknownType_%1>").arg(index);
        }

        // typeIndex 兜底：缺失或重复时按序号补齐，保证映射稳定。
        std::uint32_t resolvedTypeIndex = static_cast<std::uint32_t>(rawInfo->typeIndex);
        if (resolvedTypeIndex == 0 || usedTypeIndex.find(resolvedTypeIndex) != usedTypeIndex.end())
        {
            resolvedTypeIndex = static_cast<std::uint32_t>(index + 1);
            while (usedTypeIndex.find(resolvedTypeIndex) != usedTypeIndex.end())
            {
                ++resolvedTypeIndex;
            }
        }
        usedTypeIndex.insert(resolvedTypeIndex);

        HandleObjectTypeEntry row{};
        row.typeIndex = resolvedTypeIndex;
        row.typeNameText = typeNameText;
        row.totalObjectCount = rawInfo->totalNumberOfObjects;
        row.totalHandleCount = rawInfo->totalNumberOfHandles;
        row.validAccessMask = rawInfo->validAccessMask;
        row.securityRequired = rawInfo->securityRequired != FALSE;
        row.maintainHandleCount = rawInfo->maintainHandleCount != FALSE;
        row.poolType = rawInfo->poolType;
        row.defaultPagedPoolCharge = rawInfo->defaultPagedPoolCharge;
        row.defaultNonPagedPoolCharge = rawInfo->defaultNonPagedPoolCharge;
        snapshotRows.push_back(std::move(row));

        // 前进到下一个条目：结构体 + MaximumLength，再按指针对齐。
        const std::size_t entrySpan = alignUp(
            sizeof(RawObjectTypeInformation) + static_cast<std::size_t>(rawInfo->typeName.MaximumLength),
            sizeof(void*));
        if (entrySpan < sizeof(RawObjectTypeInformation))
        {
            break;
        }
        entryAddress += entrySpan;
        if (entryAddress >= bufferEnd)
        {
            break;
        }
    }

    std::sort(
        snapshotRows.begin(),
        snapshotRows.end(),
        [](const HandleObjectTypeEntry& leftRow, const HandleObjectTypeEntry& rightRow)
        {
            if (leftRow.typeIndex == rightRow.typeIndex)
            {
                return QString::compare(leftRow.typeNameText, rightRow.typeNameText, Qt::CaseInsensitive) < 0;
            }
            return leftRow.typeIndex < rightRow.typeIndex;
        });

    rowsOut = std::move(snapshotRows);
    kLogEvent finishEvent;
    info << finishEvent
        << "[HandleObjectTypeWorker] 对象类型采集完成，条目数="
        << rowsOut.size()
        << eol;
    return true;
}

std::unordered_map<std::uint16_t, std::string> buildTypeNameMapFromObjectTypeRows(
    const std::vector<HandleObjectTypeEntry>& rows)
{
    std::unordered_map<std::uint16_t, std::string> resultMap;
    resultMap.reserve(rows.size());
    for (const HandleObjectTypeEntry& row : rows)
    {
        if (row.typeIndex > static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()))
        {
            continue;
        }
        resultMap[static_cast<std::uint16_t>(row.typeIndex)] = row.typeNameText.toStdString();
    }
    return resultMap;
}
