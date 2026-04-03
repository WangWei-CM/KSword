#include "NetworkDock.InternalCommon.h"
#include "HttpsProxyService.h"

#include <QApplication>
#include <WinInet.h>

#pragma comment(lib, "Wininet.lib")

using namespace network_dock_detail;

namespace
{
    // HttpsParsedColumn：HTTPS解析表列索引定义。
    enum HttpsParsedColumn
    {
        HttpsParsedColumnTime = 0,
        HttpsParsedColumnSession,
        HttpsParsedColumnClient,
        HttpsParsedColumnHost,
        HttpsParsedColumnEvent,
        HttpsParsedColumnMethod,
        HttpsParsedColumnPath,
        HttpsParsedColumnStatus,
        HttpsParsedColumnTls,
        HttpsParsedColumnAlpn,
        HttpsParsedColumnDetail,
        HttpsParsedColumnCount
    };

    class HttpsParsedDetailWindow final : public QWidget
    {
    public:
        explicit HttpsParsedDetailWindow(const ks::network::HttpsProxyParsedEntry& parsedEntry, QWidget* parent = nullptr)
            : QWidget(parent)
        {
            // wrapFieldHtml 作用：
            // - 把长字段包装成可自动换行的富文本块；
            // - 避免长路径/长主机名把详情窗横向撑出屏幕。
            const auto wrapFieldHtml =
                [](const QString& fieldText) -> QString
                {
                    return QStringLiteral(
                        "<div style='white-space:pre-wrap;word-break:break-all;'>%1</div>")
                        .arg(fieldText.toHtmlEscaped());
                };

            setAttribute(Qt::WA_DeleteOnClose, true);
            setWindowFlag(Qt::Window, true);
            setWindowTitle(QStringLiteral("HTTPS详情 - #%1 %2")
                .arg(parsedEntry.sessionId)
                .arg(parsedEntry.eventTypeText));
            resize(860, 720);
            setMinimumWidth(640);
            setMaximumWidth(980);

            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(8, 8, 8, 8);
            rootLayout->setSpacing(6);

            QLabel* metaLabel = new QLabel(this);
            metaLabel->setWordWrap(true);
            metaLabel->setTextFormat(Qt::RichText);
            metaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            metaLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
            metaLabel->setText(QStringLiteral(
                "时间: %1<br/>客户端: %2<br/>目标: %3:%4<br/>事件: %5<br/>方法: %6<br/>路径: %7<br/>状态码: %8<br/>TLS: %9<br/>ALPN: %10<br/>SNI: %11<br/>详情: %12")
                .arg(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(parsedEntry.timestampMs)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                .arg(wrapFieldHtml(parsedEntry.clientEndpointText))
                .arg(wrapFieldHtml(parsedEntry.targetHostText))
                .arg(parsedEntry.targetPort)
                .arg(wrapFieldHtml(parsedEntry.eventTypeText))
                .arg(wrapFieldHtml(parsedEntry.methodText))
                .arg(wrapFieldHtml(parsedEntry.pathText))
                .arg(parsedEntry.statusCode)
                .arg(wrapFieldHtml(parsedEntry.tlsVersionText))
                .arg(wrapFieldHtml(parsedEntry.alpnText))
                .arg(wrapFieldHtml(parsedEntry.sniText))
                .arg(wrapFieldHtml(parsedEntry.detailText)));
            rootLayout->addWidget(metaLabel);

            QTabWidget* tabWidget = new QTabWidget(this);
            rootLayout->addWidget(tabWidget, 1);

            QWidget* hexPage = new QWidget(tabWidget);
            QVBoxLayout* hexLayout = new QVBoxLayout(hexPage);
            hexLayout->setContentsMargins(0, 0, 0, 0);
            hexLayout->setSpacing(4);

            QLabel* hintLabel = new QLabel(QStringLiteral("下方使用项目内现有 HexEditorWidget 展示 HTTPS 事件原始字节。"), hexPage);
            hintLabel->setWordWrap(true);
            hexLayout->addWidget(hintLabel);

            HexEditorWidget* hexEditorWidget = new HexEditorWidget(hexPage);
            hexEditorWidget->setEditable(false);
            hexEditorWidget->setBytesPerRow(16);
            if (!parsedEntry.rawBytes.isEmpty())
            {
                hexEditorWidget->setByteArray(parsedEntry.rawBytes, 0);
            }
            else
            {
                hexEditorWidget->clearData();
            }
            hexLayout->addWidget(hexEditorWidget, 1);
            tabWidget->addTab(hexPage, QStringLiteral("十六进制"));

            QWidget* textPage = new QWidget(tabWidget);
            QVBoxLayout* textLayout = new QVBoxLayout(textPage);
            textLayout->setContentsMargins(0, 0, 0, 0);
            textLayout->setSpacing(4);

            QPlainTextEdit* textEditor = new QPlainTextEdit(textPage);
            textEditor->setReadOnly(true);
            textEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
            textEditor->setPlainText(QString::fromUtf8(parsedEntry.rawBytes));
            textLayout->addWidget(textEditor, 1);
            tabWidget->addTab(textPage, QStringLiteral("文本"));
        }
    };

    // refreshInternetSettings 作用：
    // - 通知 WinINet 刷新代理配置。
    void refreshInternetSettings()
    {
        ::InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
        ::InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    }

    // writeInternetSettingString 作用：
    // - 把字符串类型代理项写入当前用户 Internet Settings。
    bool writeInternetSettingString(const wchar_t* valueName, const QString& valueText, QString* errorTextOut)
    {
        const std::wstring valueNameText = std::wstring(valueName);
        const std::wstring valueDataText = valueText.toStdWString();
        const LONG resultCode = ::RegSetKeyValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            valueNameText.c_str(),
            REG_SZ,
            valueDataText.c_str(),
            static_cast<DWORD>((valueDataText.size() + 1) * sizeof(wchar_t)));
        if (resultCode != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("写入注册表失败：%1").arg(resultCode);
            }
            return false;
        }
        return true;
    }

    // writeInternetSettingDword 作用：
    // - 把 DWORD 类型代理项写入当前用户 Internet Settings。
    bool writeInternetSettingDword(const wchar_t* valueName, const DWORD valueData, QString* errorTextOut)
    {
        const LONG resultCode = ::RegSetKeyValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            valueName,
            REG_DWORD,
            &valueData,
            sizeof(valueData));
        if (resultCode != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("写入注册表失败：%1").arg(resultCode);
            }
            return false;
        }
        return true;
    }
}

void NetworkDock::initializeHttpsAnalyzeTab()
{
    m_httpsAnalyzePage = new QWidget(this);
    m_httpsAnalyzeLayout = new QVBoxLayout(m_httpsAnalyzePage);
    m_httpsAnalyzeLayout->setContentsMargins(6, 6, 6, 6);
    m_httpsAnalyzeLayout->setSpacing(6);

    m_httpsAnalyzeControlLayout = new QHBoxLayout();
    m_httpsAnalyzeControlLayout->setSpacing(6);

    m_httpsListenAddressEdit = new QLineEdit(QStringLiteral("127.0.0.1"), m_httpsAnalyzePage);
    m_httpsListenAddressEdit->setToolTip(QStringLiteral("HTTPS代理监听地址。"));
    m_httpsListenAddressEdit->setMaximumWidth(140);

    m_httpsListenPortSpin = new QSpinBox(m_httpsAnalyzePage);
    m_httpsListenPortSpin->setRange(1, 65535);
    m_httpsListenPortSpin->setValue(8889);
    m_httpsListenPortSpin->setToolTip(QStringLiteral("HTTPS代理监听端口。"));

    m_httpsStartProxyButton = new QPushButton(QStringLiteral("启动代理"), m_httpsAnalyzePage);
    m_httpsStartProxyButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_httpsStartProxyButton->setToolTip(QStringLiteral("启动本地 HTTPS 解析代理。"));

    m_httpsStopProxyButton = new QPushButton(QStringLiteral("停止代理"), m_httpsAnalyzePage);
    m_httpsStopProxyButton->setIcon(QIcon(":/Icon/process_pause.svg"));
    m_httpsStopProxyButton->setToolTip(QStringLiteral("停止本地 HTTPS 解析代理。"));

    m_httpsTrustCertButton = new QPushButton(QStringLiteral("信任证书"), m_httpsAnalyzePage);
    m_httpsTrustCertButton->setIcon(QIcon(":/Icon/process_details.svg"));
    m_httpsTrustCertButton->setToolTip(QStringLiteral("一键生成并信任 HTTPS 代理根证书。"));

    m_httpsApplyProxyButton = new QPushButton(QStringLiteral("应用系统代理"), m_httpsAnalyzePage);
    m_httpsApplyProxyButton->setIcon(QIcon(":/Icon/process_main.svg"));
    m_httpsApplyProxyButton->setToolTip(QStringLiteral("把系统代理切换到本地 HTTPS 代理。"));

    m_httpsClearProxyButton = new QPushButton(QStringLiteral("清理系统代理"), m_httpsAnalyzePage);
    m_httpsClearProxyButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_httpsClearProxyButton->setToolTip(QStringLiteral("清除系统代理配置，恢复直连。"));

    m_httpsProxyStatusLabel = new QLabel(QStringLiteral("状态：HTTPS代理未启动"), m_httpsAnalyzePage);
    m_httpsProxyStatusLabel->setWordWrap(true);
    m_httpsProxyStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_httpsAnalyzeControlLayout->addWidget(new QLabel(QStringLiteral("监听地址:"), m_httpsAnalyzePage));
    m_httpsAnalyzeControlLayout->addWidget(m_httpsListenAddressEdit);
    m_httpsAnalyzeControlLayout->addWidget(new QLabel(QStringLiteral("端口:"), m_httpsAnalyzePage));
    m_httpsAnalyzeControlLayout->addWidget(m_httpsListenPortSpin);
    m_httpsAnalyzeControlLayout->addWidget(m_httpsStartProxyButton);
    m_httpsAnalyzeControlLayout->addWidget(m_httpsStopProxyButton);
    m_httpsAnalyzeControlLayout->addWidget(m_httpsTrustCertButton);
    m_httpsAnalyzeControlLayout->addWidget(m_httpsApplyProxyButton);
    m_httpsAnalyzeControlLayout->addWidget(m_httpsClearProxyButton);
    m_httpsAnalyzeControlLayout->addWidget(m_httpsProxyStatusLabel, 1);
    m_httpsAnalyzeLayout->addLayout(m_httpsAnalyzeControlLayout);

    m_httpsParsedTable = new QTableWidget(m_httpsAnalyzePage);
    m_httpsParsedTable->setColumnCount(HttpsParsedColumnCount);
    m_httpsParsedTable->setHorizontalHeaderLabels({
        QStringLiteral("时间"),
        QStringLiteral("会话"),
        QStringLiteral("客户端"),
        QStringLiteral("主机"),
        QStringLiteral("事件"),
        QStringLiteral("方法"),
        QStringLiteral("路径"),
        QStringLiteral("状态码"),
        QStringLiteral("TLS"),
        QStringLiteral("ALPN"),
        QStringLiteral("详情")
        });
    m_httpsParsedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_httpsParsedTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_httpsParsedTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_httpsParsedTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_httpsParsedTable->verticalHeader()->setVisible(false);
    m_httpsParsedTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_httpsParsedTable->horizontalHeader()->setSectionResizeMode(HttpsParsedColumnDetail, QHeaderView::Stretch);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnTime, 120);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnSession, 70);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnClient, 140);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnHost, 160);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnEvent, 90);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnMethod, 70);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnPath, 220);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnStatus, 70);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnTls, 80);
    m_httpsParsedTable->setColumnWidth(HttpsParsedColumnAlpn, 80);
    m_httpsAnalyzeLayout->addWidget(m_httpsParsedTable, 1);

    m_httpsProxyLogOutput = new QPlainTextEdit(m_httpsAnalyzePage);
    m_httpsProxyLogOutput->setReadOnly(true);
    m_httpsProxyLogOutput->setMaximumBlockCount(600);
    m_httpsProxyLogOutput->setPlaceholderText(QStringLiteral("HTTPS 代理启动、证书安装和解析异常会显示在这里。"));
    m_httpsProxyLogOutput->setFixedHeight(150);
    m_httpsAnalyzeLayout->addWidget(m_httpsProxyLogOutput, 0);

    m_sideTabWidget->addTab(m_httpsAnalyzePage, QIcon(":/Icon/process_details.svg"), QStringLiteral("HTTPS解析"));
    updateHttpsProxyStatusLabel(QStringLiteral("状态：HTTPS代理未启动"));

    connect(m_httpsParsedTable, &QTableWidget::cellDoubleClicked, this, [this](const int row, const int /*column*/)
        {
            openHttpsParsedDetailByRow(row);
        });
    connect(m_httpsParsedTable, &QWidget::customContextMenuRequested, this, [this](const QPoint& localPosition)
        {
            if (m_httpsParsedTable == nullptr)
            {
                return;
            }

            const QTableWidgetItem* clickedItem = m_httpsParsedTable->itemAt(localPosition);
            if (clickedItem == nullptr)
            {
                return;
            }

            QMenu contextMenu(this);
            QAction* detailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("查看详情"));
            QAction* copyAction = contextMenu.addAction(QIcon(":/Icon/log_copy.svg"), QStringLiteral("复制详情文本"));
            QAction* selectedAction = contextMenu.exec(m_httpsParsedTable->viewport()->mapToGlobal(localPosition));
            if (selectedAction == detailAction)
            {
                openHttpsParsedDetailByRow(clickedItem->row());
            }
            else if (selectedAction == copyAction)
            {
                const int rowIndex = clickedItem->row();
                if (rowIndex >= 0 && rowIndex < static_cast<int>(m_httpsParsedEntryCache.size()))
                {
                    const ks::network::HttpsProxyParsedEntry& parsedEntry = m_httpsParsedEntryCache[static_cast<std::size_t>(rowIndex)];
                    QApplication::clipboard()->setText(QString::fromUtf8(parsedEntry.rawBytes));
                }
            }
        });
}

void NetworkDock::startHttpsProxyService()
{
    if (m_httpsProxyService == nullptr)
    {
        appendHttpsProxyLogLine(QStringLiteral("HTTPS代理服务尚未初始化。"));
        return;
    }

    const QHostAddress listenAddress(m_httpsListenAddressEdit != nullptr ? m_httpsListenAddressEdit->text().trimmed() : QStringLiteral("127.0.0.1"));
    if (listenAddress.isNull())
    {
        appendHttpsProxyLogLine(QStringLiteral("监听地址无效。"));
        QMessageBox::warning(this, QStringLiteral("HTTPS解析"), QStringLiteral("监听地址无效。"));
        return;
    }

    const std::uint16_t listenPort = static_cast<std::uint16_t>(m_httpsListenPortSpin != nullptr ? m_httpsListenPortSpin->value() : 8889);
    QString errorText;
    if (!m_httpsProxyService->start(listenAddress, listenPort, &errorText))
    {
        appendHttpsProxyLogLine(QStringLiteral("启动失败：%1").arg(errorText));
        QMessageBox::warning(this, QStringLiteral("HTTPS解析"), QStringLiteral("启动 HTTPS 代理失败：\n%1").arg(errorText));
        return;
    }

    m_httpsProxyRunning = true;
    updateHttpsProxyStatusLabel(QStringLiteral("状态：HTTPS代理已启动，监听 %1:%2")
        .arg(listenAddress.toString())
        .arg(listenPort));
}

void NetworkDock::stopHttpsProxyService()
{
    if (m_httpsProxyService == nullptr)
    {
        return;
    }

    m_httpsProxyService->stop();
    m_httpsProxyRunning = false;
    updateHttpsProxyStatusLabel(QStringLiteral("状态：HTTPS代理已停止"));
}

void NetworkDock::ensureHttpsRootCertificateTrusted()
{
    if (m_httpsProxyService == nullptr)
    {
        appendHttpsProxyLogLine(QStringLiteral("HTTPS代理服务尚未初始化。"));
        return;
    }

    QString errorText;
    if (!m_httpsProxyService->ensureRootCertificate(true, &errorText))
    {
        appendHttpsProxyLogLine(QStringLiteral("信任证书失败：%1").arg(errorText));
        QMessageBox::warning(this, QStringLiteral("HTTPS解析"), QStringLiteral("信任根证书失败：\n%1").arg(errorText));
        return;
    }

    appendHttpsProxyLogLine(QStringLiteral("HTTPS 根证书已生成并导入当前用户信任根。"));
    updateHttpsProxyStatusLabel(QStringLiteral("状态：根证书已信任，可启动代理"));
}

void NetworkDock::applyHttpsSystemProxy()
{
    const QString listenAddressText = (m_httpsListenAddressEdit != nullptr)
        ? m_httpsListenAddressEdit->text().trimmed()
        : QStringLiteral("127.0.0.1");
    const QString proxyServerText = QStringLiteral("https=%1:%2")
        .arg(listenAddressText)
        .arg(m_httpsListenPortSpin != nullptr ? m_httpsListenPortSpin->value() : 8889);

    QString errorText;
    if (!writeInternetSettingString(L"ProxyServer", proxyServerText, &errorText)
        || !writeInternetSettingString(L"ProxyOverride", QStringLiteral("localhost;127.*;<local>"), &errorText)
        || !writeInternetSettingDword(L"ProxyEnable", 1, &errorText))
    {
        appendHttpsProxyLogLine(QStringLiteral("应用系统代理失败：%1").arg(errorText));
        QMessageBox::warning(this, QStringLiteral("HTTPS解析"), QStringLiteral("应用系统代理失败：\n%1").arg(errorText));
        return;
    }

    refreshInternetSettings();
    appendHttpsProxyLogLine(QStringLiteral("系统代理已切换到 %1。").arg(proxyServerText));
}

void NetworkDock::clearHttpsSystemProxy()
{
    QString errorText;
    if (!writeInternetSettingDword(L"ProxyEnable", 0, &errorText))
    {
        appendHttpsProxyLogLine(QStringLiteral("清理系统代理失败：%1").arg(errorText));
        QMessageBox::warning(this, QStringLiteral("HTTPS解析"), QStringLiteral("清理系统代理失败：\n%1").arg(errorText));
        return;
    }

    refreshInternetSettings();
    appendHttpsProxyLogLine(QStringLiteral("系统代理已关闭。"));
}

void NetworkDock::onHttpsProxyParsedEntryArrived(const ks::network::HttpsProxyParsedEntry& parsedEntry)
{
    if (m_httpsParsedTable == nullptr)
    {
        return;
    }

    m_httpsParsedEntryCache.push_back(parsedEntry);
    const int rowIndex = m_httpsParsedTable->rowCount();
    m_httpsParsedTable->insertRow(rowIndex);

    const QString timeText = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(parsedEntry.timestampMs)).toString(QStringLiteral("HH:mm:ss.zzz"));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnTime, createPacketCell(timeText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnSession, createPacketCell(QString::number(parsedEntry.sessionId)));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnClient, createPacketCell(parsedEntry.clientEndpointText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnHost, createPacketCell(QStringLiteral("%1:%2").arg(parsedEntry.targetHostText).arg(parsedEntry.targetPort)));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnEvent, createPacketCell(parsedEntry.eventTypeText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnMethod, createPacketCell(parsedEntry.methodText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnPath, createPacketCell(parsedEntry.pathText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnStatus, createPacketCell(parsedEntry.statusCode > 0 ? QString::number(parsedEntry.statusCode) : QString()));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnTls, createPacketCell(parsedEntry.tlsVersionText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnAlpn, createPacketCell(parsedEntry.alpnText));
    m_httpsParsedTable->setItem(rowIndex, HttpsParsedColumnDetail, createPacketCell(parsedEntry.detailText));
    m_httpsParsedTable->scrollToBottom();
}

void NetworkDock::openHttpsParsedDetailByRow(const int row)
{
    if (row < 0 || row >= static_cast<int>(m_httpsParsedEntryCache.size()))
    {
        return;
    }

    HttpsParsedDetailWindow* detailWindow =
        new HttpsParsedDetailWindow(m_httpsParsedEntryCache[static_cast<std::size_t>(row)], nullptr);
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();
}

void NetworkDock::appendHttpsProxyLogLine(const QString& logLine)
{
    if (m_httpsProxyLogOutput == nullptr)
    {
        return;
    }

    const QString prefixedLine = QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
        .arg(logLine);
    m_httpsProxyLogOutput->appendPlainText(prefixedLine);
}

void NetworkDock::updateHttpsProxyStatusLabel(const QString& statusText)
{
    if (m_httpsProxyStatusLabel != nullptr)
    {
        m_httpsProxyStatusLabel->setText(statusText);
    }
    if (m_httpsStartProxyButton != nullptr)
    {
        m_httpsStartProxyButton->setEnabled(!m_httpsProxyRunning);
    }
    if (m_httpsStopProxyButton != nullptr)
    {
        m_httpsStopProxyButton->setEnabled(m_httpsProxyRunning);
    }
}
