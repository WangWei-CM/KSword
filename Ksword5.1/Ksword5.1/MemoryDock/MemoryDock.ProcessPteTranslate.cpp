#include "MemoryDock.Internal.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/TableColumnAutoFit.h"

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
        return QStringLiteral(
            "QMenu{background:%1;color:%2;border:1px solid %3;}"
            "QMenu::item{padding:5px 24px 5px 24px;background:transparent;}"
            "QMenu::item:selected{background:%4;color:%6;}"
            "QMenu::item:disabled{color:%5;}")
            .arg(KswordTheme::SurfaceColorHex())
            .arg(KswordTheme::TextPrimaryColorHex())
            .arg(KswordTheme::BorderColorHex())
            .arg(KswordTheme::AccentHex(KswordTheme::AccentRole::Blue))
            .arg(KswordTheme::TextSecondaryColorHex())
            .arg(KswordTheme::OnAccentHex());
    }

    void copyPteCurrentRow(QTableWidget* table)
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
            const QTableWidgetItem* item = table->item(rowIndex, columnIndex);
            fields.push_back(item != nullptr ? item->text() : QString());
        }
        QApplication::clipboard()->setText(fields.join('\t'));
    }

    void installPteCopyMenu(QTableWidget* table)
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
        QObject::connect(table, &QTableWidget::customContextMenuRequested, table, [table](const QPoint& localPosition) {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentCell(clickedIndex.row(), clickedIndex.column());
            }

            QMenu menu(table);
            menu.setStyleSheet(pteMenuStyle());
            QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制当前行"));
            copyRowAction->setEnabled(table->currentRow() >= 0);
            if (menu.exec(table->viewport()->mapToGlobal(localPosition)) == copyRowAction)
            {
                copyPteCurrentRow(table);
            }
        });
    }

    void setPteDiagnosticRow(
        QTableWidget* table,
        const QString& detailText)
    {
        // setPteDiagnosticRow：
        // - 输入：目标 PTE 表与诊断说明；
        // - 处理：写入一行可复制诊断，UserRole+2 保存完整文本；
        // - 返回：无。用于尚未附加、查询为空或风险过滤清空时避免空表。
        if (table == nullptr)
        {
            return;
        }

        table->setRowCount(1);
        QTableWidgetItem* vaItem = makeReadOnlyItem(QStringLiteral("<无PTE证据>"));
        vaItem->setData(Qt::UserRole + 2, detailText);
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress), vaItem);
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::RegionBase), makeReadOnlyItem(QStringLiteral("N/A")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Valid), makeReadOnlyItem(QStringLiteral("-")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Shared), makeReadOnlyItem(QStringLiteral("-")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Locked), makeReadOnlyItem(QStringLiteral("-")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::LargePage), makeReadOnlyItem(QStringLiteral("-")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Bad), makeReadOnlyItem(QStringLiteral("-")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::ShareCount), makeReadOnlyItem(QStringLiteral("0")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Win32Protection), makeReadOnlyItem(QStringLiteral("N/A")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Node), makeReadOnlyItem(QStringLiteral("0")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::MappedFile), makeReadOnlyItem(QStringLiteral("N/A")));
        table->setItem(0, pteTranslateColumnIndex(PteTranslateColumn::Risk), makeReadOnlyItem(detailText));
        table->setCurrentCell(0, pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress));
    }

    // 便于按数值排序的表格项。
    class NumericItem final : public QTableWidgetItem
    {
    public:
        NumericItem(const QString& text, const qulonglong numericValue)
            : QTableWidgetItem(text)
        {
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(numericValue));
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            bool leftOk = false;
            bool rightOk = false;
            const qulonglong leftValue = data(Qt::UserRole).toULongLong(&leftOk);
            const qulonglong rightValue = other.data(Qt::UserRole).toULongLong(&rightOk);
            if (leftOk && rightOk)
            {
                return leftValue < rightValue;
            }
            return QTableWidgetItem::operator<(other);
        }
    };

    // 生成用于详情窗的多行文本。
    QString buildPteDetailText(const MemoryDock::ProcessMemoryEvidenceEntry& entry)
    {
        QString text;
        text += QStringLiteral("PTE / VA 翻译详情\n");
        text += QStringLiteral("VirtualAddress: %1\n").arg(hex64(entry.virtualAddress));
        text += QStringLiteral("RegionBaseAddress: %1\n").arg(hex64(entry.regionBaseAddress));
        text += QStringLiteral("RegionSize: %1\n").arg(hex64(entry.regionSize));
        text += QStringLiteral("Protect: 0x%1\n").arg(entry.protect, 8, 16, QChar('0'));
        text += QStringLiteral("State: 0x%1\n").arg(entry.state, 8, 16, QChar('0'));
        text += QStringLiteral("Type: 0x%1\n").arg(entry.type, 8, 16, QChar('0'));
        text += QStringLiteral("Win32Protection: 0x%1\n").arg(entry.win32Protection, 8, 16, QChar('0'));
        text += QStringLiteral("ShareCount: %1\n").arg(entry.shareCount);
        text += QStringLiteral("Node: %1\n").arg(entry.node);
        text += QStringLiteral("Valid: %1\n").arg(entry.valid ? QStringLiteral("true") : QStringLiteral("false"));
        text += QStringLiteral("Shared: %1\n").arg(entry.shared ? QStringLiteral("true") : QStringLiteral("false"));
        text += QStringLiteral("Locked: %1\n").arg(entry.locked ? QStringLiteral("true") : QStringLiteral("false"));
        text += QStringLiteral("LargePage: %1\n").arg(entry.largePage ? QStringLiteral("true") : QStringLiteral("false"));
        text += QStringLiteral("Bad: %1\n").arg(entry.bad ? QStringLiteral("true") : QStringLiteral("false"));
        text += QStringLiteral("MappedFile: %1\n").arg(entry.mappedFilePath.isEmpty() ? QStringLiteral("—") : entry.mappedFilePath);
        text += QStringLiteral("Risk: %1\n").arg(entry.riskText);
        text += QStringLiteral("Detail: %1\n").arg(entry.detailText.isEmpty() ? QStringLiteral("—") : entry.detailText);
        return text;
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
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabProcessPteTranslate);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(8);

    m_processPteTranslateRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), m_tabProcessPteTranslate);
    m_processPteTranslateRefreshButton->setToolTip(QStringLiteral("基于当前附加进程采集 PTE / VA 翻译证据"));
    m_processPteTranslateRefreshButton->setStyleSheet(buildBlueButtonStyle());

    m_processPteTranslateRiskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), m_tabProcessPteTranslate);
    m_processPteTranslateRiskOnlyCheck->setChecked(true);
    m_processPteTranslateRiskOnlyCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }"
        "QCheckBox::indicator:checked { background:%1; border:1px solid %1; }")
        .arg(KswordTheme::PrimaryBlueHex));

    m_processPteTranslateAddressEdit = new QLineEdit(m_tabProcessPteTranslate);
    m_processPteTranslateAddressEdit->setClearButtonEnabled(true);
    m_processPteTranslateAddressEdit->setPlaceholderText(QStringLiteral("输入 VA，例如 0x7FF6..."));
    m_processPteTranslateAddressEdit->setStyleSheet(buildBlueInputStyle());

    m_processPteTranslatePageCountSpin = new QSpinBox(m_tabProcessPteTranslate);
    m_processPteTranslatePageCountSpin->setRange(1, 256);
    m_processPteTranslatePageCountSpin->setValue(16);
    m_processPteTranslatePageCountSpin->setToolTip(QStringLiteral("每次采样的页数上限"));

    m_processPteTranslateStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_tabProcessPteTranslate);
    m_processPteTranslateStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_processPteTranslateStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_processPteTranslateRefreshButton);
    toolLayout->addWidget(m_processPteTranslateRiskOnlyCheck);
    toolLayout->addWidget(new QLabel(QStringLiteral("VA"), m_tabProcessPteTranslate));
    toolLayout->addWidget(m_processPteTranslateAddressEdit, 1);
    toolLayout->addWidget(new QLabel(QStringLiteral("页数"), m_tabProcessPteTranslate));
    toolLayout->addWidget(m_processPteTranslatePageCountSpin);
    toolLayout->addWidget(m_processPteTranslateStatusLabel);
    tabLayout->addLayout(toolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabProcessPteTranslate);
    tabLayout->addWidget(splitter, 1);

    m_processPteTranslateTable = new ks::ui::VisibleTableWidget(splitter);
    m_processPteTranslateTable->setColumnCount(pteTranslateColumnIndex(PteTranslateColumn::Count));
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
    m_processPteTranslateTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_processPteTranslateTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_processPteTranslateTable->horizontalHeader()->setSectionResizeMode(pteTranslateColumnIndex(PteTranslateColumn::MappedFile), QHeaderView::Stretch);
    installPteCopyMenu(m_processPteTranslateTable);
    splitter->addWidget(m_processPteTranslateTable);

    m_processPteTranslateDetailEditor = new CodeEditorWidget(splitter);
    m_processPteTranslateDetailEditor->setReadOnly(true);
    m_processPteTranslateDetailEditor->setText(QStringLiteral("请选择一条 PTE / VA 翻译记录查看详情。"));
    splitter->addWidget(m_processPteTranslateDetailEditor);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

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

    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0U)
    {
        m_processPteTranslateRefreshInProgress.store(false);
        if (m_processPteTranslateStatusLabel != nullptr)
        {
            m_processPteTranslateStatusLabel->setText(QStringLiteral("状态：请先附加进程。"));
            m_processPteTranslateStatusLabel->setStyleSheet(
                QStringLiteral("color:%1; font-weight:600;")
                    .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
        }
        return;
    }

    if (m_processPteTranslateRefreshButton != nullptr)
    {
        m_processPteTranslateRefreshButton->setEnabled(false);
    }
    if (m_processPteTranslateStatusLabel != nullptr)
    {
        m_processPteTranslateStatusLabel->setText(QStringLiteral("状态：采集中..."));
        m_processPteTranslateStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::PrimaryBlueHex));
    }

    std::uint64_t baseAddress = 0ULL;
    if (m_processPteTranslateAddressEdit != nullptr)
    {
        const QString inputText = m_processPteTranslateAddressEdit->text().trimmed();
        if (!inputText.isEmpty())
        {
            parseAddressText(inputText, baseAddress);
        }
    }
    if (baseAddress == 0ULL)
    {
        baseAddress = m_currentViewerAddress;
    }
    const std::uint32_t pageCount = m_processPteTranslatePageCountSpin != nullptr
        ? static_cast<std::uint32_t>(m_processPteTranslatePageCountSpin->value())
        : 16U;

    const std::uint64_t ticket = m_processPteTranslateRefreshTicket.fetch_add(1U) + 1U;
    const std::uint32_t pid = m_attachedPid;
    const QPointer<MemoryDock> guardThis(this);

    std::thread([guardThis, ticket, pid, baseAddress, pageCount]() {
        if (guardThis == nullptr)
        {
            return;
        }

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
                    guardThis->m_attachedProcessHandle,
                    reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)),
                    &mbi,
                    sizeof(mbi)) != sizeof(mbi))
            {
                break;
            }

            PSAPI_WORKING_SET_EX_INFORMATION wsInfo{};
            wsInfo.VirtualAddress = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(va));
            const BOOL queryOk = ::QueryWorkingSetEx(
                guardThis->m_attachedProcessHandle,
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

        QMetaObject::invokeMethod(
            guardThis.data(),
            [guardThis, ticket, entries = std::move(entries)]() mutable {
                if (guardThis == nullptr || ticket < guardThis->m_processPteTranslateRefreshTicket.load())
                {
                    return;
                }

                guardThis->m_processPteTranslateRefreshInProgress.store(false);
                if (guardThis->m_processPteTranslateRefreshButton != nullptr)
                {
                    guardThis->m_processPteTranslateRefreshButton->setEnabled(true);
                }

                guardThis->m_processPteTranslateCache = std::move(entries);
                guardThis->rebuildProcessPteTranslateTable();
                guardThis->showProcessPteTranslateDetailByCurrentRow();

                if (guardThis->m_processPteTranslateStatusLabel != nullptr)
                {
                    guardThis->m_processPteTranslateStatusLabel->setText(
                        QStringLiteral("状态：采样 %1 行").arg(guardThis->m_processPteTranslateCache.size()));
                    guardThis->m_processPteTranslateStatusLabel->setStyleSheet(QStringLiteral(
                        "color:%1; font-weight:600;")
                        .arg(guardThis->m_processPteTranslateCache.empty()
                            ? KswordTheme::ErrorColor().name(QColor::HexRgb)
                            : KswordTheme::SuccessColor().name(QColor::HexRgb)));
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void MemoryDock::rebuildProcessPteTranslateTable()
{
    // 输入：读取当前缓存和风险过滤开关。
    // 处理：把 Tab9 缓存投影到表格并保留排序友好数值列。
    // 返回：无。
    if (m_processPteTranslateTable == nullptr)
    {
        return;
    }

    const bool riskOnly = m_processPteTranslateRiskOnlyCheck != nullptr && m_processPteTranslateRiskOnlyCheck->isChecked();
    std::vector<const ProcessMemoryEvidenceEntry*> visibleEntries;
    visibleEntries.reserve(m_processPteTranslateCache.size());
    for (const ProcessMemoryEvidenceEntry& entry : m_processPteTranslateCache)
    {
        if (riskOnly && entry.riskText == QStringLiteral("正常"))
        {
            continue;
        }
        visibleEntries.push_back(&entry);
    }

    m_processPteTranslateVisibleCount = visibleEntries.size();
    const QSignalBlocker blocker(m_processPteTranslateTable);
    m_processPteTranslateTable->setSortingEnabled(false);
    m_processPteTranslateTable->setRowCount(static_cast<int>(visibleEntries.size()));
    for (int row = 0; row < static_cast<int>(visibleEntries.size()); ++row)
    {
        const ProcessMemoryEvidenceEntry& entry = *visibleEntries[static_cast<std::size_t>(row)];
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress), new NumericItem(hex64(entry.virtualAddress), entry.virtualAddress));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::RegionBase), new NumericItem(hex64(entry.regionBaseAddress), entry.regionBaseAddress));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Valid), makeReadOnlyItem(entry.valid ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Shared), makeReadOnlyItem(entry.shared ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Locked), makeReadOnlyItem(entry.locked ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::LargePage), makeReadOnlyItem(entry.largePage ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Bad), makeReadOnlyItem(entry.bad ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::ShareCount), new NumericItem(QString::number(entry.shareCount), entry.shareCount));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Win32Protection), new NumericItem(QStringLiteral("0x%1").arg(entry.win32Protection, 8, 16, QChar('0')).toUpper(), entry.win32Protection));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Node), new NumericItem(QString::number(entry.node), entry.node));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::MappedFile), makeReadOnlyItem(entry.mappedFilePath.isEmpty() ? QStringLiteral("—") : entry.mappedFilePath));
        m_processPteTranslateTable->setItem(row, pteTranslateColumnIndex(PteTranslateColumn::Risk), makeReadOnlyItem(entry.riskText));
    }
    if (visibleEntries.empty())
    {
        const QString detailText = m_processPteTranslateCache.empty()
            ? QStringLiteral("PTE / VA 翻译当前没有缓存行；请先附加进程、输入或定位 VA 后刷新。")
            : QStringLiteral("当前风险过滤隐藏了全部 %1 条 PTE / VA 翻译记录；请关闭“仅风险项”。")
                .arg(static_cast<qulonglong>(m_processPteTranslateCache.size()));
        setPteDiagnosticRow(m_processPteTranslateTable, detailText);
    }

    if (m_processPteTranslateTable->rowCount() > 0 && m_processPteTranslateTable->currentRow() < 0)
    {
        m_processPteTranslateTable->setCurrentCell(0, pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress));
    }
    m_processPteTranslateTable->setSortingEnabled(true);
    ks::ui::RequestTableColumnAutoFit(m_processPteTranslateTable);
}

void MemoryDock::showProcessPteTranslateDetailByCurrentRow()
{
    // 输入：无，读取当前表格选中行。
    // 处理：从缓存中展开当前记录到 CodeEditorWidget。
    // 返回：无。
    if (m_processPteTranslateDetailEditor == nullptr || m_processPteTranslateTable == nullptr)
    {
        return;
    }

    const int row = m_processPteTranslateTable->currentRow();
    if (row < 0 || row >= m_processPteTranslateTable->rowCount())
    {
        m_processPteTranslateDetailEditor->setText(QStringLiteral("请选择一条 PTE / VA 翻译记录查看详情。"));
        return;
    }

    const QTableWidgetItem* addressItem = m_processPteTranslateTable->item(row, pteTranslateColumnIndex(PteTranslateColumn::VirtualAddress));
    if (addressItem == nullptr)
    {
        return;
    }
    const QString diagnosticText = addressItem->data(Qt::UserRole + 2).toString();
    if (!diagnosticText.isEmpty())
    {
        m_processPteTranslateDetailEditor->setText(QStringLiteral("PTE / VA 翻译诊断\n%1").arg(diagnosticText));
        return;
    }

    bool ok = false;
    const qulonglong addressValue = addressItem->data(Qt::UserRole).toULongLong(&ok);
    if (!ok)
    {
        return;
    }

    for (const ProcessMemoryEvidenceEntry& entry : m_processPteTranslateCache)
    {
        if (entry.virtualAddress == static_cast<std::uint64_t>(addressValue))
        {
            m_processPteTranslateDetailEditor->setText(buildPteDetailText(entry));
            return;
        }
    }
}
