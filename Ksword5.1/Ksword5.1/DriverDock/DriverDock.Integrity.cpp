#include "DriverDock.Internal.h"
#include "../UI/TableColumnAutoFit.h"

#include <QPointer>
#include <QRunnable>

using namespace ksword::driver_dock_internal;

namespace
{
    enum class IntegrityColumn : int
    {
        Class = 0,
        Object,
        Target,
        Owner,
        Cpu,
        Risk,
        Confidence,
        Detail,
        Count
    };

    int integrityColumnIndex(const IntegrityColumn column)
    {
        // 输入：完整性表列枚举。
        // 处理：转换为 QTableWidget 列号。
        // 返回：列索引。
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        // 输入：64 位地址或掩码。
        // 处理：格式化为固定宽度十六进制。
        // 返回：0x 前缀大写文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString classText(const std::uint32_t evidenceClass)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_CLASS_*。
        // 处理：映射为 DriverDock 页面分组文本。
        // 返回：证据类型名称。
        switch (evidenceClass)
        {
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MODULE_VIEW: return QStringLiteral("ModuleView");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_PS_LOADED_MODULES: return QStringLiteral("PsLoadedModules");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DRIVER_OBJECT: return QStringLiteral("DriverObject");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DRIVER_SECTION: return QStringLiteral("DriverSection");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MAJOR_FUNCTION: return QStringLiteral("MajorFunction");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_FAST_IO: return QStringLiteral("FastIo");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DEVICE_CHAIN: return QStringLiteral("DeviceChain");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_SERVICE: return QStringLiteral("Service");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_CPU_CONTROL: return QStringLiteral("CPU");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE: return QStringLiteral("Descriptor");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY: return QStringLiteral("MSR");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER: return QStringLiteral("IDT");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_OPTIONAL_GLOBAL: return QStringLiteral("OptionalGlobal");
        default: return QStringLiteral("Class(%1)").arg(evidenceClass);
        }
    }

    QString riskText(const std::uint32_t flags)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_RISK_* 位集合。
        // 处理：转换为紧凑风险标签。
        // 返回：无风险返回“正常”。
        if (flags == 0U)
        {
            return QStringLiteral("正常");
        }
        QStringList parts;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_UNAVAILABLE) parts << QStringLiteral("不可用");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_QUERY_FAILED) parts << QStringLiteral("查询失败");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED) parts << QStringLiteral("模块未解析");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH) parts << QStringLiteral("Owner不匹配");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE) parts << QStringLiteral("外跳");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_SECTION_MISMATCH) parts << QStringLiteral("Section不匹配");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_SERVICE_MISSING) parts << QStringLiteral("服务缺失");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_EMPTY_UNLOAD) parts << QStringLiteral("Unload为空");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DEVICE_LOOP) parts << QStringLiteral("Device环");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_ATTACHED_LOOP) parts << QStringLiteral("Attached环");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH) parts << QStringLiteral("跨驱动挂接");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_NULL_POINTER) parts << QStringLiteral("空指针");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER) parts << QStringLiteral("IDT外部Owner");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED) parts << QStringLiteral("WP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED) parts << QStringLiteral("NXE关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED) parts << QStringLiteral("SMEP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED) parts << QStringLiteral("SMAP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID) parts << QStringLiteral("描述符异常");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DYNDATA_UNAVAILABLE) parts << QStringLiteral("DynData缺失");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_TRUNCATED) parts << QStringLiteral("截断");
        return parts.join(QStringLiteral(" | "));
    }

    class NumericItem final : public QTableWidgetItem
    {
    public:
        NumericItem(const QString& text, const qulonglong value)
            : QTableWidgetItem(text)
        {
            // 输入：显示文本和排序数值。
            // 处理：UserRole 保存数值。
            // 返回：构造函数无返回。
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(value));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            // 输入：另一 item。
            // 处理：优先按 UserRole 数值排序。
            // 返回：排序比较结果。
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

    QTableWidgetItem* textItem(const QString& value)
    {
        // 输入：展示文本。
        // 处理：创建只读 item。
        // 返回：交给表格接管生命周期。
        QTableWidgetItem* item = new QTableWidgetItem(value);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QTableWidgetItem* numericItem(const QString& text, const qulonglong value)
    {
        // 输入：展示文本和排序数值。
        // 处理：创建 NumericItem。
        // 返回：交给表格接管生命周期。
        return new NumericItem(text, value);
    }

    bool parseAddressText(const QString& text, std::uint64_t& valueOut)
    {
        // 输入：用户输入的十六进制或十进制地址。
        // 处理：支持 0x 前缀；空文本解析为 0。
        // 返回：true 表示解析成功。
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
        {
            valueOut = 0;
            return true;
        }
        bool ok = false;
        valueOut = trimmed.toULongLong(&ok, trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
        return ok;
    }

    QString detailText(const ksword::ark::DriverIntegrityEvidenceEntry& row)
    {
        // 输入：完整性证据行。
        // 处理：展开所有关键字段。
        // 返回：详情文本。
        QString text;
        text += QStringLiteral("驱动完整性证据详情\n");
        text += QStringLiteral("Class: %1 (%2)\n").arg(classText(row.evidenceClass)).arg(row.evidenceClass);
        text += QStringLiteral("RiskFlags: %1 (0x%2)\n").arg(riskText(row.riskFlags)).arg(row.riskFlags, 8, 16, QChar('0'));
        text += QStringLiteral("SourceMask: 0x%1\n").arg(row.sourceMask, 8, 16, QChar('0'));
        text += QStringLiteral("Confidence: %1\n").arg(row.confidence);
        text += QStringLiteral("ObjectAddress: %1\n").arg(hex64(row.objectAddress));
        text += QStringLiteral("TargetAddress: %1\n").arg(hex64(row.targetAddress));
        text += QStringLiteral("OwnerModule: %1\n").arg(QString::fromStdWString(row.ownerModule));
        text += QStringLiteral("OwnerModuleBase: %1\n").arg(hex64(row.ownerModuleBase));
        text += QStringLiteral("OwnerModuleSize: %1\n").arg(row.ownerModuleSize);
        text += QStringLiteral("CPU: group=%1 cpu=%2 vector=%3\n")
            .arg(row.processorGroup)
            .arg(row.processorNumber)
            .arg(row.vector);
        text += QStringLiteral("Detail: %1\n").arg(QString::fromStdWString(row.detail));
        return text;
    }
}

void DriverDock::initializeIntegrityTab()
{
    // 输入：无，由 initializeUi 调用。
    // 处理：创建只读驱动完整性页。
    // 返回：无。
    m_integrityPage = new QWidget(m_tabWidget);
    m_integrityLayout = new QVBoxLayout(m_integrityPage);
    m_integrityLayout->setContentsMargins(4, 4, 4, 4);
    m_integrityLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(6);

    m_integrityDriverNameEdit = new QLineEdit(m_integrityPage);
    m_integrityDriverNameEdit->setPlaceholderText(QStringLiteral("\\Driver\\Name（可选）"));
    m_integrityModuleBaseEdit = new QLineEdit(m_integrityPage);
    m_integrityModuleBaseEdit->setPlaceholderText(QStringLiteral("模块基址（可选）"));
    m_integrityModuleBaseEdit->setMaximumWidth(150);

    m_integrityFillFromSelectionButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_details.svg")), QString(), m_integrityPage);
    m_integrityFillFromSelectionButton->setFixedWidth(34);
    m_integrityFillFromSelectionButton->setToolTip(QStringLiteral("从当前服务选择填充 DriverObject 名称"));

    m_integrityRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_integrityPage);
    m_integrityRefreshButton->setFixedWidth(34);
    m_integrityRefreshButton->setToolTip(QStringLiteral("查询 DriverObject/LDR/FastIo/CPU 完整性证据"));

    m_integrityCpuOnlyButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_threads.svg")), QString(), m_integrityPage);
    m_integrityCpuOnlyButton->setFixedWidth(34);
    m_integrityCpuOnlyButton->setToolTip(QStringLiteral("仅查询 CPU entry / IDT / MSR 证据"));

    m_integrityRiskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), m_integrityPage);
    m_integrityRiskOnlyCheck->setChecked(false);

    m_integrityMaxRowsSpin = new QSpinBox(m_integrityPage);
    m_integrityMaxRowsSpin->setRange(64, static_cast<int>(KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS));
    m_integrityMaxRowsSpin->setValue(1024);
    m_integrityMaxRowsSpin->setToolTip(QStringLiteral("最大返回证据行数"));

    m_integrityStatusLabel = new QLabel(QStringLiteral("状态：等待查询"), m_integrityPage);
    m_integrityStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    toolLayout->addWidget(new QLabel(QStringLiteral("DriverObject:"), m_integrityPage));
    toolLayout->addWidget(m_integrityDriverNameEdit, 1);
    toolLayout->addWidget(m_integrityModuleBaseEdit);
    toolLayout->addWidget(m_integrityFillFromSelectionButton);
    toolLayout->addWidget(m_integrityRefreshButton);
    toolLayout->addWidget(m_integrityCpuOnlyButton);
    toolLayout->addWidget(m_integrityRiskOnlyCheck);
    toolLayout->addWidget(new QLabel(QStringLiteral("行数:"), m_integrityPage));
    toolLayout->addWidget(m_integrityMaxRowsSpin);
    toolLayout->addWidget(m_integrityStatusLabel, 1);
    m_integrityLayout->addLayout(toolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_integrityPage);
    m_integrityLayout->addWidget(splitter, 1);

    m_integrityTable = new QTableWidget(splitter);
    m_integrityTable->setColumnCount(integrityColumnIndex(IntegrityColumn::Count));
    m_integrityTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("对象"),
        QStringLiteral("目标"),
        QStringLiteral("Owner"),
        QStringLiteral("CPU/Vector"),
        QStringLiteral("风险"),
        QStringLiteral("置信度"),
        QStringLiteral("Detail")
        });
    m_integrityTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_integrityTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_integrityTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_integrityTable->setAlternatingRowColors(true);
    m_integrityTable->setSortingEnabled(true);
    m_integrityTable->verticalHeader()->setVisible(false);
    m_integrityTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_integrityTable->horizontalHeader()->setSectionResizeMode(integrityColumnIndex(IntegrityColumn::Detail), QHeaderView::Stretch);
    splitter->addWidget(m_integrityTable);

    m_integrityDetailEdit = new QPlainTextEdit(splitter);
    m_integrityDetailEdit->setReadOnly(true);
    m_integrityDetailEdit->setPlaceholderText(QStringLiteral("选择一条完整性证据查看 DriverObject / MajorFunction / FastIo / LDR / CPU entry 详情。"));
    splitter->addWidget(m_integrityDetailEdit);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_integrityPage, QIcon(QStringLiteral(":/Icon/process_critical.svg")), QStringLiteral("驱动完整性"));
}

void DriverDock::refreshDriverIntegrityAsync(const bool cpuOnly)
{
    // 输入：cpuOnly 控制查询范围。
    // 处理：后台调用 ArkDriverClient，主线程回填完整性证据。
    // 返回：无。
    if (m_integrityQuerying)
    {
        return;
    }
    std::uint64_t moduleBase = 0;
    if (!parseAddressText(m_integrityModuleBaseEdit != nullptr ? m_integrityModuleBaseEdit->text() : QString(), moduleBase))
    {
        if (m_integrityStatusLabel != nullptr)
        {
            m_integrityStatusLabel->setText(QStringLiteral("状态：模块基址解析失败"));
        }
        return;
    }

    const std::wstring driverName = m_integrityDriverNameEdit != nullptr
        ? m_integrityDriverNameEdit->text().trimmed().toStdWString()
        : std::wstring();
    const unsigned long maxRows = static_cast<unsigned long>(
        m_integrityMaxRowsSpin != nullptr ? m_integrityMaxRowsSpin->value() : 1024);

    m_integrityQuerying = true;
    const std::uint64_t ticket = ++m_integrityQueryTicket;
    if (m_integrityRefreshButton != nullptr) m_integrityRefreshButton->setEnabled(false);
    if (m_integrityCpuOnlyButton != nullptr) m_integrityCpuOnlyButton->setEnabled(false);
    if (m_integrityStatusLabel != nullptr) m_integrityStatusLabel->setText(QStringLiteral("状态：查询中..."));

    QPointer<DriverDock> guardThis(this);
    QRunnable* task = QRunnable::create([guardThis, ticket, driverName, moduleBase, maxRows, cpuOnly]() {
        const ksword::ark::DriverClient client;
        ksword::ark::DriverIntegrityResult result = cpuOnly
            ? client.queryKernelCpuIntegrity(
                KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU | KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES,
                maxRows,
                KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS)
            : client.queryDriverIntegrity(
                driverName,
                moduleBase,
                KSWORD_ARK_DRIVER_INTEGRITY_FLAG_DEFAULT | KSWORD_ARK_DRIVER_INTEGRITY_FLAG_OPTIONAL_GLOBALS,
                maxRows,
                KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS);

        QMetaObject::invokeMethod(guardThis.data(), [guardThis, ticket, result = std::move(result)]() mutable {
            if (guardThis == nullptr || guardThis->m_integrityQueryTicket != ticket)
            {
                return;
            }
            guardThis->m_integrityQuerying = false;
            if (guardThis->m_integrityRefreshButton != nullptr) guardThis->m_integrityRefreshButton->setEnabled(true);
            if (guardThis->m_integrityCpuOnlyButton != nullptr) guardThis->m_integrityCpuOnlyButton->setEnabled(true);

            guardThis->m_lastDriverIntegrityResult = result;
            guardThis->m_driverIntegrityCache = result.entries;
            guardThis->rebuildDriverIntegrityTable();
            guardThis->showSelectedDriverIntegrityDetail();

            if (guardThis->m_integrityStatusLabel != nullptr)
            {
                if (!result.io.ok)
                {
                    guardThis->m_integrityStatusLabel->setText(result.unsupported
                        ? QStringLiteral("状态：未集成/驱动过旧，等待 R0 支持")
                        : QStringLiteral("状态：查询失败 %1").arg(QString::fromStdString(result.io.message)));
                    guardThis->m_integrityStatusLabel->setStyleSheet(QStringLiteral("color:#B23A3A; font-weight:700;"));
                }
                else
                {
                    guardThis->m_integrityStatusLabel->setText(
                        QStringLiteral("状态：返回 %1/%2，CPU %3，模块 %4")
                        .arg(result.entries.size())
                        .arg(result.totalCount)
                        .arg(result.cpuCount)
                        .arg(result.moduleCount));
                    guardThis->m_integrityStatusLabel->setStyleSheet(QStringLiteral("color:#2F7D32; font-weight:700;"));
                }
            }
        }, Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void DriverDock::rebuildDriverIntegrityTable()
{
    // 输入：无，读取 m_driverIntegrityCache 和过滤控件。
    // 处理：只重绘表格，不访问驱动。
    // 返回：无。
    if (m_integrityTable == nullptr)
    {
        return;
    }
    const bool riskOnly = m_integrityRiskOnlyCheck != nullptr && m_integrityRiskOnlyCheck->isChecked();
    std::vector<std::size_t> visibleIndexes;
    for (std::size_t index = 0; index < m_driverIntegrityCache.size(); ++index)
    {
        if (riskOnly && m_driverIntegrityCache[index].riskFlags == 0U)
        {
            continue;
        }
        visibleIndexes.push_back(index);
    }

    const QSignalBlocker blocker(m_integrityTable);
    m_integrityTable->setSortingEnabled(false);
    m_integrityTable->setRowCount(static_cast<int>(visibleIndexes.size()));
    for (int rowIndex = 0; rowIndex < static_cast<int>(visibleIndexes.size()); ++rowIndex)
    {
        const std::size_t cacheIndex = visibleIndexes[static_cast<std::size_t>(rowIndex)];
        const auto& row = m_driverIntegrityCache[cacheIndex];
        QTableWidgetItem* classItem = textItem(classText(row.evidenceClass));
        classItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Class), classItem);
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Object), numericItem(hex64(row.objectAddress), row.objectAddress));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Target), numericItem(hex64(row.targetAddress), row.targetAddress));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Owner),
            textItem(QStringLiteral("%1 %2").arg(QString::fromStdWString(row.ownerModule), hex64(row.ownerModuleBase))));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Cpu),
            textItem(QStringLiteral("G%1 CPU%2 V%3").arg(row.processorGroup).arg(row.processorNumber).arg(row.vector)));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Risk), textItem(riskText(row.riskFlags)));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Confidence), numericItem(QString::number(row.confidence), row.confidence));
        m_integrityTable->setItem(rowIndex, integrityColumnIndex(IntegrityColumn::Detail), textItem(QString::fromStdWString(row.detail)));
    }
    if (m_integrityTable->rowCount() > 0 && m_integrityTable->currentRow() < 0)
    {
        m_integrityTable->setCurrentCell(0, integrityColumnIndex(IntegrityColumn::Class));
    }
    m_integrityTable->setSortingEnabled(true);
    ks::ui::RequestTableColumnAutoFit(m_integrityTable);
}

void DriverDock::showSelectedDriverIntegrityDetail()
{
    // 输入：无，读取当前完整性表选中行。
    // 处理：通过缓存索引展开详情。
    // 返回：无。
    if (m_integrityDetailEdit == nullptr || m_integrityTable == nullptr)
    {
        return;
    }
    const int currentRow = m_integrityTable->currentRow();
    if (currentRow < 0)
    {
        m_integrityDetailEdit->setPlainText(QStringLiteral("请选择一条驱动完整性证据。"));
        return;
    }
    const QTableWidgetItem* classItem = m_integrityTable->item(currentRow, integrityColumnIndex(IntegrityColumn::Class));
    bool ok = false;
    const qulonglong cacheIndex = classItem != nullptr ? classItem->data(Qt::UserRole + 1).toULongLong(&ok) : 0ULL;
    if (!ok || cacheIndex >= static_cast<qulonglong>(m_driverIntegrityCache.size()))
    {
        return;
    }
    m_integrityDetailEdit->setPlainText(detailText(m_driverIntegrityCache[static_cast<std::size_t>(cacheIndex)]));
}
