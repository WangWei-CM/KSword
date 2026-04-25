#include "KernelDockSsdtWorker.h"

#include "../../../shared/KswordArkLogProtocol.h"
#include "../../../shared/driver/KswordArkKernelIoctl.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QStringList>

namespace
{
    constexpr std::size_t kSsdtResponseHeaderSize =
        sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY);

    QString fixedAnsiToQString(const char* textBuffer, const std::size_t maxBytes)
    {
        if (textBuffer == nullptr || maxBytes == 0U)
        {
            return QString();
        }

        std::size_t length = 0U;
        while (length < maxBytes && textBuffer[length] != '\0')
        {
            ++length;
        }
        return QString::fromLocal8Bit(textBuffer, static_cast<int>(length));
    }

    QString formatAddressHex(const std::uint64_t addressValue)
    {
        return QStringLiteral("0x%1")
            .arg(addressValue, 16, 16, QChar('0'))
            .toUpper();
    }
}

bool runSsdtSnapshotTask(std::vector<KernelSsdtEntry>& rowsOut, QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    const HANDLE driverHandle = ::CreateFileW(
        KSWORD_ARK_LOG_WIN32_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (driverHandle == INVALID_HANDLE_VALUE)
    {
        errorTextOut = QStringLiteral("连接驱动失败，error=%1").arg(::GetLastError());
        return false;
    }

    KSWORD_ARK_ENUM_SSDT_REQUEST request{};
    request.flags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
    request.reserved = 0U;

    std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
    DWORD bytesReturned = 0U;
    const BOOL ioctlOk = ::DeviceIoControl(
        driverHandle,
        IOCTL_KSWORD_ARK_ENUM_SSDT,
        &request,
        static_cast<DWORD>(sizeof(request)),
        responseBuffer.data(),
        static_cast<DWORD>(responseBuffer.size()),
        &bytesReturned,
        nullptr);
    const DWORD ioctlError = ioctlOk ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(driverHandle);

    if (ioctlOk == FALSE)
    {
        errorTextOut = QStringLiteral("查询 SSDT 失败，error=%1").arg(ioctlError);
        return false;
    }

    if (bytesReturned < kSsdtResponseHeaderSize)
    {
        errorTextOut = QStringLiteral("SSDT 响应过短，bytesReturned=%1").arg(bytesReturned);
        return false;
    }

    const auto* responseHeader =
        reinterpret_cast<const KSWORD_ARK_ENUM_SSDT_RESPONSE*>(responseBuffer.data());
    if (responseHeader->entrySize < sizeof(KSWORD_ARK_SSDT_ENTRY))
    {
        errorTextOut = QStringLiteral("SSDT entrySize 非法，entrySize=%1").arg(responseHeader->entrySize);
        return false;
    }

    const std::size_t availableEntryCountByBytes =
        (static_cast<std::size_t>(bytesReturned) - kSsdtResponseHeaderSize) /
        static_cast<std::size_t>(responseHeader->entrySize);
    const std::size_t returnedEntryCount = std::min<std::size_t>(
        static_cast<std::size_t>(responseHeader->returnedCount),
        availableEntryCountByBytes);

    rowsOut.reserve(returnedEntryCount);
    for (std::size_t entryIndex = 0U; entryIndex < returnedEntryCount; ++entryIndex)
    {
        const std::size_t entryOffset =
            kSsdtResponseHeaderSize + (entryIndex * static_cast<std::size_t>(responseHeader->entrySize));
        if (entryOffset + sizeof(KSWORD_ARK_SSDT_ENTRY) > responseBuffer.size())
        {
            break;
        }

        const auto* sourceEntry =
            reinterpret_cast<const KSWORD_ARK_SSDT_ENTRY*>(responseBuffer.data() + entryOffset);

        KernelSsdtEntry row{};
        row.serviceIndex = static_cast<std::uint32_t>(sourceEntry->serviceIndex);
        row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
        row.zwRoutineAddress = static_cast<std::uint64_t>(sourceEntry->zwRoutineAddress);
        row.serviceRoutineAddress = static_cast<std::uint64_t>(sourceEntry->serviceRoutineAddress);
        row.serviceTableBase = static_cast<std::uint64_t>(responseHeader->serviceTableBase);
        row.serviceNameText = fixedAnsiToQString(sourceEntry->serviceName, sizeof(sourceEntry->serviceName));
        row.moduleNameText = fixedAnsiToQString(sourceEntry->moduleName, sizeof(sourceEntry->moduleName));
        row.indexResolved = (row.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_INDEX_RESOLVED) != 0U;
        const bool tableAddressValid = (row.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_TABLE_ADDRESS_VALID) != 0U;
        row.querySucceeded = true;

        QStringList statusParts;
        statusParts.push_back(row.indexResolved ? QStringLiteral("索引已解析") : QStringLiteral("索引未解析"));
        statusParts.push_back(tableAddressValid ? QStringLiteral("表项地址已解析") : QStringLiteral("表项地址不可用"));
        row.statusText = statusParts.join(QStringLiteral(" | "));

        row.detailText = QStringLiteral(
            "协议版本: %1\n"
            "总条目: %2\n"
            "返回条目: %3\n"
            "服务名称: %4\n"
            "模块名称: %5\n"
            "服务索引: %6\n"
            "Zw导出地址: %7\n"
            "服务表基址: %8\n"
            "表项服务地址: %9\n"
            "驱动标志: 0x%10")
            .arg(responseHeader->version)
            .arg(responseHeader->totalCount)
            .arg(responseHeader->returnedCount)
            .arg(row.serviceNameText.isEmpty() ? QStringLiteral("<空>") : row.serviceNameText)
            .arg(row.moduleNameText.isEmpty() ? QStringLiteral("<空>") : row.moduleNameText)
            .arg(row.indexResolved ? QString::number(row.serviceIndex) : QStringLiteral("<未知>"))
            .arg(formatAddressHex(row.zwRoutineAddress))
            .arg(formatAddressHex(row.serviceTableBase))
            .arg(formatAddressHex(row.serviceRoutineAddress))
            .arg(static_cast<unsigned int>(row.flags), 8, 16, QChar('0'));

        rowsOut.push_back(std::move(row));
    }

    std::sort(rowsOut.begin(), rowsOut.end(), [](const KernelSsdtEntry& left, const KernelSsdtEntry& right) {
        if (left.indexResolved != right.indexResolved)
        {
            return left.indexResolved && !right.indexResolved;
        }
        if (left.serviceIndex != right.serviceIndex)
        {
            return left.serviceIndex < right.serviceIndex;
        }
        return QString::compare(left.serviceNameText, right.serviceNameText, Qt::CaseInsensitive) < 0;
    });

    return true;
}
