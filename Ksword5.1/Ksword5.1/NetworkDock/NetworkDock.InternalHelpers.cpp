#include "NetworkDock.InternalHelpers.h"

#include "../UI/HexEditorWidget.h"
#include "../ksword/network/network_format_tools.h"
#include "../theme.h"

#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace network_dock_detail
{
    namespace
    {
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
                "QMenu{"
                "  background:%6;"
                "  color:%2;"
                "  border:1px solid %4;"
                "}"
                "QMenu::item:selected{"
                "  background:%7;"
                "  color:#FFFFFF;"
                "}"
                "QMenu::separator{"
                "  height:1px;"
                "  background:%4;"
                "  margin:2px 6px;"
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
                const QString timeText = toQString(
                    ks::network::FormatUnixTimestampMs(packetRecord.captureTimestampMs, true));
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

                // 详情页签：当前仅保留十六进制视图页。
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
            }
        };
    }

    QString toQString(const std::string& textValue)
    {
        return QString::fromUtf8(textValue.c_str());
    }

    QString formatEndpointText(const std::string& ipAddress, const std::uint16_t portNumber)
    {
        return toQString(ks::network::FormatEndpointText(ipAddress, portNumber));
    }

    QString formatBytesToHexText(const std::vector<std::uint8_t>& byteArray, const std::size_t maxBytesToRender)
    {
        return toQString(ks::network::FormatBytesToHexPreview(byteArray, maxBytesToRender));
    }

    std::pair<std::size_t, std::size_t> buildPayloadByteRange(const ks::network::PacketRecord& packetRecord)
    {
        const ks::network::PayloadByteRange payloadRange =
            ks::network::BuildPayloadByteRange(packetRecord);
        return { payloadRange.offset, payloadRange.length };
    }

    QString buildPayloadAsciiPreviewText(const ks::network::PacketRecord& packetRecord)
    {
        return toQString(ks::network::BuildPayloadAsciiPreviewText(packetRecord));
    }

    QString buildPayloadAsciiFullText(const ks::network::PacketRecord& packetRecord)
    {
        return toQString(ks::network::BuildPayloadAsciiFullText(packetRecord));
    }

    QString buildPayloadHexFullText(const ks::network::PacketRecord& packetRecord)
    {
        return toQString(ks::network::BuildPayloadHexFullText(packetRecord));
    }

    QString buildPacketHexAsciiDumpText(const ks::network::PacketRecord& packetRecord)
    {
        return toQString(ks::network::BuildPacketHexAsciiDumpText(packetRecord));
    }

    QString buildPacketCopyHeaderLine(const ks::network::PacketRecord& packetRecord)
    {
        return toQString(ks::network::BuildPacketCopyHeaderLine(packetRecord));
    }

    QString formatIpv4HostOrder(const std::uint32_t ipv4HostOrder)
    {
        return toQString(ks::network::FormatIpv4HostOrder(ipv4HostOrder));
    }

    bool tryParseIpv4Text(const QString& ipv4Text, std::uint32_t& ipv4HostOrderOut)
    {
        return ks::network::TryParseIpv4Text(ipv4Text.trimmed().toStdString(), &ipv4HostOrderOut);
    }

    bool tryParseIpv4RangeText(
        const QString& rangeText,
        std::pair<std::uint32_t, std::uint32_t>& rangeOut,
        QString& normalizeTextOut)
    {
        std::string normalizedText;
        const bool parseOk = ks::network::TryParseIpv4RangeText(
            rangeText.trimmed().toStdString(),
            &rangeOut,
            &normalizedText);
        normalizeTextOut = toQString(normalizedText);
        return parseOk;
    }

    bool tryParsePortRangeText(
        const QString& rangeText,
        std::pair<std::uint16_t, std::uint16_t>& rangeOut,
        QString& normalizeTextOut)
    {
        std::string normalizedText;
        const bool parseOk = ks::network::TryParsePortRangeText(
            rangeText.trimmed().toStdString(),
            &rangeOut,
            &normalizedText);
        normalizeTextOut = toQString(normalizedText);
        return parseOk;
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

        const QString timeText = toQString(
            ks::network::FormatUnixTimestampMs(packetRecord.captureTimestampMs, false));
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


