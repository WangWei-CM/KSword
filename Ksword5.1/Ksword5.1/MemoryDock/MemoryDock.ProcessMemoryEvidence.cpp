#include "MemoryDock.Internal.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/TableColumnAutoFit.h"

using namespace ksword::memory_dock_internal;

namespace
{
    // 进程内存证据页列定义：用于稳定排序、过滤和详情定位。
    enum class ProcessMemoryEvidenceColumn : int
    {
        VirtualAddress = 0,
        RegionBase,
        Protect,
        State,
        Type,
        Valid,
        Shared,
        Locked,
        LargePage,
        Bad,
        ShareCount,
        Win32Protection,
        MappedFile,
        Risk,
        Count
    };

    int evidenceColumnIndex(const ProcessMemoryEvidenceColumn column)
    {
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString protectText(const std::uint32_t protect)
    {
        switch (protect & 0xFFU)
        {
        case PAGE_READONLY: return QStringLiteral("R--");
        case PAGE_READWRITE: return QStringLiteral("RW-");
        case PAGE_WRITECOPY: return QStringLiteral("RWC");
        case PAGE_EXECUTE: return QStringLiteral("--X");
        case PAGE_EXECUTE_READ: return QStringLiteral("R-X");
        case PAGE_EXECUTE_READWRITE: return QStringLiteral("RWX");
        case PAGE_EXECUTE_WRITECOPY: return QStringLiteral("RXC");
        default: return QStringLiteral("0x%1").arg(protect, 8, 16, QChar('0')).toUpper();
        }
    }

    QString stateText(const std::uint32_t state)
    {
        switch (state)
        {
        case MEM_COMMIT: return QStringLiteral("COMMIT");
        case MEM_RESERVE: return QStringLiteral("RESERVE");
        case MEM_FREE: return QStringLiteral("FREE");
        default: return QStringLiteral("0x%1").arg(state, 8, 16, QChar('0')).toUpper();
        }
    }

    QString typeText(const std::uint32_t type)
    {
        switch (type)
        {
        case MEM_IMAGE: return QStringLiteral("IMAGE");
        case MEM_MAPPED: return QStringLiteral("MAPPED");
        case MEM_PRIVATE: return QStringLiteral("PRIVATE");
        default: return QStringLiteral("0x%1").arg(type, 8, 16, QChar('0')).toUpper();
        }
    }

    QString buildRiskText(
        const std::uint32_t protect,
        const std::uint32_t state,
        const std::uint32_t type,
        const bool valid,
        const bool largePage,
        const bool bad)
    {
        QStringList parts;
        if (!valid)
        {
            parts << QStringLiteral("无效页");
        }
        if (state != MEM_COMMIT)
        {
            parts << QStringLiteral("非提交");
        }
        if ((protect & PAGE_EXECUTE_READWRITE) == PAGE_EXECUTE_READWRITE ||
            (protect & PAGE_EXECUTE_WRITECOPY) == PAGE_EXECUTE_WRITECOPY)
        {
            parts << QStringLiteral("可写可执行");
        }
        if (type == MEM_PRIVATE && (protect & PAGE_EXECUTE) != 0U)
        {
            parts << QStringLiteral("私有执行");
        }
        if (largePage)
        {
            parts << QStringLiteral("大页");
        }
        if (bad)
        {
            parts << QStringLiteral("坏页");
        }
        return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QStringLiteral(" | "));
    }

    QString buildDetailText(const MemoryDock::ProcessMemoryEvidenceEntry& entry)
    {
        QString text;
        text += QStringLiteral("进程内存证据详情\n");
        text += QStringLiteral("VA: %1\n").arg(hex64(entry.virtualAddress));
        text += QStringLiteral("RegionBase: %1\n").arg(hex64(entry.regionBaseAddress));
        text += QStringLiteral("RegionSize: %1\n").arg(hex64(entry.regionSize));
        text += QStringLiteral("Protect: %1\n").arg(protectText(entry.protect));
        text += QStringLiteral("State: %1\n").arg(stateText(entry.state));
        text += QStringLiteral("Type: %1\n").arg(typeText(entry.type));
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

    QTableWidgetItem* makeItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        return item;
    }

    QString evidenceMenuStyle()
    {
        // evidenceMenuStyle：
        // - 输入：无；
        // - 处理：使用主题色生成不透明 QMenu 样式；
        // - 返回：右键菜单样式，避免透明菜单导致不可读。
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

    void copyEvidenceCurrentRow(QTableWidget* table)
    {
        // copyEvidenceCurrentRow：
        // - 输入：当前内存证据表；
        // - 处理：把当前行按 TSV 写入剪贴板；
        // - 返回：无，剪贴板不可用或无当前行时直接返回。
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

    void installEvidenceCopyMenu(QTableWidget* table)
    {
        // installEvidenceCopyMenu：
        // - 输入：进程内存证据表；
        // - 处理：安装只读复制菜单；
        // - 返回：无，不改变目标进程内存。
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
            menu.setStyleSheet(evidenceMenuStyle());
            QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制当前行"));
            copyRowAction->setEnabled(table->currentRow() >= 0);
            if (menu.exec(table->viewport()->mapToGlobal(localPosition)) == copyRowAction)
            {
                copyEvidenceCurrentRow(table);
            }
        });
    }

    void setProcessEvidenceDiagnosticRow(
        QTableWidget* table,
        const QString& detailText)
    {
        // setProcessEvidenceDiagnosticRow：
        // - 输入：目标进程内存证据表和诊断说明；
        // - 处理：写入一行可复制诊断，UserRole+2 保存完整文本；
        // - 返回：无。用于附加失败、空缓存或过滤空结果时避免空表。
        if (table == nullptr)
        {
            return;
        }

        table->setRowCount(1);
        QTableWidgetItem* vaItem = makeItem(QStringLiteral("<无进程内存证据>"));
        vaItem->setData(Qt::UserRole + 2, detailText);
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::VirtualAddress), vaItem);
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::RegionBase), makeItem(QStringLiteral("N/A")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Protect), makeItem(QStringLiteral("N/A")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::State), makeItem(QStringLiteral("N/A")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Type), makeItem(QStringLiteral("诊断")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Valid), makeItem(QStringLiteral("-")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Shared), makeItem(QStringLiteral("-")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Locked), makeItem(QStringLiteral("-")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::LargePage), makeItem(QStringLiteral("-")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Bad), makeItem(QStringLiteral("-")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::ShareCount), makeItem(QStringLiteral("0")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Win32Protection), makeItem(QStringLiteral("N/A")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::MappedFile), makeItem(QStringLiteral("N/A")));
        table->setItem(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Risk), makeItem(detailText));
        table->setCurrentCell(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::VirtualAddress));
    }

    class NumericItem final : public QTableWidgetItem
    {
    public:
        NumericItem(const QString& text, const qulonglong value)
            : QTableWidgetItem(text)
        {
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(value));
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
}

void MemoryDock::initializeProcessMemoryEvidenceTab()
{
    // 输入：无，由 initializeTabs 调用。
    // 处理：构建只读进程内存证据页，围绕 VirtualQueryEx / QueryWorkingSetEx 做风险投影。
    // 返回：无。
    kLogEvent initEvent;
    info << initEvent
        << "[MemoryDock] initializeProcessMemoryEvidenceTab: 构建进程内存证据页面。"
        << eol;

    m_tabProcessMemoryEvidence = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabProcessMemoryEvidence);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(8);

    m_processMemoryEvidenceRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceRefreshButton->setToolTip(QStringLiteral("按当前附加进程采集内存证据"));
    m_processMemoryEvidenceRefreshButton->setStyleSheet(buildBlueButtonStyle());

    m_processMemoryEvidenceRiskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceRiskOnlyCheck->setChecked(true);
    m_processMemoryEvidenceRiskOnlyCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }"
        "QCheckBox::indicator:checked { background:%1; border:1px solid %1; }")
        .arg(KswordTheme::PrimaryBlueHex));

    m_processMemoryEvidenceImageOnlyCheck = new QCheckBox(QStringLiteral("仅 IMAGE"), m_tabProcessMemoryEvidence);

    m_processMemoryEvidenceStartEdit = new QLineEdit(m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceStartEdit->setClearButtonEnabled(true);
    m_processMemoryEvidenceStartEdit->setPlaceholderText(QStringLiteral("起始 VA"));
    m_processMemoryEvidenceStartEdit->setStyleSheet(buildBlueInputStyle());

    m_processMemoryEvidenceEndEdit = new QLineEdit(m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceEndEdit->setClearButtonEnabled(true);
    m_processMemoryEvidenceEndEdit->setPlaceholderText(QStringLiteral("结束 VA"));
    m_processMemoryEvidenceEndEdit->setStyleSheet(buildBlueInputStyle());

    m_processMemoryEvidenceFilterEdit = new QLineEdit(m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceFilterEdit->setClearButtonEnabled(true);
    m_processMemoryEvidenceFilterEdit->setPlaceholderText(QStringLiteral("过滤映射文件 / 风险文本"));
    m_processMemoryEvidenceFilterEdit->setStyleSheet(buildBlueInputStyle());

    m_processMemoryEvidenceMaxRowsSpin = new QSpinBox(m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceMaxRowsSpin->setRange(32, 8192);
    m_processMemoryEvidenceMaxRowsSpin->setValue(512);
    m_processMemoryEvidenceMaxRowsSpin->setToolTip(QStringLiteral("单次采样最大页数"));

    m_processMemoryEvidenceStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_tabProcessMemoryEvidence);
    m_processMemoryEvidenceStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_processMemoryEvidenceStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_processMemoryEvidenceRefreshButton);
    toolLayout->addWidget(m_processMemoryEvidenceRiskOnlyCheck);
    toolLayout->addWidget(m_processMemoryEvidenceImageOnlyCheck);
    toolLayout->addWidget(new QLabel(QStringLiteral("起始"), m_tabProcessMemoryEvidence));
    toolLayout->addWidget(m_processMemoryEvidenceStartEdit);
    toolLayout->addWidget(new QLabel(QStringLiteral("结束"), m_tabProcessMemoryEvidence));
    toolLayout->addWidget(m_processMemoryEvidenceEndEdit);
    toolLayout->addWidget(new QLabel(QStringLiteral("过滤"), m_tabProcessMemoryEvidence));
    toolLayout->addWidget(m_processMemoryEvidenceFilterEdit, 1);
    toolLayout->addWidget(new QLabel(QStringLiteral("行数"), m_tabProcessMemoryEvidence));
    toolLayout->addWidget(m_processMemoryEvidenceMaxRowsSpin);
    toolLayout->addWidget(m_processMemoryEvidenceStatusLabel);
    tabLayout->addLayout(toolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabProcessMemoryEvidence);
    tabLayout->addWidget(splitter, 1);

    m_processMemoryEvidenceTable = new ks::ui::VisibleTableWidget(splitter);
    m_processMemoryEvidenceTable->setColumnCount(evidenceColumnIndex(ProcessMemoryEvidenceColumn::Count));
    m_processMemoryEvidenceTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("VA"),
        QStringLiteral("区域基址"),
        QStringLiteral("保护"),
        QStringLiteral("状态"),
        QStringLiteral("类型"),
        QStringLiteral("有效"),
        QStringLiteral("共享"),
        QStringLiteral("锁定"),
        QStringLiteral("大页"),
        QStringLiteral("坏页"),
        QStringLiteral("ShareCount"),
        QStringLiteral("Win32Prot"),
        QStringLiteral("映射文件"),
        QStringLiteral("风险")
    });
    m_processMemoryEvidenceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processMemoryEvidenceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processMemoryEvidenceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processMemoryEvidenceTable->setAlternatingRowColors(true);
    m_processMemoryEvidenceTable->setSortingEnabled(true);
    m_processMemoryEvidenceTable->verticalHeader()->setVisible(false);
    m_processMemoryEvidenceTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_processMemoryEvidenceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_processMemoryEvidenceTable->horizontalHeader()->setSectionResizeMode(evidenceColumnIndex(ProcessMemoryEvidenceColumn::MappedFile), QHeaderView::Stretch);
    installEvidenceCopyMenu(m_processMemoryEvidenceTable);
    splitter->addWidget(m_processMemoryEvidenceTable);

    m_processMemoryEvidenceDetailEditor = new CodeEditorWidget(splitter);
    m_processMemoryEvidenceDetailEditor->setReadOnly(true);
    m_processMemoryEvidenceDetailEditor->setText(QStringLiteral("请选择一条进程内存证据记录查看详情。"));
    splitter->addWidget(m_processMemoryEvidenceDetailEditor);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_tabProcessMemoryEvidence, QStringLiteral("进程内存证据"));
}

void MemoryDock::refreshProcessMemoryEvidenceAsync()
{
    // 输入：无，由刷新按钮或 tab 路由触发。
    // 处理：基于当前附加进程的地址范围采集 VirtualQueryEx / QueryWorkingSetEx 证据。
    // 返回：无。
    if (m_processMemoryEvidenceRefreshInProgress.exchange(true))
    {
        return;
    }

    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0U)
    {
        m_processMemoryEvidenceRefreshInProgress.store(false);
        if (m_processMemoryEvidenceStatusLabel != nullptr)
        {
            m_processMemoryEvidenceStatusLabel->setText(QStringLiteral("状态：请先附加进程。"));
            m_processMemoryEvidenceStatusLabel->setStyleSheet(
                QStringLiteral("color:%1; font-weight:600;")
                    .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
        }
        return;
    }

    if (m_processMemoryEvidenceRefreshButton != nullptr)
    {
        m_processMemoryEvidenceRefreshButton->setEnabled(false);
    }
    if (m_processMemoryEvidenceStatusLabel != nullptr)
    {
        m_processMemoryEvidenceStatusLabel->setText(QStringLiteral("状态：采集中..."));
        m_processMemoryEvidenceStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::PrimaryBlueHex));
    }

    std::uint64_t startAddress = 0ULL;
    std::uint64_t endAddress = 0ULL;
    if (m_processMemoryEvidenceStartEdit != nullptr)
    {
        parseAddressText(m_processMemoryEvidenceStartEdit->text().trimmed(), startAddress);
    }
    if (m_processMemoryEvidenceEndEdit != nullptr)
    {
        parseAddressText(m_processMemoryEvidenceEndEdit->text().trimmed(), endAddress);
    }

    const std::uint32_t maxRows = m_processMemoryEvidenceMaxRowsSpin != nullptr
        ? static_cast<std::uint32_t>(m_processMemoryEvidenceMaxRowsSpin->value())
        : 512U;
    const bool imageOnly = m_processMemoryEvidenceImageOnlyCheck != nullptr && m_processMemoryEvidenceImageOnlyCheck->isChecked();

    const std::uint64_t ticket = m_processMemoryEvidenceRefreshTicket.fetch_add(1U) + 1U;
    const QPointer<MemoryDock> guardThis(this);

    std::thread([guardThis, ticket, startAddress, endAddress, maxRows, imageOnly]() {
        if (guardThis == nullptr)
        {
            return;
        }

        std::vector<ProcessMemoryEvidenceEntry> entries;
        SYSTEM_INFO systemInfo{};
        ::GetSystemInfo(&systemInfo);
        const std::uint64_t pageSize = static_cast<std::uint64_t>(systemInfo.dwPageSize);
        const std::uint64_t minAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const std::uint64_t maxAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        std::uint64_t currentAddress = startAddress != 0ULL ? startAddress : minAddress;
        const std::uint64_t stopAddress = endAddress != 0ULL ? endAddress : maxAddress;
        if (pageSize != 0ULL)
        {
            currentAddress = (currentAddress / pageSize) * pageSize;
        }

        while (currentAddress < stopAddress && entries.size() < maxRows)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQueryEx(
                    guardThis->m_attachedProcessHandle,
                    reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(currentAddress)),
                    &mbi,
                    sizeof(mbi)) != sizeof(mbi))
            {
                break;
            }

            const std::uint64_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uint64_t regionSize = static_cast<std::uint64_t>(mbi.RegionSize);
            const bool regionIsImage = mbi.Type == MEM_IMAGE;
            if (imageOnly && !regionIsImage)
            {
                const std::uint64_t nextAddress = regionBase + regionSize;
                if (nextAddress <= currentAddress)
                {
                    break;
                }
                currentAddress = nextAddress;
                continue;
            }

            const std::uint64_t pageCount = pageSize == 0ULL ? 1ULL : std::max<std::uint64_t>(1ULL, regionSize / pageSize);
            for (std::uint64_t pageIndex = 0; pageIndex < pageCount && entries.size() < maxRows; ++pageIndex)
            {
                const std::uint64_t va = regionBase + (pageIndex * pageSize);
                if (va >= stopAddress)
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
                entry.regionBaseAddress = regionBase;
                entry.regionSize = regionSize;
                entry.protect = static_cast<std::uint32_t>(mbi.Protect);
                entry.state = static_cast<std::uint32_t>(mbi.State);
                entry.type = static_cast<std::uint32_t>(mbi.Type);
                entry.mappedFilePath = (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) ? QStringLiteral("") : QString();

                if (queryOk != FALSE)
                {
                    entry.valid = wsInfo.VirtualAttributes.Valid != 0;
                    entry.shared = wsInfo.VirtualAttributes.Shared != 0;
                    entry.locked = wsInfo.VirtualAttributes.Locked != 0;
                    entry.largePage = wsInfo.VirtualAttributes.LargePage != 0;
                    entry.bad = wsInfo.VirtualAttributes.Bad != 0;
                    entry.shareCount = static_cast<std::uint32_t>(wsInfo.VirtualAttributes.ShareCount);
                    entry.win32Protection = static_cast<std::uint32_t>(wsInfo.VirtualAttributes.Win32Protection);
                    entry.node = static_cast<std::uint32_t>(wsInfo.VirtualAttributes.Node);
                }

                entry.riskText = buildRiskText(entry.protect, entry.state, entry.type, entry.valid, entry.largePage, entry.bad);
                entry.detailText = QStringLiteral("VirtualQueryEx + QueryWorkingSetEx 只读证据");
                entries.push_back(std::move(entry));
            }

            const std::uint64_t nextAddress = regionBase + regionSize;
            if (nextAddress <= currentAddress)
            {
                break;
            }
            currentAddress = nextAddress;
        }

        QMetaObject::invokeMethod(
            guardThis.data(),
            [guardThis, ticket, entries = std::move(entries)]() mutable {
                if (guardThis == nullptr || ticket < guardThis->m_processMemoryEvidenceRefreshTicket.load())
                {
                    return;
                }

                guardThis->m_processMemoryEvidenceRefreshInProgress.store(false);
                if (guardThis->m_processMemoryEvidenceRefreshButton != nullptr)
                {
                    guardThis->m_processMemoryEvidenceRefreshButton->setEnabled(true);
                }

                guardThis->m_processMemoryEvidenceCache = std::move(entries);
                guardThis->rebuildProcessMemoryEvidenceTable();
                guardThis->showProcessMemoryEvidenceDetailByCurrentRow();
                if (guardThis->m_processMemoryEvidenceStatusLabel != nullptr)
                {
                    guardThis->m_processMemoryEvidenceStatusLabel->setText(
                        QStringLiteral("状态：采样 %1 行").arg(guardThis->m_processMemoryEvidenceCache.size()));
                    guardThis->m_processMemoryEvidenceStatusLabel->setStyleSheet(QStringLiteral(
                        "color:%1; font-weight:600;")
                        .arg(guardThis->m_processMemoryEvidenceCache.empty()
                            ? KswordTheme::ErrorColor().name(QColor::HexRgb)
                            : KswordTheme::SuccessColor().name(QColor::HexRgb)));
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void MemoryDock::rebuildProcessMemoryEvidenceTable()
{
    // 输入：读取当前缓存和过滤选项。
    // 处理：把进程内存证据投影到只读表格。
    // 返回：无。
    if (m_processMemoryEvidenceTable == nullptr)
    {
        return;
    }

    const bool riskOnly = m_processMemoryEvidenceRiskOnlyCheck != nullptr && m_processMemoryEvidenceRiskOnlyCheck->isChecked();
    const QString filterText = m_processMemoryEvidenceFilterEdit != nullptr
        ? m_processMemoryEvidenceFilterEdit->text().trimmed()
        : QString();

    std::vector<const ProcessMemoryEvidenceEntry*> visibleEntries;
    visibleEntries.reserve(m_processMemoryEvidenceCache.size());
    for (const ProcessMemoryEvidenceEntry& entry : m_processMemoryEvidenceCache)
    {
        if (riskOnly && entry.riskText == QStringLiteral("正常"))
        {
            continue;
        }
        if (!filterText.isEmpty())
        {
            const QString detailText = entry.detailText;
            const QString mappedText = entry.mappedFilePath;
            if (!detailText.contains(filterText, Qt::CaseInsensitive) &&
                !mappedText.contains(filterText, Qt::CaseInsensitive) &&
                !entry.riskText.contains(filterText, Qt::CaseInsensitive))
            {
                continue;
            }
        }
        visibleEntries.push_back(&entry);
    }

    m_processMemoryEvidenceVisibleCount = visibleEntries.size();
    const QSignalBlocker blocker(m_processMemoryEvidenceTable);
    m_processMemoryEvidenceTable->setSortingEnabled(false);
    m_processMemoryEvidenceTable->setRowCount(static_cast<int>(visibleEntries.size()));
    for (int row = 0; row < static_cast<int>(visibleEntries.size()); ++row)
    {
        const ProcessMemoryEvidenceEntry& entry = *visibleEntries[static_cast<std::size_t>(row)];
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::VirtualAddress), new NumericItem(hex64(entry.virtualAddress), entry.virtualAddress));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::RegionBase), new NumericItem(hex64(entry.regionBaseAddress), entry.regionBaseAddress));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Protect), makeItem(protectText(entry.protect)));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::State), makeItem(stateText(entry.state)));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Type), makeItem(typeText(entry.type)));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Valid), makeItem(entry.valid ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Shared), makeItem(entry.shared ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Locked), makeItem(entry.locked ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::LargePage), makeItem(entry.largePage ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Bad), makeItem(entry.bad ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::ShareCount), new NumericItem(QString::number(entry.shareCount), entry.shareCount));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Win32Protection), new NumericItem(QStringLiteral("0x%1").arg(entry.win32Protection, 8, 16, QChar('0')).toUpper(), entry.win32Protection));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::MappedFile), makeItem(entry.mappedFilePath.isEmpty() ? QStringLiteral("—") : entry.mappedFilePath));
        m_processMemoryEvidenceTable->setItem(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::Risk), makeItem(entry.riskText));
    }
    if (visibleEntries.empty())
    {
        const QString detailText = m_processMemoryEvidenceCache.empty()
            ? QStringLiteral("进程内存证据当前没有缓存行；请先附加进程并刷新，或检查采样范围。")
            : QStringLiteral("当前过滤条件隐藏了全部 %1 条进程内存证据；请清空过滤或关闭风险/IMAGE 过滤。")
                .arg(static_cast<qulonglong>(m_processMemoryEvidenceCache.size()));
        setProcessEvidenceDiagnosticRow(m_processMemoryEvidenceTable, detailText);
    }

    if (m_processMemoryEvidenceTable->rowCount() > 0 && m_processMemoryEvidenceTable->currentRow() < 0)
    {
        m_processMemoryEvidenceTable->setCurrentCell(0, evidenceColumnIndex(ProcessMemoryEvidenceColumn::VirtualAddress));
    }
    m_processMemoryEvidenceTable->setSortingEnabled(true);
    ks::ui::RequestTableColumnAutoFit(m_processMemoryEvidenceTable);
}

void MemoryDock::showProcessMemoryEvidenceDetailByCurrentRow()
{
    // 输入：无，读取当前表格选中行。
    // 处理：从缓存中展开当前证据记录到详情编辑器。
    // 返回：无。
    if (m_processMemoryEvidenceDetailEditor == nullptr || m_processMemoryEvidenceTable == nullptr)
    {
        return;
    }

    const int row = m_processMemoryEvidenceTable->currentRow();
    if (row < 0 || row >= m_processMemoryEvidenceTable->rowCount())
    {
        m_processMemoryEvidenceDetailEditor->setText(QStringLiteral("请选择一条进程内存证据记录查看详情。"));
        return;
    }

    const QTableWidgetItem* addressItem = m_processMemoryEvidenceTable->item(row, evidenceColumnIndex(ProcessMemoryEvidenceColumn::VirtualAddress));
    if (addressItem == nullptr)
    {
        return;
    }
    const QString diagnosticText = addressItem->data(Qt::UserRole + 2).toString();
    if (!diagnosticText.isEmpty())
    {
        m_processMemoryEvidenceDetailEditor->setText(QStringLiteral("进程内存证据诊断\n%1").arg(diagnosticText));
        return;
    }

    bool ok = false;
    const qulonglong addressValue = addressItem->data(Qt::UserRole).toULongLong(&ok);
    if (!ok)
    {
        return;
    }

    for (const ProcessMemoryEvidenceEntry& entry : m_processMemoryEvidenceCache)
    {
        if (entry.virtualAddress == static_cast<std::uint64_t>(addressValue))
        {
            m_processMemoryEvidenceDetailEditor->setText(buildDetailText(entry));
            return;
        }
    }
}
