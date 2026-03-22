#include "NetworkDock.h"
#include "../ProcessDock/ProcessDetailWindow.h"

#include <QAction>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm> // std::min/std::max：预览长度与范围标准化。
#include <atomic>    // std::atomic_bool：跨线程状态门控。
#include <limits>    // std::numeric_limits：包长上限范围表达。
#include <string>    // std::string：日志桥接文本类型。
#include <thread>    // std::thread：长耗时请求放到后台执行。
#include <unordered_set> // std::unordered_set：ARP接口索引去重。
#include <vector>    // std::vector：批量刷新队列临时容器。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <IcmpAPI.h>
#include <Iphlpapi.h>
#include <Windns.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Dnsapi.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace
{
    // kHexBytesPerRow：
    // - 报文详情窗口每行显示 16 字节；
    // - 与内存模块十六进制查看器保持一致。
    constexpr int kHexBytesPerRow = 16;

    // toQString：std::string -> QString 转换辅助。
    QString toQString(const std::string& textValue)
    {
        return QString::fromUtf8(textValue.c_str());
    }

    // formatEndpointText：把“地址 + 端口”格式化为地址端点文本。
    QString formatEndpointText(const std::string& ipAddress, const std::uint16_t portNumber)
    {
        // endpointAddress 用途：统一承载地址字符串，后续根据是否 IPv6 决定是否加方括号。
        const QString endpointAddress = toQString(ipAddress);
        if (endpointAddress.contains(':'))
        {
            // IPv6 端点采用 [addr]:port 形式，避免与地址内部冒号冲突。
            return QString("[%1]:%2").arg(endpointAddress).arg(portNumber);
        }
        return QString("%1:%2").arg(endpointAddress).arg(portNumber);
    }

    // formatBytesToHexText：
    // - 把字节数组格式化为十六进制字符串（用于日志与结果摘要）；
    // - maxBytesToRender 用于限制输出长度，避免超大响应刷屏。
    QString formatBytesToHexText(const std::vector<std::uint8_t>& byteArray, const std::size_t maxBytesToRender)
    {
        if (byteArray.empty())
        {
            return QStringLiteral("<empty>");
        }

        const std::size_t renderLength = std::min<std::size_t>(byteArray.size(), maxBytesToRender);
        QStringList parts;
        parts.reserve(static_cast<int>(renderLength));
        for (std::size_t index = 0; index < renderLength; ++index)
        {
            parts.push_back(QString("%1").arg(static_cast<unsigned>(byteArray[index]), 2, 16, QChar('0')).toUpper());
        }

        QString hexText = parts.join(' ');
        if (renderLength < byteArray.size())
        {
            hexText += QString(" ... (total=%1 bytes)").arg(static_cast<qulonglong>(byteArray.size()));
        }
        return hexText;
    }

    // isVisibleAsciiByte：
    // - 判断字节是否属于可直接阅读的可见 ASCII 字符；
    // - 用于预览提取和十六进制旁路 ASCII 渲染。
    bool isVisibleAsciiByte(const std::uint8_t byteValue)
    {
        return byteValue >= 32 && byteValue <= 126;
    }

    // buildPayloadByteRange：
    // - 统一计算 payload 在 packetBytes 中的安全区间；
    // - 返回值 first=payload 起始偏移，second=payload 可读长度。
    std::pair<std::size_t, std::size_t> buildPayloadByteRange(const ks::network::PacketRecord& packetRecord)
    {
        if (packetRecord.packetBytes.empty() || packetRecord.payloadOffset >= packetRecord.packetBytes.size())
        {
            return { 0, 0 };
        }

        const std::size_t payloadOffset = packetRecord.payloadOffset;
        const std::size_t maxReadableLength = packetRecord.packetBytes.size() - payloadOffset;
        const std::size_t expectedPayloadLength =
            (packetRecord.payloadSize == 0)
            ? maxReadableLength
            : std::min<std::size_t>(static_cast<std::size_t>(packetRecord.payloadSize), maxReadableLength);
        return { payloadOffset, expectedPayloadLength };
    }

    // normalizeAsciiCharForPreview：
    // - 把单字节映射为预览可显示字符；
    // - 非可见字符统一折叠为 '.'，换行/制表折叠为空格。
    QChar normalizeAsciiCharForPreview(const std::uint8_t byteValue)
    {
        if (isVisibleAsciiByte(byteValue))
        {
            return QChar(byteValue);
        }
        if (byteValue == '\r' || byteValue == '\n' || byteValue == '\t')
        {
            return QChar(' ');
        }
        return QChar('.');
    }

    // buildPayloadAsciiPreviewText：
    // - 生成抓包列表“内容预览”列文本；
    // - 优先提取可读 ASCII 片段，让预览比纯十六进制更有语义。
    QString buildPayloadAsciiPreviewText(const ks::network::PacketRecord& packetRecord)
    {
        const auto [payloadOffset, payloadLength] = buildPayloadByteRange(packetRecord);
        if (payloadLength == 0)
        {
            return QStringLiteral("<empty>");
        }

        constexpr std::size_t kPreviewByteLimit = 180;
        const std::size_t previewLength = std::min<std::size_t>(payloadLength, kPreviewByteLimit);

        // readableSegments 用途：收集长度足够的可见 ASCII 连续片段。
        QStringList readableSegments;
        QString currentSegment;
        for (std::size_t index = 0; index < previewLength; ++index)
        {
            const std::uint8_t byteValue = packetRecord.packetBytes[payloadOffset + index];
            if (isVisibleAsciiByte(byteValue) || byteValue == ' ' || byteValue == '\t')
            {
                currentSegment.push_back(isVisibleAsciiByte(byteValue) ? QChar(byteValue) : QChar(' '));
                continue;
            }

            if (currentSegment.trimmed().size() >= 4)
            {
                readableSegments.push_back(currentSegment.simplified());
            }
            currentSegment.clear();
        }
        if (currentSegment.trimmed().size() >= 4)
        {
            readableSegments.push_back(currentSegment.simplified());
        }

        // previewText 用途：最终展示在列表列中的“可读预览文本”。
        QString previewText;
        if (!readableSegments.isEmpty())
        {
            previewText = readableSegments.join(QStringLiteral(" | "));
        }
        else
        {
            previewText.reserve(static_cast<int>(previewLength));
            for (std::size_t index = 0; index < previewLength; ++index)
            {
                const std::uint8_t byteValue = packetRecord.packetBytes[payloadOffset + index];
                previewText.push_back(normalizeAsciiCharForPreview(byteValue));
            }
            previewText = previewText.simplified();
        }

        if (previewText.isEmpty())
        {
            previewText = QStringLiteral("<binary payload>");
        }

        if (previewLength < payloadLength)
        {
            previewText += QStringLiteral(" ...");
        }
        if (packetRecord.packetBytesTruncated)
        {
            previewText += QStringLiteral(" [truncated]");
        }
        return previewText;
    }

    // buildPayloadAsciiFullText：
    // - 把 payload 转成可阅读 ASCII 文本（用于详情与批量复制）；
    // - CRLF 会被保留为换行，非可打印字节会替换为 '.'.
    QString buildPayloadAsciiFullText(const ks::network::PacketRecord& packetRecord)
    {
        const auto [payloadOffset, payloadLength] = buildPayloadByteRange(packetRecord);
        if (payloadLength == 0)
        {
            return QStringLiteral("<empty>");
        }

        QString asciiText;
        asciiText.reserve(static_cast<int>(payloadLength));
        for (std::size_t index = 0; index < payloadLength; ++index)
        {
            const std::uint8_t byteValue = packetRecord.packetBytes[payloadOffset + index];
            if (byteValue == '\r')
            {
                if (index + 1 < payloadLength && packetRecord.packetBytes[payloadOffset + index + 1] == '\n')
                {
                    ++index;
                }
                asciiText.push_back('\n');
                continue;
            }
            if (byteValue == '\n')
            {
                asciiText.push_back('\n');
                continue;
            }
            if (byteValue == '\t')
            {
                asciiText.push_back('\t');
                continue;
            }

            asciiText.push_back(isVisibleAsciiByte(byteValue) ? QChar(byteValue) : QChar('.'));
        }

        if (packetRecord.packetBytesTruncated)
        {
            asciiText += QStringLiteral("\n[truncated capture]");
        }
        return asciiText;
    }

    // buildPayloadHexFullText：
    // - 把 payload 原始字节完整转换为十六进制字符串（空格分隔）；
    // - 供“抓包回放到请求构造”时直接填充 HEX 载荷输入框。
    QString buildPayloadHexFullText(const ks::network::PacketRecord& packetRecord)
    {
        const auto [payloadOffset, payloadLength] = buildPayloadByteRange(packetRecord);
        if (payloadLength == 0)
        {
            return QString();
        }

        // payloadHexParts 用途：逐字节收集 HEX 片段，最终 join 成单行文本。
        QStringList payloadHexParts;
        payloadHexParts.reserve(static_cast<int>(payloadLength));
        for (std::size_t index = 0; index < payloadLength; ++index)
        {
            const std::uint8_t byteValue = packetRecord.packetBytes[payloadOffset + index];
            payloadHexParts.push_back(
                QStringLiteral("%1").arg(static_cast<unsigned>(byteValue), 2, 16, QChar('0')).toUpper());
        }
        return payloadHexParts.join(' ');
    }

    // buildPacketHexAsciiDumpText：
    // - 生成完整“偏移 + 十六进制 + ASCII”文本转储；
    // - 使用纯文本输出，支持像编辑器一样跨行选中复制。
    QString buildPacketHexAsciiDumpText(const ks::network::PacketRecord& packetRecord)
    {
        const std::vector<std::uint8_t>& packetBytes = packetRecord.packetBytes;
        if (packetBytes.empty())
        {
            return QStringLiteral("00000000  -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --  |<empty>|");
        }

        QStringList outputLines;
        const std::size_t rowCount =
            (packetBytes.size() + static_cast<std::size_t>(kHexBytesPerRow) - 1) /
            static_cast<std::size_t>(kHexBytesPerRow);
        outputLines.reserve(static_cast<int>(rowCount) + 1);
        outputLines.push_back(QStringLiteral("偏移(h)   16进制字节                                              ASCII"));

        for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
        {
            const std::size_t rowOffset = rowIndex * static_cast<std::size_t>(kHexBytesPerRow);

            // hexColumnText 用途：固定宽度拼接 16 个十六进制字节。
            QString hexColumnText;
            hexColumnText.reserve(kHexBytesPerRow * 3);
            // asciiColumnText 用途：与 hexColumnText 同步输出人类可读字符。
            QString asciiColumnText;
            asciiColumnText.reserve(kHexBytesPerRow);

            for (int byteColumn = 0; byteColumn < kHexBytesPerRow; ++byteColumn)
            {
                const std::size_t byteIndex = rowOffset + static_cast<std::size_t>(byteColumn);
                if (byteIndex < packetBytes.size())
                {
                    const std::uint8_t byteValue = packetBytes[byteIndex];
                    hexColumnText += QStringLiteral("%1 ").arg(static_cast<unsigned>(byteValue), 2, 16, QChar('0')).toUpper();
                    asciiColumnText.push_back(isVisibleAsciiByte(byteValue) ? QChar(byteValue) : QChar('.'));
                }
                else
                {
                    hexColumnText += QStringLiteral("   ");
                    asciiColumnText.push_back(' ');
                }
            }
            if (!hexColumnText.isEmpty())
            {
                hexColumnText.chop(1);
            }

            const QString offsetText = QStringLiteral("%1").arg(static_cast<qulonglong>(rowOffset), 8, 16, QChar('0')).toUpper();
            outputLines.push_back(QStringLiteral("%1  %2  |%3|")
                .arg(offsetText)
                .arg(hexColumnText)
                .arg(asciiColumnText));
        }

        if (packetRecord.packetBytesTruncated)
        {
            outputLines.push_back(QStringLiteral("[truncated capture: original bytes exceed retain limit]"));
        }
        return outputLines.join('\n');
    }

    // buildPacketCopyHeaderLine：
    // - 构建“复制到剪贴板”时每个报文块的元信息头。
    QString buildPacketCopyHeaderLine(const ks::network::PacketRecord& packetRecord)
    {
        const QString timeText = QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(packetRecord.captureTimestampMs)).toString("yyyy-MM-dd HH:mm:ss.zzz");
        return QStringLiteral("#%1 | 时间=%2 | PID=%3 | 协议=%4 | 方向=%5 | 本地=%6 | 远端=%7 | 长度=%8/%9")
            .arg(packetRecord.sequenceId)
            .arg(timeText)
            .arg(packetRecord.processId)
            .arg(toQString(ks::network::PacketProtocolToString(packetRecord.protocol)))
            .arg(toQString(ks::network::PacketDirectionToString(packetRecord.direction)))
            .arg(formatEndpointText(packetRecord.localAddress, packetRecord.localPort))
            .arg(formatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort))
            .arg(packetRecord.payloadSize)
            .arg(packetRecord.totalPacketSize);
    }


    // formatIpv4HostOrder：
    // - 把主机序 IPv4 32 位值转成点分十进制文本；
    // - 用于过滤状态标签与调试输出。
    QString formatIpv4HostOrder(const std::uint32_t ipv4HostOrder)
    {
        const std::uint32_t octet1 = (ipv4HostOrder >> 24) & 0xFFU;
        const std::uint32_t octet2 = (ipv4HostOrder >> 16) & 0xFFU;
        const std::uint32_t octet3 = (ipv4HostOrder >> 8) & 0xFFU;
        const std::uint32_t octet4 = ipv4HostOrder & 0xFFU;
        return QString("%1.%2.%3.%4").arg(octet1).arg(octet2).arg(octet3).arg(octet4);
    }

    // tryParseIpv4Text：
    // - 解析标准 IPv4 点分十进制文本（如 192.168.1.5）；
    // - 解析成功时输出主机序 32 位值。
    bool tryParseIpv4Text(const QString& ipv4Text, std::uint32_t& ipv4HostOrderOut)
    {
        const QString trimmedText = ipv4Text.trimmed();
        if (trimmedText.isEmpty())
        {
            return false;
        }

        const QStringList segments = trimmedText.split('.');
        if (segments.size() != 4)
        {
            return false;
        }

        std::uint32_t ipv4Value = 0;
        for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
        {
            bool segmentParseOk = false;
            const int segmentValue = segments[segmentIndex].toInt(&segmentParseOk, 10);
            if (!segmentParseOk || segmentValue < 0 || segmentValue > 255)
            {
                return false;
            }

            ipv4Value = (ipv4Value << 8) | static_cast<std::uint32_t>(segmentValue);
        }

        ipv4HostOrderOut = ipv4Value;
        return true;
    }

    // tryParseIpv4RangeText：
    // - 解析 IP 段过滤表达式；
    // - 支持三种格式：
    //   1) CIDR：192.168.1.0/24
    //   2) 范围：192.168.1.10-192.168.1.200
    //   3) 单 IP：192.168.1.8
    // - 解析成功后输出主机序闭区间 [min,max]。
    bool tryParseIpv4RangeText(
        const QString& rangeText,
        std::pair<std::uint32_t, std::uint32_t>& rangeOut,
        QString& normalizeTextOut)
    {
        const QString trimmedText = rangeText.trimmed();
        if (trimmedText.isEmpty())
        {
            return false;
        }

        // 优先识别 CIDR 语法。
        const int slashIndex = trimmedText.indexOf('/');
        if (slashIndex > 0)
        {
            const QString ipText = trimmedText.left(slashIndex).trimmed();
            const QString prefixText = trimmedText.mid(slashIndex + 1).trimmed();

            std::uint32_t baseIpHostOrder = 0;
            if (!tryParseIpv4Text(ipText, baseIpHostOrder))
            {
                return false;
            }

            bool prefixParseOk = false;
            const int prefixLength = prefixText.toInt(&prefixParseOk, 10);
            if (!prefixParseOk || prefixLength < 0 || prefixLength > 32)
            {
                return false;
            }

            const std::uint32_t netmask = (prefixLength == 0)
                ? 0U
                : (0xFFFFFFFFU << static_cast<unsigned>(32 - prefixLength));
            const std::uint32_t rangeBegin = baseIpHostOrder & netmask;
            const std::uint32_t rangeEnd = rangeBegin | (~netmask);
            rangeOut = { rangeBegin, rangeEnd };
            normalizeTextOut = QString("%1/%2").arg(formatIpv4HostOrder(rangeBegin)).arg(prefixLength);
            return true;
        }

        // 再识别“起始IP-结束IP”语法。
        const int dashIndex = trimmedText.indexOf('-');
        if (dashIndex > 0)
        {
            const QString beginIpText = trimmedText.left(dashIndex).trimmed();
            const QString endIpText = trimmedText.mid(dashIndex + 1).trimmed();

            std::uint32_t beginIpHostOrder = 0;
            std::uint32_t endIpHostOrder = 0;
            if (!tryParseIpv4Text(beginIpText, beginIpHostOrder) ||
                !tryParseIpv4Text(endIpText, endIpHostOrder))
            {
                return false;
            }

            const std::uint32_t normalizedBegin = std::min(beginIpHostOrder, endIpHostOrder);
            const std::uint32_t normalizedEnd = std::max(beginIpHostOrder, endIpHostOrder);
            rangeOut = { normalizedBegin, normalizedEnd };
            normalizeTextOut = QString("%1-%2")
                .arg(formatIpv4HostOrder(normalizedBegin))
                .arg(formatIpv4HostOrder(normalizedEnd));
            return true;
        }

        // 最后按“单 IP 精确匹配”处理。
        std::uint32_t singleIpHostOrder = 0;
        if (!tryParseIpv4Text(trimmedText, singleIpHostOrder))
        {
            return false;
        }
        rangeOut = { singleIpHostOrder, singleIpHostOrder };
        normalizeTextOut = formatIpv4HostOrder(singleIpHostOrder);
        return true;
    }

    // tryParsePortRangeText：
    // - 解析端口过滤表达式；
    // - 支持“单值”(80) 或 “范围”(1000-2000)；
    // - 解析成功时输出闭区间 [min,max]。
    bool tryParsePortRangeText(
        const QString& rangeText,
        std::pair<std::uint16_t, std::uint16_t>& rangeOut,
        QString& normalizeTextOut)
    {
        const QString trimmedText = rangeText.trimmed();
        if (trimmedText.isEmpty())
        {
            return false;
        }

        const auto tryParsePort = [](const QString& text, std::uint16_t& valueOut) -> bool
            {
                bool parseOk = false;
                const unsigned long portValue = text.trimmed().toULong(&parseOk, 10);
                if (!parseOk || portValue > 65535UL)
                {
                    return false;
                }
                valueOut = static_cast<std::uint16_t>(portValue);
                return true;
            };

        const int dashIndex = trimmedText.indexOf('-');
        if (dashIndex > 0)
        {
            const QString beginPortText = trimmedText.left(dashIndex).trimmed();
            const QString endPortText = trimmedText.mid(dashIndex + 1).trimmed();
            std::uint16_t beginPort = 0;
            std::uint16_t endPort = 0;
            if (!tryParsePort(beginPortText, beginPort) || !tryParsePort(endPortText, endPort))
            {
                return false;
            }

            const std::uint16_t normalizedBegin = std::min(beginPort, endPort);
            const std::uint16_t normalizedEnd = std::max(beginPort, endPort);
            rangeOut = { normalizedBegin, normalizedEnd };
            normalizeTextOut = QString("%1-%2").arg(normalizedBegin).arg(normalizedEnd);
            return true;
        }

        std::uint16_t singlePort = 0;
        if (!tryParsePort(trimmedText, singlePort))
        {
            return false;
        }
        rangeOut = { singlePort, singlePort };
        normalizeTextOut = QString::number(singlePort);
        return true;
    }

    // createPacketCell：
    // - 统一创建只读单元格；
    // - 避免每列重复设置 editable flag。
    QTableWidgetItem* createPacketCell(const QString& cellText)
    {
        QTableWidgetItem* tableItem = new QTableWidgetItem(cellText);
        tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        return tableItem;
    }

    // PacketDetailWindow：
    // - 报文详情独立窗口（show 非阻塞，不阻塞主 UI）；
    // - 十六进制区使用纯文本编辑器，支持像文本一样跨行连续选择复制。
    class PacketDetailWindow final : public QWidget
    {
    public:
        explicit PacketDetailWindow(const ks::network::PacketRecord& packetRecord, QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setAttribute(Qt::WA_DeleteOnClose, true);
            setWindowFlag(Qt::Window, true);
            setWindowTitle(QStringLiteral("报文详情 - #%1").arg(packetRecord.sequenceId));
            resize(1120, 760);

            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(8, 8, 8, 8);
            rootLayout->setSpacing(6);

            // 元信息区域：时间、协议、方向、PID、端点、长度。
            QLabel* metaLabel = new QLabel(this);
            const QString timeText = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(packetRecord.captureTimestampMs)).toString("yyyy-MM-dd HH:mm:ss.zzz");
            metaLabel->setText(QStringLiteral(
                "时间: %1\n协议: %2  方向: %3\nPID: %4  进程: %5\n本地: %6\n远端: %7\n总长度: %8 bytes, 负载: %9 bytes")
                .arg(timeText)
                .arg(toQString(ks::network::PacketProtocolToString(packetRecord.protocol)))
                .arg(toQString(ks::network::PacketDirectionToString(packetRecord.direction)))
                .arg(packetRecord.processId)
                .arg(toQString(packetRecord.processName))
                .arg(formatEndpointText(packetRecord.localAddress, packetRecord.localPort))
                .arg(formatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort))
                .arg(packetRecord.totalPacketSize)
                .arg(packetRecord.payloadSize));
            metaLabel->setWordWrap(true);
            rootLayout->addWidget(metaLabel);

            // 可读摘要：先展示“内容预览列同款”的语义化 ASCII 摘要，便于快速判断协议文本。
            QLabel* readablePreviewLabel = new QLabel(this);
            readablePreviewLabel->setText(QStringLiteral("可读ASCII摘要: %1").arg(buildPayloadAsciiPreviewText(packetRecord)));
            readablePreviewLabel->setWordWrap(true);
            readablePreviewLabel->setToolTip(QStringLiteral("该摘要优先提取 payload 中可读 ASCII 片段。"));
            rootLayout->addWidget(readablePreviewLabel);

            // 详情页签：把十六进制视图与 ASCII 文本视图分开，提升阅读与复制体验。
            QTabWidget* detailTabWidget = new QTabWidget(this);
            rootLayout->addWidget(detailTabWidget, 1);

            // 十六进制页：使用 QPlainTextEdit，支持跨行连续选择。
            QWidget* hexPage = new QWidget(detailTabWidget);
            QVBoxLayout* hexPageLayout = new QVBoxLayout(hexPage);
            hexPageLayout->setContentsMargins(0, 0, 0, 0);
            hexPageLayout->setSpacing(4);

            QLabel* hexHintLabel = new QLabel(QStringLiteral("十六进制区支持像文本编辑器一样跨行拖拽选择。"), hexPage);
            hexPageLayout->addWidget(hexHintLabel);

            QPlainTextEdit* hexTextEditor = new QPlainTextEdit(hexPage);
            hexTextEditor->setReadOnly(true);
            hexTextEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
            hexTextEditor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            hexTextEditor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            hexTextEditor->setPlainText(buildPacketHexAsciiDumpText(packetRecord));
            hexTextEditor->setToolTip(QStringLiteral("可直接 Ctrl+C 复制选中的十六进制文本。"));

            // 按需求调大十六进制字号，提升长时间查看体验。
            QFont hexFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            const int basePointSize = (hexFont.pointSize() > 0) ? hexFont.pointSize() : 10;
            hexFont.setPointSize(std::max(11, basePointSize + 2));
            hexTextEditor->setFont(hexFont);
            hexPageLayout->addWidget(hexTextEditor, 1);

            detailTabWidget->addTab(hexPage, QStringLiteral("十六进制"));

            // ASCII 页：展示 payload 的文本化内容，优先保留 CRLF 语义。
            QWidget* asciiPage = new QWidget(detailTabWidget);
            QVBoxLayout* asciiPageLayout = new QVBoxLayout(asciiPage);
            asciiPageLayout->setContentsMargins(0, 0, 0, 0);
            asciiPageLayout->setSpacing(4);

            QLabel* asciiHintLabel = new QLabel(QStringLiteral("下方为 payload 的 ASCII 视图（不可打印字节以 '.' 代替）。"), asciiPage);
            asciiHintLabel->setWordWrap(true);
            asciiPageLayout->addWidget(asciiHintLabel);

            QPlainTextEdit* asciiTextEditor = new QPlainTextEdit(asciiPage);
            asciiTextEditor->setReadOnly(true);
            asciiTextEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
            asciiTextEditor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            asciiTextEditor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            asciiTextEditor->setPlainText(buildPayloadAsciiFullText(packetRecord));
            asciiTextEditor->setToolTip(QStringLiteral("可直接复制 payload 的 ASCII 文本内容。"));
            asciiPageLayout->addWidget(asciiTextEditor, 1);

            detailTabWidget->addTab(asciiPage, QStringLiteral("报文ASCII"));
        }
    };

    // populatePacketRow：
    // - 在指定表格行写入报文展示列；
    // - 流量监控主表统一复用同一渲染逻辑。
    void populatePacketRow(
        QTableWidget* tableWidget,
        const int rowIndex,
        const ks::network::PacketRecord& packetRecord,
        const std::uint64_t sequenceId,
        const QIcon& processIcon)
    {
        if (tableWidget == nullptr || rowIndex < 0)
        {
            return;
        }

        const QString timeText = QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(packetRecord.captureTimestampMs)).toString("HH:mm:ss.zzz");
        const QString protocolText = toQString(ks::network::PacketProtocolToString(packetRecord.protocol));
        const QString directionText = toQString(ks::network::PacketDirectionToString(packetRecord.direction));
        const QString pidText = QString::number(packetRecord.processId);
        const QString processNameText = toQString(packetRecord.processName);
        const QString localEndpointText = formatEndpointText(packetRecord.localAddress, packetRecord.localPort);
        const QString remoteEndpointText = formatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort);
        const QString packetSizeText = QString::number(packetRecord.totalPacketSize);
        const QString payloadSizeText = QString::number(packetRecord.payloadSize);
        const QString previewText = buildPayloadAsciiPreviewText(packetRecord);

        QTableWidgetItem* timeItem = createPacketCell(timeText);
        timeItem->setData(Qt::UserRole, static_cast<qulonglong>(sequenceId));
        tableWidget->setItem(rowIndex, 0, timeItem);
        tableWidget->setItem(rowIndex, 1, createPacketCell(protocolText));
        tableWidget->setItem(rowIndex, 2, createPacketCell(directionText));
        tableWidget->setItem(rowIndex, 3, createPacketCell(pidText));
        QTableWidgetItem* processNameItem = createPacketCell(processNameText);
        processNameItem->setIcon(processIcon);
        tableWidget->setItem(rowIndex, 4, processNameItem);
        tableWidget->setItem(rowIndex, 5, createPacketCell(localEndpointText));
        tableWidget->setItem(rowIndex, 6, createPacketCell(remoteEndpointText));
        tableWidget->setItem(rowIndex, 7, createPacketCell(packetSizeText));
        tableWidget->setItem(rowIndex, 8, createPacketCell(payloadSizeText));
        tableWidget->setItem(rowIndex, 9, createPacketCell(previewText));
    }
}

NetworkDock::NetworkDock(QWidget* parent)
    : QWidget(parent)
{
    // 创建后台服务对象：负责抓包、PID 映射、限速逻辑。
    m_trafficService = std::make_unique<ks::network::TrafficMonitorService>();

    // 初始化界面和连接逻辑。
    initializeUi();
    initializeConnections();

    // 限速页使用定时器轮询刷新规则状态（触发次数、当前窗口字节等）。
    m_rateLimitRefreshTimer = new QTimer(this);
    // 频率适度降低，减少后台快照 + UI 渲染对主线程的周期性压力。
    m_rateLimitRefreshTimer->setInterval(1100);
    connect(m_rateLimitRefreshTimer, &QTimer::timeout, this, [this]()
        {
            // 仅当“进程限速”页可见时刷新，避免隐藏页无意义占用 UI 线程。
            if (m_sideTabWidget == nullptr || m_sideTabWidget->currentWidget() != m_rateLimitPage)
            {
                return;
            }
            refreshRateLimitTable();
        });
    m_rateLimitRefreshTimer->start();

    // 报文批量刷新定时器：
    // - 由 UI 线程周期性批量消费后台队列；
    // - 避免“每包一个 invokeMethod”把事件循环塞爆。
    m_packetFlushTimer = new QTimer(this);
    m_packetFlushTimer->setInterval(20);
    connect(m_packetFlushTimer, &QTimer::timeout, this, [this]()
        {
            flushPendingPacketsToUi();
        });
    m_packetFlushTimer->start();

    // 连接管理刷新定时器：
    // - 用于周期更新 TCP/UDP 表；
    // - 自动刷新关闭或页面不可见时，定时器回调会直接跳过；
    // - 目的是避免隐藏页持续枚举连接造成 UI 卡顿。
    m_connectionRefreshTimer = new QTimer(this);
    m_connectionRefreshTimer->setInterval(2200);
    connect(m_connectionRefreshTimer, &QTimer::timeout, this, [this]()
        {
            if (m_sideTabWidget == nullptr || m_sideTabWidget->currentWidget() != m_connectionManagePage)
            {
                return;
            }
            if (m_autoRefreshConnectionButton != nullptr && !m_autoRefreshConnectionButton->isChecked())
            {
                return;
            }
            refreshConnectionTables();
        });
    m_connectionRefreshTimer->start();

    // 把后台线程回调转发到 UI 线程，保证表格操作线程安全。
    m_trafficService->SetPacketCallback([this](const ks::network::PacketRecord& packetRecord)
        {
            // 抓包线程仅执行“入队”轻量动作，不直接碰 UI 控件。
            std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
            if (m_pendingPacketQueue.size() >= kMaxPendingPacketQueueCount)
            {
                // 队列满时丢弃最旧报文，保持系统持续可用。
                m_pendingPacketQueue.pop_front();
                ++m_droppedPacketCount;
            }
            m_pendingPacketQueue.push_back(packetRecord);
        });

    m_trafficService->SetStatusCallback([this](const std::string& statusText)
        {
            QMetaObject::invokeMethod(this, [this, statusText]()
                {
                    onStatusMessageArrived(statusText);
                }, Qt::QueuedConnection);
        });

    m_trafficService->SetRateLimitActionCallback([this](const ks::network::RateLimitActionEvent& actionEvent)
        {
            QMetaObject::invokeMethod(this, [this, actionEvent]()
                {
                    onRateLimitActionArrived(actionEvent);
                }, Qt::QueuedConnection);
        });

    // 初始化日志。
    kLogEvent initializeEvent;
    info << initializeEvent << "[NetworkDock] 网络面板初始化完成。" << eol;

    // 首次加载不再强制立刻枚举连接：
    // - 如果当前就处于连接管理页，则仍执行一次首刷；
    // - 否则延迟到用户切入该页后由自动刷新触发，减少主线程启动负担。
    if (m_sideTabWidget != nullptr && m_sideTabWidget->currentWidget() == m_connectionManagePage)
    {
        refreshConnectionTables();
    }
    else if (m_connectionStatusLabel != nullptr)
    {
        m_connectionStatusLabel->setText(QStringLiteral("状态：进入此页面后自动刷新"));
    }
}

NetworkDock::~NetworkDock()
{
    // 若有异步停止线程在执行，析构时同步等待一次，确保服务对象仍然有效。
    if (m_monitorStopThread != nullptr && m_monitorStopThread->joinable())
    {
        m_monitorStopThread->join();
    }
    m_monitorStopThread.reset();

    // 窗口销毁前主动停止后台线程，避免析构后回调悬空。
    if (m_trafficService != nullptr)
    {
        m_trafficService->StopCapture();
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[NetworkDock] 网络面板已析构，抓包线程已停止。" << eol;
}


// ============================================================
// 说明：NetworkDock.cpp 作为聚合入口，仅保留公共 include、辅助函数与构造析构。
// 具体业务实现按功能拆分到多个 .inc 文件，便于解耦与控制单文件体积。
// ============================================================

#include "NetworkDock.UiBuild.inc"
#include "NetworkDock.UiConnections.inc"
#include "NetworkDock.MonitorPipeline.inc"
#include "NetworkDock.FilterAndProcessActions.inc"
#include "NetworkDock.RateLimit.inc"
#include "NetworkDock.ConnectionManage.inc"
#include "NetworkDock.ManualRequest.inc"
#include "NetworkDock.NetworkDiagnostics.inc"
#include "NetworkDock.DetailAndUtils.inc"

