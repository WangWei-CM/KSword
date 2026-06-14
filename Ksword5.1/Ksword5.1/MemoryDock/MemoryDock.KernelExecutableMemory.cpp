#include "MemoryDock.Internal.h"

#include <QPixmap>

using namespace ksword::memory_dock_internal;

namespace
{
    // KernelExecutableColumn:
    // - Input: table column enum used by MemoryDock's kernel executable scan UI.
    // - Processing: keeps column numbers stable when rows are rebuilt or sorted.
    // - Return behavior: enum values are converted to int at call sites.
    enum class KernelExecutableColumn : int
    {
        Va = 0,
        PageCount,
        PageSize,
        Permissions,
        Owner,
        ModulePath,
        RiskFlags,
        Count
    };

    int kernelExecutableColumnIndex(const KernelExecutableColumn column)
    {
        // 输入：KernelExecutableColumn 枚举值。
        // 处理：执行窄化到 QTableWidget 使用的 int 列号。
        // 返回：对应的表格列号。
        return static_cast<int>(column);
    }

    QString wideToQString(const std::wstring& text)
    {
        // 输入：ArkDriverClient 返回的 UTF-16 宽字符串。
        // 处理：统一转为 QString，空文本保留为空。
        // 返回：可供 Qt UI 展示和过滤的 QString。
        return text.empty() ? QString() : QString::fromStdWString(text);
    }

    QString hexValue(const std::uint64_t value)
    {
        // 输入：64 位诊断值。
        // 处理：格式化为固定宽度十六进制，便于地址列排序外的文本展示。
        // 返回：0x 前缀大写十六进制字符串。
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString permissionText(const std::uint32_t flags)
    {
        // 输入：KernelExecutableMemoryPermission* 位集合。
        // 处理：转换为紧凑权限文本，保留 NX/Large/User/Global 等诊断位。
        // 返回：权限展示字符串，例如 R-X | Large | Global。
        QStringList parts;
        QString rwx;
        rwx += (flags & ksword::ark::KernelExecutableMemoryPermissionPresent) ? QChar('R') : QChar('-');
        rwx += (flags & ksword::ark::KernelExecutableMemoryPermissionWritable) ? QChar('W') : QChar('-');
        rwx += (flags & ksword::ark::KernelExecutableMemoryPermissionNoExecute) ? QChar('-') : QChar('X');
        parts << rwx;
        if (flags & ksword::ark::KernelExecutableMemoryPermissionNoExecute)
        {
            parts << QStringLiteral("NX");
        }
        if (flags & ksword::ark::KernelExecutableMemoryPermissionLargePage)
        {
            parts << QStringLiteral("Large");
        }
        if (flags & ksword::ark::KernelExecutableMemoryPermissionUser)
        {
            parts << QStringLiteral("User");
        }
        if (flags & ksword::ark::KernelExecutableMemoryPermissionGlobal)
        {
            parts << QStringLiteral("Global");
        }
        return parts.join(QStringLiteral(" | "));
    }

    QString riskFlagsText(const std::uint32_t flags)
    {
        // 输入：KernelExecutableMemoryRisk* 位集合。
        // 处理：把风险位映射为面向分析人员的短标签。
        // 返回：无风险时返回“正常”，否则返回用竖线分隔的风险标签。
        if (flags == 0U)
        {
            return QStringLiteral("正常");
        }

        QStringList parts;
        if (flags & ksword::ark::KernelExecutableMemoryRiskWritableExecutable)
        {
            parts << QStringLiteral("WX");
        }
        if (flags & ksword::ark::KernelExecutableMemoryRiskModuleNonTextExecutable)
        {
            parts << QStringLiteral("非.text可执行");
        }
        if (flags & ksword::ark::KernelExecutableMemoryRiskSectionWritable)
        {
            parts << QStringLiteral("节可写");
        }
        if (flags & ksword::ark::KernelExecutableMemoryRiskLargePage)
        {
            parts << QStringLiteral("大页");
        }
        return parts.join(QStringLiteral(" | "));
    }

    QString ownerKindText(const std::uint32_t ownerKind)
    {
        // 输入：Prompt-1 响应中的 ownerKind 枚举值。
        // 处理：只按共享协议定义的 ownerKind 做 UI 文本映射，不推断额外语义。
        // 返回：Owner 列和详情页可直接展示的中文分类文本。
        switch (ownerKind)
        {
        case KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_TEXT:
            return QStringLiteral("模块 .text");
        case KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_NON_TEXT:
            return QStringLiteral("模块非 .text");
        case KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_WRITABLE_EXECUTABLE:
            return QStringLiteral("模块 WX");
        case KSWORD_ARK_KERNEL_EXEC_OWNER_UNKNOWN:
        default:
            return QStringLiteral("未知(%1)").arg(ownerKind);
        }
    }

    class KernelExecutableNumericItem final : public QTableWidgetItem
    {
    public:
        // 输入：显示文本和原始数值，显示文本按调用方格式保留。
        // 处理：数值只写入 UserRole，避免 Qt 把 EditRole 合并到 DisplayRole 后覆盖 VA 十六进制文本。
        // 返回：构造函数无返回值，item 生命周期交给 QTableWidget。
        KernelExecutableNumericItem(const QString& text, const qulonglong numericValue)
            : QTableWidgetItem(text)
        {
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(numericValue));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }

        // 输入：同列其它 item。
        // 处理：两侧都有 UserRole 数值时按数值排序，否则退回 Qt 默认文本排序。
        // 返回：true 表示当前 item 应排在 other 之前。
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

    QTableWidgetItem* createTextItem(const QString& text)
    {
        // 输入：单元格展示文本。
        // 处理：创建只读表格项并设置垂直居中。
        // 返回：交给 QTableWidget 接管生命周期的 item 指针。
        QTableWidgetItem* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QTableWidgetItem* createNumericItem(const QString& text, const qulonglong numericValue)
    {
        // 输入：展示文本和用于排序/反查的原始数值。
        // 处理：使用自定义 item 保留 DisplayRole 文本，同时用 UserRole 提供数值排序。
        // 返回：交给 QTableWidget 接管生命周期的 item 指针。
        return new KernelExecutableNumericItem(text, numericValue);
    }

    bool entryMatchesModuleFilter(
        const ksword::ark::KernelExecutableMemoryPageEntry& entry,
        const QString& moduleFilter)
    {
        // 输入：R3 扫描行和模块路径过滤文本。
        // 处理：只按模块路径字段做本地包含匹配；空过滤直接通过。
        // 返回：true 表示该行应显示。
        if (moduleFilter.isEmpty())
        {
            return true;
        }

        return wideToQString(entry.modulePath).contains(moduleFilter, Qt::CaseInsensitive);
    }

    QString buildKernelExecutableDetailText(
        const ksword::ark::KernelExecutableMemoryPageEntry& entry)
    {
        // 输入：当前选中的内核可执行页扫描行。
        // 处理：生成适合 CodeEditorWidget 展示的多行诊断文本。
        // 返回：详情文本，调用方直接 setText。
        QString detailText;
        detailText += QStringLiteral("内核可执行页扫描详情\n");
        detailText += QStringLiteral("VA: %1\n").arg(hexValue(entry.virtualAddress));
        detailText += QStringLiteral("RegionSize: %1\n").arg(hexValue(entry.regionSize));
        detailText += QStringLiteral("PageCount: %1\n").arg(entry.pageCount);
        detailText += QStringLiteral("PageSize: %1\n").arg(entry.pageSize);
        detailText += QStringLiteral("Permissions: %1 (0x%2)\n")
            .arg(permissionText(entry.permissionFlags))
            .arg(entry.permissionFlags, 8, 16, QChar('0'));
        detailText += QStringLiteral("RiskFlags: %1 (0x%2)\n")
            .arg(riskFlagsText(entry.riskFlags))
            .arg(entry.riskFlags, 8, 16, QChar('0'));
        detailText += QStringLiteral("Status: %1\n").arg(entry.status);
        detailText += QStringLiteral("LastStatus: 0x%1\n")
            .arg(static_cast<qulonglong>(static_cast<unsigned long>(entry.lastStatus)), 8, 16, QChar('0'));
        detailText += QStringLiteral("OwnerKind: %1\n").arg(entry.ownerKind);
        detailText += QStringLiteral("Owner: %1\n").arg(ownerKindText(entry.ownerKind));
        detailText += QStringLiteral("OwnerAddress: %1\n").arg(hexValue(entry.ownerAddress));
        detailText += QStringLiteral("ModuleBase: %1\n").arg(hexValue(entry.moduleBase));
        detailText += QStringLiteral("ModuleSize: %1\n").arg(hexValue(entry.moduleSize));
        detailText += QStringLiteral("ModulePath: %1\n").arg(wideToQString(entry.modulePath));

        const QString r0Detail = wideToQString(entry.detail).trimmed();
        if (!r0Detail.isEmpty())
        {
            detailText += QStringLiteral("\nR0 Detail:\n%1\n").arg(r0Detail);
        }
        return detailText;
    }

    QString kernelExecutableStatusStyle(const QString& colorText)
    {
        // 输入：颜色字符串。
        // 处理：统一生成状态标签样式。
        // 返回：可直接 setStyleSheet 的 CSS 文本。
        return QStringLiteral("color:%1; font-weight:700;").arg(colorText);
    }
}

void MemoryDock::initializeKernelExecutableMemoryScanTab()
{
    // 输入：无，由 initializeTabs 调用。
    // 处理：构建内核可执行页扫描页面，包含刷新入口、风险/路径过滤、表格、详情编辑器和右下角 Kernel.png。
    // 返回：无。
    kLogEvent tab7InitEvent;
    info << tab7InitEvent
        << "[MemoryDock] initializeKernelExecutableMemoryScanTab: 构建内核可执行页扫描页面。"
        << eol;

    m_tabKernelExecutableMemory = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabKernelExecutableMemory);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(8);

    m_kernelExecutableRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), m_tabKernelExecutableMemory);
    m_kernelExecutableRefreshButton->setToolTip(QStringLiteral("调用 ArkDriverClient 执行 R0 内核可执行页扫描"));
    m_kernelExecutableRefreshButton->setStyleSheet(buildBlueButtonStyle());

    m_kernelExecutableRiskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), m_tabKernelExecutableMemory);
    m_kernelExecutableRiskOnlyCheck->setChecked(true);
    m_kernelExecutableRiskOnlyCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }"
        "QCheckBox::indicator:checked { background:%1; border:1px solid %1; }")
        .arg(KswordTheme::PrimaryBlueHex));

    m_kernelExecutableModuleFilterEdit = new QLineEdit(m_tabKernelExecutableMemory);
    m_kernelExecutableModuleFilterEdit->setClearButtonEnabled(true);
    m_kernelExecutableModuleFilterEdit->setPlaceholderText(QStringLiteral("按模块路径过滤，如 ntoskrnl.exe / drivers\\xxx.sys"));
    m_kernelExecutableModuleFilterEdit->setStyleSheet(buildBlueInputStyle());

    m_kernelExecutableStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_tabKernelExecutableMemory);
    m_kernelExecutableStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelExecutableStatusLabel->setStyleSheet(kernelExecutableStatusStyle(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_kernelExecutableRefreshButton, 0);
    toolLayout->addWidget(m_kernelExecutableRiskOnlyCheck, 0);
    toolLayout->addWidget(m_kernelExecutableModuleFilterEdit, 1);
    toolLayout->addWidget(m_kernelExecutableStatusLabel, 0);
    tabLayout->addLayout(toolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabKernelExecutableMemory);
    tabLayout->addWidget(splitter, 1);

    m_kernelExecutableTable = new QTableWidget(splitter);
    m_kernelExecutableTable->setColumnCount(kernelExecutableColumnIndex(KernelExecutableColumn::Count));
    m_kernelExecutableTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("VA"),
        QStringLiteral("页数"),
        QStringLiteral("页大小"),
        QStringLiteral("权限"),
        QStringLiteral("Owner"),
        QStringLiteral("模块路径"),
        QStringLiteral("风险标志")
        });
    m_kernelExecutableTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_kernelExecutableTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_kernelExecutableTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_kernelExecutableTable->setAlternatingRowColors(true);
    m_kernelExecutableTable->setSortingEnabled(true);
    m_kernelExecutableTable->verticalHeader()->setVisible(false);
    m_kernelExecutableTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_kernelExecutableTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_kernelExecutableTable->horizontalHeader()->setSectionResizeMode(kernelExecutableColumnIndex(KernelExecutableColumn::ModulePath), QHeaderView::Stretch);
    m_kernelExecutableTable->setColumnWidth(kernelExecutableColumnIndex(KernelExecutableColumn::Va), 170);
    m_kernelExecutableTable->setColumnWidth(kernelExecutableColumnIndex(KernelExecutableColumn::Owner), 180);
    m_kernelExecutableTable->setColumnWidth(kernelExecutableColumnIndex(KernelExecutableColumn::RiskFlags), 220);
    splitter->addWidget(m_kernelExecutableTable);

    QWidget* detailPanel = new QWidget(splitter);
    QHBoxLayout* detailLayout = new QHBoxLayout(detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(8);

    m_kernelExecutableDetailEditor = new CodeEditorWidget(detailPanel);
    m_kernelExecutableDetailEditor->setReadOnly(true);
    m_kernelExecutableDetailEditor->setText(QStringLiteral("请选择一条内核可执行页记录查看详情。"));
    detailLayout->addWidget(m_kernelExecutableDetailEditor, 1);

    QVBoxLayout* badgeLayout = new QVBoxLayout();
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    badgeLayout->addStretch(1);
    m_kernelExecutableBadgeLabel = new QLabel(detailPanel);
    m_kernelExecutableBadgeLabel->setToolTip(QStringLiteral("R0 功能入口：内核可执行页扫描"));
    m_kernelExecutableBadgeLabel->setPixmap(QPixmap(QStringLiteral(":/Image/kernel_badge.png")).scaled(
        36,
        36,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
    m_kernelExecutableBadgeLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    badgeLayout->addWidget(m_kernelExecutableBadgeLabel, 0, Qt::AlignRight | Qt::AlignBottom);
    detailLayout->addLayout(badgeLayout);
    splitter->addWidget(detailPanel);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_tabKernelExecutableMemory, QStringLiteral("内核可执行页"));
}

void MemoryDock::refreshKernelExecutableMemoryScanAsync()
{
    // 输入：由刷新按钮或初始化路径触发，无参数。
    // 处理：异步调用 DriverClient::scanKernelExecutableMemory，主线程仅负责落地结果。
    // 返回：无。
    if (m_kernelExecutableRefreshInProgress.exchange(true))
    {
        return;
    }

    if (m_kernelExecutableRefreshButton != nullptr)
    {
        m_kernelExecutableRefreshButton->setEnabled(false);
    }
    if (m_kernelExecutableStatusLabel != nullptr)
    {
        m_kernelExecutableStatusLabel->setText(QStringLiteral("状态：扫描中..."));
        m_kernelExecutableStatusLabel->setStyleSheet(kernelExecutableStatusStyle(KswordTheme::PrimaryBlueHex));
    }

    const std::uint64_t ticket = m_kernelExecutableRefreshTicket.fetch_add(1U) + 1U;
    const QPointer<MemoryDock> guardThis(this);

    std::thread([guardThis, ticket]() {
        if (guardThis == nullptr)
        {
            return;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::KernelExecutableMemoryScanResult scanResult = driverClient.scanKernelExecutableMemory(
            KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_ALL,
            4096U,
            std::wstring());

        QMetaObject::invokeMethod(
            guardThis.data(),
            [guardThis, ticket, scanResult = std::move(scanResult)]() mutable {
                if (guardThis == nullptr)
                {
                    return;
                }

                if (ticket < guardThis->m_kernelExecutableRefreshTicket.load())
                {
                    return;
                }

                guardThis->m_kernelExecutableRefreshInProgress.store(false);
                if (guardThis->m_kernelExecutableRefreshButton != nullptr)
                {
                    guardThis->m_kernelExecutableRefreshButton->setEnabled(true);
                }

                if (!scanResult.io.ok)
                {
                    guardThis->m_kernelExecutableCache.clear();
                    guardThis->m_kernelExecutableVisibleCount = 0U;
                    guardThis->rebuildKernelExecutableMemoryScanTable();

                    const QString unsupportedText = scanResult.unsupported
                        ? QStringLiteral("不支持/驱动版本过旧")
                        : QStringLiteral("扫描失败");
                    if (guardThis->m_kernelExecutableStatusLabel != nullptr)
                    {
                        guardThis->m_kernelExecutableStatusLabel->setText(
                            QStringLiteral("状态：%1").arg(unsupportedText));
                        guardThis->m_kernelExecutableStatusLabel->setStyleSheet(kernelExecutableStatusStyle(
                            scanResult.unsupported ? QStringLiteral("#B23A3A") : KswordTheme::TextSecondaryHex()));
                    }
                    if (guardThis->m_kernelExecutableDetailEditor != nullptr)
                    {
                        guardThis->m_kernelExecutableDetailEditor->setText(
                            scanResult.unsupported
                            ? QStringLiteral("不支持/驱动版本过旧。\n\n当前驱动未提供 Prompt 1 的内核可执行页扫描 IOCTL，或协议版本过旧。")
                            : QStringLiteral("内核可执行页扫描失败。\n\nWin32: %1\n详情: %2")
                                .arg(scanResult.io.win32Error)
                                .arg(QString::fromStdString(scanResult.io.message)));
                    }
                    return;
                }

                guardThis->m_kernelExecutableCache = scanResult.entries;
                guardThis->rebuildKernelExecutableMemoryScanTable();
                guardThis->showKernelExecutableMemoryDetailByCurrentRow();
                if (guardThis->m_kernelExecutableStatusLabel != nullptr)
                {
                    guardThis->m_kernelExecutableStatusLabel->setText(
                        QStringLiteral("状态：总计 %1，显示 %2，模块 %3")
                        .arg(scanResult.totalCount)
                        .arg(guardThis->m_kernelExecutableVisibleCount)
                        .arg(scanResult.moduleCount));
                    guardThis->m_kernelExecutableStatusLabel->setStyleSheet(kernelExecutableStatusStyle(
                        scanResult.entries.empty() ? QStringLiteral("#B23A3A") : QStringLiteral("#2F7D32")));
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void MemoryDock::rebuildKernelExecutableMemoryScanTable()
{
    // 输入：无，依赖 m_kernelExecutableCache 与当前过滤控件。
    // 处理：只做缓存到表格的投影，不重新调用 DriverClient。
    // 返回：无。
    const QString moduleFilter = m_kernelExecutableModuleFilterEdit != nullptr
        ? m_kernelExecutableModuleFilterEdit->text().trimmed()
        : QString();
    const bool riskOnly = m_kernelExecutableRiskOnlyCheck != nullptr
        ? m_kernelExecutableRiskOnlyCheck->isChecked()
        : false;

    std::vector<const ksword::ark::KernelExecutableMemoryPageEntry*> visibleEntries;
    visibleEntries.reserve(m_kernelExecutableCache.size());
    for (const ksword::ark::KernelExecutableMemoryPageEntry& entry : m_kernelExecutableCache)
    {
        if (riskOnly && entry.riskFlags == 0U)
        {
            continue;
        }
        if (!entryMatchesModuleFilter(entry, moduleFilter))
        {
            continue;
        }
        visibleEntries.push_back(&entry);
    }

    m_kernelExecutableVisibleCount = visibleEntries.size();
    if (m_kernelExecutableTable == nullptr)
    {
        return;
    }

    m_kernelExecutableTable->setSortingEnabled(false);
    const QSignalBlocker blocker(m_kernelExecutableTable);
    m_kernelExecutableTable->setRowCount(static_cast<int>(visibleEntries.size()));
    for (int row = 0; row < static_cast<int>(visibleEntries.size()); ++row)
    {
        const ksword::ark::KernelExecutableMemoryPageEntry& entry = *visibleEntries[static_cast<std::size_t>(row)];
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::Va),
            createNumericItem(hexValue(entry.virtualAddress), entry.virtualAddress));
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::PageCount),
            createNumericItem(QString::number(entry.pageCount), entry.pageCount));
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::PageSize),
            createNumericItem(QString::number(entry.pageSize), entry.pageSize));
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::Permissions),
            createTextItem(permissionText(entry.permissionFlags)));
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::Owner),
            createTextItem(ownerKindText(entry.ownerKind)));
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::ModulePath),
            createTextItem(wideToQString(entry.modulePath)));
        m_kernelExecutableTable->setItem(row, kernelExecutableColumnIndex(KernelExecutableColumn::RiskFlags),
            createTextItem(riskFlagsText(entry.riskFlags)));
    }
    if (m_kernelExecutableTable->rowCount() > 0 && m_kernelExecutableTable->currentRow() < 0)
    {
        m_kernelExecutableTable->setCurrentCell(0, kernelExecutableColumnIndex(KernelExecutableColumn::Va));
    }
    m_kernelExecutableTable->setSortingEnabled(true);
}

void MemoryDock::showKernelExecutableMemoryDetailByCurrentRow()
{
    // 输入：无，依赖当前表格选中行。
    // 处理：把当前行对应的 R3 记录展开到 CodeEditorWidget。
    // 返回：无。
    if (m_kernelExecutableDetailEditor == nullptr || m_kernelExecutableTable == nullptr)
    {
        return;
    }

    const int currentRow = m_kernelExecutableTable->currentRow();
    if (currentRow < 0 || currentRow >= m_kernelExecutableTable->rowCount())
    {
        m_kernelExecutableDetailEditor->setText(QStringLiteral("请选择一条内核可执行页记录查看详情。"));
        return;
    }

    const QTableWidgetItem* vaItem = m_kernelExecutableTable->item(currentRow, kernelExecutableColumnIndex(KernelExecutableColumn::Va));
    if (vaItem == nullptr)
    {
        return;
    }

    bool ok = false;
    const qulonglong va = vaItem->data(Qt::UserRole).toULongLong(&ok);
    if (!ok)
    {
        return;
    }

    for (const ksword::ark::KernelExecutableMemoryPageEntry& entry : m_kernelExecutableCache)
    {
        if (entry.virtualAddress == va)
        {
            m_kernelExecutableDetailEditor->setText(buildKernelExecutableDetailText(entry));
            return;
        }
    }
}
