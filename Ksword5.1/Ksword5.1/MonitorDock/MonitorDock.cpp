
#include "MonitorDock.h"

// 监控页实现：包含 WMI/ETW 两个标签页，所有重活走异步线程。
#include "../ProcessDock/ProcessDetailWindow.h"
#include "../theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolBox>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Wbemidl.h>
#include <Objbase.h>
#include <comdef.h>
#include <atlbase.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#pragma comment(lib, "Wbemuuid.lib")
#pragma comment(lib, "Tdh.lib")

namespace
{
    // 按主题统一按钮风格。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:#FFFFFF;border:1px solid %2;border-radius:3px;padding:4px 8px;}"
            "QPushButton:hover{background:%3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // 输入框统一样式。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit,QPlainTextEdit,QComboBox,QSpinBox{border:1px solid #C8DDF4;border-radius:3px;background:#FFFFFF;padding:2px 6px;}"
            "QLineEdit:focus,QPlainTextEdit:focus,QComboBox:focus,QSpinBox:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 表头统一样式。
    QString blueHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:#FFFFFF;border:1px solid #E6E6E6;padding:4px;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 线程内 COM 初始化。
    bool initCom(QString* errorOut)
    {
        if (errorOut != nullptr)
        {
            errorOut->clear();
        }

        const HRESULT initResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
        {
            if (errorOut != nullptr)
            {
                *errorOut = QString("CoInitializeEx失败:0x%1").arg(static_cast<unsigned long>(initResult), 0, 16);
            }
            return false;
        }

        const HRESULT secResult = ::CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr);

        if (FAILED(secResult) && secResult != RPC_E_TOO_LATE)
        {
            if (errorOut != nullptr)
            {
                *errorOut = QString("CoInitializeSecurity失败:0x%1").arg(static_cast<unsigned long>(secResult), 0, 16);
            }
            return false;
        }

        return true;
    }

    // VARIANT 转 QString。
    QString variantToText(const VARIANT& value)
    {
        switch (value.vt)
        {
        case VT_BSTR:
            return value.bstrVal != nullptr ? QString::fromWCharArray(value.bstrVal) : QString();
        case VT_I4:
        case VT_INT:
            return QString::number(value.intVal);
        case VT_UI4:
        case VT_UINT:
            return QString::number(value.uintVal);
        case VT_I8:
            return QString::number(static_cast<qint64>(value.llVal));
        case VT_UI8:
            return QString::number(static_cast<qulonglong>(value.ullVal));
        case VT_BOOL:
            return value.boolVal == VARIANT_TRUE ? QStringLiteral("true") : QStringLiteral("false");
        case VT_NULL:
        case VT_EMPTY:
            return QStringLiteral("<null>");
        default:
            return QString("<vt=%1>").arg(value.vt);
        }
    }

    // 文本匹配辅助：
    // - 支持普通 contains 与正则匹配两种模式；
    // - 支持大小写敏感切换；
    // - pattern 为空时视为“匹配通过”。
    bool textMatch(
        const QString& sourceText,
        const QString& patternText,
        const bool useRegex,
        const Qt::CaseSensitivity caseSensitivity)
    {
        if (patternText.trimmed().isEmpty())
        {
            return true;
        }

        if (!useRegex)
        {
            return sourceText.contains(patternText, caseSensitivity);
        }

        QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
        if (caseSensitivity == Qt::CaseInsensitive)
        {
            options |= QRegularExpression::CaseInsensitiveOption;
        }
        const QRegularExpression regex(patternText, options);
        if (!regex.isValid())
        {
            return false;
        }
        return regex.match(sourceText).hasMatch();
    }

    // 连接 ROOT\\CIMV2。
    bool connectWmi(IWbemServices** serviceOut, QString* errorOut)
    {
        if (serviceOut == nullptr)
        {
            return false;
        }
        *serviceOut = nullptr;

        CComPtr<IWbemLocator> locator;
        const HRESULT createResult = ::CoCreateInstance(
            CLSID_WbemLocator,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IWbemLocator,
            reinterpret_cast<void**>(&locator));

        if (FAILED(createResult) || locator == nullptr)
        {
            if (errorOut != nullptr)
            {
                *errorOut = QString("创建WbemLocator失败:0x%1").arg(static_cast<unsigned long>(createResult), 0, 16);
            }
            return false;
        }

        CComPtr<IWbemServices> service;
        const HRESULT connectResult = locator->ConnectServer(
            _bstr_t(L"ROOT\\CIMV2"),
            nullptr,
            nullptr,
            nullptr,
            WBEM_FLAG_CONNECT_USE_MAX_WAIT,
            nullptr,
            nullptr,
            &service);

        if (FAILED(connectResult) || service == nullptr)
        {
            if (errorOut != nullptr)
            {
                *errorOut = QString("连接ROOT\\CIMV2失败:0x%1").arg(static_cast<unsigned long>(connectResult), 0, 16);
            }
            return false;
        }

        const HRESULT blanketResult = ::CoSetProxyBlanket(
            service,
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE);

        if (FAILED(blanketResult))
        {
            if (errorOut != nullptr)
            {
                *errorOut = QString("CoSetProxyBlanket失败:0x%1").arg(static_cast<unsigned long>(blanketResult), 0, 16);
            }
            return false;
        }

        *serviceOut = service.Detach();
        return true;
    }

    // GUID 转文本。
    QString guidToText(const GUID& guidValue)
    {
        wchar_t buffer[64] = {};
        if (::StringFromGUID2(guidValue, buffer, static_cast<int>(std::size(buffer))) <= 0)
        {
            return QStringLiteral("{00000000-0000-0000-0000-000000000000}");
        }
        return QString::fromWCharArray(buffer);
    }

    // 从 "pid/name" 文本里提取 PID。
    bool parsePid(const QString& text, std::uint32_t& pidOut)
    {
        pidOut = 0;
        const QRegularExpression regex(QStringLiteral("(\\d+)"));
        const QRegularExpressionMatch match = regex.match(text);
        if (!match.hasMatch())
        {
            return false;
        }

        bool ok = false;
        const std::uint32_t pid = match.captured(1).toUInt(&ok);
        if (!ok || pid == 0)
        {
            return false;
        }

        pidOut = pid;
        return true;
    }

    // 打开进程详情窗口。
    void openProcessDetail(QWidget* parent, std::uint32_t pid)
    {
        if (pid == 0)
        {
            return;
        }

        ks::process::ProcessRecord record;
        bool ok = ks::process::QueryProcessStaticDetailByPid(pid, record);

        if (!ok)
        {
            std::vector<ks::process::ProcessRecord> list = ks::process::EnumerateProcesses(
                ks::process::ProcessEnumStrategy::Auto);
            const auto foundIt = std::find_if(
                list.begin(),
                list.end(),
                [pid](const ks::process::ProcessRecord& item) {
                    return item.pid == pid;
                });
            if (foundIt != list.end())
            {
                record = *foundIt;
                ok = true;
            }
        }

        if (!ok)
        {
            QMessageBox::warning(parent, QStringLiteral("进程详情"), QStringLiteral("未找到 PID=%1").arg(pid));
            return;
        }

        ProcessDetailWindow* window = new ProcessDetailWindow(record, nullptr);
        window->setAttribute(Qt::WA_DeleteOnClose, true);
        window->show();
        window->raise();
        window->activateWindow();
    }

    // 当前时间转 100ns 文本。
    QString now100nsText()
    {
        FILETIME fileTime{};
        ::GetSystemTimeAsFileTime(&fileTime);

        ULARGE_INTEGER value{};
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return QString::number(static_cast<qulonglong>(value.QuadPart));
    }

    // 文本 GUID 转结构 GUID：支持 "{...}" 或 "..." 两种输入。
    bool parseGuidText(const QString& text, GUID& guidOut)
    {
        QString normalized = text.trimmed();
        if (normalized.isEmpty())
        {
            return false;
        }
        if (!normalized.startsWith('{'))
        {
            normalized = QStringLiteral("{%1}").arg(normalized);
        }

        const std::wstring guidTextWide = normalized.toStdWString();
        HRESULT hr = ::CLSIDFromString(
            const_cast<LPOLESTR>(guidTextWide.c_str()),
            &guidOut);
        return SUCCEEDED(hr);
    }

    // ETW 级别文本映射到 TRACE_LEVEL_*。
    UCHAR etwLevelFromText(const QString& levelText)
    {
        if (levelText.contains(QStringLiteral("Critical"), Qt::CaseInsensitive))
        {
            return TRACE_LEVEL_CRITICAL;
        }
        if (levelText.contains(QStringLiteral("Error"), Qt::CaseInsensitive))
        {
            return TRACE_LEVEL_ERROR;
        }
        if (levelText.contains(QStringLiteral("Warning"), Qt::CaseInsensitive))
        {
            return TRACE_LEVEL_WARNING;
        }
        if (levelText.contains(QStringLiteral("Verbose"), Qt::CaseInsensitive))
        {
            return TRACE_LEVEL_VERBOSE;
        }
        return TRACE_LEVEL_INFORMATION;
    }

    // 关键字掩码解析：支持 "0x..." 与十进制文本。
    ULONGLONG parseKeywordMaskText(const QString& maskText)
    {
        const QString trimmed = maskText.trimmed();
        if (trimmed.isEmpty())
        {
            return 0ULL;
        }

        bool ok = false;
        ULONGLONG mask = 0ULL;
        if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            mask = trimmed.mid(2).toULongLong(&ok, 16);
        }
        else
        {
            mask = trimmed.toULongLong(&ok, 16);
            if (!ok)
            {
                mask = trimmed.toULongLong(&ok, 10);
            }
        }
        return ok ? mask : 0ULL;
    }

    // ETW 事件名解析：通过 TDH 元数据读 EventName，失败则返回空。
    QString queryEtwEventName(const EVENT_RECORD* eventRecord)
    {
        if (eventRecord == nullptr)
        {
            return QString();
        }

        DWORD bufferSize = 0;
        ULONG status = ::TdhGetEventInformation(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            nullptr,
            &bufferSize);
        if (status != ERROR_INSUFFICIENT_BUFFER || bufferSize == 0)
        {
            return QString();
        }

        std::vector<unsigned char> buffer(bufferSize);
        auto* info = reinterpret_cast<PTRACE_EVENT_INFO>(buffer.data());
        status = ::TdhGetEventInformation(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            info,
            &bufferSize);
        if (status != ERROR_SUCCESS || info == nullptr || info->EventNameOffset == 0)
        {
            return QString();
        }

        const wchar_t* namePtr = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const unsigned char*>(info) + info->EventNameOffset);
        if (namePtr == nullptr || *namePtr == L'\0')
        {
            return QString();
        }
        return QString::fromWCharArray(namePtr);
    }

    // 100ns 时间戳文本格式化：直接输出 FILETIME 基准整数，满足计划要求。
    QString etwTimestamp100nsText(const EVENT_RECORD* eventRecord)
    {
        if (eventRecord == nullptr)
        {
            return now100nsText();
        }
        return QString::number(static_cast<qulonglong>(eventRecord->EventHeader.TimeStamp.QuadPart));
    }
}

MonitorDock::MonitorDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent event;
    info << event << "[MonitorDock] 构造开始。" << eol;

    initializeUi();
    initializeConnections();

    // WMI 事件表刷新节流：后台线程先入队，主线程按 100ms 批量刷入，避免事件风暴卡 UI。
    m_wmiUiUpdateTimer = new QTimer(this);
    m_wmiUiUpdateTimer->setInterval(100);
    connect(m_wmiUiUpdateTimer, &QTimer::timeout, this, [this]() {
        flushWmiPendingRows();
    });

    refreshWmiProvidersAsync();
    refreshWmiEventClassesAsync();
    refreshEtwProvidersAsync();

    kLogEvent finishEvent;
    info << finishEvent << "[MonitorDock] 构造完成。" << eol;
}

MonitorDock::~MonitorDock()
{
    // 析构阶段必须同步等待线程退出，防止对象释放后后台线程仍访问成员。
    stopWmiSubscriptionInternal(true);
    stopEtwCaptureInternal(true);

    if (m_wmiUiUpdateTimer != nullptr)
    {
        m_wmiUiUpdateTimer->stop();
    }

    if (m_etwUiUpdateTimer != nullptr)
    {
        m_etwUiUpdateTimer->stop();
    }
}

void MonitorDock::initializeUi()
{
    // 根布局和总 Tab。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_sideTabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_sideTabWidget, 1);

    initializeWmiTab();
    initializeEtwTab();
}

void MonitorDock::initializeWmiTab()
{
    m_wmiPage = new QWidget(m_sideTabWidget);
    m_wmiLayout = new QVBoxLayout(m_wmiPage);
    m_wmiLayout->setContentsMargins(4, 4, 4, 4);
    m_wmiLayout->setSpacing(6);

    m_wmiSideToolBox = new QToolBox(m_wmiPage);
    // 让折叠栏获得可伸缩空间，避免“WMI订阅”页在中等窗口高度下被压缩出内部滚动条。
    m_wmiLayout->addWidget(m_wmiSideToolBox, 1);

    // Provider 折叠页。
    m_wmiProviderPanel = new QWidget(m_wmiSideToolBox);
    m_wmiProviderPanelLayout = new QVBoxLayout(m_wmiProviderPanel);
    m_wmiProviderPanelLayout->setContentsMargins(4, 4, 4, 4);
    m_wmiProviderPanelLayout->setSpacing(6);

    m_wmiProviderControlLayout = new QHBoxLayout();
    m_wmiProviderControlLayout->setContentsMargins(0, 0, 0, 0);
    m_wmiProviderControlLayout->setSpacing(6);

    m_wmiProviderFilterEdit = new QLineEdit(m_wmiProviderPanel);
    m_wmiProviderFilterEdit->setPlaceholderText(QStringLiteral("按Provider或命名空间过滤"));
    m_wmiProviderFilterEdit->setStyleSheet(blueInputStyle());

    m_wmiProviderRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_wmiProviderPanel);
    m_wmiProviderRefreshButton->setToolTip(QStringLiteral("刷新WMI Provider"));
    m_wmiProviderRefreshButton->setStyleSheet(blueButtonStyle());
    m_wmiProviderRefreshButton->setFixedWidth(34);

    m_wmiProviderStatusLabel = new QLabel(QStringLiteral("● 待刷新"), m_wmiProviderPanel);
    m_wmiProviderStatusLabel->setStyleSheet(QStringLiteral("color:#5F5F5F;font-weight:600;"));

    m_wmiProviderControlLayout->addWidget(m_wmiProviderFilterEdit, 1);
    m_wmiProviderControlLayout->addWidget(m_wmiProviderRefreshButton);
    m_wmiProviderControlLayout->addWidget(m_wmiProviderStatusLabel);

    m_wmiProviderModel = new QStandardItemModel(0, 5, m_wmiProviderPanel);
    m_wmiProviderModel->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Provider名称"),
        QStringLiteral("Namespace"),
        QStringLiteral("CLSID"),
        QStringLiteral("EventClass数量"),
        QStringLiteral("状态")
    });

    m_wmiProviderProxyModel = new QSortFilterProxyModel(m_wmiProviderPanel);
    m_wmiProviderProxyModel->setSourceModel(m_wmiProviderModel);
    m_wmiProviderProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_wmiProviderProxyModel->setFilterKeyColumn(-1);

    m_wmiProviderTableView = new QTableView(m_wmiProviderPanel);
    m_wmiProviderTableView->setModel(m_wmiProviderProxyModel);
    m_wmiProviderTableView->setSortingEnabled(true);
    m_wmiProviderTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_wmiProviderTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_wmiProviderTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wmiProviderTableView->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_wmiProviderTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_wmiProviderTableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    m_wmiProviderPanelLayout->addLayout(m_wmiProviderControlLayout);
    m_wmiProviderPanelLayout->addWidget(m_wmiProviderTableView, 1);
    m_wmiSideToolBox->addItem(m_wmiProviderPanel, QStringLiteral("WMI Providers"));

    // 订阅折叠页。
    m_wmiSubscribePanel = new QWidget(m_wmiSideToolBox);
    m_wmiSubscribeLayout = new QVBoxLayout(m_wmiSubscribePanel);
    m_wmiSubscribeLayout->setContentsMargins(4, 4, 4, 4);
    m_wmiSubscribeLayout->setSpacing(6);

    m_wmiEventClassControlLayout = new QHBoxLayout();
    m_wmiEventClassControlLayout->setContentsMargins(0, 0, 0, 0);
    m_wmiEventClassControlLayout->setSpacing(6);

    m_wmiSelectAllClassesButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_wmiSubscribePanel);
    m_wmiSelectAllClassesButton->setToolTip(QStringLiteral("全选事件类"));
    m_wmiSelectAllClassesButton->setStyleSheet(blueButtonStyle());
    m_wmiSelectAllClassesButton->setFixedWidth(34);

    m_wmiSelectNoneClassesButton = new QPushButton(QIcon(":/Icon/process_pause.svg"), QString(), m_wmiSubscribePanel);
    m_wmiSelectNoneClassesButton->setToolTip(QStringLiteral("全不选事件类"));
    m_wmiSelectNoneClassesButton->setStyleSheet(blueButtonStyle());
    m_wmiSelectNoneClassesButton->setFixedWidth(34);

    m_wmiSelectWin32ClassesButton = new QPushButton(QIcon(":/Icon/process_tree.svg"), QString(), m_wmiSubscribePanel);
    m_wmiSelectWin32ClassesButton->setToolTip(QStringLiteral("仅选择Win32_*"));
    m_wmiSelectWin32ClassesButton->setStyleSheet(blueButtonStyle());
    m_wmiSelectWin32ClassesButton->setFixedWidth(34);

    m_wmiEventClassControlLayout->addWidget(new QLabel(QStringLiteral("事件类"), m_wmiSubscribePanel));
    m_wmiEventClassControlLayout->addStretch(1);
    m_wmiEventClassControlLayout->addWidget(m_wmiSelectAllClassesButton);
    m_wmiEventClassControlLayout->addWidget(m_wmiSelectNoneClassesButton);
    m_wmiEventClassControlLayout->addWidget(m_wmiSelectWin32ClassesButton);

    m_wmiEventClassTable = new QTableWidget(m_wmiSubscribePanel);
    m_wmiEventClassTable->setColumnCount(3);
    m_wmiEventClassTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("启用"),
        QStringLiteral("事件类"),
        QStringLiteral("匹配")
    });
    m_wmiEventClassTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_wmiEventClassTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_wmiEventClassTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wmiEventClassTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_wmiEventClassTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_wmiEventClassTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_wmiEventClassTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // 事件类表先设置为可收敛尺寸策略，具体高度由 updateWmiSubscribePanelCompactLayout 动态计算。
    m_wmiEventClassTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QHBoxLayout* whereLayout = new QHBoxLayout();
    whereLayout->setContentsMargins(0, 0, 0, 0);
    whereLayout->setSpacing(6);
    whereLayout->addWidget(new QLabel(QStringLiteral("WHERE模板"), m_wmiSubscribePanel));

    m_wmiWhereTemplateCombo = new QComboBox(m_wmiSubscribePanel);
    m_wmiWhereTemplateCombo->setStyleSheet(blueInputStyle());
    m_wmiWhereTemplateCombo->addItem(QStringLiteral("空模板"), QString());
    m_wmiWhereTemplateCombo->addItem(QStringLiteral("powershell"), QStringLiteral("TargetInstance.Name LIKE '%powershell%'"));
    m_wmiWhereTemplateCombo->addItem(QStringLiteral("PID>1000"), QStringLiteral("TargetInstance.ProcessId > 1000"));
    m_wmiWhereTemplateCombo->addItem(QStringLiteral("Session=0"), QStringLiteral("TargetInstance.SessionId = 0"));
    whereLayout->addWidget(m_wmiWhereTemplateCombo, 1);

    m_wmiWhereEditor = new QPlainTextEdit(m_wmiSubscribePanel);
    m_wmiWhereEditor->setPlaceholderText(QStringLiteral("可选：输入WQL WHERE子句"));
    // WHERE 子句改为单行输入体验，降低折叠页高度并避免多行占位。
    m_wmiWhereEditor->setMaximumBlockCount(1);
    m_wmiWhereEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_wmiWhereEditor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_wmiWhereEditor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_wmiWhereEditor->setStyleSheet(blueInputStyle());
    // 按用户要求只保留一行高度。
    m_wmiWhereEditor->setFixedHeight(30);

    m_wmiSubscribeControlLayout = new QHBoxLayout();
    m_wmiSubscribeControlLayout->setContentsMargins(0, 0, 0, 0);
    m_wmiSubscribeControlLayout->setSpacing(6);

    // WMI 订阅控制按钮移到折叠栏外（父控件改为 m_wmiPage），
    // 避免订阅折叠页继续增高导致滚动条出现。
    m_wmiStartSubscribeButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_wmiPage);
    m_wmiStartSubscribeButton->setToolTip(QStringLiteral("开始订阅"));
    m_wmiStartSubscribeButton->setStyleSheet(blueButtonStyle());
    m_wmiStartSubscribeButton->setFixedWidth(34);

    m_wmiStopSubscribeButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), QString(), m_wmiPage);
    m_wmiStopSubscribeButton->setToolTip(QStringLiteral("停止订阅"));
    m_wmiStopSubscribeButton->setStyleSheet(blueButtonStyle());
    m_wmiStopSubscribeButton->setFixedWidth(34);

    m_wmiPauseSubscribeButton = new QPushButton(QIcon(":/Icon/process_pause.svg"), QString(), m_wmiPage);
    m_wmiPauseSubscribeButton->setToolTip(QStringLiteral("暂停/继续订阅"));
    m_wmiPauseSubscribeButton->setStyleSheet(blueButtonStyle());
    m_wmiPauseSubscribeButton->setFixedWidth(34);

    m_wmiSubscribeStatusLabel = new QLabel(QStringLiteral("● 未订阅"), m_wmiPage);
    m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));

    m_wmiSubscribeControlLayout->addWidget(new QLabel(QStringLiteral("WMI订阅控制"), m_wmiPage));
    m_wmiSubscribeControlLayout->addStretch(1);
    m_wmiSubscribeControlLayout->addWidget(m_wmiStartSubscribeButton);
    m_wmiSubscribeControlLayout->addWidget(m_wmiStopSubscribeButton);
    m_wmiSubscribeControlLayout->addWidget(m_wmiPauseSubscribeButton);
    m_wmiSubscribeControlLayout->addWidget(m_wmiSubscribeStatusLabel);

    m_wmiSubscribeLayout->addLayout(m_wmiEventClassControlLayout);
    m_wmiSubscribeLayout->addWidget(m_wmiEventClassTable, 1);
    m_wmiSubscribeLayout->addLayout(whereLayout);
    m_wmiSubscribeLayout->addWidget(m_wmiWhereEditor, 0);
    // 初始化时先按“紧凑高度”收敛订阅面板，防止首帧就出现折叠页内部滚动条。
    updateWmiSubscribePanelCompactLayout();

    m_wmiSideToolBox->addItem(m_wmiSubscribePanel, QStringLiteral("WMI订阅"));

    // 把订阅控制区放在折叠栏外，减少子折叠页高度，避免出现额外滚动条。
    m_wmiLayout->addLayout(m_wmiSubscribeControlLayout, 0);

    // 结果表。
    m_wmiEventTable = new QTableWidget(m_wmiPage);
    m_wmiEventTable->setColumnCount(5);
    m_wmiEventTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("时间戳(ms)"),
        QStringLiteral("事件来源"),
        QStringLiteral("事件类"),
        QStringLiteral("PID/进程"),
        QStringLiteral("事件详情")
    });
    m_wmiEventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_wmiEventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_wmiEventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wmiEventTable->setAlternatingRowColors(true);
    m_wmiEventTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_wmiEventTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_wmiEventTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_wmiEventTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_wmiEventTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_wmiEventTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_wmiEventTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

    // 结果筛选条：提供全字段/分字段/正则/大小写/反向匹配与贴底滚动控制。
    QWidget* wmiFilterWidget = new QWidget(m_wmiPage);
    QVBoxLayout* wmiFilterLayout = new QVBoxLayout(wmiFilterWidget);
    wmiFilterLayout->setContentsMargins(0, 0, 0, 0);
    wmiFilterLayout->setSpacing(4);

    QHBoxLayout* wmiFilterTopRow = new QHBoxLayout();
    wmiFilterTopRow->setContentsMargins(0, 0, 0, 0);
    wmiFilterTopRow->setSpacing(6);
    m_wmiEventGlobalFilterEdit = new QLineEdit(wmiFilterWidget);
    m_wmiEventGlobalFilterEdit->setPlaceholderText(QStringLiteral("全字段筛选（时间/来源/类/PID/详情）"));
    m_wmiEventGlobalFilterEdit->setStyleSheet(blueInputStyle());
    m_wmiEventProviderFilterEdit = new QLineEdit(wmiFilterWidget);
    m_wmiEventProviderFilterEdit->setPlaceholderText(QStringLiteral("来源筛选"));
    m_wmiEventProviderFilterEdit->setStyleSheet(blueInputStyle());
    m_wmiEventClassFilterEdit = new QLineEdit(wmiFilterWidget);
    m_wmiEventClassFilterEdit->setPlaceholderText(QStringLiteral("事件类筛选"));
    m_wmiEventClassFilterEdit->setStyleSheet(blueInputStyle());
    wmiFilterTopRow->addWidget(new QLabel(QStringLiteral("筛选"), wmiFilterWidget));
    wmiFilterTopRow->addWidget(m_wmiEventGlobalFilterEdit, 2);
    wmiFilterTopRow->addWidget(m_wmiEventProviderFilterEdit, 1);
    wmiFilterTopRow->addWidget(m_wmiEventClassFilterEdit, 1);

    QHBoxLayout* wmiFilterBottomRow = new QHBoxLayout();
    wmiFilterBottomRow->setContentsMargins(0, 0, 0, 0);
    wmiFilterBottomRow->setSpacing(6);
    m_wmiEventPidFilterEdit = new QLineEdit(wmiFilterWidget);
    m_wmiEventPidFilterEdit->setPlaceholderText(QStringLiteral("PID/进程筛选"));
    m_wmiEventPidFilterEdit->setStyleSheet(blueInputStyle());
    m_wmiEventDetailFilterEdit = new QLineEdit(wmiFilterWidget);
    m_wmiEventDetailFilterEdit->setPlaceholderText(QStringLiteral("详情筛选"));
    m_wmiEventDetailFilterEdit->setStyleSheet(blueInputStyle());
    m_wmiEventRegexCheck = new QCheckBox(QStringLiteral("正则"), wmiFilterWidget);
    m_wmiEventCaseCheck = new QCheckBox(QStringLiteral("区分大小写"), wmiFilterWidget);
    m_wmiEventInvertCheck = new QCheckBox(QStringLiteral("反向筛选"), wmiFilterWidget);
    m_wmiEventKeepBottomCheck = new QCheckBox(QStringLiteral("保持表格在底部"), wmiFilterWidget);
    m_wmiEventKeepBottomCheck->setChecked(true);
    m_wmiEventFilterClearButton = new QPushButton(QIcon(":/Icon/log_clear.svg"), QString(), wmiFilterWidget);
    m_wmiEventFilterClearButton->setStyleSheet(blueButtonStyle());
    m_wmiEventFilterClearButton->setToolTip(QStringLiteral("清空所有WMI筛选条件"));
    m_wmiEventFilterClearButton->setFixedWidth(34);
    m_wmiEventFilterStatusLabel = new QLabel(QStringLiteral("可见: 0 / 0"), wmiFilterWidget);
    m_wmiEventFilterStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;"));

    wmiFilterBottomRow->addWidget(m_wmiEventPidFilterEdit, 1);
    wmiFilterBottomRow->addWidget(m_wmiEventDetailFilterEdit, 2);
    wmiFilterBottomRow->addWidget(m_wmiEventRegexCheck, 0);
    wmiFilterBottomRow->addWidget(m_wmiEventCaseCheck, 0);
    wmiFilterBottomRow->addWidget(m_wmiEventInvertCheck, 0);
    wmiFilterBottomRow->addWidget(m_wmiEventKeepBottomCheck, 0);
    wmiFilterBottomRow->addWidget(m_wmiEventFilterClearButton, 0);
    wmiFilterBottomRow->addWidget(m_wmiEventFilterStatusLabel, 0);

    wmiFilterLayout->addLayout(wmiFilterTopRow);
    wmiFilterLayout->addLayout(wmiFilterBottomRow);

    m_wmiLayout->addWidget(wmiFilterWidget, 0);
    m_wmiLayout->addWidget(m_wmiEventTable, 1);
    // 调整 WMI 页面纵向占比：
    // - 折叠配置区与外部控制区保持紧凑；
    // - 事件结果表优先占用剩余空间。
    m_wmiLayout->setStretch(0, 2);
    m_wmiLayout->setStretch(1, 0);
    m_wmiLayout->setStretch(2, 0);
    m_wmiLayout->setStretch(3, 3);
    m_sideTabWidget->addTab(m_wmiPage, QStringLiteral("WMI"));
}

void MonitorDock::updateWmiSubscribePanelCompactLayout()
{
    if (m_wmiEventClassTable == nullptr)
    {
        return;
    }

    // fallbackVisibleRowCount 用途：事件类尚未加载时，先按固定可视行数预留紧凑高度。
    const int fallbackVisibleRowCount = 4;
    // maxVisibleRowCount 用途：限制折叠页最大可视行数，防止表格过高挤压折叠栏。
    const int maxVisibleRowCount = 6;
    // currentRowCount 用途：记录当前事件类表总行数，作为可视高度计算输入。
    const int currentRowCount = m_wmiEventClassTable->rowCount();
    // visibleRowCount 用途：将可视行数夹在 [fallbackVisibleRowCount, maxVisibleRowCount] 区间内。
    const int visibleRowCount = std::clamp(
        currentRowCount > 0 ? currentRowCount : fallbackVisibleRowCount,
        fallbackVisibleRowCount,
        maxVisibleRowCount);

    // headerHeight 用途：记录事件类表头高度；若表头为空则使用默认值避免高度为 0。
    int headerHeight = 24;
    QHeaderView* headerView = m_wmiEventClassTable->horizontalHeader();
    if (headerView != nullptr)
    {
        headerHeight = std::max(20, headerView->height());
    }

    // rowHeight 用途：记录单行默认高度，参与表格总高度估算。
    int rowHeight = 24;
    QHeaderView* verticalHeader = m_wmiEventClassTable->verticalHeader();
    if (verticalHeader != nullptr)
    {
        rowHeight = std::max(20, verticalHeader->defaultSectionSize());
    }

    // framePixels 用途：补偿表格边框占用像素，防止最底行被裁切。
    const int framePixels = m_wmiEventClassTable->frameWidth() * 2;
    // safetyPadding 用途：额外留白，吸收不同系统样式下的高度浮动。
    const int safetyPadding = 6;
    // tableTargetHeight 用途：最终写回到事件类表的紧凑高度。
    const int tableTargetHeight =
        headerHeight + (visibleRowCount * rowHeight) + framePixels + safetyPadding;
    m_wmiEventClassTable->setMinimumHeight(tableTargetHeight);
    m_wmiEventClassTable->setMaximumHeight(tableTargetHeight);
}

void MonitorDock::initializeEtwTab()
{
    m_etwPage = new QWidget(m_sideTabWidget);
    m_etwLayout = new QVBoxLayout(m_etwPage);
    m_etwLayout->setContentsMargins(4, 4, 4, 4);
    m_etwLayout->setSpacing(6);

    m_etwSideToolBox = new QToolBox(m_etwPage);
    m_etwLayout->addWidget(m_etwSideToolBox, 0);

    // Provider 折叠页。
    m_etwProviderPanel = new QWidget(m_etwSideToolBox);
    m_etwProviderPanelLayout = new QVBoxLayout(m_etwProviderPanel);
    m_etwProviderPanelLayout->setContentsMargins(4, 4, 4, 4);
    m_etwProviderPanelLayout->setSpacing(6);

    m_etwProviderControlLayout = new QHBoxLayout();
    m_etwProviderControlLayout->setContentsMargins(0, 0, 0, 0);
    m_etwProviderControlLayout->setSpacing(6);

    m_etwProviderRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_etwProviderPanel);
    m_etwProviderRefreshButton->setToolTip(QStringLiteral("刷新ETW Provider"));
    m_etwProviderRefreshButton->setStyleSheet(blueButtonStyle());
    m_etwProviderRefreshButton->setFixedWidth(34);

    m_etwProviderStatusLabel = new QLabel(QStringLiteral("● 待刷新"), m_etwProviderPanel);
    m_etwProviderStatusLabel->setStyleSheet(QStringLiteral("color:#5F5F5F;font-weight:600;"));

    m_etwProviderControlLayout->addWidget(new QLabel(QStringLiteral("ETW Providers"), m_etwProviderPanel));
    m_etwProviderControlLayout->addStretch(1);
    m_etwProviderControlLayout->addWidget(m_etwProviderRefreshButton);
    m_etwProviderControlLayout->addWidget(m_etwProviderStatusLabel);

    // Provider 区改为左右分栏：
    // - 左侧：预置常用 Provider 模板（含分类筛选）；
    // - 右侧：系统枚举出的全部 Provider 勾选列表。
    QHBoxLayout* etwProviderSplitLayout = new QHBoxLayout();
    etwProviderSplitLayout->setContentsMargins(0, 0, 0, 0);
    etwProviderSplitLayout->setSpacing(6);

    QWidget* etwPresetWidget = new QWidget(m_etwProviderPanel);
    QVBoxLayout* etwPresetLayout = new QVBoxLayout(etwPresetWidget);
    etwPresetLayout->setContentsMargins(0, 0, 0, 0);
    etwPresetLayout->setSpacing(4);

    QHBoxLayout* etwPresetHeaderLayout = new QHBoxLayout();
    etwPresetHeaderLayout->setContentsMargins(0, 0, 0, 0);
    etwPresetHeaderLayout->setSpacing(6);
    etwPresetHeaderLayout->addWidget(new QLabel(QStringLiteral("常用模板"), etwPresetWidget));

    m_etwPresetCategoryCombo = new QComboBox(etwPresetWidget);
    m_etwPresetCategoryCombo->setStyleSheet(blueInputStyle());
    m_etwPresetCategoryCombo->addItems(QStringList{
        QStringLiteral("全部分类"),
        QStringLiteral("进程线程"),
        QStringLiteral("文件注册表"),
        QStringLiteral("网络通信"),
        QStringLiteral("安全审计"),
        QStringLiteral("脚本管理")
    });
    etwPresetHeaderLayout->addWidget(m_etwPresetCategoryCombo, 1);
    etwPresetLayout->addLayout(etwPresetHeaderLayout);

    m_etwPresetProviderList = new QListWidget(etwPresetWidget);
    m_etwPresetProviderList->setAlternatingRowColors(true);
    m_etwPresetProviderList->setMinimumHeight(180);
    etwPresetLayout->addWidget(m_etwPresetProviderList, 1);

    // 预置模板条目：按分类提供最常用 Provider，便于一键勾选常见监控场景。
    struct EtwPresetTemplate
    {
        const wchar_t* categoryText;
        const wchar_t* providerNameText;
    };
    const std::vector<EtwPresetTemplate> presetTemplateList{
        { L"进程线程", L"Microsoft-Windows-Kernel-Process" },
        { L"进程线程", L"Microsoft-Windows-Kernel-Thread" },
        { L"进程线程", L"Microsoft-Windows-Kernel-Image" },
        { L"文件注册表", L"Microsoft-Windows-Kernel-File" },
        { L"文件注册表", L"Microsoft-Windows-Kernel-Registry" },
        { L"网络通信", L"Microsoft-Windows-TCPIP" },
        { L"网络通信", L"Microsoft-Windows-DNS-Client" },
        { L"网络通信", L"Microsoft-Windows-Winsock-AFD" },
        { L"安全审计", L"Microsoft-Windows-Security-Auditing" },
        { L"安全审计", L"Microsoft-Windows-Defender" },
        { L"脚本管理", L"Microsoft-Windows-PowerShell" },
        { L"脚本管理", L"Microsoft-Windows-WMI-Activity" },
        { L"脚本管理", L"Microsoft-Windows-TaskScheduler" }
    };
    for (const EtwPresetTemplate& preset : presetTemplateList)
    {
        const QString categoryText = QString::fromWCharArray(preset.categoryText);
        const QString providerNameText = QString::fromWCharArray(preset.providerNameText);
        QListWidgetItem* item = new QListWidgetItem(
            QStringLiteral("[%1] %2").arg(categoryText, providerNameText),
            m_etwPresetProviderList);
        item->setData(Qt::UserRole, providerNameText);
        item->setData(Qt::UserRole + 1, categoryText);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }

    QWidget* etwAllProviderWidget = new QWidget(m_etwProviderPanel);
    QVBoxLayout* etwAllProviderLayout = new QVBoxLayout(etwAllProviderWidget);
    etwAllProviderLayout->setContentsMargins(0, 0, 0, 0);
    etwAllProviderLayout->setSpacing(4);
    etwAllProviderLayout->addWidget(new QLabel(QStringLiteral("系统Providers"), etwAllProviderWidget));

    m_etwProviderList = new QListWidget(etwAllProviderWidget);
    m_etwProviderList->setAlternatingRowColors(true);
    m_etwProviderList->setMinimumHeight(180);
    etwAllProviderLayout->addWidget(m_etwProviderList, 1);

    etwProviderSplitLayout->addWidget(etwPresetWidget, 1);
    etwProviderSplitLayout->addWidget(etwAllProviderWidget, 2);

    m_etwProviderPanelLayout->addLayout(m_etwProviderControlLayout);
    m_etwProviderPanelLayout->addLayout(etwProviderSplitLayout, 1);

    m_etwSideToolBox->addItem(m_etwProviderPanel, QStringLiteral("ETW Providers"));

    // 参数折叠页。
    QWidget* capturePanel = new QWidget(m_etwSideToolBox);
    QVBoxLayout* captureLayout = new QVBoxLayout(capturePanel);
    captureLayout->setContentsMargins(4, 4, 4, 4);
    captureLayout->setSpacing(6);

    QFormLayout* formLayout = new QFormLayout();
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->setSpacing(6);

    m_etwManualProviderEdit = new QLineEdit(capturePanel);
    m_etwManualProviderEdit->setPlaceholderText(QStringLiteral("可选：手动输入Provider"));
    m_etwManualProviderEdit->setStyleSheet(blueInputStyle());

    m_etwLevelCombo = new QComboBox(capturePanel);
    m_etwLevelCombo->setStyleSheet(blueInputStyle());
    m_etwLevelCombo->addItems(QStringList{
        QStringLiteral("Critical"),
        QStringLiteral("Error"),
        QStringLiteral("Warning"),
        QStringLiteral("Information"),
        QStringLiteral("Verbose")
    });
    m_etwLevelCombo->setCurrentIndex(3);

    m_etwKeywordMaskEdit = new QLineEdit(capturePanel);
    m_etwKeywordMaskEdit->setStyleSheet(blueInputStyle());
    m_etwKeywordMaskEdit->setText(QStringLiteral("0xFFFFFFFFFFFFFFFF"));

    m_etwBufferSizeSpin = new QSpinBox(capturePanel);
    m_etwBufferSizeSpin->setRange(64, 4096);
    m_etwBufferSizeSpin->setValue(256);
    m_etwBufferSizeSpin->setStyleSheet(blueInputStyle());

    m_etwMinBufferSpin = new QSpinBox(capturePanel);
    m_etwMinBufferSpin->setRange(2, 128);
    m_etwMinBufferSpin->setValue(16);
    m_etwMinBufferSpin->setStyleSheet(blueInputStyle());

    m_etwMaxBufferSpin = new QSpinBox(capturePanel);
    m_etwMaxBufferSpin->setRange(4, 256);
    m_etwMaxBufferSpin->setValue(64);
    m_etwMaxBufferSpin->setStyleSheet(blueInputStyle());

    formLayout->addRow(QStringLiteral("手动Provider"), m_etwManualProviderEdit);
    formLayout->addRow(QStringLiteral("级别"), m_etwLevelCombo);
    formLayout->addRow(QStringLiteral("关键字掩码"), m_etwKeywordMaskEdit);
    formLayout->addRow(QStringLiteral("缓冲区大小(KB)"), m_etwBufferSizeSpin);
    formLayout->addRow(QStringLiteral("最小缓冲区"), m_etwMinBufferSpin);
    formLayout->addRow(QStringLiteral("最大缓冲区"), m_etwMaxBufferSpin);

    m_etwCaptureControlLayout = new QHBoxLayout();
    m_etwCaptureControlLayout->setContentsMargins(0, 0, 0, 0);
    m_etwCaptureControlLayout->setSpacing(6);

    // ETW 控制按钮挪到折叠栏外（父控件改为 m_etwPage），避免折叠页过高。
    m_etwStartButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_etwPage);
    m_etwStartButton->setToolTip(QStringLiteral("开始监听"));
    m_etwStartButton->setStyleSheet(blueButtonStyle());
    m_etwStartButton->setFixedWidth(34);

    m_etwStopButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), QString(), m_etwPage);
    m_etwStopButton->setToolTip(QStringLiteral("停止监听"));
    m_etwStopButton->setStyleSheet(blueButtonStyle());
    m_etwStopButton->setFixedWidth(34);

    m_etwPauseButton = new QPushButton(QIcon(":/Icon/process_pause.svg"), QString(), m_etwPage);
    m_etwPauseButton->setToolTip(QStringLiteral("暂停/继续"));
    m_etwPauseButton->setStyleSheet(blueButtonStyle());
    m_etwPauseButton->setFixedWidth(34);

    m_etwExportButton = new QPushButton(QIcon(":/Icon/log_export.svg"), QString(), m_etwPage);
    m_etwExportButton->setToolTip(QStringLiteral("导出TSV"));
    m_etwExportButton->setStyleSheet(blueButtonStyle());
    m_etwExportButton->setFixedWidth(34);

    m_etwCaptureStatusLabel = new QLabel(QStringLiteral("● 未监听"), m_etwPage);
    m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));

    m_etwCaptureControlLayout->addWidget(new QLabel(QStringLiteral("ETW控制"), m_etwPage));
    m_etwCaptureControlLayout->addStretch(1);
    m_etwCaptureControlLayout->addWidget(m_etwStartButton);
    m_etwCaptureControlLayout->addWidget(m_etwStopButton);
    m_etwCaptureControlLayout->addWidget(m_etwPauseButton);
    m_etwCaptureControlLayout->addWidget(m_etwExportButton);
    m_etwCaptureControlLayout->addWidget(m_etwCaptureStatusLabel);

    captureLayout->addLayout(formLayout);
    m_etwSideToolBox->addItem(capturePanel, QStringLiteral("ETW捕获"));

    // 把 ETW 控制栏放在折叠栏外，统一和 WMI 的操作区布局。
    m_etwLayout->addLayout(m_etwCaptureControlLayout, 0);

    // 结果表。
    m_etwEventTable = new QTableWidget(m_etwPage);
    m_etwEventTable->setColumnCount(7);
    m_etwEventTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("时间戳(100ns)"),
        QStringLiteral("Provider"),
        QStringLiteral("事件ID"),
        QStringLiteral("事件名称"),
        QStringLiteral("PID/TID"),
        QStringLiteral("事件数据(JSON)"),
        QStringLiteral("ActivityId")
    });
    m_etwEventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_etwEventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_etwEventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_etwEventTable->setAlternatingRowColors(true);
    m_etwEventTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_etwEventTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_etwEventTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);

    m_etwLayout->addWidget(m_etwEventTable, 1);
    m_etwLayout->setStretch(0, 2);
    m_etwLayout->setStretch(1, 0);
    m_etwLayout->setStretch(2, 3);

    m_etwUiUpdateTimer = new QTimer(this);
    m_etwUiUpdateTimer->setInterval(100);

    m_sideTabWidget->addTab(m_etwPage, QStringLiteral("ETW"));
}

void MonitorDock::initializeConnections()
{
    // WMI 基础交互。
    connect(m_wmiProviderFilterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] WMI Provider过滤词变更, keyword="
            << text.toStdString()
            << eol;
        applyWmiProviderFilter();
    });

    connect(m_wmiProviderRefreshButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击刷新WMI Provider与事件类。"
            << eol;
        refreshWmiProvidersAsync();
        refreshWmiEventClassesAsync();
    });

    connect(m_wmiSelectAllClassesButton, &QPushButton::clicked, this, [this]() {
        for (int row = 0; row < m_wmiEventClassTable->rowCount(); ++row)
        {
            QTableWidgetItem* item = m_wmiEventClassTable->item(row, 0);
            if (item != nullptr)
            {
                item->setCheckState(Qt::Checked);
            }
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] WMI事件类操作：全选。"
            << eol;
    });

    connect(m_wmiSelectNoneClassesButton, &QPushButton::clicked, this, [this]() {
        for (int row = 0; row < m_wmiEventClassTable->rowCount(); ++row)
        {
            QTableWidgetItem* item = m_wmiEventClassTable->item(row, 0);
            if (item != nullptr)
            {
                item->setCheckState(Qt::Unchecked);
            }
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] WMI事件类操作：全不选。"
            << eol;
    });

    connect(m_wmiSelectWin32ClassesButton, &QPushButton::clicked, this, [this]() {
        int checkedCount = 0;
        for (int row = 0; row < m_wmiEventClassTable->rowCount(); ++row)
        {
            QTableWidgetItem* checkItem = m_wmiEventClassTable->item(row, 0);
            QTableWidgetItem* classItem = m_wmiEventClassTable->item(row, 1);
            if (checkItem == nullptr || classItem == nullptr)
            {
                continue;
            }
            checkItem->setCheckState(classItem->text().startsWith(QStringLiteral("Win32_"), Qt::CaseInsensitive)
                ? Qt::Checked
                : Qt::Unchecked);
            if (checkItem->checkState() == Qt::Checked)
            {
                ++checkedCount;
            }
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] WMI事件类操作：仅选Win32_*, selectedCount="
            << checkedCount
            << eol;
    });

    connect(m_wmiWhereTemplateCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0)
        {
            return;
        }
        const QString text = m_wmiWhereTemplateCombo->itemData(index).toString().trimmed();
        if (text.isEmpty())
        {
            return;
        }
        // 单行 WHERE 输入框逻辑：
        // - 空内容时直接填模板；
        // - 非空时在同一行用 AND 拼接，避免 appendPlainText 产生换行。
        const QString existingWhere = m_wmiWhereEditor->toPlainText().trimmed();
        if (existingWhere.isEmpty())
        {
            m_wmiWhereEditor->setPlainText(text);
        }
        else
        {
            m_wmiWhereEditor->setPlainText(existingWhere + QStringLiteral(" AND ") + text);
        }

        kLogEvent event;
        info << event
            << "[MonitorDock] 追加WMI WHERE模板, template="
            << text.toStdString()
            << eol;
    });

    connect(m_wmiStartSubscribeButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击开始WMI订阅。"
            << eol;
        startWmiSubscription();
    });

    connect(m_wmiStopSubscribeButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击停止WMI订阅。"
            << eol;
        stopWmiSubscription();
    });

    connect(m_wmiPauseSubscribeButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击切换WMI暂停状态。"
            << eol;
        setWmiSubscriptionPaused(!m_wmiSubscribePaused.load());
    });

    connect(m_wmiEventTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showWmiEventContextMenu(pos);
    });

    // WMI 结果筛选交互：任一条件变化后实时重算可见行。
    const auto bindWmiFilter = [this](QLineEdit* edit) {
        if (edit == nullptr)
        {
            return;
        }
        connect(edit, &QLineEdit::textChanged, this, [this]() {
            applyWmiEventFilter();
        });
    };
    bindWmiFilter(m_wmiEventGlobalFilterEdit);
    bindWmiFilter(m_wmiEventProviderFilterEdit);
    bindWmiFilter(m_wmiEventClassFilterEdit);
    bindWmiFilter(m_wmiEventPidFilterEdit);
    bindWmiFilter(m_wmiEventDetailFilterEdit);

    if (m_wmiEventRegexCheck != nullptr)
    {
        connect(m_wmiEventRegexCheck, &QCheckBox::toggled, this, [this]() {
            applyWmiEventFilter();
        });
    }
    if (m_wmiEventCaseCheck != nullptr)
    {
        connect(m_wmiEventCaseCheck, &QCheckBox::toggled, this, [this]() {
            applyWmiEventFilter();
        });
    }
    if (m_wmiEventInvertCheck != nullptr)
    {
        connect(m_wmiEventInvertCheck, &QCheckBox::toggled, this, [this]() {
            applyWmiEventFilter();
        });
    }
    if (m_wmiEventFilterClearButton != nullptr)
    {
        connect(m_wmiEventFilterClearButton, &QPushButton::clicked, this, [this]() {
            clearWmiEventFilter();
        });
    }

    // ETW 基础交互。
    if (m_etwPresetCategoryCombo != nullptr && m_etwPresetProviderList != nullptr)
    {
        const auto applyPresetCategoryFilter = [this](const QString& categoryText) {
            const QString normalizedCategory = categoryText.trimmed();
            int visibleCount = 0;
            for (int row = 0; row < m_etwPresetProviderList->count(); ++row)
            {
                QListWidgetItem* item = m_etwPresetProviderList->item(row);
                if (item == nullptr)
                {
                    continue;
                }
                const QString itemCategory = item->data(Qt::UserRole + 1).toString();
                const bool isVisible = normalizedCategory.isEmpty()
                    || normalizedCategory == QStringLiteral("全部分类")
                    || itemCategory.compare(normalizedCategory, Qt::CaseInsensitive) == 0;
                item->setHidden(!isVisible);
                if (isVisible)
                {
                    ++visibleCount;
                }
            }

            kLogEvent event;
            dbg << event
                << "[MonitorDock] ETW预置模板分类筛选, category="
                << normalizedCategory.toStdString()
                << ", visibleCount="
                << visibleCount
                << eol;
        };

        connect(m_etwPresetCategoryCombo, &QComboBox::currentTextChanged, this, [applyPresetCategoryFilter](const QString& text) {
            applyPresetCategoryFilter(text);
        });
        applyPresetCategoryFilter(m_etwPresetCategoryCombo->currentText());
    }

    connect(m_etwProviderRefreshButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击刷新ETW Provider。"
            << eol;
        refreshEtwProvidersAsync();
    });

    connect(m_etwStartButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击开始ETW监听。"
            << eol;
        startEtwCapture();
    });

    connect(m_etwStopButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击停止ETW监听。"
            << eol;
        stopEtwCapture();
    });

    connect(m_etwPauseButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击切换ETW暂停状态。"
            << eol;
        setEtwCapturePaused(!m_etwCapturePaused.load());
    });

    connect(m_etwExportButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[MonitorDock] 用户点击导出ETW事件。"
            << eol;
        exportEtwRowsToTsv();
    });

    connect(m_etwUiUpdateTimer, &QTimer::timeout, this, [this]() {
        std::vector<QStringList> rows;
        {
            std::lock_guard<std::mutex> lock(m_etwPendingMutex);
            rows.swap(m_etwPendingRows);
        }

        if (rows.empty())
        {
            return;
        }

        for (const QStringList& rowValues : rows)
        {
            if (rowValues.size() < 7)
            {
                continue;
            }
            const int row = m_etwEventTable->rowCount();
            m_etwEventTable->insertRow(row);
            for (int col = 0; col < 7; ++col)
            {
                QTableWidgetItem* item = new QTableWidgetItem(rowValues.at(col));
                item->setToolTip(rowValues.at(col));
                m_etwEventTable->setItem(row, col, item);
            }
        }

        while (m_etwEventTable->rowCount() > 6000)
        {
            m_etwEventTable->removeRow(0);
        }
    });

    connect(m_etwEventTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showEtwEventContextMenu(pos);
    });
}

void MonitorDock::refreshWmiProvidersAsync()
{
    kLogEvent startEvent;
    info << startEvent
        << "[MonitorDock] 开始异步刷新WMI Provider。"
        << eol;

    m_wmiProviderStatusLabel->setText(QStringLiteral("● 刷新中..."));
    m_wmiProviderStatusLabel->setStyleSheet(QStringLiteral("color:#1F4E7A;font-weight:600;"));

    if (m_wmiProviderRefreshProgressPid == 0)
    {
        m_wmiProviderRefreshProgressPid = kPro.add("监控", "刷新WMI Provider");
    }
    kPro.set(m_wmiProviderRefreshProgressPid, "开始枚举WMI Provider", 0, 10.0f);

    QPointer<MonitorDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<WmiProviderEntry> providers;
        QString errorText;

        if (!initCom(&errorText))
        {
            QMetaObject::invokeMethod(qApp, [guardThis, errorText]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_wmiProviderStatusLabel->setText(QStringLiteral("● 初始化失败"));
                guardThis->m_wmiProviderStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                if (guardThis->m_wmiProviderRefreshProgressPid != 0)
                {
                    kPro.set(guardThis->m_wmiProviderRefreshProgressPid, "WMI Provider刷新失败", 0, 100.0f);
                    guardThis->m_wmiProviderRefreshProgressPid = 0;
                }

                kLogEvent event;
                err << event << "[MonitorDock] WMI Provider初始化失败:" << errorText.toStdString() << eol;
            }, Qt::QueuedConnection);
            return;
        }

        CComPtr<IWbemServices> service;
        if (!connectWmi(&service, &errorText) || service == nullptr)
        {
            ::CoUninitialize();
            QMetaObject::invokeMethod(qApp, [guardThis, errorText]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_wmiProviderStatusLabel->setText(QStringLiteral("● 连接失败"));
                guardThis->m_wmiProviderStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                if (guardThis->m_wmiProviderRefreshProgressPid != 0)
                {
                    kPro.set(guardThis->m_wmiProviderRefreshProgressPid, "WMI Provider刷新失败", 0, 100.0f);
                    guardThis->m_wmiProviderRefreshProgressPid = 0;
                }

                kLogEvent event;
                err << event << "[MonitorDock] WMI连接失败:" << errorText.toStdString() << eol;
            }, Qt::QueuedConnection);
            return;
        }

        CComPtr<IEnumWbemClassObject> enumerator;
        const HRESULT queryResult = service->ExecQuery(
            bstr_t("WQL"),
            bstr_t("SELECT * FROM __Win32Provider"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            &enumerator);

        if (SUCCEEDED(queryResult) && enumerator != nullptr)
        {
            while (true)
            {
                CComPtr<IWbemClassObject> item;
                ULONG count = 0;
                const HRESULT nextResult = enumerator->Next(100, 1, &item, &count);
                if (FAILED(nextResult) || count == 0 || item == nullptr)
                {
                    break;
                }

                VARIANT nameValue;
                VARIANT clsidValue;
                VARIANT hostingValue;
                ::VariantInit(&nameValue);
                ::VariantInit(&clsidValue);
                ::VariantInit(&hostingValue);

                item->Get(L"Name", 0, &nameValue, nullptr, nullptr);
                item->Get(L"CLSID", 0, &clsidValue, nullptr, nullptr);
                item->Get(L"HostingModel", 0, &hostingValue, nullptr, nullptr);

                WmiProviderEntry entry;
                entry.providerName = variantToText(nameValue);
                entry.nameSpaceText = QStringLiteral("ROOT\\CIMV2");
                entry.clsidText = variantToText(clsidValue);
                entry.eventClassCount = 0;
                entry.subscribable = !entry.providerName.trimmed().isEmpty()
                    && !variantToText(hostingValue).contains(QStringLiteral("Decoupled"), Qt::CaseInsensitive);

                providers.push_back(entry);

                ::VariantClear(&nameValue);
                ::VariantClear(&clsidValue);
                ::VariantClear(&hostingValue);
            }
        }

        int classCount = 0;
        CComPtr<IEnumWbemClassObject> classEnum;
        const HRESULT classQueryResult = service->ExecQuery(
            bstr_t("WQL"),
            bstr_t("SELECT * FROM meta_class WHERE __CLASS LIKE 'Win32\\_%Event'"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            &classEnum);
        if (SUCCEEDED(classQueryResult) && classEnum != nullptr)
        {
            while (true)
            {
                CComPtr<IWbemClassObject> item;
                ULONG count = 0;
                const HRESULT nextResult = classEnum->Next(100, 1, &item, &count);
                if (FAILED(nextResult) || count == 0 || item == nullptr)
                {
                    break;
                }
                ++classCount;
            }
        }

        for (WmiProviderEntry& entry : providers)
        {
            entry.eventClassCount = classCount;
        }

        ::CoUninitialize();

        QMetaObject::invokeMethod(qApp, [guardThis, providers]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_wmiProviders = providers;
            guardThis->m_wmiProviderModel->removeRows(0, guardThis->m_wmiProviderModel->rowCount());

            for (const WmiProviderEntry& entry : guardThis->m_wmiProviders)
            {
                QList<QStandardItem*> rowItems;
                rowItems << new QStandardItem(entry.providerName)
                    << new QStandardItem(entry.nameSpaceText)
                    << new QStandardItem(entry.clsidText)
                    << new QStandardItem(QString::number(entry.eventClassCount))
                    << new QStandardItem(entry.subscribable ? QStringLiteral("可订阅") : QStringLiteral("受限"));
                guardThis->m_wmiProviderModel->appendRow(rowItems);
            }

            guardThis->applyWmiProviderFilter();
            guardThis->m_wmiProviderStatusLabel->setText(
                QStringLiteral("● 已刷新 %1 项").arg(guardThis->m_wmiProviders.size()));
            guardThis->m_wmiProviderStatusLabel->setStyleSheet(QStringLiteral("color:#2F7D32;font-weight:600;"));
            if (guardThis->m_wmiProviderRefreshProgressPid != 0)
            {
                kPro.set(guardThis->m_wmiProviderRefreshProgressPid, "WMI Provider完成", 0, 100.0f);
                guardThis->m_wmiProviderRefreshProgressPid = 0;
            }

            kLogEvent event;
            info << event
                << "[MonitorDock] WMI Provider刷新完成, providerCount="
                << guardThis->m_wmiProviders.size()
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void MonitorDock::refreshWmiEventClassesAsync()
{
    kLogEvent startEvent;
    info << startEvent
        << "[MonitorDock] 开始异步刷新WMI事件类。"
        << eol;

    m_wmiEventClassTable->setRowCount(0);

    QPointer<MonitorDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<QString> classes;
        QString errorText;

        if (!initCom(&errorText))
        {
            kLogEvent event;
            err << event
                << "[MonitorDock] WMI事件类刷新失败：COM初始化失败, error="
                << errorText.toStdString()
                << eol;
            return;
        }

        CComPtr<IWbemServices> service;
        if (!connectWmi(&service, &errorText) || service == nullptr)
        {
            ::CoUninitialize();
            kLogEvent event;
            err << event
                << "[MonitorDock] WMI事件类刷新失败：连接失败, error="
                << errorText.toStdString()
                << eol;
            return;
        }

        CComPtr<IEnumWbemClassObject> classEnum;
        const HRESULT enumResult = service->CreateClassEnum(
            _bstr_t(L"__Event"),
            WBEM_FLAG_DEEP | WBEM_FLAG_FORWARD_ONLY,
            nullptr,
            &classEnum);

        if (SUCCEEDED(enumResult) && classEnum != nullptr)
        {
            while (true)
            {
                CComPtr<IWbemClassObject> item;
                ULONG count = 0;
                const HRESULT nextResult = classEnum->Next(100, 1, &item, &count);
                if (FAILED(nextResult) || count == 0 || item == nullptr)
                {
                    break;
                }

                VARIANT classValue;
                ::VariantInit(&classValue);
                item->Get(L"__CLASS", 0, &classValue, nullptr, nullptr);
                const QString classText = variantToText(classValue).trimmed();
                ::VariantClear(&classValue);

                if (!classText.isEmpty())
                {
                    classes.push_back(classText);
                }
            }
        }

        std::sort(classes.begin(), classes.end(), [](const QString& a, const QString& b) {
            return QString::compare(a, b, Qt::CaseInsensitive) < 0;
        });
        classes.erase(
            std::unique(classes.begin(), classes.end(), [](const QString& a, const QString& b) {
                return QString::compare(a, b, Qt::CaseInsensitive) == 0;
            }),
            classes.end());

        ::CoUninitialize();

        QMetaObject::invokeMethod(qApp, [guardThis, classes]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_wmiEventClassTable->setRowCount(static_cast<int>(classes.size()));
            for (int row = 0; row < static_cast<int>(classes.size()); ++row)
            {
                const QString className = classes[static_cast<std::size_t>(row)];

                QTableWidgetItem* checkItem = new QTableWidgetItem();
                checkItem->setFlags(checkItem->flags() | Qt::ItemIsUserCheckable);
                checkItem->setCheckState(className.startsWith(QStringLiteral("Win32_"), Qt::CaseInsensitive)
                    ? Qt::Checked
                    : Qt::Unchecked);

                guardThis->m_wmiEventClassTable->setItem(row, 0, checkItem);
                guardThis->m_wmiEventClassTable->setItem(row, 1, new QTableWidgetItem(className));
                guardThis->m_wmiEventClassTable->setItem(
                    row,
                    2,
                    new QTableWidgetItem(className.startsWith(QStringLiteral("Win32_"), Qt::CaseInsensitive)
                        ? QStringLiteral("Win32")
                        : QStringLiteral("其他")));
            }
            // 事件类刷新后重新计算折叠页目标高度，确保“WMI订阅”页不会因为行数变化撑爆折叠栏。
            guardThis->updateWmiSubscribePanelCompactLayout();

            kLogEvent event;
            info << event
                << "[MonitorDock] WMI事件类刷新完成, classCount="
                << classes.size()
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void MonitorDock::applyWmiProviderFilter()
{
    const QString keyword = m_wmiProviderFilterEdit->text().trimmed();
    m_wmiProviderProxyModel->setFilterFixedString(keyword);

    kLogEvent event;
    dbg << event
        << "[MonitorDock] 应用WMI Provider过滤, keyword="
        << keyword.toStdString()
        << eol;
}

void MonitorDock::applyWmiEventFilter()
{
    if (m_wmiEventTable == nullptr)
    {
        return;
    }

    const QString globalKeyword = m_wmiEventGlobalFilterEdit != nullptr
        ? m_wmiEventGlobalFilterEdit->text().trimmed()
        : QString();
    const QString providerKeyword = m_wmiEventProviderFilterEdit != nullptr
        ? m_wmiEventProviderFilterEdit->text().trimmed()
        : QString();
    const QString classKeyword = m_wmiEventClassFilterEdit != nullptr
        ? m_wmiEventClassFilterEdit->text().trimmed()
        : QString();
    const QString pidKeyword = m_wmiEventPidFilterEdit != nullptr
        ? m_wmiEventPidFilterEdit->text().trimmed()
        : QString();
    const QString detailKeyword = m_wmiEventDetailFilterEdit != nullptr
        ? m_wmiEventDetailFilterEdit->text().trimmed()
        : QString();
    const bool useRegex = m_wmiEventRegexCheck != nullptr && m_wmiEventRegexCheck->isChecked();
    const bool invertMatch = m_wmiEventInvertCheck != nullptr && m_wmiEventInvertCheck->isChecked();
    const Qt::CaseSensitivity caseSensitivity =
        (m_wmiEventCaseCheck != nullptr && m_wmiEventCaseCheck->isChecked())
        ? Qt::CaseSensitive
        : Qt::CaseInsensitive;

    int visibleCount = 0;
    const int totalCount = m_wmiEventTable->rowCount();
    for (int row = 0; row < totalCount; ++row)
    {
        const QString tsText = m_wmiEventTable->item(row, 0) != nullptr
            ? m_wmiEventTable->item(row, 0)->text()
            : QString();
        const QString providerText = m_wmiEventTable->item(row, 1) != nullptr
            ? m_wmiEventTable->item(row, 1)->text()
            : QString();
        const QString classText = m_wmiEventTable->item(row, 2) != nullptr
            ? m_wmiEventTable->item(row, 2)->text()
            : QString();
        const QString pidText = m_wmiEventTable->item(row, 3) != nullptr
            ? m_wmiEventTable->item(row, 3)->text()
            : QString();
        const QString detailText = m_wmiEventTable->item(row, 4) != nullptr
            ? m_wmiEventTable->item(row, 4)->text()
            : QString();

        const QString mergedText = QStringLiteral("%1 %2 %3 %4 %5")
            .arg(tsText, providerText, classText, pidText, detailText);
        const bool globalMatch = textMatch(mergedText, globalKeyword, useRegex, caseSensitivity);
        const bool providerMatch = textMatch(providerText, providerKeyword, useRegex, caseSensitivity);
        const bool classMatch = textMatch(classText, classKeyword, useRegex, caseSensitivity);
        const bool pidMatch = textMatch(pidText, pidKeyword, useRegex, caseSensitivity);
        const bool detailMatch = textMatch(detailText, detailKeyword, useRegex, caseSensitivity);
        bool showRow = globalMatch && providerMatch && classMatch && pidMatch && detailMatch;
        if (invertMatch)
        {
            showRow = !showRow;
        }

        m_wmiEventTable->setRowHidden(row, !showRow);
        if (showRow)
        {
            ++visibleCount;
        }
    }

    if (m_wmiEventFilterStatusLabel != nullptr)
    {
        m_wmiEventFilterStatusLabel->setText(QStringLiteral("可见: %1 / %2").arg(visibleCount).arg(totalCount));
    }
    if (m_wmiEventKeepBottomCheck != nullptr && m_wmiEventKeepBottomCheck->isChecked())
    {
        m_wmiEventTable->scrollToBottom();
    }

    kLogEvent event;
    dbg << event
        << "[MonitorDock] 应用WMI事件筛选, total="
        << totalCount
        << ", visible="
        << visibleCount
        << ", regex="
        << (useRegex ? "true" : "false")
        << ", invert="
        << (invertMatch ? "true" : "false")
        << eol;
}

void MonitorDock::clearWmiEventFilter()
{
    if (m_wmiEventGlobalFilterEdit != nullptr) m_wmiEventGlobalFilterEdit->clear();
    if (m_wmiEventProviderFilterEdit != nullptr) m_wmiEventProviderFilterEdit->clear();
    if (m_wmiEventClassFilterEdit != nullptr) m_wmiEventClassFilterEdit->clear();
    if (m_wmiEventPidFilterEdit != nullptr) m_wmiEventPidFilterEdit->clear();
    if (m_wmiEventDetailFilterEdit != nullptr) m_wmiEventDetailFilterEdit->clear();
    if (m_wmiEventRegexCheck != nullptr) m_wmiEventRegexCheck->setChecked(false);
    if (m_wmiEventCaseCheck != nullptr) m_wmiEventCaseCheck->setChecked(false);
    if (m_wmiEventInvertCheck != nullptr) m_wmiEventInvertCheck->setChecked(false);

    applyWmiEventFilter();

    kLogEvent event;
    info << event
        << "[MonitorDock] 已清空WMI事件筛选条件。"
        << eol;
}

void MonitorDock::startWmiSubscription()
{
    if (m_wmiSubscribeRunning.load())
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] 忽略启动WMI订阅：当前已在运行。"
            << eol;
        return;
    }

    std::vector<QString> classList;
    for (int row = 0; row < m_wmiEventClassTable->rowCount(); ++row)
    {
        QTableWidgetItem* checkItem = m_wmiEventClassTable->item(row, 0);
        QTableWidgetItem* classItem = m_wmiEventClassTable->item(row, 1);
        if (checkItem != nullptr && classItem != nullptr && checkItem->checkState() == Qt::Checked)
        {
            classList.push_back(classItem->text().trimmed());
        }
    }

    if (classList.empty())
    {
        kLogEvent event;
        warn << event
            << "[MonitorDock] 启动WMI订阅失败：未选择事件类。"
            << eol;
        QMessageBox::information(this, QStringLiteral("WMI订阅"), QStringLiteral("请至少选择一个事件类。"));
        return;
    }

    const QString whereClause = m_wmiWhereEditor->toPlainText().trimmed();

    {
        kLogEvent event;
        info << event
            << "[MonitorDock] 启动WMI订阅, classCount="
            << classList.size()
            << ", whereClause="
            << whereClause.toStdString()
            << eol;
    }

    m_wmiSubscribeRunning.store(true);
    m_wmiSubscribePaused.store(false);
    m_wmiSubscribeStopFlag.store(false);

    if (m_wmiSubscribeProgressPid == 0)
    {
        m_wmiSubscribeProgressPid = kPro.add("监控", "WMI订阅");
    }
    kPro.set(m_wmiSubscribeProgressPid, "建立WMI订阅", 0, 10.0f);

    m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 订阅中"));
    m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#1F4E7A;font-weight:600;"));
    if (m_wmiUiUpdateTimer != nullptr && !m_wmiUiUpdateTimer->isActive())
    {
        m_wmiUiUpdateTimer->start();
    }
    {
        // 启动新一轮订阅前清空待刷队列，避免残留事件混入新会话。
        std::lock_guard<std::mutex> lock(m_wmiPendingMutex);
        m_wmiPendingRows.clear();
    }

    QPointer<MonitorDock> guardThis(this);
    m_wmiSubscribeThread = std::make_unique<std::thread>([guardThis, classList, whereClause]() {
        QString errorText;
        if (!initCom(&errorText))
        {
            QMetaObject::invokeMethod(qApp, [guardThis, errorText]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_wmiSubscribeRunning.store(false);
                guardThis->m_wmiSubscribeStatusLabel->setText(QString("● 初始化失败: %1").arg(errorText));
                guardThis->m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                if (guardThis->m_wmiSubscribeProgressPid != 0)
                {
                    kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI订阅失败", 0, 100.0f);
                    guardThis->m_wmiSubscribeProgressPid = 0;
                }
            }, Qt::QueuedConnection);
            return;
        }

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI COM初始化完成", 0, 20.0f);
        }, Qt::QueuedConnection);

        CComPtr<IWbemServices> service;
        if (!connectWmi(&service, &errorText) || service == nullptr)
        {
            ::CoUninitialize();
            QMetaObject::invokeMethod(qApp, [guardThis, errorText]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_wmiSubscribeRunning.store(false);
                guardThis->m_wmiSubscribeStatusLabel->setText(QString("● 连接失败: %1").arg(errorText));
                guardThis->m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                if (guardThis->m_wmiSubscribeProgressPid != 0)
                {
                    kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI连接失败", 0, 100.0f);
                    guardThis->m_wmiSubscribeProgressPid = 0;
                }
            }, Qt::QueuedConnection);
            return;
        }

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI服务连接成功", 0, 30.0f);
        }, Qt::QueuedConnection);

        struct ClassEnum
        {
            QString className;
            CComPtr<IEnumWbemClassObject> enumerator;
        };

        std::vector<ClassEnum> enums;
        for (std::size_t classIndex = 0; classIndex < classList.size(); ++classIndex)
        {
            const QString className = classList[classIndex];
            if (guardThis == nullptr || guardThis->m_wmiSubscribeStopFlag.load())
            {
                break;
            }

            const float setupProgressValue = 30.0f
                + (classList.empty()
                    ? 0.0f
                    : (static_cast<float>(classIndex + 1) * 15.0f / static_cast<float>(classList.size())));
            QMetaObject::invokeMethod(qApp, [guardThis, setupProgressValue, className]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                kPro.set(
                    guardThis->m_wmiSubscribeProgressPid,
                    QString("建立订阅: %1").arg(className).toStdString(),
                    0,
                    setupProgressValue);
            }, Qt::QueuedConnection);

            QString query = QStringLiteral("SELECT * FROM %1").arg(className);
            if (!whereClause.isEmpty())
            {
                query += QStringLiteral(" WHERE ") + whereClause;
            }

            CComPtr<IEnumWbemClassObject> enumerator;
            const HRESULT subResult = service->ExecNotificationQuery(
                bstr_t("WQL"),
                bstr_t(query.toStdWString().c_str()),
                WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
                nullptr,
                &enumerator);

            if (SUCCEEDED(subResult) && enumerator != nullptr)
            {
                enums.push_back(ClassEnum{ className, enumerator });
            }
            else
            {
                kLogEvent event;
                warn << event
                    << "[MonitorDock] WMI订阅类创建失败, className="
                    << className.toStdString()
                    << ", query="
                    << query.toStdString()
                    << ", hr="
                    << static_cast<unsigned long>(subResult)
                    << eol;
            }
        }

        if (enums.empty())
        {
            ::CoUninitialize();
            QMetaObject::invokeMethod(qApp, [guardThis]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_wmiSubscribeRunning.store(false);
                guardThis->m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 未建立任何有效订阅"));
                guardThis->m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                if (guardThis->m_wmiSubscribeProgressPid != 0)
                {
                    kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI订阅失败(无有效类)", 0, 100.0f);
                    guardThis->m_wmiSubscribeProgressPid = 0;
                }
            }, Qt::QueuedConnection);
            return;
        }

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI订阅已建立", 0, 45.0f);
        }, Qt::QueuedConnection);

        std::size_t eventCount = 0;
        while (guardThis != nullptr && !guardThis->m_wmiSubscribeStopFlag.load())
        {
            if (guardThis->m_wmiSubscribePaused.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                continue;
            }

            bool hasEvent = false;
            for (ClassEnum& classEnum : enums)
            {
                if (guardThis == nullptr || guardThis->m_wmiSubscribeStopFlag.load())
                {
                    break;
                }

                CComPtr<IWbemClassObject> eventObject;
                ULONG count = 0;
                const HRESULT nextResult = classEnum.enumerator->Next(100, 1, &eventObject, &count);
                if (FAILED(nextResult) || count == 0 || eventObject == nullptr)
                {
                    continue;
                }

                hasEvent = true;
                ++eventCount;

                QString pidText = QStringLiteral("-");
                QString detailText;

                VARIANT targetValue;
                ::VariantInit(&targetValue);
                if (SUCCEEDED(eventObject->Get(L"TargetInstance", 0, &targetValue, nullptr, nullptr))
                    && targetValue.vt == VT_UNKNOWN
                    && targetValue.punkVal != nullptr)
                {
                    CComPtr<IWbemClassObject> targetObject;
                    targetValue.punkVal->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(&targetObject));
                    if (targetObject != nullptr)
                    {
                        VARIANT processIdValue;
                        VARIANT nameValue;
                        ::VariantInit(&processIdValue);
                        ::VariantInit(&nameValue);
                        targetObject->Get(L"ProcessId", 0, &processIdValue, nullptr, nullptr);
                        targetObject->Get(L"Name", 0, &nameValue, nullptr, nullptr);

                        const QString pidPart = variantToText(processIdValue);
                        const QString namePart = variantToText(nameValue);
                        if (!pidPart.isEmpty() || !namePart.isEmpty())
                        {
                            pidText = QStringLiteral("%1 / %2").arg(pidPart, namePart);
                        }

                        ::VariantClear(&processIdValue);
                        ::VariantClear(&nameValue);
                    }
                }
                ::VariantClear(&targetValue);

                eventObject->BeginEnumeration(0);
                int propertyCount = 0;
                while (propertyCount < 8)
                {
                    BSTR propertyName = nullptr;
                    VARIANT propertyValue;
                    CIMTYPE typeValue = 0;
                    LONG flavor = 0;
                    ::VariantInit(&propertyValue);

                    const HRESULT nextProperty = eventObject->Next(0, &propertyName, &propertyValue, &typeValue, &flavor);
                    if (nextProperty == WBEM_S_NO_MORE_DATA)
                    {
                        ::VariantClear(&propertyValue);
                        break;
                    }

                    if (SUCCEEDED(nextProperty) && propertyName != nullptr)
                    {
                        if (!detailText.isEmpty())
                        {
                            detailText += QStringLiteral("; ");
                        }
                        detailText += QString::fromWCharArray(propertyName) + QStringLiteral("=") + variantToText(propertyValue);
                        ++propertyCount;
                    }

                    if (propertyName != nullptr)
                    {
                        ::SysFreeString(propertyName);
                    }
                    ::VariantClear(&propertyValue);
                }
                eventObject->EndEnumeration();

                if (detailText.isEmpty())
                {
                    detailText = QStringLiteral("<无详情>");
                }

                if (guardThis != nullptr)
                {
                    // 高频事件先入队，交由 UI 节流器批量刷新，避免每条事件都触发表格重排。
                    guardThis->enqueueWmiEventRow(classEnum.className, classEnum.className, pidText, detailText);
                }
            }

            if (!hasEvent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }

            if (guardThis != nullptr && eventCount % 20 == 0)
            {
                const float progressValue = 45.0f + static_cast<float>(std::min<std::size_t>(eventCount, 200)) * 0.25f;
                QMetaObject::invokeMethod(qApp, [guardThis, progressValue]() {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI事件接收中", 0, std::min(progressValue, 95.0f));
                }, Qt::QueuedConnection);
            }
        }

        ::CoUninitialize();

        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->flushWmiPendingRows();
            if (guardThis->m_wmiUiUpdateTimer != nullptr)
            {
                guardThis->m_wmiUiUpdateTimer->stop();
            }
            guardThis->m_wmiSubscribeRunning.store(false);
            guardThis->m_wmiSubscribePaused.store(false);
            guardThis->m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 已停止"));
            guardThis->m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));
            if (guardThis->m_wmiSubscribeProgressPid != 0)
            {
                kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI订阅结束", 0, 100.0f);
                guardThis->m_wmiSubscribeProgressPid = 0;
            }

            kLogEvent event;
            info << event
                << "[MonitorDock] WMI订阅线程已退出。"
                << eol;
        }, Qt::QueuedConnection);
    });
}

void MonitorDock::stopWmiSubscription()
{
    stopWmiSubscriptionInternal(false);
}

void MonitorDock::stopWmiSubscriptionInternal(bool waitForThread)
{
    {
        kLogEvent event;
        info << event
            << "[MonitorDock] 停止WMI订阅请求, waitForThread="
            << (waitForThread ? "true" : "false")
            << eol;
    }

    m_wmiSubscribeStopFlag.store(true);

    if (m_wmiSubscribeStatusLabel != nullptr)
    {
        m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 停止中..."));
        m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#AA7B1C;font-weight:600;"));
    }

    if (m_wmiSubscribeThread == nullptr || !m_wmiSubscribeThread->joinable())
    {
        m_wmiSubscribeThread.reset();
        m_wmiSubscribeRunning.store(false);
        m_wmiSubscribePaused.store(false);
        if (m_wmiUiUpdateTimer != nullptr)
        {
            m_wmiUiUpdateTimer->stop();
        }
        if (m_wmiSubscribeProgressPid != 0)
        {
            kPro.set(m_wmiSubscribeProgressPid, "WMI订阅结束", 0, 100.0f);
            m_wmiSubscribeProgressPid = 0;
        }
        kLogEvent event;
        dbg << event
            << "[MonitorDock] 停止WMI订阅：当前无活动线程。"
            << eol;
        return;
    }

    if (waitForThread)
    {
        // 析构路径：同步等待线程退出，确保对象销毁时无并发访问。
        m_wmiSubscribeThread->join();
        m_wmiSubscribeThread.reset();
        m_wmiSubscribeRunning.store(false);
        m_wmiSubscribePaused.store(false);
        if (m_wmiUiUpdateTimer != nullptr)
        {
            m_wmiUiUpdateTimer->stop();
        }
        if (m_wmiSubscribeProgressPid != 0)
        {
            kPro.set(m_wmiSubscribeProgressPid, "WMI订阅结束", 0, 100.0f);
            m_wmiSubscribeProgressPid = 0;
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] 停止WMI订阅：同步等待线程结束完成。"
            << eol;
        return;
    }

    // 交互路径：把 join 下放到后台，避免主线程等待导致卡顿。
    std::unique_ptr<std::thread> joinThread = std::move(m_wmiSubscribeThread);
    QPointer<MonitorDock> guardThis(this);
    std::thread([joinThread = std::move(joinThread), guardThis]() mutable {
        if (joinThread != nullptr && joinThread->joinable())
        {
            joinThread->join();
        }
        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_wmiSubscribeRunning.store(false);
            guardThis->m_wmiSubscribePaused.store(false);
            if (guardThis->m_wmiUiUpdateTimer != nullptr)
            {
                guardThis->m_wmiUiUpdateTimer->stop();
            }
            guardThis->flushWmiPendingRows();
            guardThis->m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 已停止"));
            guardThis->m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));
            if (guardThis->m_wmiSubscribeProgressPid != 0)
            {
                kPro.set(guardThis->m_wmiSubscribeProgressPid, "WMI订阅结束", 0, 100.0f);
                guardThis->m_wmiSubscribeProgressPid = 0;
            }

            kLogEvent event;
            info << event
                << "[MonitorDock] 停止WMI订阅：异步线程回收完成。"
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void MonitorDock::setWmiSubscriptionPaused(bool paused)
{
    if (!m_wmiSubscribeRunning.load())
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] 忽略WMI暂停操作：订阅未运行。"
            << eol;
        return;
    }

    m_wmiSubscribePaused.store(paused);
    if (paused)
    {
        m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 已暂停"));
        m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#AA7B1C;font-weight:600;"));
    }
    else
    {
        m_wmiSubscribeStatusLabel->setText(QStringLiteral("● 订阅中"));
        m_wmiSubscribeStatusLabel->setStyleSheet(QStringLiteral("color:#1F4E7A;font-weight:600;"));
    }

    kLogEvent event;
    info << event
        << "[MonitorDock] WMI订阅暂停状态变更, paused="
        << (paused ? "true" : "false")
        << eol;
}

void MonitorDock::enqueueWmiEventRow(
    const QString& providerName,
    const QString& className,
    const QString& pidAndName,
    const QString& detailText)
{
    // 后台线程仅负责写入待处理队列，避免直接触发表格重绘造成主线程抖动。
    QStringList rowValues;
    rowValues.reserve(5);
    rowValues << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
        << providerName
        << className
        << pidAndName
        << detailText;

    std::lock_guard<std::mutex> lock(m_wmiPendingMutex);
    m_wmiPendingRows.push_back(std::move(rowValues));
}

void MonitorDock::flushWmiPendingRows()
{
    // 主线程批量刷入：每个周期限制条数，防止一次性插入过多行阻塞 UI。
    std::vector<QStringList> rowsToFlush;
    {
        std::lock_guard<std::mutex> lock(m_wmiPendingMutex);
        if (m_wmiPendingRows.empty())
        {
            return;
        }

        constexpr std::size_t kMaxRowsPerFlush = 240;
        const std::size_t flushCount = std::min<std::size_t>(kMaxRowsPerFlush, m_wmiPendingRows.size());
        rowsToFlush.reserve(flushCount);
        for (std::size_t rowIndex = 0; rowIndex < flushCount; ++rowIndex)
        {
            rowsToFlush.push_back(std::move(m_wmiPendingRows[rowIndex]));
        }
        using DiffType = std::vector<QStringList>::difference_type;
        m_wmiPendingRows.erase(
            m_wmiPendingRows.begin(),
            m_wmiPendingRows.begin() + static_cast<DiffType>(flushCount));
    }

    for (const QStringList& rowValues : rowsToFlush)
    {
        if (rowValues.size() < 5)
        {
            continue;
        }
        appendWmiEventRow(rowValues[1], rowValues[2], rowValues[3], rowValues[4]);

        QTableWidgetItem* tsItem = m_wmiEventTable->item(m_wmiEventTable->rowCount() - 1, 0);
        if (tsItem != nullptr)
        {
            tsItem->setText(rowValues[0]);
            tsItem->setToolTip(rowValues[0]);
        }
    }

    // 每批次刷入完成后统一应用筛选，避免逐行重算造成 UI 抖动。
    applyWmiEventFilter();

    kLogEvent event;
    dbg << event
        << "[MonitorDock] 批量刷新WMI事件到UI, flushCount="
        << rowsToFlush.size()
        << ", tableRowCount="
        << m_wmiEventTable->rowCount()
        << eol;
}

void MonitorDock::appendWmiEventRow(
    const QString& providerName,
    const QString& className,
    const QString& pidAndName,
    const QString& detailText)
{
    const int row = m_wmiEventTable->rowCount();
    m_wmiEventTable->insertRow(row);

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));

    QTableWidgetItem* tsItem = new QTableWidgetItem(ts);
    QTableWidgetItem* providerItem = new QTableWidgetItem(providerName);
    QTableWidgetItem* classItem = new QTableWidgetItem(className);
    QTableWidgetItem* pidItem = new QTableWidgetItem(pidAndName);
    QTableWidgetItem* detailItem = new QTableWidgetItem(detailText);

    tsItem->setToolTip(ts);
    providerItem->setToolTip(providerName);
    classItem->setToolTip(className);
    pidItem->setToolTip(pidAndName);
    detailItem->setToolTip(detailText);

    m_wmiEventTable->setItem(row, 0, tsItem);
    m_wmiEventTable->setItem(row, 1, providerItem);
    m_wmiEventTable->setItem(row, 2, classItem);
    m_wmiEventTable->setItem(row, 3, pidItem);
    m_wmiEventTable->setItem(row, 4, detailItem);

    while (m_wmiEventTable->rowCount() > 6000)
    {
        m_wmiEventTable->removeRow(0);
    }
}

void MonitorDock::showWmiEventContextMenu(const QPoint& position)
{
    const QModelIndex index = m_wmiEventTable->indexAt(position);
    if (!index.isValid())
    {
        return;
    }

    const int row = index.row();
    const int col = index.column();

    QMenu menu(this);
    QAction* copyCellAction = menu.addAction(QIcon(":/Icon/log_copy.svg"), QStringLiteral("复制单元格"));
    QAction* copyRowAction = menu.addAction(QIcon(":/Icon/log_clipboard.svg"), QStringLiteral("复制整行"));
    menu.addSeparator();
    QAction* gotoProcessAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));

    QAction* action = menu.exec(m_wmiEventTable->viewport()->mapToGlobal(position));
    if (action == nullptr)
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] WMI事件右键菜单取消。"
            << eol;
        return;
    }

    if (action == copyCellAction)
    {
        QTableWidgetItem* item = m_wmiEventTable->item(row, col);
        if (item != nullptr)
        {
            QApplication::clipboard()->setText(item->text());
        }
        kLogEvent event;
        dbg << event
            << "[MonitorDock] WMI事件右键操作：复制单元格, row="
            << row
            << ", col="
            << col
            << eol;
        return;
    }

    if (action == copyRowAction)
    {
        QStringList values;
        for (int i = 0; i < m_wmiEventTable->columnCount(); ++i)
        {
            QTableWidgetItem* item = m_wmiEventTable->item(row, i);
            values << (item != nullptr ? item->text() : QString());
        }
        QApplication::clipboard()->setText(values.join('\t'));
        kLogEvent event;
        dbg << event
            << "[MonitorDock] WMI事件右键操作：复制整行, row="
            << row
            << eol;
        return;
    }

    if (action == gotoProcessAction)
    {
        QTableWidgetItem* pidItem = m_wmiEventTable->item(row, 3);
        if (pidItem == nullptr)
        {
            return;
        }

        std::uint32_t pid = 0;
        if (!parsePid(pidItem->text(), pid))
        {
            kLogEvent event;
            warn << event
                << "[MonitorDock] WMI事件右键操作失败：PID解析失败, text="
                << pidItem->text().toStdString()
                << eol;
            QMessageBox::information(this, QStringLiteral("WMI事件"), QStringLiteral("未解析到有效PID。"));
            return;
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] WMI事件右键操作：转到进程详情, pid="
            << pid
            << eol;
        openProcessDetail(this, pid);
    }
}

void MonitorDock::refreshEtwProvidersAsync()
{
    kLogEvent startEvent;
    info << startEvent
        << "[MonitorDock] 开始异步刷新ETW Provider。"
        << eol;

    m_etwProviderStatusLabel->setText(QStringLiteral("● 刷新中..."));
    m_etwProviderStatusLabel->setStyleSheet(QStringLiteral("color:#1F4E7A;font-weight:600;"));

    if (m_etwCaptureProgressPid == 0)
    {
        m_etwCaptureProgressPid = kPro.add("监控", "刷新ETW Provider");
    }
    kPro.set(m_etwCaptureProgressPid, "调用TdhEnumerateProviders", 0, 10.0f);

    QPointer<MonitorDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<EtwProviderEntry> providers;

        ULONG bufferSize = 0;
        ULONG status = ::TdhEnumerateProviders(nullptr, &bufferSize);
        if (status == ERROR_INSUFFICIENT_BUFFER && bufferSize > 0)
        {
            std::vector<unsigned char> buffer(bufferSize, 0);
            auto* info = reinterpret_cast<PROVIDER_ENUMERATION_INFO*>(buffer.data());
            status = ::TdhEnumerateProviders(info, &bufferSize);
            if (status == ERROR_SUCCESS && info != nullptr)
            {
                providers.reserve(info->NumberOfProviders);
                for (ULONG i = 0; i < info->NumberOfProviders; ++i)
                {
                    const TRACE_PROVIDER_INFO& item = info->TraceProviderInfoArray[i];
                    const wchar_t* namePtr = reinterpret_cast<const wchar_t*>(buffer.data() + item.ProviderNameOffset);

                    EtwProviderEntry entry;
                    entry.providerName = namePtr != nullptr ? QString::fromWCharArray(namePtr) : QStringLiteral("<Unknown>");
                    entry.providerGuidText = guidToText(item.ProviderGuid);
                    providers.push_back(entry);
                }
            }
        }

        QMetaObject::invokeMethod(qApp, [guardThis, providers, status]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_etwProviders = providers;
            guardThis->m_etwProviderList->clear();

            for (const EtwProviderEntry& entry : guardThis->m_etwProviders)
            {
                QListWidgetItem* item = new QListWidgetItem(
                    QStringLiteral("%1 (%2)").arg(entry.providerName, entry.providerGuidText),
                    guardThis->m_etwProviderList);
                item->setData(Qt::UserRole, entry.providerName);
                item->setData(Qt::UserRole + 1, entry.providerGuidText);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
            }

            if (status == ERROR_SUCCESS)
            {
                guardThis->m_etwProviderStatusLabel->setText(
                    QStringLiteral("● 已刷新 %1 项").arg(guardThis->m_etwProviders.size()));
                guardThis->m_etwProviderStatusLabel->setStyleSheet(QStringLiteral("color:#2F7D32;font-weight:600;"));
                kPro.set(guardThis->m_etwCaptureProgressPid, "ETW Provider完成", 0, 100.0f);

                kLogEvent event;
                info << event
                    << "[MonitorDock] ETW Provider刷新完成, providerCount="
                    << guardThis->m_etwProviders.size()
                    << eol;
            }
            else
            {
                guardThis->m_etwProviderStatusLabel->setText(QStringLiteral("● 刷新失败:%1").arg(status));
                guardThis->m_etwProviderStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                kPro.set(guardThis->m_etwCaptureProgressPid, "ETW Provider失败", 0, 100.0f);

                kLogEvent event;
                err << event
                    << "[MonitorDock] ETW Provider刷新失败, status="
                    << status
                    << eol;
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void WINAPI MonitorDock::etwEventRecordCallback(struct _EVENT_RECORD* eventRecordPtr)
{
    if (eventRecordPtr == nullptr)
    {
        return;
    }

    EVENT_RECORD* eventRecord = reinterpret_cast<EVENT_RECORD*>(eventRecordPtr);
    if (eventRecord == nullptr || eventRecord->UserContext == nullptr)
    {
        return;
    }

    auto* monitorDock = reinterpret_cast<MonitorDock*>(eventRecord->UserContext);
    monitorDock->enqueueEtwEventFromRecord(eventRecordPtr);
}

void MonitorDock::enqueueEtwEventFromRecord(const struct _EVENT_RECORD* eventRecordPtr)
{
    const EVENT_RECORD* eventRecord = reinterpret_cast<const EVENT_RECORD*>(eventRecordPtr);
    if (eventRecord == nullptr)
    {
        return;
    }

    if (m_etwCaptureStopFlag.load() || m_etwCapturePaused.load())
    {
        return;
    }

    // Provider 名称优先按 GUID 回查缓存列表，找不到则直接显示 GUID。
    const QString providerGuidText = guidToText(eventRecord->EventHeader.ProviderId);
    QString providerName = providerGuidText;
    for (const EtwProviderEntry& entry : m_etwProviders)
    {
        if (entry.providerGuidText.compare(providerGuidText, Qt::CaseInsensitive) == 0)
        {
            providerName = entry.providerName;
            break;
        }
    }

    const int eventId = static_cast<int>(eventRecord->EventHeader.EventDescriptor.Id);
    QString eventName = queryEtwEventName(eventRecord);
    if (eventName.trimmed().isEmpty())
    {
        eventName = QStringLiteral("Event_%1").arg(eventId);
    }

    const std::uint32_t pidValue = static_cast<std::uint32_t>(eventRecord->EventHeader.ProcessId);
    const std::uint32_t tidValue = static_cast<std::uint32_t>(eventRecord->EventHeader.ThreadId);
    const QString timestampText = etwTimestamp100nsText(eventRecord);
    const QString activityIdText = guidToText(eventRecord->EventHeader.ActivityId);

    const ULONGLONG keywordValue = eventRecord->EventHeader.EventDescriptor.Keyword;
    const UCHAR levelValue = eventRecord->EventHeader.EventDescriptor.Level;
    const UCHAR opcodeValue = eventRecord->EventHeader.EventDescriptor.Opcode;
    const USHORT taskValue = eventRecord->EventHeader.EventDescriptor.Task;
    const QString detailJson = QStringLiteral(
        "{\"providerGuid\":\"%1\",\"eventId\":%2,\"level\":%3,\"task\":%4,\"opcode\":%5,\"keyword\":\"0x%6\"}")
        .arg(providerGuidText)
        .arg(eventId)
        .arg(static_cast<int>(levelValue))
        .arg(static_cast<int>(taskValue))
        .arg(static_cast<int>(opcodeValue))
        .arg(QString::number(static_cast<qulonglong>(keywordValue), 16).toUpper());

    QStringList rowValues;
    rowValues << timestampText;
    rowValues << providerName;
    rowValues << QString::number(eventId);
    rowValues << eventName;
    rowValues << QStringLiteral("%1 / %2").arg(pidValue).arg(tidValue);
    rowValues << detailJson;
    rowValues << activityIdText;

    {
        std::lock_guard<std::mutex> lock(m_etwPendingMutex);
        m_etwPendingRows.push_back(rowValues);
    }
}

void MonitorDock::startEtwCapture()
{
    if (m_etwCaptureRunning.load())
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] 忽略启动ETW：当前已在监听。"
            << eol;
        return;
    }

    if (m_etwCaptureThread != nullptr && m_etwCaptureThread->joinable())
    {
        m_etwCaptureThread->join();
        m_etwCaptureThread.reset();
    }

    // 收集勾选的 Provider，并解析 GUID。
    struct ProviderSelection
    {
        QString name; // Provider 名称，用于日志/显示。
        GUID guid{};  // Provider GUID，用于 EnableTraceEx2。
    };

    std::vector<ProviderSelection> selectedProviders;
    selectedProviders.reserve(static_cast<std::size_t>(
        m_etwProviderList->count()
        + (m_etwPresetProviderList != nullptr ? m_etwPresetProviderList->count() : 0)
        + 1));

    auto tryAppendProvider = [&selectedProviders](const QString& providerName, const QString& guidText) {
        GUID guidValue{};
        if (!parseGuidText(guidText, guidValue))
        {
            return false;
        }

        const bool duplicate = std::any_of(
            selectedProviders.begin(),
            selectedProviders.end(),
            [&guidValue](const ProviderSelection& item) {
                return ::IsEqualGUID(item.guid, guidValue) != FALSE;
            });
        if (duplicate)
        {
            return true;
        }

        ProviderSelection selection;
        selection.name = providerName;
        selection.guid = guidValue;
        selectedProviders.push_back(selection);
        return true;
    };

    for (int i = 0; i < m_etwProviderList->count(); ++i)
    {
        QListWidgetItem* item = m_etwProviderList->item(i);
        if (item == nullptr || item->checkState() != Qt::Checked)
        {
            continue;
        }
        const QString providerName = item->data(Qt::UserRole).toString();
        const QString providerGuid = item->data(Qt::UserRole + 1).toString();
        tryAppendProvider(providerName, providerGuid);
    }

    // 预置模板勾选项：按名称到系统 Provider 列表里映射并追加 GUID。
    int presetCheckedCount = 0;
    int presetMatchedCount = 0;
    if (m_etwPresetProviderList != nullptr)
    {
        for (int i = 0; i < m_etwPresetProviderList->count(); ++i)
        {
            QListWidgetItem* item = m_etwPresetProviderList->item(i);
            if (item == nullptr || item->checkState() != Qt::Checked)
            {
                continue;
            }
            ++presetCheckedCount;

            const QString presetProviderName = item->data(Qt::UserRole).toString().trimmed();
            if (presetProviderName.isEmpty())
            {
                continue;
            }

            const auto exactFound = std::find_if(
                m_etwProviders.begin(),
                m_etwProviders.end(),
                [presetProviderName](const EtwProviderEntry& entry) {
                    return entry.providerName.compare(presetProviderName, Qt::CaseInsensitive) == 0;
                });

            bool matched = false;
            if (exactFound != m_etwProviders.end())
            {
                matched = tryAppendProvider(exactFound->providerName, exactFound->providerGuidText);
            }
            else
            {
                // 模糊回退：允许模板名与系统 Provider 名称存在后缀差异。
                const auto fuzzyFound = std::find_if(
                    m_etwProviders.begin(),
                    m_etwProviders.end(),
                    [presetProviderName](const EtwProviderEntry& entry) {
                        return entry.providerName.contains(presetProviderName, Qt::CaseInsensitive)
                            || presetProviderName.contains(entry.providerName, Qt::CaseInsensitive);
                    });
                if (fuzzyFound != m_etwProviders.end())
                {
                    matched = tryAppendProvider(fuzzyFound->providerName, fuzzyFound->providerGuidText);
                }
            }

            if (matched)
            {
                ++presetMatchedCount;
            }
            else
            {
                kLogEvent event;
                warn << event
                    << "[MonitorDock] ETW预置模板未命中系统Provider, template="
                    << presetProviderName.toStdString()
                    << eol;
            }
        }
    }

    // 手动输入既支持 GUID，也支持 Provider 名称。
    const QString manualText = m_etwManualProviderEdit->text().trimmed();
    if (!manualText.isEmpty())
    {
        if (!tryAppendProvider(manualText, manualText))
        {
            const auto found = std::find_if(
                m_etwProviders.begin(),
                m_etwProviders.end(),
                [manualText](const EtwProviderEntry& entry) {
                    return entry.providerName.compare(manualText, Qt::CaseInsensitive) == 0;
                });
            if (found != m_etwProviders.end())
            {
                tryAppendProvider(found->providerName, found->providerGuidText);
            }
        }
    }

    {
        kLogEvent event;
        info << event
            << "[MonitorDock] ETW预置模板统计, checked="
            << presetCheckedCount
            << ", matched="
            << presetMatchedCount
            << eol;
    }

    if (selectedProviders.empty())
    {
        kLogEvent event;
        warn << event
            << "[MonitorDock] 启动ETW失败：未选择可用Provider。"
            << eol;
        QMessageBox::information(this, QStringLiteral("ETW监听"), QStringLiteral("请至少选择一个可解析 GUID 的 Provider。"));
        return;
    }

    // 清空待刷新队列，避免把上一轮数据混入本轮。
    {
        std::lock_guard<std::mutex> lock(m_etwPendingMutex);
        m_etwPendingRows.clear();
    }

    m_etwCaptureRunning.store(true);
    m_etwCapturePaused.store(false);
    m_etwCaptureStopFlag.store(false);
    m_etwSessionHandle = 0;
    m_etwTraceHandle = 0;
    m_etwSessionName = QStringLiteral("KswordEtw_%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));

    if (m_etwCaptureProgressPid == 0)
    {
        m_etwCaptureProgressPid = kPro.add("监控", "ETW监听");
    }
    kPro.set(m_etwCaptureProgressPid, "准备ETW实时会话", 0, 10.0f);

    m_etwCaptureStatusLabel->setText(QStringLiteral("● 监听中"));
    m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#1F4E7A;font-weight:600;"));

    if (m_etwUiUpdateTimer != nullptr && !m_etwUiUpdateTimer->isActive())
    {
        m_etwUiUpdateTimer->start();
    }

    const UCHAR traceLevel = etwLevelFromText(m_etwLevelCombo->currentText());
    const ULONGLONG keywordMask = parseKeywordMaskText(m_etwKeywordMaskEdit->text());
    const ULONG bufferSizeKb = static_cast<ULONG>(m_etwBufferSizeSpin->value());
    const ULONG minBuffer = static_cast<ULONG>(m_etwMinBufferSpin->value());
    const ULONG maxBuffer = static_cast<ULONG>(m_etwMaxBufferSpin->value());

    {
        kLogEvent event;
        info << event
            << "[MonitorDock] 启动ETW监听, providerCount="
            << selectedProviders.size()
            << ", level="
            << static_cast<int>(traceLevel)
            << ", keywordMask=0x"
            << QString::number(static_cast<qulonglong>(keywordMask), 16).toUpper().toStdString()
            << ", bufferSizeKb="
            << bufferSizeKb
            << ", minBuffer="
            << minBuffer
            << ", maxBuffer="
            << maxBuffer
            << eol;
    }

    QPointer<MonitorDock> guardThis(this);
    m_etwCaptureThread = std::make_unique<std::thread>(
        [guardThis, selectedProviders, traceLevel, keywordMask, bufferSizeKb, minBuffer, maxBuffer]() {
        if (guardThis == nullptr)
        {
            return;
        }

        const std::wstring sessionNameWide = guardThis->m_etwSessionName.toStdWString();
        const ULONG propertyBufferSize = static_cast<ULONG>(
            sizeof(EVENT_TRACE_PROPERTIES) + (sessionNameWide.size() + 1) * sizeof(wchar_t));
        std::vector<unsigned char> propertyBuffer(propertyBufferSize, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());

        properties->Wnode.BufferSize = propertyBufferSize;
        properties->Wnode.ClientContext = 2; // 2=SystemTime（100ns）。
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->BufferSize = bufferSizeKb;
        properties->MinimumBuffers = minBuffer;
        properties->MaximumBuffers = maxBuffer;

        wchar_t* loggerNamePtr = reinterpret_cast<wchar_t*>(propertyBuffer.data() + properties->LoggerNameOffset);
        if (!sessionNameWide.empty())
        {
            ::wcscpy_s(loggerNamePtr, sessionNameWide.size() + 1, sessionNameWide.c_str());
        }

        TRACEHANDLE sessionHandle = 0;
        ULONG startStatus = ::StartTraceW(&sessionHandle, loggerNamePtr, properties);
        if (startStatus == ERROR_ALREADY_EXISTS)
        {
            ::ControlTraceW(0, loggerNamePtr, properties, EVENT_TRACE_CONTROL_STOP);
            startStatus = ::StartTraceW(&sessionHandle, loggerNamePtr, properties);
        }

        if (startStatus != ERROR_SUCCESS)
        {
            QMetaObject::invokeMethod(qApp, [guardThis, startStatus]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_etwCaptureRunning.store(false);
                guardThis->m_etwCapturePaused.store(false);
                guardThis->m_etwCaptureStatusLabel->setText(QStringLiteral("● 启动失败:%1").arg(startStatus));
                guardThis->m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                kPro.set(guardThis->m_etwCaptureProgressPid, "ETW会话启动失败", 0, 100.0f);
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_etwSessionHandle = static_cast<std::uint64_t>(sessionHandle);
        kPro.set(guardThis->m_etwCaptureProgressPid, "启用Provider", 0, 30.0f);

        int enableSuccessCount = 0;
        for (const ProviderSelection& provider : selectedProviders)
        {
            if (guardThis == nullptr || guardThis->m_etwCaptureStopFlag.load())
            {
                break;
            }

            const ULONG enableStatus = ::EnableTraceEx2(
                sessionHandle,
                &provider.guid,
                EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                traceLevel,
                keywordMask,
                0,
                0,
                nullptr);

            if (enableStatus == ERROR_SUCCESS)
            {
                ++enableSuccessCount;
            }
            else
            {
                kLogEvent enableEvent;
                warn << enableEvent
                    << "[MonitorDock] EnableTraceEx2失败 provider="
                    << provider.name.toStdString()
                    << ", status="
                    << enableStatus
                    << eol;
            }
        }

        if (enableSuccessCount == 0)
        {
            ::ControlTraceW(sessionHandle, loggerNamePtr, properties, EVENT_TRACE_CONTROL_STOP);
            guardThis->m_etwSessionHandle = 0;
            QMetaObject::invokeMethod(qApp, [guardThis]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_etwCaptureRunning.store(false);
                guardThis->m_etwCapturePaused.store(false);
                guardThis->m_etwCaptureStatusLabel->setText(QStringLiteral("● 无可用Provider"));
                guardThis->m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                kPro.set(guardThis->m_etwCaptureProgressPid, "Provider启用失败", 0, 100.0f);
            }, Qt::QueuedConnection);
            return;
        }

        // 打开实时消费句柄并进入 ProcessTrace 阻塞读取。
        EVENT_TRACE_LOGFILEW logFile{};
        logFile.LoggerName = loggerNamePtr;
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        logFile.EventRecordCallback = &MonitorDock::etwEventRecordCallback;
        logFile.Context = guardThis.data();

        TRACEHANDLE traceHandle = ::OpenTraceW(&logFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            ::ControlTraceW(sessionHandle, loggerNamePtr, properties, EVENT_TRACE_CONTROL_STOP);
            guardThis->m_etwSessionHandle = 0;

            const ULONG lastError = ::GetLastError();
            QMetaObject::invokeMethod(qApp, [guardThis, lastError]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_etwCaptureRunning.store(false);
                guardThis->m_etwCapturePaused.store(false);
                guardThis->m_etwCaptureStatusLabel->setText(QStringLiteral("● OpenTrace失败:%1").arg(lastError));
                guardThis->m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#A43434;font-weight:600;"));
                kPro.set(guardThis->m_etwCaptureProgressPid, "OpenTrace失败", 0, 100.0f);
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_etwTraceHandle = static_cast<std::uint64_t>(traceHandle);
        kPro.set(guardThis->m_etwCaptureProgressPid, "ETW事件接收中", 0, 55.0f);

        const ULONG processStatus = ::ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        ::CloseTrace(traceHandle);
        guardThis->m_etwTraceHandle = 0;

        ::ControlTraceW(sessionHandle, loggerNamePtr, properties, EVENT_TRACE_CONTROL_STOP);
        guardThis->m_etwSessionHandle = 0;

        QMetaObject::invokeMethod(qApp, [guardThis, processStatus]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_etwCaptureRunning.store(false);
            guardThis->m_etwCapturePaused.store(false);

            if (processStatus == ERROR_SUCCESS)
            {
                guardThis->m_etwCaptureStatusLabel->setText(QStringLiteral("● 已停止"));
                guardThis->m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));
            }
            else
            {
                guardThis->m_etwCaptureStatusLabel->setText(QStringLiteral("● 处理结束:%1").arg(processStatus));
                guardThis->m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#AA7B1C;font-weight:600;"));
            }
            kPro.set(guardThis->m_etwCaptureProgressPid, "ETW监听结束", 0, 100.0f);
        }, Qt::QueuedConnection);
    });
}

void MonitorDock::stopEtwCapture()
{
    stopEtwCaptureInternal(false);
}

void MonitorDock::stopEtwCaptureInternal(bool waitForThread)
{
    {
        kLogEvent event;
        info << event
            << "[MonitorDock] 停止ETW请求, waitForThread="
            << (waitForThread ? "true" : "false")
            << eol;
    }

    m_etwCaptureStopFlag.store(true);

    // 先关闭消费句柄，打断 ProcessTrace 阻塞。
    if (m_etwTraceHandle != 0)
    {
        TRACEHANDLE traceHandle = static_cast<TRACEHANDLE>(m_etwTraceHandle);
        ::CloseTrace(traceHandle);
        m_etwTraceHandle = 0;
    }

    // 再停止会话，确保 ETW 内核资源被释放。
    if (m_etwSessionHandle != 0)
    {
        const std::wstring sessionNameWide = m_etwSessionName.toStdWString();
        const ULONG propertyBufferSize = static_cast<ULONG>(
            sizeof(EVENT_TRACE_PROPERTIES) + (sessionNameWide.size() + 1) * sizeof(wchar_t));
        std::vector<unsigned char> propertyBuffer(propertyBufferSize, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());
        properties->Wnode.BufferSize = propertyBufferSize;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        wchar_t* loggerNamePtr = reinterpret_cast<wchar_t*>(propertyBuffer.data() + properties->LoggerNameOffset);
        if (!sessionNameWide.empty())
        {
            ::wcscpy_s(loggerNamePtr, sessionNameWide.size() + 1, sessionNameWide.c_str());
        }

        ::ControlTraceW(
            static_cast<TRACEHANDLE>(m_etwSessionHandle),
            loggerNamePtr,
            properties,
            EVENT_TRACE_CONTROL_STOP);
        m_etwSessionHandle = 0;
    }

    if (m_etwCaptureThread == nullptr || !m_etwCaptureThread->joinable())
    {
        m_etwCaptureThread.reset();
        m_etwCaptureRunning.store(false);
        m_etwCapturePaused.store(false);
        if (m_etwCaptureStatusLabel != nullptr)
        {
            m_etwCaptureStatusLabel->setText(QStringLiteral("● 已停止"));
            m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));
        }
        if (m_etwUiUpdateTimer != nullptr && m_etwUiUpdateTimer->isActive())
        {
            m_etwUiUpdateTimer->stop();
        }
        kLogEvent event;
        dbg << event
            << "[MonitorDock] 停止ETW：当前无活动线程。"
            << eol;
        return;
    }

    if (waitForThread)
    {
        // 析构路径：同步等待线程退出，确保对象销毁前完全回收 ETW 线程。
        m_etwCaptureThread->join();
        m_etwCaptureThread.reset();
        m_etwCaptureRunning.store(false);
        m_etwCapturePaused.store(false);
        if (m_etwCaptureStatusLabel != nullptr)
        {
            m_etwCaptureStatusLabel->setText(QStringLiteral("● 已停止"));
            m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));
        }
        if (m_etwUiUpdateTimer != nullptr && m_etwUiUpdateTimer->isActive())
        {
            m_etwUiUpdateTimer->stop();
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] 停止ETW：同步等待线程结束完成。"
            << eol;
        return;
    }

    // 交互路径：异步 join，防止点击“停止监听”时 UI 阻塞。
    if (m_etwCaptureStatusLabel != nullptr)
    {
        m_etwCaptureStatusLabel->setText(QStringLiteral("● 停止中..."));
        m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#AA7B1C;font-weight:600;"));
    }
    std::unique_ptr<std::thread> joinThread = std::move(m_etwCaptureThread);
    QPointer<MonitorDock> guardThis(this);
    std::thread([joinThread = std::move(joinThread), guardThis]() mutable {
        if (joinThread != nullptr && joinThread->joinable())
        {
            joinThread->join();
        }
        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_etwCaptureRunning.store(false);
            guardThis->m_etwCapturePaused.store(false);
            if (guardThis->m_etwCaptureStatusLabel != nullptr)
            {
                guardThis->m_etwCaptureStatusLabel->setText(QStringLiteral("● 已停止"));
                guardThis->m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#4A4A4A;font-weight:600;"));
            }
            if (guardThis->m_etwUiUpdateTimer != nullptr && guardThis->m_etwUiUpdateTimer->isActive())
            {
                guardThis->m_etwUiUpdateTimer->stop();
            }

            kLogEvent event;
            info << event
                << "[MonitorDock] 停止ETW：异步线程回收完成。"
                << eol;
        }, Qt::QueuedConnection);
    }).detach();

    if (m_etwUiUpdateTimer != nullptr && m_etwUiUpdateTimer->isActive())
    {
        m_etwUiUpdateTimer->stop();
    }
}

void MonitorDock::setEtwCapturePaused(bool paused)
{
    if (!m_etwCaptureRunning.load())
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] 忽略ETW暂停操作：监听未运行。"
            << eol;
        return;
    }

    m_etwCapturePaused.store(paused);
    if (paused)
    {
        m_etwCaptureStatusLabel->setText(QStringLiteral("● 已暂停"));
        m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#AA7B1C;font-weight:600;"));
    }
    else
    {
        m_etwCaptureStatusLabel->setText(QStringLiteral("● 监听中"));
        m_etwCaptureStatusLabel->setStyleSheet(QStringLiteral("color:#1F4E7A;font-weight:600;"));
    }

    kLogEvent event;
    info << event
        << "[MonitorDock] ETW暂停状态变更, paused="
        << (paused ? "true" : "false")
        << eol;
}

void MonitorDock::appendEtwEventRow(
    const QString& providerName,
    int eventId,
    const QString& eventName,
    std::uint32_t pidValue,
    std::uint32_t tidValue,
    const QString& detailJson,
    const QString& activityIdText)
{
    const int row = m_etwEventTable->rowCount();
    m_etwEventTable->insertRow(row);

    const QStringList values{
        now100nsText(),
        providerName,
        QString::number(eventId),
        eventName,
        QStringLiteral("%1 / %2").arg(pidValue).arg(tidValue),
        detailJson,
        activityIdText
    };

    for (int i = 0; i < values.size(); ++i)
    {
        QTableWidgetItem* item = new QTableWidgetItem(values.at(i));
        item->setToolTip(values.at(i));
        m_etwEventTable->setItem(row, i, item);
    }
}

void MonitorDock::exportEtwRowsToTsv()
{
    if (m_etwEventTable->rowCount() == 0)
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] ETW导出取消：无可导出事件。"
            << eol;
        QMessageBox::information(this, QStringLiteral("导出ETW"), QStringLiteral("当前没有可导出的事件。"));
        return;
    }

    const QString defaultName = QStringLiteral("etw_events_%1.tsv")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出ETW结果"),
        defaultName,
        QStringLiteral("TSV文件 (*.tsv);;文本文件 (*.txt)"));

    if (path.trimmed().isEmpty())
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] ETW导出取消：用户未选择路径。"
            << eol;
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        kLogEvent event;
        err << event
            << "[MonitorDock] ETW导出失败：无法写入文件, path="
            << path.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("导出ETW"), QStringLiteral("无法写入文件：%1").arg(path));
        return;
    }

    QTextStream out(&file);

    QStringList header;
    for (int col = 0; col < m_etwEventTable->columnCount(); ++col)
    {
        QTableWidgetItem* item = m_etwEventTable->horizontalHeaderItem(col);
        header << (item != nullptr ? item->text() : QString());
    }
    out << header.join('\t') << '\n';

    for (int row = 0; row < m_etwEventTable->rowCount(); ++row)
    {
        QStringList values;
        for (int col = 0; col < m_etwEventTable->columnCount(); ++col)
        {
            QTableWidgetItem* item = m_etwEventTable->item(row, col);
            values << (item != nullptr ? item->text().replace('\t', ' ') : QString());
        }
        out << values.join('\t') << '\n';
    }

    file.close();

    kLogEvent event;
    info << event << "[MonitorDock] ETW导出完成:" << path.toStdString() << ", rows=" << m_etwEventTable->rowCount() << eol;
    QMessageBox::information(this, QStringLiteral("导出ETW"), QStringLiteral("导出完成：%1").arg(path));
}

void MonitorDock::showEtwEventContextMenu(const QPoint& position)
{
    const QModelIndex index = m_etwEventTable->indexAt(position);
    if (!index.isValid())
    {
        return;
    }

    const int row = index.row();
    const int col = index.column();

    QMenu menu(this);
    QAction* copyCellAction = menu.addAction(QIcon(":/Icon/log_copy.svg"), QStringLiteral("复制单元格"));
    QAction* copyRowAction = menu.addAction(QIcon(":/Icon/log_clipboard.svg"), QStringLiteral("复制整行"));
    menu.addSeparator();
    QAction* gotoProcessAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));

    QAction* action = menu.exec(m_etwEventTable->viewport()->mapToGlobal(position));
    if (action == nullptr)
    {
        kLogEvent event;
        dbg << event
            << "[MonitorDock] ETW事件右键菜单取消。"
            << eol;
        return;
    }

    if (action == copyCellAction)
    {
        QTableWidgetItem* item = m_etwEventTable->item(row, col);
        if (item != nullptr)
        {
            QApplication::clipboard()->setText(item->text());
        }
        kLogEvent event;
        dbg << event
            << "[MonitorDock] ETW事件右键操作：复制单元格, row="
            << row
            << ", col="
            << col
            << eol;
        return;
    }

    if (action == copyRowAction)
    {
        QStringList values;
        for (int i = 0; i < m_etwEventTable->columnCount(); ++i)
        {
            QTableWidgetItem* item = m_etwEventTable->item(row, i);
            values << (item != nullptr ? item->text() : QString());
        }
        QApplication::clipboard()->setText(values.join('\t'));
        kLogEvent event;
        dbg << event
            << "[MonitorDock] ETW事件右键操作：复制整行, row="
            << row
            << eol;
        return;
    }

    if (action == gotoProcessAction)
    {
        QTableWidgetItem* pidItem = m_etwEventTable->item(row, 4);
        if (pidItem == nullptr)
        {
            return;
        }

        std::uint32_t pid = 0;
        if (!parsePid(pidItem->text(), pid))
        {
            kLogEvent event;
            warn << event
                << "[MonitorDock] ETW事件右键操作失败：PID解析失败, text="
                << pidItem->text().toStdString()
                << eol;
            QMessageBox::information(this, QStringLiteral("ETW事件"), QStringLiteral("未解析到有效PID。"));
            return;
        }
        kLogEvent event;
        info << event
            << "[MonitorDock] ETW事件右键操作：转到进程详情, pid="
            << pid
            << eol;
        openProcessDetail(this, pid);
    }
}
