#include "FileMappedProcessWindow.h"
#include "FilePropertyTableModel.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/TableContextMenu.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/FlatButtonTheme.h"

// ============================================================
// FileMappedProcessWindow.cpp
// 作用：
// - 实现文件映射进程反查窗口；
// - 每个文件通过 ArkDriverClient 查询 R0 Data/Image ControlArea 映射；
// - UI 只展示结果和诊断，不直接触碰 KswordARK DeviceIoControl。
// ============================================================

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/UI_All.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/DetailDialogChrome.h"
#include "../ksword/log/log.h"
#include "../theme.h"
#include "../Internationalization/LanguageManager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
#include <QTableView>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <TlHelp32.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace
{

    // formatHex 作用：把整数格式化为 0x 前缀十六进制文本。
    // 参数 value：数值。
    // 参数 width：十六进制宽度，0 表示不补齐。
    // 返回：格式化字符串。
    QString formatHex(const std::uint64_t value, const int width = 0)
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

    // buildDriverNtPath 作用：将 Win32 路径转换为驱动可打开的 NT 路径。
    // 参数 path：用户态文件路径。
    // 返回：\??\ 或 \Device\ 前缀路径，失败返回空。
    QString buildDriverNtPath(const QString& path)
    {
        const QString nativePathText = QDir::toNativeSeparators(path).trimmed();
        if (nativePathText.isEmpty())
        {
            return QString();
        }
        if (nativePathText.startsWith(QStringLiteral("\\??\\")))
        {
            return nativePathText;
        }
        if (nativePathText.startsWith(QStringLiteral("\\\\?\\")))
        {
            return QStringLiteral("\\??\\") + nativePathText.mid(4);
        }
        if (nativePathText.startsWith(QStringLiteral("\\Device\\")))
        {
            return nativePathText;
        }
        if (nativePathText.startsWith(QStringLiteral("\\\\")))
        {
            return QStringLiteral("\\??\\UNC\\") + nativePathText.mid(2);
        }
        return QStringLiteral("\\??\\") + nativePathText;
    }

    // sectionKindText 作用：把 R0 sectionKind 转成 UI 文本。
    // 参数 sectionKind：KSWORD_ARK_FILE_SECTION_KIND_*。
    // 返回：Data/Image/Unknown。
    QString sectionKindText(const std::uint32_t sectionKind)
    {
        switch (sectionKind)
        {
        case KSWORD_ARK_FILE_SECTION_KIND_DATA:
            return QStringLiteral("Data");
        case KSWORD_ARK_FILE_SECTION_KIND_IMAGE:
            return QStringLiteral("Image");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // viewMapTypeText 作用：把 ControlArea 映射类型转成 UI 文本。
    // 参数 viewMapType：KSWORD_ARK_SECTION_MAP_TYPE_*。
    // 返回：映射类型文本。
    QString viewMapTypeText(const std::uint32_t viewMapType)
    {
        switch (viewMapType)
        {
        case KSWORD_ARK_SECTION_MAP_TYPE_PROCESS:
            return QStringLiteral("Process");
        case KSWORD_ARK_SECTION_MAP_TYPE_SESSION:
            return QStringLiteral("Session");
        case KSWORD_ARK_SECTION_MAP_TYPE_SYSTEM_CACHE:
            return QStringLiteral("SystemCache");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // queryStatusText 作用：把文件 Section 查询状态转换为可读文本。
    // 参数 queryStatus：KSWORD_ARK_FILE_SECTION_QUERY_STATUS_*。
    // 返回：状态字符串。
    QString queryStatusText(const std::uint32_t queryStatus)
    {
        switch (queryStatus)
        {
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_DYNDATA_MISSING:
            return QStringLiteral("DynData Missing");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OPEN_FAILED:
            return QStringLiteral("File Open Failed");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OBJECT_FAILED:
            return QStringLiteral("FileObject Failed");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_SECTION_POINTERS_MISSING:
            return QStringLiteral("SectionObjectPointer Missing");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_CONTROL_AREA_MISSING:
            return QStringLiteral("ControlArea Missing");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED:
            return QStringLiteral("Mapping Query Failed");
        case KSWORD_ARK_FILE_SECTION_QUERY_STATUS_BUFFER_TOO_SMALL:
            return QStringLiteral("Buffer Too Small");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // fileSectionIoMessageText 作用：
    // - 将 ArkDriverClient 的低层 IO 文本翻译成文件映射窗口可直接展示的诊断；
    // - 参数 messageText：DriverClient 返回的窄字符串，可能包含 DeviceIoControl/operation 等内部字段；
    // - 返回：面向用户的中文说明，保留 unsupported/unavailable 等关键信号但隐藏实现噪声。
    QString fileSectionIoMessageText(const std::string& messageText)
    {
        const QString rawMessageText = QString::fromStdString(messageText).trimmed();
        if (rawMessageText.isEmpty())
        {
            return QStringLiteral("驱动未返回额外说明。");
        }

        const QString lowerMessageText = rawMessageText.toLower();
        if (lowerMessageText.contains(QStringLiteral("unsupported")) ||
            lowerMessageText.contains(QStringLiteral("not supported")) ||
            lowerMessageText.contains(QStringLiteral("status=0xc00000bb")))
        {
            return QStringLiteral("当前驱动不支持文件映射反查。");
        }
        if (lowerMessageText.contains(QStringLiteral("unavailable")) ||
            lowerMessageText.contains(QStringLiteral("device handle unavailable")))
        {
            return QStringLiteral("KswordARK 驱动当前不可用或尚未打开。");
        }
        if (lowerMessageText.contains(QStringLiteral("buffer too small")) ||
            lowerMessageText.contains(QStringLiteral("buffer_too_small")))
        {
            return QStringLiteral("驱动返回缓冲区不足，结果可能被截断。");
        }
        if (lowerMessageText.contains(QStringLiteral("deviceiocontrol")))
        {
            return QStringLiteral("文件映射查询失败，请确认驱动已加载且版本匹配。");
        }
        if (lowerMessageText.contains(QStringLiteral("file-section path invalid")) ||
            lowerMessageText.contains(QStringLiteral("path invalid")))
        {
            return QStringLiteral("传入驱动的 NT 文件路径无效。");
        }

        return rawMessageText;
    }

    // collectProcessNameMap 作用：采集 PID -> 进程名映射，避免每行重复 Toolhelp。
    // 参数：无。
    // 返回：PID 到 exe 名称的 map。
    std::map<std::uint32_t, QString> collectProcessNameMap()
    {
        std::map<std::uint32_t, QString> processNameMap;
        const HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return processNameMap;
        }

        PROCESSENTRY32W processEntry{};
        processEntry.dwSize = sizeof(processEntry);
        if (::Process32FirstW(snapshotHandle, &processEntry) != FALSE)
        {
            do
            {
                processNameMap[processEntry.th32ProcessID] = QString::fromWCharArray(processEntry.szExeFile);
            } while (::Process32NextW(snapshotHandle, &processEntry) != FALSE);
        }
        ::CloseHandle(snapshotHandle);
        return processNameMap;
    }

    // appendUniqueLine 作用：向诊断列表追加去重文本。
    // 参数 lines：诊断列表。
    // 参数 line：待追加文本。
    // 返回：无。
    void appendUniqueLine(QStringList& lines, const QString& line)
    {
        const QString normalizedLine = line.trimmed();
        if (!normalizedLine.isEmpty() && !lines.contains(normalizedLine))
        {
            lines.push_back(normalizedLine);
        }
    }
}

FileMappedProcessWindow::FileMappedProcessWindow(const std::vector<QString>& targetPaths, QWidget* parent)
    : QDialog(parent)
    , m_targetPaths(targetPaths)
{
    initializeUi();
    ks::ui::ApplyDetailDialogChrome(this);
    initializeConnections();
    requestRefresh(true);
}

void FileMappedProcessWindow::setOpenProcessDetailCallback(OpenProcessDetailCallback callback)
{
    m_openProcessDetailCallback = std::move(callback);
}

void FileMappedProcessWindow::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    applyAdaptiveColumnWidths();
}

void FileMappedProcessWindow::initializeUi()
{
    setObjectName(QStringLiteral("FileMappedProcessWindowRoot"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    // 根表面与表头仅由下方共享详情主题拥有，避免旧透明规则覆盖统一分层。
    setWindowTitle(QStringLiteral("文件映射进程(R0 Section/ControlArea)"));
    ks::ui::applyResponsiveWindowGeometry(
        this,
        parentWidget(),
        QSize(1120, 680),
        QSize(720, 480));

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(6);

    m_toolbarLayout = new QHBoxLayout();
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(6);

    m_refreshButton = new QPushButton(this);
    m_refreshButton->setIcon(QIcon(":/Icon/handle_refresh.svg"));
    KswordTheme::ApplyCompactIconButtonMetrics(m_refreshButton);
    m_refreshButton->setToolTip(QStringLiteral("刷新 R0 映射进程扫描"));
    ks::ui::ApplyFlatButtonTheme(m_refreshButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);

    m_openProcessButton = new QPushButton(this);
    m_openProcessButton->setIcon(QIcon(":/Icon/process_details.svg"));
    KswordTheme::ApplyCompactIconButtonMetrics(m_openProcessButton);
    m_openProcessButton->setToolTip(QStringLiteral("转到当前行进程详情"));
    ks::ui::ApplyFlatButtonTheme(m_openProcessButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);

    QStringList targetTextList;
    for (const QString& pathText : m_targetPaths)
    {
        targetTextList.push_back(QDir::toNativeSeparators(pathText));
    }
    m_targetLabel = new QLabel(QStringLiteral("目标：%1").arg(targetTextList.join(QStringLiteral(" | "))), this);
    m_targetLabel->setMinimumWidth(0);
    m_targetLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_targetLabel->setToolTip(m_targetLabel->text());
    m_targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_targetLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextPrimaryHex()));

    m_statusLabel = new QLabel(QStringLiteral("● 等待扫描"), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_resultTable = new ks::ui::TableActionTableView(this);
    ks::ui::SetTableActionBarMode(m_resultTable, ks::ui::TableActionBarMode::None);
    auto* model = new ks::fileui::PropertyTableModel(QStringList{
        QStringLiteral("目标文件"),
        QStringLiteral("Section"),
        QStringLiteral("PID"),
        QStringLiteral("进程名"),
        QStringLiteral("映射类型"),
        QStringLiteral("Base"),
        QStringLiteral("End"),
        QStringLiteral("大小"),
        QStringLiteral("ControlArea")
        }, m_resultTable, [](const QString& value) { return ks::i18n::displayText(value); });
    m_resultTable->setModel(model);
    new ks::fileui::PropertyTableCommitHost(m_resultTable, model, this, QStringLiteral("file-mapped-process-snapshot"));
    m_resultTable->setProperty("ksword_detail_preserve_model_data", true);
    m_resultTable->setProperty("ks_i18n_preserve_model_data", true);
    m_resultTable->verticalHeader()->hide();
    m_resultTable->setWordWrap(false);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setSortingEnabled(true);
    m_resultTable->setContextMenuPolicy(Qt::CustomContextMenu);
    if (m_resultTable->horizontalHeader() != nullptr)
    {
        m_resultTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        m_resultTable->horizontalHeader()->setStretchLastSection(false);
    }

    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_openProcessButton);
    m_toolbarLayout->addWidget(m_targetLabel, 1);
    ks::ui::NormalizeToolbarRow(m_toolbarLayout);
    m_rootLayout->addLayout(m_toolbarLayout);
    m_rootLayout->addWidget(m_statusLabel);
    m_rootLayout->addWidget(m_resultTable, 1);
    applyAdaptiveColumnWidths();
}

void FileMappedProcessWindow::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
        {
            requestRefresh(true);
        });
    connect(m_openProcessButton, &QPushButton::clicked, this, [this]()
        {
            openCurrentProcessDetail();
        });
    connect(m_resultTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& localPosition)
        {
            showTableContextMenu(localPosition);
        });
}

void FileMappedProcessWindow::requestRefresh(const bool forceRefresh)
{
    if (m_refreshInProgress)
    {
        if (forceRefresh)
        {
            m_refreshPending = true;
        }
        return;
    }

    const std::uint64_t currentTicket = ++m_refreshTicket;
    m_refreshInProgress = true;
    static_cast<ks::fileui::PropertyTableModel*>(m_resultTable->model())->invalidateRequest();
    m_statusLabel->setText(QStringLiteral("● 正在通过 R0 查询 Section/ControlArea 映射..."));
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:700;").arg(KswordTheme::PrimaryBlueHex));

    if (m_refreshProgressPid <= 0)
    {
        m_refreshProgressPid = kPro.addReusable(this, "文件映射", "准备 R0 Section 反查");
    }
    kPro.set(m_refreshProgressPid, "后台查询文件映射进程", 0, 20.0f);

    const std::vector<QString> targetPathsSnapshot = m_targetPaths;
    const int progressPid = m_refreshProgressPid;
    QPointer<FileMappedProcessWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, currentTicket, targetPathsSnapshot, progressPid]()
        {
            const auto beginTime = std::chrono::steady_clock::now();
            RefreshResult refreshResult{};
            QStringList diagnosticLines;
            std::set<QString> dedupeKeys;
            const std::map<std::uint32_t, QString> processNameMap = collectProcessNameMap();
            const ksword::ark::DriverClient driverClient;

            for (const QString& targetPath : targetPathsSnapshot)
            {
                QFileInfo fileInfo(targetPath);
                if (!fileInfo.exists() || !fileInfo.isFile())
                {
                    appendUniqueLine(diagnosticLines, QStringLiteral("%1: 不是可扫描文件").arg(QDir::toNativeSeparators(targetPath)));
                    continue;
                }

                const QString ntPath = buildDriverNtPath(fileInfo.absoluteFilePath());
                if (ntPath.isEmpty())
                {
                    appendUniqueLine(diagnosticLines, QStringLiteral("%1: NT路径转换失败").arg(QDir::toNativeSeparators(targetPath)));
                    continue;
                }

                const ksword::ark::FileSectionMappingsQueryResult queryResult =
                    driverClient.queryFileSectionMappings(
                        ntPath.toStdWString(),
                        KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL,
                        KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT);
                if (!queryResult.io.ok)
                {
                    appendUniqueLine(
                        diagnosticLines,
                        QStringLiteral("%1: IO失败 %2")
                            .arg(QDir::toNativeSeparators(targetPath), fileSectionIoMessageText(queryResult.io.message)));
                    continue;
                }

                appendUniqueLine(
                    diagnosticLines,
                    QStringLiteral("%1: %2 total=%3 returned=%4 dataCA=%5 imageCA=%6")
                        .arg(QFileInfo(targetPath).fileName())
                        .arg(queryStatusText(queryResult.queryStatus))
                        .arg(queryResult.totalCount)
                        .arg(queryResult.returnedCount)
                        .arg(formatHex(queryResult.dataControlAreaAddress))
                        .arg(formatHex(queryResult.imageControlAreaAddress)));

                for (const ksword::ark::FileSectionMappingEntry& mappingEntry : queryResult.mappings)
                {
                    const QString dedupeKey = QStringLiteral("%1|%2|%3|%4|%5")
                        .arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()).toLower())
                        .arg(mappingEntry.sectionKind)
                        .arg(mappingEntry.processId)
                        .arg(static_cast<qulonglong>(mappingEntry.startVa), 0, 16)
                        .arg(static_cast<qulonglong>(mappingEntry.endVa), 0, 16);
                    if (dedupeKeys.find(dedupeKey) != dedupeKeys.end())
                    {
                        continue;
                    }
                    dedupeKeys.insert(dedupeKey);

                    MappedProcessRow row{};
                    row.targetPath = fileInfo.absoluteFilePath();
                    row.map = mappingEntry;
                    const auto processNameIt = processNameMap.find(mappingEntry.processId);
                    row.processName = (processNameIt != processNameMap.end())
                        ? processNameIt->second
                        : QStringLiteral("PID %1").arg(mappingEntry.processId);
                    refreshResult.rows.push_back(std::move(row));
                }

                if (progressPid > 0)
                {
                    kPro.set(progressPid, "正在合并文件映射进程结果", 0, 70.0f);
                }
            }

            std::sort(
                refreshResult.rows.begin(),
                refreshResult.rows.end(),
                [](const MappedProcessRow& left, const MappedProcessRow& right)
                {
                    if (left.targetPath.compare(right.targetPath, Qt::CaseInsensitive) != 0)
                    {
                        return left.targetPath.compare(right.targetPath, Qt::CaseInsensitive) < 0;
                    }
                    if (left.map.processId != right.map.processId)
                    {
                        return left.map.processId < right.map.processId;
                    }
                    if (left.map.sectionKind != right.map.sectionKind)
                    {
                        return left.map.sectionKind < right.map.sectionKind;
                    }
                    return left.map.startVa < right.map.startVa;
                });

            refreshResult.diagnosticText = diagnosticLines.join(QStringLiteral(" | "));
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            auto display = ks::fileui::makePropertyTableSnapshot();
            display->rows.reserve(refreshResult.rows.size());
            for (std::size_t index = 0; index < refreshResult.rows.size(); ++index)
            {
                const auto& row = refreshResult.rows.at(index);
                const auto sizeBytes = row.map.endVa > row.map.startVa ? row.map.endVa - row.map.startVa : 0ULL;
                display->append({ks::fileui::propertyRowKey({row.targetPath, QString::number(row.map.sectionKind),
                    QString::number(row.map.viewMapType), QString::number(row.map.processId), QString::number(row.map.controlAreaAddress),
                    QString::number(row.map.startVa), QString::number(row.map.endVa)}),
                    {QDir::toNativeSeparators(row.targetPath), sectionKindText(row.map.sectionKind), QString::number(row.map.processId),
                        row.processName, viewMapTypeText(row.map.viewMapType), formatHex(row.map.startVa), formatHex(row.map.endVa),
                        QString::number(static_cast<qulonglong>(sizeBytes)), formatHex(row.map.controlAreaAddress)},
                    {{Qt::UserRole, static_cast<qulonglong>(index)}}});
            }
            refreshResult.displayRows = display;
            const auto snapshot = std::shared_ptr<RefreshResult>(new RefreshResult(std::move(refreshResult)), [](RefreshResult* value)
                { QThreadPool::globalInstance()->start(QRunnable::create([value]() { delete value; })); });
            QMetaObject::invokeMethod(qApp, [guardThis, currentTicket, snapshot]()
                {
                    if (guardThis) guardThis->applyRefreshResult(currentTicket, snapshot);
                }, Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void FileMappedProcessWindow::applyRefreshResult(
    const std::uint64_t refreshTicket, std::shared_ptr<const RefreshResult> refreshResult)
{
    if (refreshTicket != m_refreshTicket) return;
    const QPointer<FileMappedProcessWindow> guard(this);
    static_cast<ks::fileui::PropertyTableModel*>(m_resultTable->model())->setSnapshot(refreshResult->displayRows,
        [guard, refreshTicket, refreshResult]()
        {
            if (!guard || refreshTicket != guard->m_refreshTicket) return;
            guard->m_rows = refreshResult;
            QString status = QStringLiteral("● 扫描完成 %1 ms | 映射行:%2").arg(refreshResult->elapsedMs).arg(refreshResult->rows.size());
            if (!refreshResult->diagnosticText.trimmed().isEmpty())
                status += QStringLiteral(" | 存在诊断；详情已写入日志。");
            guard->m_statusLabel->setText(status);
            if (!guard) return;
            guard->m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;")
                .arg((refreshResult->rows.empty() ? KswordTheme::WarningColor() : KswordTheme::SuccessColor()).name(QColor::HexRgb)));
        });
    if (!guard) return;
    m_refreshInProgress = false;
    kPro.set(m_refreshProgressPid, "文件映射进程扫描完成", 0, 100.0f);
    if (!refreshResult->diagnosticText.trimmed().isEmpty())
    {
        kLogEvent diagnosticEvent;
        warn << diagnosticEvent << "[FileMappedProcessWindow] scan completed with diagnostics, targetCount="
            << m_targetPaths.size() << ", rowCount=" << refreshResult->rows.size()
            << ", detail=" << refreshResult->diagnosticText.toStdString() << eol;
    }
    if (m_refreshPending)
    {
        m_refreshPending = false;
        QMetaObject::invokeMethod(this, [guard]() { if (guard) guard->requestRefresh(true); }, Qt::QueuedConnection);
    }
}

const FileMappedProcessWindow::MappedProcessRow* FileMappedProcessWindow::selectedRow() const
{
    if (!m_resultTable || !m_rows || !m_resultTable->currentIndex().isValid()) return nullptr;
    const auto index = m_resultTable->model()->index(m_resultTable->currentIndex().row(), 0);
    const auto row = static_cast<std::size_t>(index.data(Qt::UserRole).toULongLong());
    return row < m_rows->rows.size() ? &m_rows->rows.at(row) : nullptr;
}

void FileMappedProcessWindow::openCurrentProcessDetail()
{
    const MappedProcessRow* row = selectedRow();
    if (row == nullptr)
    {
        QMessageBox::information(this, QStringLiteral("进程详情"), QStringLiteral("请先选择一条映射记录。"));
        return;
    }
    if (row->map.processId == 0)
    {
        QMessageBox::information(this, QStringLiteral("进程详情"), QStringLiteral("当前映射没有关联进程 PID。"));
        return;
    }
    if (!m_openProcessDetailCallback)
    {
        QMessageBox::warning(this, QStringLiteral("进程详情"), QStringLiteral("未配置进程详情跳转回调。"));
        return;
    }
    m_openProcessDetailCallback(row->map.processId);
}

void FileMappedProcessWindow::copyCurrentRow()
{
    if (m_resultTable == nullptr || !m_resultTable->currentIndex().isValid())
    {
        return;
    }

    QStringList fields;
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        fields.push_back(m_resultTable->model()->index(m_resultTable->currentIndex().row(), columnIndex).data().toString());
    }
    QApplication::clipboard()->setText(fields.join('\t'));
}

void FileMappedProcessWindow::showTableContextMenu(const QPoint& localPosition)
{
    if (m_resultTable == nullptr)
    {
        return;
    }
    const QModelIndex clicked = m_resultTable->indexAt(localPosition);
    if (!clicked.isValid()) return;
    m_resultTable->setCurrentIndex(clicked);
    const QPointer<FileMappedProcessWindow> guard(this);
    QMenu menu;
    connect(this, &QObject::destroyed, &menu, &QMenu::close);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* openProcessAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));
    QAction* copyRowAction = menu.addAction(QIcon(":/Icon/handle_copy_row.svg"), QStringLiteral("复制整行"));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);

    QAction* selectedAction = menu.exec(m_resultTable->viewport()->mapToGlobal(localPosition));
    if (!guard || selectedAction == nullptr)
    {
        return;
    }
    if (selectedAction == openProcessAction)
    {
        openCurrentProcessDetail();
        return;
    }
    if (selectedAction == copyRowAction)
    {
        copyCurrentRow();
    }
}

void FileMappedProcessWindow::applyAdaptiveColumnWidths()
{
    if (m_resultTable == nullptr || m_resultTable->horizontalHeader() == nullptr)
    {
        return;
    }

    QHeaderView* header = m_resultTable->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);

    const int viewportWidth = m_resultTable->viewport()->width();
    if (viewportWidth <= 0)
    {
        return;
    }

    const int sectionWidth = 90;
    const int pidWidth = 82;
    const int nameWidth = 150;
    const int typeWidth = 120;
    const int addressWidth = 150;
    const int sizeWidth = 100;
    const int controlAreaWidth = 150;
    const int fixedWidth = sectionWidth + pidWidth + nameWidth + typeWidth + addressWidth * 2 + sizeWidth + controlAreaWidth;
    const int targetWidth = std::max(280, viewportWidth - fixedWidth - 24);

    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::TargetPath), targetWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::SectionKind), sectionWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ProcessId), pidWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ProcessName), nameWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ViewMapType), typeWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::BaseAddress), addressWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::EndAddress), addressWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::Size), sizeWidth);
    m_resultTable->setColumnWidth(static_cast<int>(TableColumn::ControlArea), controlAreaWidth);
}
