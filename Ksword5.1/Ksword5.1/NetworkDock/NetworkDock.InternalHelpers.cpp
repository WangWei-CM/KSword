#include "NetworkDock.InternalHelpers.h"

#include "../UI/HexEditorWidget.h"
#include "../theme.h"

#include <QDateTime>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm> // std::min/std::max：预览长度与范围标准化。

namespace network_dock_detail
{
    namespace
    {
        // kHexBytesPerRow：
        // - 报文详情窗口每行显示 16 字节；
        // - 与内存模块十六进制查看器保持一致。
        constexpr int kHexBytesPerRow = 16;

        // isVisibleAsciiByte：
        // - 判断字节是否属于可直接阅读的可见 ASCII 字符；
        // - 用于预览提取和十六进制旁路 ASCII 渲染。
        bool isVisibleAsciiByte(const std::uint8_t byteValue)
        {
            return byteValue >= 32 && byteValue <= 126;
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

        // buildPacketDetailWindowStyle 作用：
        // - 为“流量监控 -> 报文详情”窗口生成独立主题样式；
        // - 重点覆盖 QPlainTextEdit/QTabWidget/Page，修复深色模式下白底残留。
        QString buildPacketDetailWindowStyle()
        {
            const QString windowBackground = KswordTheme::SurfaceHex();
            const QString panelBackground = KswordTheme::IsDarkModeEnabled()
                ? QStringLiteral("#141414")
                : QStringLiteral("#F8FAFC");
            const QString inputBackground = KswordTheme::IsDarkModeEnabled()
                ? QStringLiteral("#101010")
                : QStringLiteral("#FFFFFF");
            const QString borderColor = KswordTheme::BorderHex();
            const QString textColor = KswordTheme::TextPrimaryHex();
            const QString secondaryTextColor = KswordTheme::TextSecondaryHex();
            const QString accentColor = KswordTheme::PrimaryBlueHex;

            return QStringLiteral(
                "QWidget{"
                "  background:%1;"
                "  color:%2;"
                "}"
                "QLabel{"
                "  background:transparent;"
                "  color:%2;"
                "}"
                "QTabWidget::pane{"
                "  background:%3;"
                "  border:1px solid %4;"
                "  border-radius:4px;"
                "}"
                "QTabBar::tab{"
                "  background:%1;"
                "  color:%5;"
                "  border:1px solid %4;"
                "  padding:6px 12px;"
                "  margin-right:2px;"
                "  border-top-left-radius:4px;"
                "  border-top-right-radius:4px;"
                "}"
                "QTabBar::tab:selected{"
                "  background:%3;"
                "  color:%2;"
                "  border-bottom-color:%3;"
                "}"
                "QPlainTextEdit{"
                "  background:%6;"
                "  color:%2;"
                "  border:1px solid %4;"
                "  selection-background-color:%7;"
                "  selection-color:#FFFFFF;"
                "}"
                "QScrollBar:vertical,QScrollBar:horizontal{"
                "  background:%3;"
                "}"
                "QScrollBar::handle:vertical,QScrollBar::handle:horizontal{"
                "  background:%7;"
                "}")
                .arg(windowBackground)
                .arg(textColor)
                .arg(panelBackground)
                .arg(borderColor)
                .arg(secondaryTextColor)
                .arg(inputBackground)
                .arg(accentColor);
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
                setAttribute(Qt::WA_StyledBackground, true);
                setAutoFillBackground(true);
                setWindowTitle(QStringLiteral("报文详情 - #%1").arg(packetRecord.sequenceId));
                resize(1120, 760);
                setStyleSheet(buildPacketDetailWindowStyle());

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
                metaLabel->setStyleSheet(QStringLiteral("padding:6px 8px;border:1px solid %1;border-radius:4px;")
                    .arg(KswordTheme::BorderHex()));
                rootLayout->addWidget(metaLabel);

                // 可读摘要：先展示“内容预览列同款”的语义化 ASCII 摘要，便于快速判断协议文本。
                QLabel* readablePreviewLabel = new QLabel(this);
                readablePreviewLabel->setText(QStringLiteral("可读ASCII摘要: %1").arg(buildPayloadAsciiPreviewText(packetRecord)));
                readablePreviewLabel->setWordWrap(true);
                readablePreviewLabel->setToolTip(QStringLiteral("该摘要优先提取 payload 中可读 ASCII 片段。"));
                readablePreviewLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
                rootLayout->addWidget(readablePreviewLabel);

                // 详情页签：把十六进制视图与 ASCII 文本视图分开，提升阅读与复制体验。
                QTabWidget* detailTabWidget = new QTabWidget(this);
                rootLayout->addWidget(detailTabWidget, 1);

                // 十六进制页：统一复用 HexEditorWidget，功能与内存/文件模块保持一致。
                QWidget* hexPage = new QWidget(detailTabWidget);
                QVBoxLayout* hexPageLayout = new QVBoxLayout(hexPage);
                hexPageLayout->setContentsMargins(0, 0, 0, 0);
                hexPageLayout->setSpacing(4);

                QLabel* hexHintLabel = new QLabel(
                    QStringLiteral("十六进制区域支持 Ctrl+F 异步查找、Ctrl+G 跳转、批量复制与导出。"),
                    hexPage);
                hexHintLabel->setWordWrap(true);
                hexHintLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
                hexPageLayout->addWidget(hexHintLabel);

                HexEditorWidget* hexEditorWidget = new HexEditorWidget(hexPage);
                hexEditorWidget->setEditable(false);
                hexEditorWidget->setBytesPerRow(16);
                if (!packetRecord.packetBytes.empty())
                {
                    const QByteArray packetBytes(
                        reinterpret_cast<const char*>(packetRecord.packetBytes.data()),
                        static_cast<int>(packetRecord.packetBytes.size()));
                    hexEditorWidget->setByteArray(packetBytes, 0);
                }
                else
                {
                    hexEditorWidget->clearData();
                }
                hexPageLayout->addWidget(hexEditorWidget, 1);

                detailTabWidget->addTab(hexPage, QStringLiteral("十六进制"));

                // ASCII 页：展示 payload 的文本化内容，优先保留 CRLF 语义。
                QWidget* asciiPage = new QWidget(detailTabWidget);
                QVBoxLayout* asciiPageLayout = new QVBoxLayout(asciiPage);
                asciiPageLayout->setContentsMargins(0, 0, 0, 0);
                asciiPageLayout->setSpacing(4);

                QLabel* asciiHintLabel = new QLabel(QStringLiteral("下方为 payload 的 ASCII 视图（不可打印字节以 '.' 代替）。"), asciiPage);
                asciiHintLabel->setWordWrap(true);
                asciiHintLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
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
    }

    QString toQString(const std::string& textValue)
    {
        return QString::fromUtf8(textValue.c_str());
    }

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

    QString formatIpv4HostOrder(const std::uint32_t ipv4HostOrder)
    {
        const std::uint32_t octet1 = (ipv4HostOrder >> 24) & 0xFFU;
        const std::uint32_t octet2 = (ipv4HostOrder >> 16) & 0xFFU;
        const std::uint32_t octet3 = (ipv4HostOrder >> 8) & 0xFFU;
        const std::uint32_t octet4 = ipv4HostOrder & 0xFFU;
        return QString("%1.%2.%3.%4").arg(octet1).arg(octet2).arg(octet3).arg(octet4);
    }

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

    QTableWidgetItem* createPacketCell(const QString& cellText)
    {
        QTableWidgetItem* tableItem = new QTableWidgetItem(cellText);
        tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        return tableItem;
    }

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

    void showPacketDetailWindow(const ks::network::PacketRecord& packetRecord)
    {
        // 详情窗口使用 show() 非模态弹出，不阻塞主 UI。
        // 独立窗口：不挂在 Dock 下，避免主窗口布局变化影响详情窗口。
        PacketDetailWindow* detailWindow = new PacketDetailWindow(packetRecord, nullptr);
        detailWindow->setWindowFlag(Qt::Window, true);
        detailWindow->show();
        detailWindow->raise();
        detailWindow->activateWindow();
    }
}


