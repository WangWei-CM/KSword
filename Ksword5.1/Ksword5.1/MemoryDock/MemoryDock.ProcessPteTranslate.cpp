#include "MemoryDock.Internal.h"
#include "../UI/TableContextMenu.h"
#include "../UI/ParameterWorkspace.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/StructuredFieldView.h"
#include "../UI/AdaptivePageScroll.h" // ks::ui::EnablePageInnerScroll：页内滚动壳。
#include "../UI/TableInteractionSupport.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/DetailLayoutRegistry.h"
#include "../UI/TableColumnAutoFit.h"

// 分层工具条用到的 Qt 控件在这里显式补齐：Internal.h 虽然已经聚合了同名头，
// 但那份聚合头是多人共用的，本页面不依赖它的收录清单，避免它增删条目时被连累。
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>

#include <memory>
#include "../UI/TableColumnAutoFit.h"
#include "../UI/TableRowFilterSupport.h"

using namespace ksword::memory_dock_internal;

namespace
{
    // PTE / VA 翻译页列定义：保持排序、过滤和详情反查使用稳定列号。
    enum class PteTranslateColumn : int
    {
        VirtualAddress = 0,
        RegionBase,
        Valid,
        Shared,
        Locked,
        LargePage,
        Bad,
        ShareCount,
        Win32Protection,
        Node,
        MappedFile,
        Risk,
        Count
    };

    // 将列枚举转为 Qt 列索引。
    int pteTranslateColumnIndex(const PteTranslateColumn column)
    {
        return static_cast<int>(column);
    }

    // 把 `QWidget` 侧宽字符串安全转为 `QString`。
    QString wideToQString(const std::wstring& value)
    {
        return value.empty() ? QString() : QString::fromStdWString(value);
    }

    // 把地址格式化为稳定的 16 进制字符串。
    QString hex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    // 组合一条只读风险文本。
    QString buildPteRiskText(
        const std::uint32_t protect,
        const bool valid,
        const bool bad,
        const bool largePage)
    {
        QStringList parts;
        if (!valid)
        {
            parts << QStringLiteral("无效页");
        }
        if ((protect & PAGE_EXECUTE_READWRITE) == PAGE_EXECUTE_READWRITE ||
            (protect & PAGE_EXECUTE_WRITECOPY) == PAGE_EXECUTE_WRITECOPY)
        {
            parts << QStringLiteral("可写可执行");
        }
        if (bad)
        {
            parts << QStringLiteral("Bad 页");
        }
        if (largePage)
        {
            parts << QStringLiteral("大页");
        }
        return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QStringLiteral(" | "));
    }

    // 构造只读表格项。
    QTableWidgetItem* makeReadOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        return item;
    }

    QString pteMenuStyle()
    {
        // pteMenuStyle：
        // - 输入：无；
        // - 处理：生成不透明右键菜单样式；
        // - 返回：QMenu 样式文本。
        // 右键菜单一律走全局主题实现，避免每个页面各拼一份互相漂移的 QSS。
        return KswordTheme::ContextMenuStyle();
    }

    void copyPteCurrentRow(MemoryTableView* table)
    {
        // copyPteCurrentRow：
        // - 输入：PTE/VA 翻译表；
        // - 处理：复制当前行所有列；
        // - 返回：无，失败时静默返回。
        if (table == nullptr || QApplication::clipboard() == nullptr)
        {
            return;
        }

        const int rowIndex = table->currentRow();
        if (rowIndex < 0 || rowIndex >= table->rowCount())
        {
            return;
        }

        QStringList fields;
        fields.reserve(table->columnCount());
        for (int columnIndex = 0; columnIndex < table->columnCount(); ++columnIndex)
        {
            const QModelIndex item = table->model()->index(rowIndex, columnIndex);
            fields.push_back(item.isValid() ? item.data(Qt::DisplayRole).toString() : QString());
        }
        QApplication::clipboard()->setText(fields.join('\t'));
    }

    void installPteCopyMenu(MemoryTableView* table)
    {
        // installPteCopyMenu：
        // - 输入：PTE/VA 翻译表；
        // - 处理：安装只读复制菜单；
        // - 返回：无，不读取或修改额外内存。
        if (table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QTableView::customContextMenuRequested, table, [table](const QPoint& localPosition) {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentCell(clickedIndex.row(), clickedIndex.column());
            }

            QMenu menu(table);
            menu.setStyleSheet(pteMenuStyle());
            QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制当前行"));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            // 菜单项也补上 tooltip：与页面内其它按钮保持一致的可发现性。
            copyRowAction->setToolTip(QStringLiteral("把选中行的全部列以制表符分隔复制到剪贴板"));
            copyRowAction->setEnabled(table->currentRow() >= 0);
            // PTE/VA 翻译的虚拟地址属于 Dock 附加的进程：提供"在内存工作台打开"。
            (void)addOpenInWorkbenchAction(
                menu, table, table->currentRow(),
                pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress), false);
            if (menu.exec(table->viewport()->mapToGlobal(localPosition)) == copyRowAction)
            {
                copyPteCurrentRow(table);
            }
        });
    }



    // 数值列一律改用 ks::ui::NumericTableItem：排序值写在 NumericSortRole 上，
    // 页面本地那份把数值塞进 Qt::UserRole 的私有 NumericItem 已删除，避免与
    // 其它页面的 UserRole 约定互相踩踏，也不必再各自维护一份 operator<。

    // 生成用于详情窗的结构字段。
    ks::ui::FieldDocument buildPteDetailDocument(const MemoryDock::ProcessMemoryEvidenceEntry& entry)
    {
        ks::ui::FieldDocument document;
        document.section(QStringLiteral("PTE / VA 翻译详情"));
        document.field(QStringLiteral("VirtualAddress"), QStringLiteral("%1").arg(hex64(entry.virtualAddress)));
        document.field(QStringLiteral("RegionBaseAddress"), QStringLiteral("%1").arg(hex64(entry.regionBaseAddress)));
        document.field(QStringLiteral("RegionSize"), QStringLiteral("%1").arg(hex64(entry.regionSize)));
        document.field(QStringLiteral("Protect"), QStringLiteral("0x%1").arg(entry.protect, 8, 16, QChar('0')), true);
        document.field(QStringLiteral("State"), QStringLiteral("0x%1").arg(entry.state, 8, 16, QChar('0')), true);
        document.field(QStringLiteral("Type"), QStringLiteral("0x%1").arg(entry.type, 8, 16, QChar('0')), true);
        document.field(QStringLiteral("Win32Protection"), QStringLiteral("0x%1").arg(entry.win32Protection, 8, 16, QChar('0')));
        document.field(QStringLiteral("ShareCount"), QStringLiteral("%1").arg(entry.shareCount));
        document.field(QStringLiteral("Node"), QStringLiteral("%1").arg(entry.node));
        document.field(QStringLiteral("Valid"), QStringLiteral("%1").arg(entry.valid ? QStringLiteral("true") : QStringLiteral("false")), true);
        document.field(QStringLiteral("Shared"), QStringLiteral("%1").arg(entry.shared ? QStringLiteral("true") : QStringLiteral("false")), true);
        document.field(QStringLiteral("Locked"), QStringLiteral("%1").arg(entry.locked ? QStringLiteral("true") : QStringLiteral("false")), true);
        document.field(QStringLiteral("LargePage"), QStringLiteral("%1").arg(entry.largePage ? QStringLiteral("true") : QStringLiteral("false")), true);
        document.field(QStringLiteral("Bad"), QStringLiteral("%1").arg(entry.bad ? QStringLiteral("true") : QStringLiteral("false")), true);
        document.field(QStringLiteral("MappedFile"), QStringLiteral("%1").arg(entry.mappedFilePath.isEmpty() ? QStringLiteral("—") : entry.mappedFilePath));
        document.field(QStringLiteral("Risk"), QStringLiteral("%1").arg(entry.riskText), true);
        document.field(QStringLiteral("Detail"), QStringLiteral("%1").arg(entry.detailText.isEmpty() ? QStringLiteral("—") : entry.detailText));
        return document;
    }
}

void MemoryDock::initializeProcessPteTranslateTab()
{
    // 输入：无，由 initializeTabs 调用。
    // 处理：构建 PTE / VA 翻译页，仅展示当前附加进程的只读工作集视图。
    // 返回：无。
    kLogEvent initEvent;
    info << initEvent
        << "[MemoryDock] initializeProcessPteTranslateTab: 构建 PTE / VA 翻译页面。"
        << eol;

    m_tabProcessPteTranslate = new QWidget(m_tabWidget);
    // 参数独立滚动，刷新固定在参数栏底部，表格和详情持续保留结果区高度。
    QWidget* const tabContent = ks::ui::EnablePageInnerScroll(m_tabProcessPteTranslate);
    QVBoxLayout* tabLayout = new QVBoxLayout(tabContent);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);
    auto* workspace = new ks::ui::ParameterWorkspace(m_tabProcessPteTranslate, 420);
    tabLayout->addWidget(workspace);

    // 执行动作由工作区固定承载，本地筛选仍只改变当前结果视图。
    m_processPteTranslateRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), m_tabProcessPteTranslate);
    m_processPteTranslateRefreshButton->setToolTip(QStringLiteral("基于当前附加进程采集 PTE / VA 翻译证据"));
    m_processPteTranslateRefreshButton->setStyleSheet(buildBlueButtonStyle());

    m_processPteTranslateRiskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), m_tabProcessPteTranslate);
    m_processPteTranslateRiskOnlyCheck->setChecked(true);
    m_processPteTranslateRiskOnlyCheck->setToolTip(QStringLiteral("只保留风险判定不为正常的页记录"));
    m_processPteTranslateRiskOnlyCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }")
        .arg(KswordTheme::TextPrimaryHex()));

    m_processPteTranslateStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_tabProcessPteTranslate);
    m_processPteTranslateStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_processPteTranslateStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    workspace->actions()->addWidget(m_processPteTranslateRefreshButton);
    workspace->actions()->addStretch();
    m_processPteTranslateStatusLabel->setWordWrap(true);
    workspace->results()->addWidget(m_processPteTranslateStatusLabel);

    // 第二层：采样参数分组，起点 VA 与采样页数各配一个文字标签，
    // 避免用户对着一个孤零零的“VA”缩写猜它到底要填什么。
    QGroupBox* translateParameterGroup = new QGroupBox(QStringLiteral("翻译参数"), m_tabProcessPteTranslate);
    ks::ui::StyleParameterSection(translateParameterGroup);
    QGridLayout* translateParameterLayout = new QGridLayout(translateParameterGroup);
    translateParameterLayout->setContentsMargins(10, 8, 10, 8);
    translateParameterLayout->setHorizontalSpacing(8);
    translateParameterLayout->setVerticalSpacing(6);

    // 参数名统一走次级文字色（动态 token，主题切换能跟随），与输入内容拉开主次。
    const QString parameterCaptionStyle =
        QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex());

    QLabel* addressCaption = new QLabel(QStringLiteral("起始虚拟地址"), translateParameterGroup);
    addressCaption->setStyleSheet(parameterCaptionStyle);

    m_processPteTranslateAddressEdit = new QLineEdit(translateParameterGroup);
    m_processPteTranslateAddressEdit->setClearButtonEnabled(true);
    m_processPteTranslateAddressEdit->setPlaceholderText(QStringLiteral("输入 VA，例如 0x7FF6..."));
    m_processPteTranslateAddressEdit->setToolTip(QStringLiteral("采样起点虚拟地址，留空则沿用查看器当前地址"));
    m_processPteTranslateAddressEdit->setStyleSheet(buildBlueInputStyle());

    QLabel* pageCountCaption = new QLabel(QStringLiteral("采样页数"), translateParameterGroup);
    pageCountCaption->setStyleSheet(parameterCaptionStyle);

    m_processPteTranslatePageCountSpin = new QSpinBox(translateParameterGroup);
    m_processPteTranslatePageCountSpin->setRange(1, 256);
    m_processPteTranslatePageCountSpin->setValue(16);
    m_processPteTranslatePageCountSpin->setToolTip(QStringLiteral("每次采样的页数上限"));

    // 地址和值分行，让完整地址在参数栏中获得全部可用宽度。
    translateParameterLayout->addWidget(addressCaption, 0, 0);
    translateParameterLayout->addWidget(m_processPteTranslateAddressEdit, 1, 0);
    translateParameterLayout->addWidget(pageCountCaption, 2, 0);
    translateParameterLayout->addWidget(m_processPteTranslatePageCountSpin, 3, 0);
    translateParameterLayout->addWidget(m_processPteTranslateRiskOnlyCheck, 4, 0);
    translateParameterLayout->setColumnStretch(0, 1);
    workspace->parameters()->addWidget(translateParameterGroup);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabProcessPteTranslate);
    workspace->results()->addWidget(splitter, 1);

    m_processPteTranslateTable = new MemoryTableView(splitter);
    // 小范围地址翻译是参数查询的结果，紧凑复制/导出即可。
    ks::ui::SetTableActionBarMode(m_processPteTranslateTable, ks::ui::TableActionBarMode::Compact);
    m_processPteTranslateTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("VA"),
        QStringLiteral("区域基址"),
        QStringLiteral("有效"),
        QStringLiteral("共享"),
        QStringLiteral("锁定"),
        QStringLiteral("大页"),
        QStringLiteral("坏页"),
        QStringLiteral("ShareCount"),
        QStringLiteral("Win32Prot"),
        QStringLiteral("Node"),
        QStringLiteral("映射文件"),
        QStringLiteral("风险")
    });
    m_processPteTranslateTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processPteTranslateTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processPteTranslateTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processPteTranslateTable->setAlternatingRowColors(true);
    m_processPteTranslateTable->setSortingEnabled(true);
    m_processPteTranslateTable->verticalHeader()->setVisible(false);
    m_processPteTranslateTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_processPteTranslateTable->horizontalHeader()->setSectionResizeMode(pteTranslateColumnIndex(PteTranslateColumn::MappedFile), QHeaderView::Stretch);
    installPteCopyMenu(m_processPteTranslateTable);
    splitter->addWidget(m_processPteTranslateTable);
    ks::ui::SetTableColumnAutoFitEnabled(m_processPteTranslateTable, false);
    ks::ui::SetTableRowFilterBusinessOwnership(m_processPteTranslateTable, ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
    initializeCoreProjection(CoreKind::PteEvidence);

    m_processPteTranslateDetailEditor = new ks::ui::StructuredFieldView(splitter);
    m_processPteTranslateDetailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("请选择一条 PTE / VA 翻译记录查看详情。")));
    splitter->addWidget(m_processPteTranslateDetailEditor);

    ks::ui::DetailLayoutRegistry::registerStructuredHost(
        m_processPteTranslateTable,
        m_processPteTranslateDetailEditor,
        m_tabProcessPteTranslate);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    workspace->finish();
    m_tabWidget->addTab(m_tabProcessPteTranslate, QStringLiteral("PTE / VA 翻译"));
}

void MemoryDock::refreshProcessPteTranslateAsync()
{
    // 输入：无，由刷新按钮或 tab 路由触发。
    // 处理：基于当前附加进程和输入 VA 采样 QueryWorkingSetEx / VirtualQueryEx 结果。
    // 返回：无。
    if (m_processPteTranslateRefreshInProgress.exchange(true))
    {
        return;
    }

    kLogEvent refreshEvent;
    info << refreshEvent
        << "[MemoryDock] refreshProcessPteTranslateAsync: 开始解析页表，刷新按钮已置灰。"
        << eol;

    // 进入采集前先把反馈给足：按钮压灰防重复点击，状态标签明确写“正在解析页表…”。
    if (m_processPteTranslateRefreshButton != nullptr)
    {
        m_processPteTranslateRefreshButton->setEnabled(false);
    }
    if (m_processPteTranslateStatusLabel != nullptr)
    {
        m_processPteTranslateStatusLabel->setText(QStringLiteral("状态：正在解析页表…"));
        m_processPteTranslateStatusLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::PrimaryBlueHex));
    }

    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0U)
    {
        // 未附加就直接退出：上面已经压灰了按钮，这里必须把它放回来，否则页面永远点不动。
        m_processPteTranslateRefreshInProgress.store(false);
        if (m_processPteTranslateRefreshButton != nullptr)
        {
            m_processPteTranslateRefreshButton->setEnabled(true);
        }
        if (m_processPteTranslateStatusLabel != nullptr)
        {
            m_processPteTranslateStatusLabel->setText(QStringLiteral("状态：请先附加进程。"));
            m_processPteTranslateStatusLabel->setStyleSheet(
                QStringLiteral("color:%1; font-weight:600;")
                    .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
        }
        return;
    }

    std::uint32_t duplicateError = ERROR_SUCCESS;
    const std::shared_ptr<void> processHandleLease =
        duplicateAttachedProcessHandleForWorker(&duplicateError);
    if (!processHandleLease)
    {
        m_processPteTranslateRefreshInProgress.store(false);
        if (m_processPteTranslateRefreshButton != nullptr)
        {
            m_processPteTranslateRefreshButton->setEnabled(true);
        }
        if (m_processPteTranslateStatusLabel != nullptr)
        {
            m_processPteTranslateStatusLabel->setText(
                QStringLiteral("状态：复制进程句柄失败（Win32=%1）。").arg(duplicateError));
            m_processPteTranslateStatusLabel->setStyleSheet(
                QStringLiteral("color:%1; font-weight:600;")
                    .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
        }
        return;
    }

    // 按钮与状态已在函数入口统一置位，此处不再重复下发，直接取采样参数。
    std::uint64_t baseAddress = 0ULL;
    const QString addressText = m_processPteTranslateAddressEdit != nullptr
        ? m_processPteTranslateAddressEdit->text().trimmed() : QString();
    // 明确输入必须解析成功；仅空输入才借用当前工作台插入点，不回退旧页缓存或地址零。
    const auto workbenchAddress = addressText.isEmpty()
        ? workbenchFocusAddress() : std::optional<std::uint64_t>();
    const bool hasAddress = addressText.isEmpty()
        ? workbenchAddress.has_value() : parseAddressText(addressText, baseAddress);
    if (!hasAddress)
    {
        // 本次没有启动采样任务，立即恢复按钮和状态；复制句柄随局部租约正常关闭。
        m_processPteTranslateRefreshInProgress.store(false);
        if (m_processPteTranslateRefreshButton != nullptr)
        {
            m_processPteTranslateRefreshButton->setEnabled(true);
        }
        if (m_processPteTranslateStatusLabel != nullptr)
        {
            m_processPteTranslateStatusLabel->setText(QStringLiteral(
                "状态：请填写有效地址，或在内存工作台选择地址。"));
            m_processPteTranslateStatusLabel->setStyleSheet(
                QStringLiteral("color:%1; font-weight:600;")
                    .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
        }
        return;
    }
    if (workbenchAddress.has_value())
    {
        baseAddress = *workbenchAddress;
    }
    const std::uint32_t pageCount = m_processPteTranslatePageCountSpin != nullptr
        ? static_cast<std::uint32_t>(m_processPteTranslatePageCountSpin->value())
        : 16U;

    const std::uint64_t ticket = m_processPteTranslateRefreshTicket.fetch_add(1U) + 1U;
    const std::uint64_t attachmentGeneration = m_processAttachmentGeneration.load();
    const std::uint32_t pid = m_attachedPid;
    const QPointer<MemoryDock> guardThis(this);

    std::thread([guardThis,
                 processHandleLease,
                 ticket,
                 attachmentGeneration,
                 pid,
                 baseAddress,
                 pageCount]() {
        const HANDLE workerProcessHandle = static_cast<HANDLE>(processHandleLease.get());
        std::vector<ProcessMemoryEvidenceEntry> entries;
        entries.reserve(pageCount);

        MEMORY_BASIC_INFORMATION mbi{};
        SYSTEM_INFO systemInfo{};
        ::GetSystemInfo(&systemInfo);
        const std::uint64_t pageSize = static_cast<std::uint64_t>(systemInfo.dwPageSize);
        const std::uint64_t alignedBase = pageSize == 0ULL ? baseAddress : (baseAddress / pageSize) * pageSize;

        for (std::uint32_t index = 0; index < pageCount; ++index)
        {
            const std::uint64_t va = alignedBase + (static_cast<std::uint64_t>(index) * pageSize);
            if (va < alignedBase)
            {
                break;
            }

            if (::VirtualQueryEx(
                    workerProcessHandle,
                    reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)),
                    &mbi,
                    sizeof(mbi)) != sizeof(mbi))
            {
                break;
            }

            PSAPI_WORKING_SET_EX_INFORMATION wsInfo{};
            wsInfo.VirtualAddress = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(va));
            const BOOL queryOk = ::QueryWorkingSetEx(
                workerProcessHandle,
                &wsInfo,
                static_cast<DWORD>(sizeof(wsInfo)));

            ProcessMemoryEvidenceEntry entry{};
            entry.virtualAddress = va;
            entry.regionBaseAddress = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            entry.regionSize = static_cast<std::uint64_t>(mbi.RegionSize);
            entry.protect = static_cast<std::uint32_t>(mbi.Protect);
            entry.state = static_cast<std::uint32_t>(mbi.State);
            entry.type = static_cast<std::uint32_t>(mbi.Type);
            entry.mappedFilePath = mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED
                ? QStringLiteral("")
                : QString();

            if (queryOk != FALSE)
            {
                const auto flags = wsInfo.VirtualAttributes.Flags;
                entry.valid = wsInfo.VirtualAttributes.Valid != 0;
                entry.shareCount = static_cast<std::uint32_t>(wsInfo.VirtualAttributes.ShareCount);
                entry.win32Protection = static_cast<std::uint32_t>(wsInfo.VirtualAttributes.Win32Protection);
                entry.shared = wsInfo.VirtualAttributes.Shared != 0;
                entry.node = static_cast<std::uint32_t>(wsInfo.VirtualAttributes.Node);
                entry.locked = wsInfo.VirtualAttributes.Locked != 0;
                entry.largePage = wsInfo.VirtualAttributes.LargePage != 0;
                entry.bad = wsInfo.VirtualAttributes.Bad != 0;
                Q_UNUSED(flags);
            }

            entry.riskText = buildPteRiskText(entry.protect, entry.valid, entry.bad, entry.largePage);
            entry.detailText = QStringLiteral("PID=%1 | VA=%2 | Base=%3 | Size=%4")
                .arg(pid)
                .arg(hex64(entry.virtualAddress))
                .arg(hex64(entry.regionBaseAddress))
                .arg(hex64(entry.regionSize));
            entries.push_back(std::move(entry));
        }

        const auto entriesSnapshot = ks::memory_detail::ValueStorage<std::vector<ProcessMemoryEvidenceEntry>>(std::move(entries));
        QMetaObject::invokeMethod(
            qApp,
            [guardThis, ticket, attachmentGeneration, entriesSnapshot]() {
                if (guardThis == nullptr) return;
                auto commitSnapshot = [guardThis, ticket, attachmentGeneration, entriesSnapshot]() mutable
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }

                    // 无论快照是否过期，回到主线程就先把“刷新中”状态拆掉：
                    // 旧写法把恢复放在 ticket 校验之后，一旦重新附加进程就会把按钮永久压灰。
                    guardThis->m_processPteTranslateRefreshInProgress.store(false);
                    if (guardThis->m_processPteTranslateRefreshButton != nullptr)
                    {
                        guardThis->m_processPteTranslateRefreshButton->setEnabled(true);
                    }

                    if (ticket != guardThis->m_processPteTranslateRefreshTicket.load() ||
                        attachmentGeneration != guardThis->m_processAttachmentGeneration.load())
                    {
                        return;
                    }

                    guardThis->m_processPteTranslateCache = entriesSnapshot;
                    guardThis->rebuildProcessPteTranslateTable();
                    guardThis->showProcessPteTranslateDetailByCurrentRow();

                    if (guardThis->m_processPteTranslateStatusLabel != nullptr)
                    {
                        guardThis->m_processPteTranslateStatusLabel->setText(
                            QStringLiteral("状态：采样 %1 行").arg(guardThis->m_processPteTranslateCache->size()));
                        guardThis->m_processPteTranslateStatusLabel->setStyleSheet(QStringLiteral(
                            "color:%1; font-weight:600;")
                            .arg(guardThis->m_processPteTranslateCache->empty()
                                ? KswordTheme::ErrorColor().name(QColor::HexRgb)
                                : KswordTheme::SuccessColor().name(QColor::HexRgb)));
                    }
                };

                if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
                    guardThis.data(),
                    QStringLiteral("memory-process-pte-snapshot"),
                    { guardThis->m_processPteTranslateTable },
                    commitSnapshot))
                {
                    return;
                }
                commitSnapshot();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MemoryDock::rebuildProcessPteTranslateTable()
{
    requestCoreProjection(CoreKind::PteEvidence, true);
}

void MemoryDock::showProcessPteTranslateDetailByCurrentRow()
{
    if (!m_processPteTranslateTable || !m_processPteTranslateTable->isVisible() || !coreProjectionIsCurrent(CoreKind::PteEvidence)) { return; }
    // 输入：无，读取当前表格选中行。
    // 处理：从缓存中展开当前记录到 StructuredFieldView。
    // 返回：无。
    if (m_processPteTranslateDetailEditor == nullptr || m_processPteTranslateTable == nullptr)
    {
        return;
    }

    const int row = m_processPteTranslateTable->currentRow();
    if (row < 0 || row >= m_processPteTranslateTable->rowCount())
    {
        m_processPteTranslateDetailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("请选择一条 PTE / VA 翻译记录查看详情。")));
        return;
    }

    const QModelIndex addressItem = m_processPteTranslateTable->model()->index(row, pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress));
    if (!addressItem.isValid())
    {
        m_processPteTranslateDetailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("当前记录的缓存已失效，请刷新后重试。")));
        return;
    }
    const QString diagnosticText = addressItem.data(Qt::UserRole + 2).toString();
    if (!diagnosticText.isEmpty())
    {
        m_processPteTranslateDetailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("PTE / VA 翻译诊断\n%1").arg(diagnosticText)));
        return;
    }

    // 反查键跟着填表侧一起迁到 NumericSortRole：
    // NumericTableItem 把真实数值写在 Qt::UserRole + 900，再读 Qt::UserRole 会永远取不到值。
    bool ok = false;
    const qulonglong addressValue = addressItem.data(ks::ui::NumericSortRole).toULongLong(&ok);
    if (!ok)
    {
        m_processPteTranslateDetailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("当前记录的缓存已失效，请刷新后重试。")));
        return;
    }

    const auto sourceIndex = addressItem.data(ks::memory_detail::CoreSourceIndexRole).toULongLong();
    if (sourceIndex < m_processPteTranslateCache->size()) {
        const auto& entry = (*m_processPteTranslateCache)[static_cast<std::size_t>(sourceIndex)];
        if (entry.virtualAddress == static_cast<std::uint64_t>(addressValue)) {
            m_processPteTranslateDetailEditor->setDocument(buildPteDetailDocument(entry));
            return;
        }
    }
    m_processPteTranslateDetailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("当前记录的缓存已失效，请刷新后重试。")));
}

void MemoryDock::capturePteProjection(CoreProjectionData& data) const
{
    const auto source = m_processPteTranslateCache;
    data.extraSource = source; data.extraCount = source->size();
    data.statusTemplate = m_extraStatusTemplates[2];
    data.filter = QString();
    data.extraRisk = m_processPteTranslateRiskOnlyCheck && m_processPteTranslateRiskOnlyCheck->isChecked();
    data.extraDiagnostic = source->empty() ? QStringLiteral("PTE / VA 翻译当前没有缓存行；请先附加进程、输入或定位 VA 后刷新。") : QStringLiteral("当前风险过滤隐藏了全部 %1 条 PTE / VA 翻译记录；请关闭“仅风险项”。").arg(source->size());
    data.extraMatches = [source, filter = data.filter, risk = data.extraRisk](std::size_t row) { const auto& entry = (*source)[row]; return (!risk || (entry.riskText != QStringLiteral("正常"))) && (true); };
    data.extraKey = [source](std::size_t row) { const auto& entry = (*source)[row]; return QString::number(entry.virtualAddress); };
    data.extraCell = [source, language = data.language, diagnostic = data.extraDiagnostic](std::size_t row, int column, int role) -> QVariant {
        if (row == SIZE_MAX) {
            if (role == Qt::UserRole + 2) { return diagnostic; }
            if (role != Qt::DisplayRole && role != Qt::ToolTipRole) { return {}; }
            const auto columns = QStringList{QStringLiteral("<无PTE证据>"),QStringLiteral("N/A"),QStringLiteral("-"),QStringLiteral("-"),QStringLiteral("-"),QStringLiteral("-"),QStringLiteral("-"),QStringLiteral("0"),QStringLiteral("N/A"),QStringLiteral("0"),QStringLiteral("N/A"),diagnostic};
            return column >= 0 && column < columns.size() ? QVariant(language.sourceText(columns[column])) : QVariant();
        }
        const auto& entry = (*source)[row];
        if (role == Qt::UserRole + 1 && column == 0) { return QVariant::fromValue<qulonglong>(row); }
        if (role == Qt::UserRole && column == 0) { return QVariant::fromValue<qulonglong>(entry.virtualAddress); }
        const bool numeric = role == ks::ui::NumericSortRole;
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole && !numeric) { return {}; }
        switch (column) {
        case 0: return numeric ? QVariant::fromValue<qulonglong>(entry.virtualAddress) : QVariant(language.sourceText(hex64(entry.virtualAddress)));
        case 1: return numeric ? QVariant::fromValue<qulonglong>(entry.regionBaseAddress) : QVariant(language.sourceText(hex64(entry.regionBaseAddress)));
        case 2: return QVariant(language.sourceText(entry.valid ? QStringLiteral("Yes") : QStringLiteral("No")));
        case 3: return QVariant(language.sourceText(entry.shared ? QStringLiteral("Yes") : QStringLiteral("No")));
        case 4: return QVariant(language.sourceText(entry.locked ? QStringLiteral("Yes") : QStringLiteral("No")));
        case 5: return QVariant(language.sourceText(entry.largePage ? QStringLiteral("Yes") : QStringLiteral("No")));
        case 6: return QVariant(language.sourceText(entry.bad ? QStringLiteral("Yes") : QStringLiteral("No")));
        case 7: return numeric ? QVariant::fromValue<qulonglong>(entry.shareCount) : QVariant(language.sourceText(QString::number(entry.shareCount)));
        case 8: return numeric ? QVariant::fromValue<qulonglong>(entry.win32Protection) : QVariant(language.sourceText(QStringLiteral("0x%1").arg(entry.win32Protection, 8, 16, QChar('0')).toUpper()));
        case 9: return numeric ? QVariant::fromValue<qulonglong>(entry.node) : QVariant(language.sourceText(QString::number(entry.node)));
        case 10: return QVariant(language.sourceText(entry.mappedFilePath.isEmpty() ? QStringLiteral("—") : entry.mappedFilePath));
        case 11: return QVariant(language.sourceText(entry.riskText));
        default: return {};
        }
    };
}
