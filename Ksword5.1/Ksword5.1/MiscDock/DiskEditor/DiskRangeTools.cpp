#include "DiskRangeTools.h"

// ============================================================
// DiskRangeTools.cpp
// 作用：
// 1) 实现搜索、哈希、导出、导入、对比和坏块读扫等高级磁盘范围工具；
// 2) 使用顺序块处理，避免大范围任务一次性申请巨大内存；
// 3) 所有磁盘写入仍经由 DiskEditorBackend::writeBytes 执行扇区对齐保护。
// ============================================================

#include "DiskEditorBackend.h"

#include <QElapsedTimer>
#include <QFile>
#include <QStringList>

#include <algorithm>
#include <limits>

namespace
{
    using ks::misc::DiskRangeTaskResult;
    using ks::misc::DiskSearchPatternMode;
    using ks::misc::DiskSearchResult;

    // kIoChunkBytes：范围工具默认顺序处理块大小。
    constexpr std::uint32_t kIoChunkBytes = 1024U * 1024U;

    // kPreviewBeforeBytes：搜索命中预览前置字节数。
    constexpr int kPreviewBeforeBytes = 16;

    // kPreviewAfterBytes：搜索命中预览后置字节数。
    constexpr int kPreviewAfterBytes = 32;

    // appendError：
    // - 填充失败结果；
    // - result 为任务结果；
    // - errorText 为失败文本；
    // - 返回 result 引用便于调用处直接 return。
    DiskRangeTaskResult& appendError(DiskRangeTaskResult& result, const QString& errorText)
    {
        result.success = false;
        result.errorText = errorText;
        result.summary = errorText;
        return result;
    }

    // formatHexDigest：
    // - 把哈希摘要转为大写 HEX；
    // - digest 为原始摘要；
    // - 返回展示文本。
    QString formatHexDigest(const QByteArray& digest)
    {
        return QString::fromLatin1(digest.toHex(' ')).toUpper();
    }

    // estimateSpeed：
    // - 根据计时器和字节数计算 MiB/s；
    // - timer 为已启动计时器；
    // - bytes 为处理字节数；
    // - 返回吞吐速度。
    double estimateSpeed(const QElapsedTimer& timer, const std::uint64_t bytes)
    {
        const qint64 elapsedMs = std::max<qint64>(1, timer.elapsed());
        return (static_cast<double>(bytes) / 1048576.0) / (static_cast<double>(elapsedMs) / 1000.0);
    }

    // parseHexPattern：
    // - 解析 AA BB ?? 形式的十六进制搜索模式；
    // - patternText 为用户输入；
    // - patternOut 接收字节模板；
    // - maskOut 接收掩码，0 表示通配，0xFF 表示精确匹配；
    // - errorTextOut 返回失败说明。
    bool parseHexPattern(
        const QString& patternText,
        QByteArray& patternOut,
        QByteArray& maskOut,
        QString& errorTextOut)
    {
        patternOut.clear();
        maskOut.clear();
        errorTextOut.clear();

        QString normalized = patternText.trimmed();
        if (normalized.isEmpty())
        {
            errorTextOut = QStringLiteral("搜索模式为空。");
            return false;
        }
        normalized.replace(QStringLiteral("0x"), QString(), Qt::CaseInsensitive);
        normalized.replace(QChar(','), QChar(' '));
        normalized.replace(QChar(';'), QChar(' '));
        normalized.replace(QChar('\t'), QChar(' '));

        const bool hasSeparators = normalized.contains(QChar(' '));
        if (hasSeparators)
        {
            const QStringList tokens = normalized.split(QChar(' '), Qt::SkipEmptyParts);
            for (const QString& token : tokens)
            {
                if (token == QStringLiteral("?") || token == QStringLiteral("??"))
                {
                    patternOut.append('\0');
                    maskOut.append('\0');
                    continue;
                }
                if (token.size() != 2)
                {
                    errorTextOut = QStringLiteral("HEX 模式 token 长度必须为 2：%1").arg(token);
                    return false;
                }
                bool ok = false;
                const int value = token.toInt(&ok, 16);
                if (!ok || value < 0 || value > 0xFF)
                {
                    errorTextOut = QStringLiteral("HEX 模式包含非法字节：%1").arg(token);
                    return false;
                }
                patternOut.append(static_cast<char>(value));
                maskOut.append(static_cast<char>(0xFF));
            }
        }
        else
        {
            normalized.remove(QChar(' '));
            if ((normalized.size() % 2) != 0)
            {
                errorTextOut = QStringLiteral("连续 HEX 字符串长度必须为偶数。");
                return false;
            }
            for (int index = 0; index < normalized.size(); index += 2)
            {
                const QString token = normalized.mid(index, 2);
                if (token == QStringLiteral("??"))
                {
                    patternOut.append('\0');
                    maskOut.append('\0');
                    continue;
                }
                bool ok = false;
                const int value = token.toInt(&ok, 16);
                if (!ok || value < 0 || value > 0xFF)
                {
                    errorTextOut = QStringLiteral("HEX 模式包含非法字节：%1").arg(token);
                    return false;
                }
                patternOut.append(static_cast<char>(value));
                maskOut.append(static_cast<char>(0xFF));
            }
        }

        if (patternOut.isEmpty())
        {
            errorTextOut = QStringLiteral("搜索模式为空。");
            return false;
        }
        return true;
    }

    // buildPattern：
    // - 按搜索模式构建字节模板和掩码；
    // - mode 控制 HEX/ASCII/UTF-16；
    // - 返回 true 表示解析成功。
    bool buildPattern(
        const QString& patternText,
        const DiskSearchPatternMode mode,
        QByteArray& patternOut,
        QByteArray& maskOut,
        QString& errorTextOut)
    {
        if (mode == DiskSearchPatternMode::HexBytes)
        {
            return parseHexPattern(patternText, patternOut, maskOut, errorTextOut);
        }
        if (mode == DiskSearchPatternMode::AsciiText)
        {
            patternOut = patternText.toLocal8Bit();
            maskOut = QByteArray(patternOut.size(), static_cast<char>(0xFF));
        }
        else
        {
            patternOut = QByteArray(
                reinterpret_cast<const char*>(patternText.utf16()),
                patternText.size() * static_cast<int>(sizeof(char16_t)));
            maskOut = QByteArray(patternOut.size(), static_cast<char>(0xFF));
        }

        if (patternOut.isEmpty())
        {
            errorTextOut = QStringLiteral("搜索模式为空。");
            return false;
        }
        return true;
    }

    // matchesAt：
    // - 检查 buffer 指定偏移是否匹配模式；
    // - mask 为 0 的位置表示通配符；
    // - 返回 true 表示命中。
    bool matchesAt(
        const QByteArray& buffer,
        const int offset,
        const QByteArray& pattern,
        const QByteArray& mask)
    {
        if (offset < 0 || offset + pattern.size() > buffer.size())
        {
            return false;
        }
        for (int index = 0; index < pattern.size(); ++index)
        {
            const auto maskValue = static_cast<unsigned char>(mask.at(index));
            if (maskValue == 0)
            {
                continue;
            }
            const auto left = static_cast<unsigned char>(buffer.at(offset + index));
            const auto right = static_cast<unsigned char>(pattern.at(index));
            if ((left & maskValue) != (right & maskValue))
            {
                return false;
            }
        }
        return true;
    }

    // readChunk：
    // - 使用 DiskEditorBackend 读取一个块；
    // - devicePath/offsetBytes/bytesToRead 指定范围；
    // - bytesOut 接收数据；
    // - errorTextOut 返回错误。
    bool readChunk(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint32_t bytesToRead,
        QByteArray& bytesOut,
        QString& errorTextOut)
    {
        return ks::misc::DiskEditorBackend::readBytes(
            devicePath,
            offsetBytes,
            bytesToRead,
            bytesOut,
            errorTextOut);
    }

    // safeToUInt32：
    // - 把 64 位长度夹取到 DWORD 可表达范围内；
    // - value 为输入长度；
    // - 返回 32 位长度。
    std::uint32_t safeToUInt32(const std::uint64_t value)
    {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            value,
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
    }
}

namespace ks::misc
{
    DiskRangeTaskResult DiskRangeTools::searchRange(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint64_t lengthBytes,
        const QString& patternText,
        const DiskSearchPatternMode mode,
        const int maxResults)
    {
        DiskRangeTaskResult result;
        if (lengthBytes == 0)
        {
            return appendError(result, QStringLiteral("搜索长度为 0。"));
        }

        QByteArray pattern;
        QByteArray mask;
        QString parseError;
        if (!buildPattern(patternText, mode, pattern, mask, parseError))
        {
            return appendError(result, parseError);
        }

        const int patternSize = pattern.size();
        if (patternSize <= 0)
        {
            return appendError(result, QStringLiteral("搜索模式为空。"));
        }

        QElapsedTimer timer;
        timer.start();
        QByteArray carry;
        std::uint64_t processed = 0;
        const int resultLimit = std::max(1, maxResults);

        while (processed < lengthBytes && static_cast<int>(result.searchResults.size()) < resultLimit)
        {
            const std::uint64_t remaining = lengthBytes - processed;
            const std::uint32_t requestBytes = safeToUInt32(std::min<std::uint64_t>(remaining, kIoChunkBytes));
            QByteArray chunk;
            QString errorText;
            if (!readChunk(devicePath, offsetBytes + processed, requestBytes, chunk, errorText))
            {
                return appendError(result, QStringLiteral("搜索读取失败 @ %1：%2")
                    .arg(offsetBytes + processed)
                    .arg(errorText));
            }
            if (chunk.isEmpty())
            {
                break;
            }

            const QByteArray scanBuffer = carry + chunk;
            const std::uint64_t scanBase = offsetBytes + processed - static_cast<std::uint64_t>(carry.size());
            const int maxScanOffset = scanBuffer.size() - patternSize;
            for (int index = 0; index <= maxScanOffset && static_cast<int>(result.searchResults.size()) < resultLimit; ++index)
            {
                if (!matchesAt(scanBuffer, index, pattern, mask))
                {
                    continue;
                }

                const int previewStart = std::max(0, index - kPreviewBeforeBytes);
                const int previewEnd = std::min<int>(
                    static_cast<int>(scanBuffer.size()),
                    index + patternSize + kPreviewAfterBytes);
                DiskSearchResult hit;
                hit.offsetBytes = scanBase + static_cast<std::uint64_t>(index);
                hit.preview = scanBuffer.mid(previewStart, previewEnd - previewStart);
                result.searchResults.push_back(std::move(hit));
            }

            const int carryBytes = std::min<int>(
                std::max(patternSize - 1, 0),
                static_cast<int>(scanBuffer.size()));
            carry = scanBuffer.right(carryBytes);
            processed += static_cast<std::uint64_t>(chunk.size());
        }

        result.success = true;
        result.bytesProcessed = processed;
        result.mibPerSecond = estimateSpeed(timer, processed);
        result.summary = QStringLiteral("搜索完成：处理 %1，命中 %2 条，速度 %3 MiB/s。")
            .arg(DiskEditorBackend::formatBytes(processed))
            .arg(static_cast<int>(result.searchResults.size()))
            .arg(result.mibPerSecond, 0, 'f', 2);
        result.detailLines << QStringLiteral("模式长度：%1 字节").arg(patternSize);
        if (static_cast<int>(result.searchResults.size()) >= resultLimit)
        {
            result.detailLines << QStringLiteral("已达到最大结果数 %1，搜索提前停止。").arg(resultLimit);
        }
        return result;
    }

    DiskRangeTaskResult DiskRangeTools::hashRange(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint64_t lengthBytes,
        const QCryptographicHash::Algorithm algorithm)
    {
        DiskRangeTaskResult result;
        if (lengthBytes == 0)
        {
            return appendError(result, QStringLiteral("哈希长度为 0。"));
        }

        QCryptographicHash hasher(algorithm);
        QElapsedTimer timer;
        timer.start();
        std::uint64_t processed = 0;
        while (processed < lengthBytes)
        {
            const std::uint64_t remaining = lengthBytes - processed;
            const std::uint32_t requestBytes = safeToUInt32(std::min<std::uint64_t>(remaining, kIoChunkBytes));
            QByteArray chunk;
            QString errorText;
            if (!readChunk(devicePath, offsetBytes + processed, requestBytes, chunk, errorText))
            {
                return appendError(result, QStringLiteral("哈希读取失败 @ %1：%2")
                    .arg(offsetBytes + processed)
                    .arg(errorText));
            }
            if (chunk.isEmpty())
            {
                break;
            }
            hasher.addData(chunk);
            processed += static_cast<std::uint64_t>(chunk.size());
        }

        result.success = true;
        result.digestBytes = hasher.result();
        result.bytesProcessed = processed;
        result.mibPerSecond = estimateSpeed(timer, processed);
        result.summary = QStringLiteral("哈希完成：%1，速度 %2 MiB/s。")
            .arg(formatHexDigest(result.digestBytes))
            .arg(result.mibPerSecond, 0, 'f', 2);
        result.detailLines << QStringLiteral("处理范围：offset=%1 length=%2")
            .arg(offsetBytes)
            .arg(lengthBytes);
        return result;
    }

    DiskRangeTaskResult DiskRangeTools::exportRangeToFile(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint64_t lengthBytes,
        const QString& filePath)
    {
        DiskRangeTaskResult result;
        if (lengthBytes == 0)
        {
            return appendError(result, QStringLiteral("导出长度为 0。"));
        }
        QFile output(filePath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return appendError(result, QStringLiteral("无法打开导出文件：%1").arg(output.errorString()));
        }

        QElapsedTimer timer;
        timer.start();
        std::uint64_t processed = 0;
        while (processed < lengthBytes)
        {
            const std::uint64_t remaining = lengthBytes - processed;
            const std::uint32_t requestBytes = safeToUInt32(std::min<std::uint64_t>(remaining, kIoChunkBytes));
            QByteArray chunk;
            QString errorText;
            if (!readChunk(devicePath, offsetBytes + processed, requestBytes, chunk, errorText))
            {
                output.close();
                return appendError(result, QStringLiteral("导出读取失败 @ %1：%2")
                    .arg(offsetBytes + processed)
                    .arg(errorText));
            }
            if (chunk.isEmpty())
            {
                break;
            }
            if (output.write(chunk) != chunk.size())
            {
                output.close();
                return appendError(result, QStringLiteral("写入导出文件失败：%1").arg(output.errorString()));
            }
            processed += static_cast<std::uint64_t>(chunk.size());
        }
        output.close();

        result.success = true;
        result.bytesProcessed = processed;
        result.mibPerSecond = estimateSpeed(timer, processed);
        result.summary = QStringLiteral("导出完成：%1 -> %2，速度 %3 MiB/s。")
            .arg(DiskEditorBackend::formatBytes(processed))
            .arg(filePath)
            .arg(result.mibPerSecond, 0, 'f', 2);
        return result;
    }

    DiskRangeTaskResult DiskRangeTools::importFileToRange(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const QString& filePath,
        const std::uint32_t bytesPerSector,
        const bool requireSectorAligned)
    {
        DiskRangeTaskResult result;
        QFile input(filePath);
        if (!input.open(QIODevice::ReadOnly))
        {
            return appendError(result, QStringLiteral("无法打开导入文件：%1").arg(input.errorString()));
        }

        const std::uint64_t fileBytes = static_cast<std::uint64_t>(input.size());
        const std::uint32_t sectorSize = bytesPerSector == 0 ? 512U : bytesPerSector;
        if (fileBytes == 0)
        {
            return appendError(result, QStringLiteral("导入文件为空。"));
        }
        if (requireSectorAligned
            && ((offsetBytes % sectorSize) != 0 || (fileBytes % sectorSize) != 0))
        {
            return appendError(result, QStringLiteral("导入被拒绝：偏移和文件长度必须按 %1 字节扇区对齐。").arg(sectorSize));
        }
        if (requireSectorAligned && (kIoChunkBytes % sectorSize) != 0)
        {
            return appendError(result, QStringLiteral("导入被拒绝：内部块大小不能被 %1 字节扇区整除。").arg(sectorSize));
        }

        QElapsedTimer timer;
        timer.start();
        std::uint64_t processed = 0;
        while (!input.atEnd())
        {
            QByteArray chunk = input.read(kIoChunkBytes);
            if (chunk.isEmpty())
            {
                break;
            }
            QString errorText;
            if (!DiskEditorBackend::writeBytes(
                devicePath,
                offsetBytes + processed,
                chunk,
                bytesPerSector,
                false,
                errorText))
            {
                return appendError(result, QStringLiteral("导入写入失败 @ %1：%2")
                    .arg(offsetBytes + processed)
                    .arg(errorText));
            }
            processed += static_cast<std::uint64_t>(chunk.size());
        }

        result.success = true;
        result.bytesProcessed = processed;
        result.mibPerSecond = estimateSpeed(timer, processed);
        result.summary = QStringLiteral("导入完成：%1 -> offset %2，速度 %3 MiB/s。")
            .arg(filePath)
            .arg(offsetBytes)
            .arg(result.mibPerSecond, 0, 'f', 2);
        return result;
    }

    DiskRangeTaskResult DiskRangeTools::compareRangeWithFile(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint64_t lengthBytes,
        const QString& filePath,
        const int maxDifferences)
    {
        DiskRangeTaskResult result;
        QFile input(filePath);
        if (!input.open(QIODevice::ReadOnly))
        {
            return appendError(result, QStringLiteral("无法打开对比文件：%1").arg(input.errorString()));
        }

        const std::uint64_t compareBytes = std::min<std::uint64_t>(
            lengthBytes,
            static_cast<std::uint64_t>(input.size()));
        if (compareBytes == 0)
        {
            return appendError(result, QStringLiteral("对比长度为 0。"));
        }

        QElapsedTimer timer;
        timer.start();
        std::uint64_t processed = 0;
        int differences = 0;
        const int differenceLimit = std::max(1, maxDifferences);
        while (processed < compareBytes)
        {
            const std::uint64_t remaining = compareBytes - processed;
            const std::uint32_t requestBytes = safeToUInt32(std::min<std::uint64_t>(remaining, kIoChunkBytes));
            QByteArray diskBytes;
            QString errorText;
            if (!readChunk(devicePath, offsetBytes + processed, requestBytes, diskBytes, errorText))
            {
                return appendError(result, QStringLiteral("对比读取失败 @ %1：%2")
                    .arg(offsetBytes + processed)
                    .arg(errorText));
            }
            const QByteArray fileBytes = input.read(diskBytes.size());
            const int compareSize = std::min(diskBytes.size(), fileBytes.size());
            for (int index = 0; index < compareSize; ++index)
            {
                if (diskBytes.at(index) == fileBytes.at(index))
                {
                    continue;
                }
                ++differences;
                if (result.detailLines.size() < differenceLimit)
                {
                    result.detailLines << QStringLiteral("差异 @ %1：磁盘=%2 文件=%3")
                        .arg(offsetBytes + processed + static_cast<std::uint64_t>(index))
                        .arg(static_cast<unsigned int>(static_cast<unsigned char>(diskBytes.at(index))), 2, 16, QChar('0'))
                        .arg(static_cast<unsigned int>(static_cast<unsigned char>(fileBytes.at(index))), 2, 16, QChar('0'))
                        .toUpper();
                }
            }
            processed += static_cast<std::uint64_t>(compareSize);
            if (compareSize == 0)
            {
                break;
            }
        }

        result.success = true;
        result.bytesProcessed = processed;
        result.failureCount = differences;
        result.mibPerSecond = estimateSpeed(timer, processed);
        result.summary = differences == 0
            ? QStringLiteral("对比完成：完全一致，处理 %1。").arg(DiskEditorBackend::formatBytes(processed))
            : QStringLiteral("对比完成：发现 %1 处字节差异，处理 %2。").arg(differences).arg(DiskEditorBackend::formatBytes(processed));
        if (static_cast<std::uint64_t>(input.size()) != lengthBytes)
        {
            result.detailLines << QStringLiteral("提示：文件长度 %1 与指定长度 %2 不一致，仅对比交集。")
                .arg(input.size())
                .arg(lengthBytes);
        }
        return result;
    }

    DiskRangeTaskResult DiskRangeTools::scanReadableBlocks(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint64_t lengthBytes,
        const std::uint32_t blockBytes)
    {
        DiskRangeTaskResult result;
        if (lengthBytes == 0)
        {
            return appendError(result, QStringLiteral("扫描长度为 0。"));
        }

        const std::uint32_t effectiveBlockBytes = std::clamp<std::uint32_t>(
            blockBytes == 0 ? kIoChunkBytes : blockBytes,
            512U,
            kIoChunkBytes * 8U);
        QElapsedTimer timer;
        timer.start();
        std::uint64_t processed = 0;
        int failedBlocks = 0;

        while (processed < lengthBytes)
        {
            const std::uint64_t remaining = lengthBytes - processed;
            const std::uint32_t requestBytes = safeToUInt32(std::min<std::uint64_t>(remaining, effectiveBlockBytes));
            QByteArray chunk;
            QString errorText;
            if (!readChunk(devicePath, offsetBytes + processed, requestBytes, chunk, errorText))
            {
                ++failedBlocks;
                if (result.detailLines.size() < 128)
                {
                    result.detailLines << QStringLiteral("读取失败块 @ %1 length=%2：%3")
                        .arg(offsetBytes + processed)
                        .arg(requestBytes)
                        .arg(errorText);
                }
                processed += requestBytes;
                continue;
            }
            processed += static_cast<std::uint64_t>(chunk.size());
            if (chunk.isEmpty())
            {
                break;
            }
        }

        result.success = failedBlocks == 0;
        result.bytesProcessed = processed;
        result.failureCount = failedBlocks;
        result.mibPerSecond = estimateSpeed(timer, processed);
        result.summary = failedBlocks == 0
            ? QStringLiteral("读扫完成：未发现读取失败块，速度 %1 MiB/s。").arg(result.mibPerSecond, 0, 'f', 2)
            : QStringLiteral("读扫完成：发现 %1 个读取失败块，速度 %2 MiB/s。").arg(failedBlocks).arg(result.mibPerSecond, 0, 'f', 2);
        return result;
    }
}
