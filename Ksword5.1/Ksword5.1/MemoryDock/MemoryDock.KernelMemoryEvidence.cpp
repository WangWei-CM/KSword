#include "MemoryDock.Internal.h"
#include "../UI/TableColumnAutoFit.h"

using namespace ksword::memory_dock_internal;

namespace
{
    enum class EvidenceColumn : int
    {
        Address = 0,
        Size,
        Kind,
        Owner,
        Permissions,
        Risk,
        TextHash,
        Detail,
        Count
    };

    int evidenceColumnIndex(const EvidenceColumn column)
    {
        // 输入：内核内存证据列枚举。
        // 处理：转换为 Qt 表格使用的 int 列索引。
        // 返回：对应列号。
        return static_cast<int>(column);
    }

    QString wideToQString(const std::wstring& value)
    {
        // 输入：ArkDriverClient 返回的宽字符串。
        // 处理：转换为 QString；空字符串保持为空。
        // 返回：Qt 可展示文本。
        return value.empty() ? QString() : QString::fromStdWString(value);
    }

    QString bytesToHex(const std::vector<std::uint8_t>& bytes)
    {
        // 输入：样本字节数组。
        // 处理：以空格分隔的大写十六进制展示，限制由 R0 协议保证。
        // 返回：可复制的十六进制文本。
        QStringList parts;
        parts.reserve(static_cast<int>(bytes.size()));
        for (const std::uint8_t byteValue : bytes)
        {
            parts << QStringLiteral("%1").arg(byteValue, 2, 16, QChar('0')).toUpper();
        }
        return parts.join(QStringLiteral(" "));
    }

    QString hex64(const std::uint64_t value)
    {
        // 输入：64 位地址或哈希值。
        // 处理：格式化为固定宽度十六进制。
        // 返回：0x 前缀大写字符串。
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString sizeText(const std::uint64_t bytes)
    {
        // 输入：字节数。
        // 处理：在 KB/MB/GB 和原始字节之间选择紧凑展示。
        // 返回：可读大小字符串。
        if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
        }
        if (bytes >= 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
        }
        if (bytes >= 1024ULL)
        {
            return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 2);
        }
        return QStringLiteral("%1 B").arg(static_cast<qulonglong>(bytes));
    }

    QString evidenceKindText(const std::uint32_t kind)
    {
        // 输入：KSWORD_ARK_MEMORY_EVIDENCE_KIND_*。
        // 处理：映射为 UI 分组文本。
        // 返回：中文证据类型。
        switch (kind)
        {
        case KSWORD_ARK_MEMORY_EVIDENCE_KIND_EXECUTABLE_RANGE:
            return QStringLiteral("执行页");
        case KSWORD_ARK_MEMORY_EVIDENCE_KIND_BIGPOOL:
            return QStringLiteral("BigPool");
        case KSWORD_ARK_MEMORY_EVIDENCE_KIND_TEXT_SECTION_MEMORY:
            return QStringLiteral("text hash");
        case KSWORD_ARK_MEMORY_EVIDENCE_KIND_UNKNOWN:
        default:
            return QStringLiteral("未知(%1)").arg(kind);
        }
    }

    QString ownerKindText(const std::uint32_t ownerKind)
    {
        // 输入：KSWORD_ARK_MEMORY_EVIDENCE_OWNER_*。
        // 处理：映射为 owner 分类标签。
        // 返回：中文 owner 类型。
        switch (ownerKind)
        {
        case KSWORD_ARK_MEMORY_EVIDENCE_OWNER_LOADED_MODULE:
            return QStringLiteral("LoadedModule");
        case KSWORD_ARK_MEMORY_EVIDENCE_OWNER_NONMODULE:
            return QStringLiteral("NonModule");
        case KSWORD_ARK_MEMORY_EVIDENCE_OWNER_BIGPOOL:
            return QStringLiteral("BigPool");
        case KSWORD_ARK_MEMORY_EVIDENCE_OWNER_SYSTEM_PTE:
            return QStringLiteral("SystemPte");
        case KSWORD_ARK_MEMORY_EVIDENCE_OWNER_MDL_LIKE:
            return QStringLiteral("MdlLike");
        case KSWORD_ARK_MEMORY_EVIDENCE_OWNER_UNKNOWN:
        default:
            return QStringLiteral("Unknown(%1)").arg(ownerKind);
        }
    }

    QString permissionText(const std::uint32_t flags)
    {
        // 输入：KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_* 位集合。
        // 处理：转换为 R/W/X/NX/Large 等紧凑文本。
        // 返回：权限文本。
        QStringList parts;
        QString rwx;
        rwx += (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_READ) ? QChar('R') : QChar('-');
        rwx += (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_WRITE) ? QChar('W') : QChar('-');
        rwx += (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_EXECUTE) ? QChar('X') : QChar('-');
        parts << rwx;
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_PRESENT) parts << QStringLiteral("Present");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_NX) parts << QStringLiteral("NX");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_LARGE) parts << QStringLiteral("Large");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_GLOBAL) parts << QStringLiteral("Global");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_PERMISSION_USER) parts << QStringLiteral("User");
        return parts.join(QStringLiteral(" | "));
    }

    QString riskText(const std::uint32_t flags)
    {
        // 输入：KSWORD_ARK_MEMORY_EVIDENCE_RISK_* 位集合。
        // 处理：转换为风险标签；0 返回正常。
        // 返回：风险描述文本。
        if (flags == 0U)
        {
            return QStringLiteral("正常");
        }
        QStringList parts;
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_RWX) parts << QStringLiteral("RWX");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_NONMODULE_EXECUTABLE) parts << QStringLiteral("非模块执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_MODULE_NON_TEXT_EXECUTABLE) parts << QStringLiteral("模块非text执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_EXECUTABLE_POOL) parts << QStringLiteral("执行池");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_LARGE_EXECUTABLE) parts << QStringLiteral("大页执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_OWNER_MISSING) parts << QStringLiteral("Owner缺失");
        return parts.join(QStringLiteral(" | "));
    }

    QString hashText(const ksword::ark::KernelMemoryEvidenceEntry& entry)
    {
        // 输入：内核内存证据行。
        // 处理：按 hashAlgorithm/contentHash/section 字段生成 text hash 状态。
        // 返回：无 hash 时返回“无”。
        if (entry.hashAlgorithm == KSWORD_ARK_MEMORY_EVIDENCE_HASH_NONE || entry.contentHash == 0ULL)
        {
            return QStringLiteral("无");
        }
        const QString algorithm = entry.hashAlgorithm == KSWORD_ARK_MEMORY_EVIDENCE_HASH_FNV1A64
            ? QStringLiteral("FNV1A64")
            : QStringLiteral("Hash(%1)").arg(entry.hashAlgorithm);
        const QString section = QString::fromStdString(entry.sectionName).trimmed();
        return QStringLiteral("%1 %2 %3").arg(algorithm, section.isEmpty() ? QStringLiteral(".text?") : section, hex64(entry.contentHash));
    }

    class EvidenceNumericItem final : public QTableWidgetItem
    {
    public:
        EvidenceNumericItem(const QString& displayText, const qulonglong numericValue)
            : QTableWidgetItem(displayText)
        {
            // 输入：显示文本与排序数值。
            // 处理：把数值放入 UserRole，保留 DisplayRole 十六进制格式。
            // 返回：构造函数无返回值。
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(numericValue));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            // 输入：另一表格项。
            // 处理：优先按 UserRole 数值排序。
            // 返回：true 表示当前项应排在前面。
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

    QTableWidgetItem* textItem(const QString& text)
    {
        // 输入：展示文本。
        // 处理：创建只读表格项。
        // 返回：交给 QTableWidget 接管生命周期的 item。
        QTableWidgetItem* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QTableWidgetItem* numericItem(const QString& text, const qulonglong value)
    {
        // 输入：展示文本和排序数值。
        // 处理：创建带数值排序能力的 item。
        // 返回：交给 QTableWidget 接管生命周期的 item。
        return new EvidenceNumericItem(text, value);
    }

    bool entryMatchesFilter(const ksword::ark::KernelMemoryEvidenceEntry& entry, const QString& filter)
    {
        // 输入：证据行和过滤文本。
        // 处理：在 owner/detail/risk/地址/hash 中做大小写不敏感包含匹配。
        // 返回：true 表示该行应显示。
        if (filter.isEmpty())
        {
            return true;
        }
        const QStringList fields{
            hex64(entry.virtualAddress),
            wideToQString(entry.ownerName),
            wideToQString(entry.detail),
            ownerKindText(entry.ownerKind),
            evidenceKindText(entry.evidenceKind),
            riskText(entry.riskFlags),
            hashText(entry)
        };
        for (const QString& field : fields)
        {
            if (field.contains(filter, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    QString detailText(const ksword::ark::KernelMemoryEvidenceEntry& entry)
    {
        // 输入：当前内核内存证据行。
        // 处理：展开全部关键诊断字段，供详情编辑器复制。
        // 返回：多行详情文本。
        QString text;
        text += QStringLiteral("内核内存证据详情\n");
        text += QStringLiteral("Address: %1\n").arg(hex64(entry.virtualAddress));
        text += QStringLiteral("RegionSize: %1 (%2)\n").arg(hex64(entry.regionSize), sizeText(entry.regionSize));
        text += QStringLiteral("EvidenceKind: %1\n").arg(evidenceKindText(entry.evidenceKind));
        text += QStringLiteral("OwnerKind: %1\n").arg(ownerKindText(entry.ownerKind));
        text += QStringLiteral("OwnerName: %1\n").arg(wideToQString(entry.ownerName));
        text += QStringLiteral("OwnerAddress: %1\n").arg(hex64(entry.ownerAddress));
        text += QStringLiteral("ModuleBase: %1\n").arg(hex64(entry.moduleBase));
        text += QStringLiteral("ModuleSize: %1\n").arg(sizeText(entry.moduleSize));
        text += QStringLiteral("PermissionFlags: %1 (0x%2)\n").arg(permissionText(entry.permissionFlags)).arg(entry.permissionFlags, 8, 16, QChar('0'));
        text += QStringLiteral("RiskFlags: %1 (0x%2)\n").arg(riskText(entry.riskFlags)).arg(entry.riskFlags, 8, 16, QChar('0'));
        text += QStringLiteral("BigPoolTag: 0x%1\n").arg(entry.bigPoolTag, 8, 16, QChar('0'));
        text += QStringLiteral("BigPoolFlags: 0x%1\n").arg(entry.bigPoolFlags, 8, 16, QChar('0'));
        text += QStringLiteral("Section: %1 RVA=0x%2 Size=%3\n")
            .arg(QString::fromStdString(entry.sectionName).trimmed())
            .arg(entry.sectionRva, 8, 16, QChar('0'))
            .arg(sizeText(entry.sectionSize));
        text += QStringLiteral("Hash: %1\n").arg(hashText(entry));
        text += QStringLiteral("SampleSize: %1\n").arg(entry.sampleSize);
        if (!entry.sample.empty())
        {
            text += QStringLiteral("Sample: %1\n").arg(bytesToHex(entry.sample));
        }
        text += QStringLiteral("Confidence: %1\n").arg(entry.confidence);
        text += QStringLiteral("LastStatus: 0x%1\n")
            .arg(static_cast<qulonglong>(static_cast<unsigned long>(entry.lastStatus)), 8, 16, QChar('0'));
        text += QStringLiteral("Detail: %1\n").arg(wideToQString(entry.detail));
        return text;
    }

    QString statusStyle(const QString& color)
    {
        // 输入：CSS 颜色。
        // 处理：统一生成状态标签样式。
        // 返回：stylesheet 文本。
        return QStringLiteral("color:%1; font-weight:700;").arg(color);
    }
}

void MemoryDock::initializeKernelMemoryEvidenceTab()
{
    // 输入：无，由 initializeTabs 调用。
    // 处理：创建内核内存证据只读页面。非模块执行范围扫描默认关闭，必须填写范围后显式启用。
    // 返回：无。
    m_tabKernelMemoryEvidence = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabKernelMemoryEvidence);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(8);

    m_kernelMemoryEvidenceRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新证据"), m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceRefreshButton->setToolTip(QStringLiteral("通过 ArkDriverClient 查询 R0 内核内存证据"));
    m_kernelMemoryEvidenceRefreshButton->setStyleSheet(buildBlueButtonStyle());

    m_kernelMemoryEvidenceRiskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceRiskOnlyCheck->setChecked(true);

    m_kernelMemoryEvidenceIncludeNonModuleCheck = new QCheckBox(QStringLiteral("包含非模块执行范围"), m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceIncludeNonModuleCheck->setToolTip(QStringLiteral("需要填写起止地址；不会默认扫描全内核地址空间。"));

    m_kernelMemoryEvidenceFilterEdit = new QLineEdit(m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceFilterEdit->setClearButtonEnabled(true);
    m_kernelMemoryEvidenceFilterEdit->setPlaceholderText(QStringLiteral("过滤 owner / detail / risk / hash"));
    m_kernelMemoryEvidenceFilterEdit->setStyleSheet(buildBlueInputStyle());

    m_kernelMemoryEvidenceStartEdit = new QLineEdit(m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceStartEdit->setPlaceholderText(QStringLiteral("起始VA(可选)"));
    m_kernelMemoryEvidenceStartEdit->setMaximumWidth(150);
    m_kernelMemoryEvidenceStartEdit->setStyleSheet(buildBlueInputStyle());

    m_kernelMemoryEvidenceEndEdit = new QLineEdit(m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceEndEdit->setPlaceholderText(QStringLiteral("结束VA(可选)"));
    m_kernelMemoryEvidenceEndEdit->setMaximumWidth(150);
    m_kernelMemoryEvidenceEndEdit->setStyleSheet(buildBlueInputStyle());

    m_kernelMemoryEvidenceMaxRowsSpin = new QSpinBox(m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceMaxRowsSpin->setRange(16, static_cast<int>(KSWORD_ARK_MEMORY_EVIDENCE_HARD_MAX_ROWS));
    m_kernelMemoryEvidenceMaxRowsSpin->setValue(static_cast<int>(KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_ROWS));
    m_kernelMemoryEvidenceMaxRowsSpin->setToolTip(QStringLiteral("最大返回行数"));

    m_kernelMemoryEvidenceStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_tabKernelMemoryEvidence);
    m_kernelMemoryEvidenceStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelMemoryEvidenceStatusLabel->setStyleSheet(statusStyle(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_kernelMemoryEvidenceRefreshButton);
    toolLayout->addWidget(m_kernelMemoryEvidenceRiskOnlyCheck);
    toolLayout->addWidget(m_kernelMemoryEvidenceIncludeNonModuleCheck);
    toolLayout->addWidget(m_kernelMemoryEvidenceStartEdit);
    toolLayout->addWidget(m_kernelMemoryEvidenceEndEdit);
    toolLayout->addWidget(new QLabel(QStringLiteral("行数:"), m_tabKernelMemoryEvidence));
    toolLayout->addWidget(m_kernelMemoryEvidenceMaxRowsSpin);
    toolLayout->addWidget(m_kernelMemoryEvidenceFilterEdit, 1);
    toolLayout->addWidget(m_kernelMemoryEvidenceStatusLabel);
    tabLayout->addLayout(toolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabKernelMemoryEvidence);
    tabLayout->addWidget(splitter, 1);

    m_kernelMemoryEvidenceTable = new QTableWidget(splitter);
    m_kernelMemoryEvidenceTable->setColumnCount(evidenceColumnIndex(EvidenceColumn::Count));
    m_kernelMemoryEvidenceTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("VA"),
        QStringLiteral("大小"),
        QStringLiteral("类型"),
        QStringLiteral("Owner"),
        QStringLiteral("PTE权限"),
        QStringLiteral("风险"),
        QStringLiteral("text hash/diff"),
        QStringLiteral("Detail")
        });
    m_kernelMemoryEvidenceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_kernelMemoryEvidenceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_kernelMemoryEvidenceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_kernelMemoryEvidenceTable->setAlternatingRowColors(true);
    m_kernelMemoryEvidenceTable->setSortingEnabled(true);
    m_kernelMemoryEvidenceTable->verticalHeader()->setVisible(false);
    m_kernelMemoryEvidenceTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    splitter->addWidget(m_kernelMemoryEvidenceTable);

    m_kernelMemoryEvidenceDetailEditor = new CodeEditorWidget(splitter);
    m_kernelMemoryEvidenceDetailEditor->setReadOnly(true);
    m_kernelMemoryEvidenceDetailEditor->setText(QStringLiteral(
        "请选择一条内核内存证据记录查看详情。\n"
        "说明：text diff 的磁盘对比由 R3 后续阶段完成，本页当前展示 R0 内存 hash/sample 状态。"));
    splitter->addWidget(m_kernelMemoryEvidenceDetailEditor);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_tabKernelMemoryEvidence, QStringLiteral("内核内存证据"));
}

void MemoryDock::refreshKernelMemoryEvidenceAsync()
{
    // 输入：由刷新按钮或全局刷新触发。
    // 处理：验证非模块范围边界，后台调用 ArkDriverClient，回主线程更新缓存和状态。
    // 返回：无。
    if (m_kernelMemoryEvidenceRefreshInProgress.exchange(true))
    {
        return;
    }

    std::uint64_t startAddress = 0;
    std::uint64_t endAddress = 0;
    unsigned long flags =
        KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_LOADED_MODULE_EXECUTABLE |
        KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_BIGPOOL |
        KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_TEXT_SECTION_SAMPLES |
        KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_SUSPECTED_BIGPOOL;

    if (m_kernelMemoryEvidenceIncludeNonModuleCheck != nullptr &&
        m_kernelMemoryEvidenceIncludeNonModuleCheck->isChecked())
    {
        const bool startOk = parseAddressText(m_kernelMemoryEvidenceStartEdit != nullptr ? m_kernelMemoryEvidenceStartEdit->text().trimmed() : QString(), startAddress);
        const bool endOk = parseAddressText(m_kernelMemoryEvidenceEndEdit != nullptr ? m_kernelMemoryEvidenceEndEdit->text().trimmed() : QString(), endAddress);
        if (!startOk || !endOk || startAddress >= endAddress)
        {
            m_kernelMemoryEvidenceRefreshInProgress.store(false);
            if (m_kernelMemoryEvidenceStatusLabel != nullptr)
            {
                m_kernelMemoryEvidenceStatusLabel->setText(QStringLiteral("状态：非模块执行范围需要有效的起始/结束 VA。"));
                m_kernelMemoryEvidenceStatusLabel->setStyleSheet(statusStyle(QStringLiteral("#B23A3A")));
            }
            return;
        }
        flags |= KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_NONMODULE_EXECUTABLE_RANGES;
    }

    const unsigned long maxRows = static_cast<unsigned long>(
        m_kernelMemoryEvidenceMaxRowsSpin != nullptr
            ? m_kernelMemoryEvidenceMaxRowsSpin->value()
            : static_cast<int>(KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_ROWS));

    if (m_kernelMemoryEvidenceRefreshButton != nullptr)
    {
        m_kernelMemoryEvidenceRefreshButton->setEnabled(false);
    }
    if (m_kernelMemoryEvidenceStatusLabel != nullptr)
    {
        m_kernelMemoryEvidenceStatusLabel->setText(QStringLiteral("状态：查询中..."));
        m_kernelMemoryEvidenceStatusLabel->setStyleSheet(statusStyle(KswordTheme::PrimaryBlueHex));
    }

    const std::uint64_t ticket = m_kernelMemoryEvidenceRefreshTicket.fetch_add(1U) + 1U;
    const QPointer<MemoryDock> guardThis(this);

    std::thread([guardThis, ticket, flags, maxRows, startAddress, endAddress]() {
        const ksword::ark::DriverClient client;
        const ksword::ark::KernelMemoryEvidenceResult result = client.queryKernelMemoryEvidence(
            flags,
            maxRows,
            startAddress,
            endAddress,
            KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_BYTES,
            KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_BIGPOOL_ROWS,
            KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_SAMPLE_BYTES);

        QMetaObject::invokeMethod(
            guardThis.data(),
            [guardThis, ticket, result = std::move(result)]() mutable {
                if (guardThis == nullptr || ticket < guardThis->m_kernelMemoryEvidenceRefreshTicket.load())
                {
                    return;
                }

                guardThis->m_kernelMemoryEvidenceRefreshInProgress.store(false);
                if (guardThis->m_kernelMemoryEvidenceRefreshButton != nullptr)
                {
                    guardThis->m_kernelMemoryEvidenceRefreshButton->setEnabled(true);
                }

                if (!result.io.ok)
                {
                    guardThis->m_kernelMemoryEvidenceCache.clear();
                    guardThis->m_kernelMemoryEvidenceVisibleCount = 0U;
                    guardThis->rebuildKernelMemoryEvidenceTable();
                    const QString message = result.unsupported
                        ? QStringLiteral("未集成/驱动过旧，等待 R0 支持")
                        : QStringLiteral("查询失败: %1").arg(QString::fromStdString(result.io.message));
                    if (guardThis->m_kernelMemoryEvidenceStatusLabel != nullptr)
                    {
                        guardThis->m_kernelMemoryEvidenceStatusLabel->setText(QStringLiteral("状态：%1").arg(message));
                        guardThis->m_kernelMemoryEvidenceStatusLabel->setStyleSheet(statusStyle(QStringLiteral("#B23A3A")));
                    }
                    if (guardThis->m_kernelMemoryEvidenceDetailEditor != nullptr)
                    {
                        guardThis->m_kernelMemoryEvidenceDetailEditor->setText(message);
                    }
                    return;
                }

                guardThis->m_kernelMemoryEvidenceCache = result.entries;
                guardThis->rebuildKernelMemoryEvidenceTable();
                guardThis->showKernelMemoryEvidenceDetailByCurrentRow();
                if (guardThis->m_kernelMemoryEvidenceStatusLabel != nullptr)
                {
                    guardThis->m_kernelMemoryEvidenceStatusLabel->setText(
                        QStringLiteral("状态：总计 %1，返回 %2，显示 %3，模块 %4，BigPool seen %5")
                        .arg(result.totalRows)
                        .arg(result.returnedRows)
                        .arg(guardThis->m_kernelMemoryEvidenceVisibleCount)
                        .arg(result.moduleCount)
                        .arg(result.bigPoolRowsSeen));
                    guardThis->m_kernelMemoryEvidenceStatusLabel->setStyleSheet(statusStyle(QStringLiteral("#2F7D32")));
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void MemoryDock::rebuildKernelMemoryEvidenceTable()
{
    // 输入：无，读取 m_kernelMemoryEvidenceCache 和过滤控件。
    // 处理：把缓存投影为表格，风险过滤仅在 R3 本地执行。
    // 返回：无。
    if (m_kernelMemoryEvidenceTable == nullptr)
    {
        return;
    }

    const QString filter = m_kernelMemoryEvidenceFilterEdit != nullptr
        ? m_kernelMemoryEvidenceFilterEdit->text().trimmed()
        : QString();
    const bool riskOnly = m_kernelMemoryEvidenceRiskOnlyCheck != nullptr && m_kernelMemoryEvidenceRiskOnlyCheck->isChecked();

    std::vector<std::size_t> visibleIndexes;
    visibleIndexes.reserve(m_kernelMemoryEvidenceCache.size());
    for (std::size_t index = 0; index < m_kernelMemoryEvidenceCache.size(); ++index)
    {
        const auto& entry = m_kernelMemoryEvidenceCache[index];
        if (riskOnly && entry.riskFlags == 0U)
        {
            continue;
        }
        if (!entryMatchesFilter(entry, filter))
        {
            continue;
        }
        visibleIndexes.push_back(index);
    }

    m_kernelMemoryEvidenceVisibleCount = visibleIndexes.size();
    const QSignalBlocker blocker(m_kernelMemoryEvidenceTable);
    m_kernelMemoryEvidenceTable->setSortingEnabled(false);
    m_kernelMemoryEvidenceTable->setRowCount(static_cast<int>(visibleIndexes.size()));
    for (int row = 0; row < static_cast<int>(visibleIndexes.size()); ++row)
    {
        const std::size_t cacheIndex = visibleIndexes[static_cast<std::size_t>(row)];
        const auto& entry = m_kernelMemoryEvidenceCache[cacheIndex];
        QTableWidgetItem* addressItem = numericItem(hex64(entry.virtualAddress), entry.virtualAddress);
        addressItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Address), addressItem);
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Size), numericItem(sizeText(entry.regionSize), entry.regionSize));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Kind), textItem(evidenceKindText(entry.evidenceKind)));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Owner),
            textItem(QStringLiteral("%1 %2").arg(ownerKindText(entry.ownerKind), wideToQString(entry.ownerName))));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Permissions), textItem(permissionText(entry.permissionFlags)));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Risk), textItem(riskText(entry.riskFlags)));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::TextHash), textItem(hashText(entry)));
        m_kernelMemoryEvidenceTable->setItem(row, evidenceColumnIndex(EvidenceColumn::Detail), textItem(wideToQString(entry.detail)));
    }
    if (m_kernelMemoryEvidenceTable->rowCount() > 0 && m_kernelMemoryEvidenceTable->currentRow() < 0)
    {
        m_kernelMemoryEvidenceTable->setCurrentCell(0, evidenceColumnIndex(EvidenceColumn::Address));
    }
    m_kernelMemoryEvidenceTable->setSortingEnabled(true);
    ks::ui::RequestTableColumnAutoFit(m_kernelMemoryEvidenceTable);
}

void MemoryDock::showKernelMemoryEvidenceDetailByCurrentRow()
{
    // 输入：无，读取当前表格行。
    // 处理：通过缓存索引写入详情编辑器。
    // 返回：无。
    if (m_kernelMemoryEvidenceDetailEditor == nullptr || m_kernelMemoryEvidenceTable == nullptr)
    {
        return;
    }
    const int row = m_kernelMemoryEvidenceTable->currentRow();
    if (row < 0)
    {
        m_kernelMemoryEvidenceDetailEditor->setText(QStringLiteral("请选择一条内核内存证据记录查看详情。"));
        return;
    }
    const QTableWidgetItem* addressItem = m_kernelMemoryEvidenceTable->item(row, evidenceColumnIndex(EvidenceColumn::Address));
    if (addressItem == nullptr)
    {
        return;
    }
    bool ok = false;
    const qulonglong cacheIndex = addressItem->data(Qt::UserRole + 1).toULongLong(&ok);
    if (!ok || cacheIndex >= static_cast<qulonglong>(m_kernelMemoryEvidenceCache.size()))
    {
        return;
    }
    m_kernelMemoryEvidenceDetailEditor->setText(detailText(m_kernelMemoryEvidenceCache[static_cast<std::size_t>(cacheIndex)]));
}
