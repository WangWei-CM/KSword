#include "KernelDockSsdtWorker.h"

#include "../ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <vector>
#include <utility>

#include <QStringList>

namespace
{
    QString formatAddressHex(const std::uint64_t addressValue)
    {
        return QStringLiteral("0x%1")
            .arg(addressValue, 16, 16, QChar('0'))
            .toUpper();
    }

    // ssdtWorkerIoMessageText：
    // - 输入：ArkDriverClient::enumerateSsdt 返回的原始 message；
    // - 处理：将 DeviceIoControl/unsupported/DynData/buffer 等底层日志转换成人读说明；
    // - 返回：用于 SSDT 页错误提示的中文短文本。
    QString ssdtWorkerIoMessageText(const QString& rawMessageText)
    {
        const QString trimmedText = rawMessageText.trimmed();
        if (trimmedText.isEmpty())
        {
            return QStringLiteral("驱动未返回额外说明。");
        }

        const QString lowerText = trimmedText.toLower();
        if (lowerText.contains(QStringLiteral("deviceiocontrol")))
        {
            return QStringLiteral("驱动 IOCTL 调用失败或当前驱动版本不匹配。");
        }
        if (lowerText.contains(QStringLiteral("unsupported")) ||
            lowerText.contains(QStringLiteral("not supported")) ||
            lowerText.contains(QStringLiteral("status=0xc00000bb")))
        {
            return QStringLiteral("当前驱动暂不支持 SSDT 枚举接口。");
        }
        if (lowerText.contains(QStringLiteral("dyndata")) ||
            lowerText.contains(QStringLiteral("capability")))
        {
            return QStringLiteral("动态偏移能力未满足，SSDT 表地址或解析字段暂不可用。");
        }
        if (lowerText.contains(QStringLiteral("buffer")) &&
            (lowerText.contains(QStringLiteral("small")) || lowerText.contains(QStringLiteral("trunc"))))
        {
            return QStringLiteral("驱动返回缓冲区不足，SSDT 结果可能被截断。");
        }
        return trimmedText;
    }
}

bool runSsdtSnapshotTask(std::vector<KernelSsdtEntry>& rowsOut, QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    // KernelDock only asks ArkDriverClient for the R0 SSDT snapshot. The worker
    // still owns UI row shaping, sorting, and localized error text.
    const ksword::ark::DriverClient driverClient;
    const ksword::ark::SsdtEnumResult enumResult = driverClient.enumerateSsdt(
        KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED);
    if (!enumResult.io.ok)
    {
        errorTextOut = QStringLiteral("查询 SSDT 失败，error=%1，detail=%2")
            .arg(enumResult.io.win32Error)
            .arg(ssdtWorkerIoMessageText(QString::fromStdString(enumResult.io.message)));
        return false;
    }

    rowsOut.reserve(enumResult.entries.size());
    for (const ksword::ark::SsdtEntry& sourceEntry : enumResult.entries)
    {
        KernelSsdtEntry row{};
        row.serviceIndex = static_cast<std::uint32_t>(sourceEntry.serviceIndex);
        row.flags = static_cast<std::uint32_t>(sourceEntry.flags);
        row.zwRoutineAddress = static_cast<std::uint64_t>(sourceEntry.zwRoutineAddress);
        row.serviceRoutineAddress = static_cast<std::uint64_t>(sourceEntry.serviceRoutineAddress);
        row.serviceTableBase = static_cast<std::uint64_t>(enumResult.serviceTableBase);
        row.serviceNameText = QString::fromLocal8Bit(sourceEntry.serviceName.data(), static_cast<int>(sourceEntry.serviceName.size()));
        row.moduleNameText = QString::fromLocal8Bit(sourceEntry.moduleName.data(), static_cast<int>(sourceEntry.moduleName.size()));
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
            .arg(enumResult.version)
            .arg(enumResult.totalCount)
            .arg(enumResult.returnedCount)
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
