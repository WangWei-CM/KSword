#include "HardwareR0EvidencePage.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../theme.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/TableColumnAutoFit.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <string>

namespace
{
    // R0EvidenceColumn 作用：定义硬件 R0 证据表格列；返回值由 columnIndex 转成 int。
    enum class R0EvidenceColumn : int
    {
        Class = 0,
        Cpu,
        Object,
        Target,
        Owner,
        Risk,
        Confidence,
        Detail,
        Count
    };

    int columnIndex(const R0EvidenceColumn column)
    {
        // 输入：R0EvidenceColumn 枚举。
        // 处理：转换为 QTableWidget 所需列号。
        // 返回：列索引。
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        // 输入：64 位地址、基址或掩码。
        // 处理：统一格式化为 0x + 16 位大写十六进制。
        // 返回：地址文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString hex32(const std::uint32_t value)
    {
        // 输入：32 位 flags/status 值。
        // 处理：统一格式化为 0x + 8 位大写十六进制。
        // 返回：十六进制文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(value), 8, 16, QChar('0'))
            .toUpper();
    }

    QString ntStatusText(const long statusValue)
    {
        // 输入：NTSTATUS 风格整数。
        // 处理：按无符号 32 位固定宽度展示，便于和 WinDbg 对照。
        // 返回：0xXXXXXXXX 文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(statusValue), 8, 16, QChar('0'))
            .toUpper();
    }

    QString wideToQString(const std::wstring& value)
    {
        // 输入：ArkDriverClient 返回的 std::wstring。
        // 处理：空值保持为空，非空转 QString。
        // 返回：Qt 文本。
        return value.empty() ? QString() : QString::fromStdWString(value);
    }

    QString buildBlueButtonStyle()
    {
        // 输入：无。
        // 处理：按全局主题构造蓝色按钮样式。
        // 返回：stylesheet 文本。
        return QStringLiteral(
            "QPushButton{border:1px solid %1;border-radius:4px;padding:4px 10px;color:%2;background:transparent;}"
            "QPushButton:hover{background:%3;}"
            "QPushButton:pressed{background:%1;color:#FFFFFF;}"
            "QPushButton:disabled{color:%4;border-color:%4;background:transparent;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::IsDarkModeEnabled() ? KswordTheme::PrimaryBlueSubtleDarkHex : KswordTheme::PrimaryBlueSubtleLightHex)
            .arg(KswordTheme::TextDisabledColor().name());
    }

    QString buildBlueInputStyle()
    {
        // 输入：无。
        // 处理：按全局主题构造输入框样式。
        // 返回：stylesheet 文本。
        return QStringLiteral(
            "QLineEdit{border:1px solid %1;border-radius:4px;padding:4px 6px;color:%2;background:%3;}"
            "QLineEdit:focus{border:1px solid %4;}")
            .arg(KswordTheme::BorderColorHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceColorHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString buildHeaderStyle()
    {
        // 输入：无。
        // 处理：构造表头蓝色强调样式。
        // 返回：stylesheet 文本。
        return QStringLiteral("QHeaderView::section{color:%1;font-weight:700;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString statusStyle(const QString& colorText)
    {
        // 输入：颜色文本。
        // 处理：生成状态标签 CSS。
        // 返回：stylesheet 文本。
        return QStringLiteral("color:%1; font-weight:700;").arg(colorText);
    }

    QString classText(const std::uint32_t evidenceClass)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_CLASS_*。
        // 处理：映射为硬件页可读分类。
        // 返回：分类文本。
        switch (evidenceClass)
        {
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_CPU_CONTROL: return QStringLiteral("CPU控制寄存器");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE: return QStringLiteral("IDTR/GDTR");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY: return QStringLiteral("MSR入口");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER: return QStringLiteral("IDT向量");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MODULE_VIEW: return QStringLiteral("模块视图");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_PS_LOADED_MODULES: return QStringLiteral("PsLoadedModules");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DRIVER_OBJECT: return QStringLiteral("DriverObject");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DRIVER_SECTION: return QStringLiteral("DriverSection");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MAJOR_FUNCTION: return QStringLiteral("MajorFunction");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_FAST_IO: return QStringLiteral("FastIo");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DEVICE_CHAIN: return QStringLiteral("DeviceChain");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_SERVICE: return QStringLiteral("Service");
        case KSWORD_ARK_DRIVER_INTEGRITY_CLASS_OPTIONAL_GLOBAL: return QStringLiteral("OptionalGlobal");
        default: return QStringLiteral("Class(%1)").arg(evidenceClass);
        }
    }

    QString sourceMaskText(const std::uint32_t sourceMask)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_* 位集合。
        // 处理：映射为采集来源列表。
        // 返回：来源文本。
        if (sourceMask == 0U)
        {
            return QStringLiteral("无");
        }
        QStringList parts;
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SYSTEM_MODULE) parts << QStringLiteral("SystemModule");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_AUXKLIB) parts << QStringLiteral("AuxKlib");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_PS_LOADED_MODULES) parts << QStringLiteral("PsLoadedModules");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_DRIVER_OBJECT) parts << QStringLiteral("DriverObject");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_DRIVER_SECTION) parts << QStringLiteral("DriverSection");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SERVICE_REGISTRY) parts << QStringLiteral("ServiceRegistry");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_CPU_REGISTER) parts << QStringLiteral("CPURegister");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_IDT) parts << QStringLiteral("IDT");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_GDT) parts << QStringLiteral("GDT");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_MSR) parts << QStringLiteral("MSR");
        if (sourceMask & KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_DYNDATA) parts << QStringLiteral("DynData");
        return parts.join(QStringLiteral(" | "));
    }

    QString capabilityText(const ksword::ark::DriverCapabilitiesQueryResult& result)
    {
        // 输入：ArkDriverClient 驱动能力查询结果。
        // 处理：提取 protocol、feature、DynData 和错误摘要，用于状态栏和详情区。
        // 返回：单行可读文本。
        if (!result.io.ok)
        {
            return QStringLiteral("DriverCapabilities: 不可用");
        }
        return QStringLiteral("DriverCapabilities: version=%1 features=%2/%3 dynData=0x%4")
            .arg(result.driverProtocolVersion)
            .arg(result.returnedFeatureCount)
            .arg(result.totalFeatureCount)
            .arg(static_cast<qulonglong>(result.dynDataCapabilityMask), 0, 16)
            .toUpper();
    }

    QString dynDataText(const ksword::ark::DynDataCapabilitiesResult& result)
    {
        // 输入：ArkDriverClient DynData capability 查询结果。
        // 处理：提取 statusFlags 与 capabilityMask。
        // 返回：单行可读文本。
        if (!result.io.ok)
        {
            return QStringLiteral("DynDataCapabilities: 不可用");
        }
        return QStringLiteral("DynDataCapabilities: status=0x%1 capability=0x%2")
            .arg(result.statusFlags, 8, 16, QChar('0'))
            .arg(static_cast<qulonglong>(result.capabilityMask), 0, 16)
            .toUpper();
    }

    QString riskText(const std::uint32_t flags)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_RISK_* 位集合。
        // 处理：转换为硬件页关注的风险标签。
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

    QString queryStatusText(const std::uint32_t statusValue)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_STATUS_*。
        // 处理：转换为状态摘要。
        // 返回：状态文本。
        switch (statusValue)
        {
        case KSWORD_ARK_DRIVER_INTEGRITY_STATUS_OK: return QStringLiteral("OK");
        case KSWORD_ARK_DRIVER_INTEGRITY_STATUS_PARTIAL: return QStringLiteral("Partial");
        case KSWORD_ARK_DRIVER_INTEGRITY_STATUS_NOT_FOUND: return QStringLiteral("Not found");
        case KSWORD_ARK_DRIVER_INTEGRITY_STATUS_BUFFER_TOO_SMALL: return QStringLiteral("Buffer too small");
        case KSWORD_ARK_DRIVER_INTEGRITY_STATUS_QUERY_FAILED: return QStringLiteral("Query failed");
        case KSWORD_ARK_DRIVER_INTEGRITY_STATUS_UNAVAILABLE:
        default: return QStringLiteral("Unavailable");
        }
    }

    QString cpuVectorText(const ksword::ark::DriverIntegrityEvidenceEntry& row)
    {
        // 输入：R0 evidence 行。
        // 处理：组合 processor group、processor number 和可选 IDT vector。
        // 返回：CPU/Vector 文本。
        if (row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER)
        {
            return QStringLiteral("G%1 CPU%2 V%3")
                .arg(row.processorGroup)
                .arg(row.processorNumber)
                .arg(row.vector);
        }
        return QStringLiteral("G%1 CPU%2")
            .arg(row.processorGroup)
            .arg(row.processorNumber);
    }

    bool isHardwareCpuEvidenceClass(const std::uint32_t evidenceClass)
    {
        // 输入：完整性证据 class。
        // 处理：判断是否属于硬件页第一阶段关注的 CPU/MSR/描述符/IDT 证据。
        // 返回：true 表示纳入默认展示。
        return evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_CPU_CONTROL ||
            evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE ||
            evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY ||
            evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER;
    }

    bool entryMatchesFilter(
        const ksword::ark::DriverIntegrityEvidenceEntry& row,
        const QString& filterText)
    {
        // 输入：证据行和用户过滤文本。
        // 处理：在 class/cpu/地址/owner/risk/detail 中做大小写不敏感匹配。
        // 返回：true 表示应该显示。
        if (filterText.isEmpty())
        {
            return true;
        }
        const QStringList fields{
            classText(row.evidenceClass),
            cpuVectorText(row),
            hex64(row.objectAddress),
            hex64(row.targetAddress),
            wideToQString(row.ownerModule),
            riskText(row.riskFlags),
            sourceMaskText(row.sourceMask),
            wideToQString(row.detail)
        };
        for (const QString& field : fields)
        {
            if (field.contains(filterText, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    QString detailText(const ksword::ark::DriverIntegrityEvidenceEntry& row)
    {
        // 输入：当前 R0 evidence 行。
        // 处理：展开所有关键协议字段，便于复制到调试记录。
        // 返回：多行详情文本。
        QString text;
        text += QStringLiteral("R0 硬件 / CPU 入口证据详情\n");
        text += QStringLiteral("Class: %1 (%2)\n").arg(classText(row.evidenceClass)).arg(row.evidenceClass);
        text += QStringLiteral("CPU: Group=%1 Processor=%2 Vector=%3\n")
            .arg(row.processorGroup)
            .arg(row.processorNumber)
            .arg(row.vector);
        text += QStringLiteral("ObjectAddress: %1\n").arg(hex64(row.objectAddress));
        text += QStringLiteral("TargetAddress: %1\n").arg(hex64(row.targetAddress));
        text += QStringLiteral("OwnerModule: %1\n").arg(wideToQString(row.ownerModule));
        text += QStringLiteral("OwnerModuleBase: %1\n").arg(hex64(row.ownerModuleBase));
        text += QStringLiteral("OwnerModuleSize: %1 (%2)\n")
            .arg(hex32(row.ownerModuleSize))
            .arg(row.ownerModuleSize);
        text += QStringLiteral("SourceMask: %1 (%2)\n").arg(sourceMaskText(row.sourceMask), hex32(row.sourceMask));
        text += QStringLiteral("RiskFlags: %1 (%2)\n").arg(riskText(row.riskFlags), hex32(row.riskFlags));
        text += QStringLiteral("Confidence: %1\n").arg(row.confidence);
        text += QStringLiteral("Detail: %1\n").arg(wideToQString(row.detail));
        return text;
    }

    class NumericItem final : public QTableWidgetItem
    {
    public:
        NumericItem(const QString& displayText, const qulonglong numericValue)
            : QTableWidgetItem(displayText)
        {
            // 输入：显示文本与排序数值。
            // 处理：UserRole 保存数值，DisplayRole 保留原格式。
            // 返回：构造函数无返回值。
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(numericValue));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            // 输入：另一表格项。
            // 处理：优先按 UserRole 数值排序。
            // 返回：比较结果。
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
        // 输入：显示文本。
        // 处理：创建只读表格项。
        // 返回：由 QTableWidget 接管生命周期。
        QTableWidgetItem* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QTableWidgetItem* numericItem(const QString& text, const qulonglong value)
    {
        // 输入：显示文本和排序数值。
        // 处理：创建 NumericItem。
        // 返回：由 QTableWidget 接管生命周期。
        return new NumericItem(text, value);
    }

    struct R0EvidenceQueryBundle
    {
        // capabilityResult：驱动统一能力查询结果，仅用于页头诊断。
        ksword::ark::DriverCapabilitiesQueryResult capabilityResult;
        // dynDataResult：DynData capability 查询结果，仅用于说明 IDT owner 归属质量。
        ksword::ark::DynDataCapabilitiesResult dynDataResult;
        // integrityResult：CPU/MSR/IDT/GDT evidence 主结果。
        ksword::ark::DriverIntegrityResult integrityResult;
    };
}

HardwareR0EvidencePage::HardwareR0EvidencePage(QWidget* parent)
    : QWidget(parent)
{
    // 构造流程：先建 UI，再连接信号，最后延迟启动只读 R0 查询，避免阻塞 Dock 创建链路。
    initializeUi();
    initializeConnections();
    QTimer::singleShot(0, this, [this]()
    {
        refreshEvidenceAsync(false);
    });
}

void HardwareR0EvidencePage::initializeUi()
{
    // 根布局只承载工具栏和上下分割器；真实内容控件均挂在 Qt 父子树上自动释放。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(8);

    m_refreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新R0证据"), this);
    m_refreshButton->setToolTip(QStringLiteral("通过 ArkDriverClient 查询 R0 CPU/MSR/IDT/GDT 证据；不执行任何写操作。"));
    m_refreshButton->setStyleSheet(buildBlueButtonStyle());

    m_riskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), this);
    m_riskOnlyCheck->setToolTip(QStringLiteral("只显示 R0 返回 riskFlags 非零的 CPU/IDT/MSR/描述符证据。"));

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(QStringLiteral("过滤 class / CPU / owner / risk / detail"));
    m_filterEdit->setStyleSheet(buildBlueInputStyle());

    m_maxRowsSpin = new QSpinBox(this);
    m_maxRowsSpin->setRange(64, 65536);
    m_maxRowsSpin->setValue(static_cast<int>(KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS));
    m_maxRowsSpin->setToolTip(QStringLiteral("R0 evidence 最大返回行数。"));

    m_idtVectorsSpin = new QSpinBox(this);
    m_idtVectorsSpin->setRange(0, static_cast<int>(KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS));
    m_idtVectorsSpin->setValue(64);
    m_idtVectorsSpin->setToolTip(QStringLiteral("每 CPU 展开的 IDT 向量数；0 表示不展开 IDT 向量行。"));

    m_statusLabel = new QLabel(QStringLiteral("状态：等待 R0 查询"), this);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->setStyleSheet(statusStyle(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(m_refreshButton, 0);
    toolLayout->addWidget(m_riskOnlyCheck, 0);
    toolLayout->addWidget(new QLabel(QStringLiteral("行数:"), this), 0);
    toolLayout->addWidget(m_maxRowsSpin, 0);
    toolLayout->addWidget(new QLabel(QStringLiteral("IDT/CPU:"), this), 0);
    toolLayout->addWidget(m_idtVectorsSpin, 0);
    toolLayout->addWidget(m_filterEdit, 1);
    toolLayout->addWidget(m_statusLabel, 0);
    m_rootLayout->addLayout(toolLayout, 0);

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    m_rootLayout->addWidget(splitter, 1);

    m_evidenceTable = new QTableWidget(splitter);
    m_evidenceTable->setColumnCount(columnIndex(R0EvidenceColumn::Count));
    m_evidenceTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("CPU/Vector"),
        QStringLiteral("对象/寄存器"),
        QStringLiteral("目标/入口"),
        QStringLiteral("Owner模块"),
        QStringLiteral("风险"),
        QStringLiteral("置信度"),
        QStringLiteral("Detail")
        });
    m_evidenceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_evidenceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_evidenceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_evidenceTable->setAlternatingRowColors(true);
    m_evidenceTable->setSortingEnabled(true);
    m_evidenceTable->verticalHeader()->setVisible(false);
    m_evidenceTable->horizontalHeader()->setStyleSheet(buildHeaderStyle());
    m_evidenceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_evidenceTable->horizontalHeader()->setSectionResizeMode(columnIndex(R0EvidenceColumn::Detail), QHeaderView::Stretch);
    splitter->addWidget(m_evidenceTable);

    QWidget* detailPanel = new QWidget(splitter);
    QHBoxLayout* detailLayout = new QHBoxLayout(detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(8);

    m_detailEditor = new CodeEditorWidget(detailPanel);
    m_detailEditor->setReadOnly(true);
    m_detailEditor->setText(QStringLiteral("请选择一条 R0 CPU/MSR/IDT/GDT 证据查看详情。"));
    detailLayout->addWidget(m_detailEditor, 1);

    QVBoxLayout* badgeLayout = new QVBoxLayout();
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    badgeLayout->addStretch(1);
    m_kernelBadgeLabel = new QLabel(detailPanel);
    m_kernelBadgeLabel->setToolTip(QStringLiteral("R0 功能入口：硬件 CPU/IDT/MSR 内核证据"));
    m_kernelBadgeLabel->setPixmap(QPixmap(QStringLiteral(":/Image/kernel_badge.png")).scaled(
        36,
        36,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
    m_kernelBadgeLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    badgeLayout->addWidget(m_kernelBadgeLabel, 0, Qt::AlignRight | Qt::AlignBottom);
    detailLayout->addLayout(badgeLayout, 0);
    splitter->addWidget(detailPanel);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
}

void HardwareR0EvidencePage::initializeConnections()
{
    // 所有 UI 事件只触发表格重绘或异步刷新，不在 UI 线程直接访问 R0。
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
    {
        refreshEvidenceAsync(true);
    });
    connect(m_riskOnlyCheck, &QCheckBox::toggled, this, [this]()
    {
        rebuildEvidenceTable();
        showSelectedEvidenceDetail();
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]()
    {
        rebuildEvidenceTable();
        showSelectedEvidenceDetail();
    });
    connect(m_evidenceTable, &QTableWidget::itemSelectionChanged, this, [this]()
    {
        showSelectedEvidenceDetail();
    });
}

void HardwareR0EvidencePage::refreshEvidenceAsync(const bool forceRefresh)
{
    // 输入：forceRefresh 只影响状态文本。
    // 处理：后台串行查询能力、DynData 和 CPU integrity；主线程按 ticket 回填。
    // 返回：无。
    if (m_refreshing.exchange(true))
    {
        setStatusText(QStringLiteral("状态：R0 查询仍在进行中"), KswordTheme::TextSecondaryHex());
        return;
    }

    const std::uint64_t ticket = ++m_refreshTicket;
    const unsigned long maxRows = static_cast<unsigned long>(m_maxRowsSpin != nullptr ? m_maxRowsSpin->value() : KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS);
    const unsigned long idtVectors = static_cast<unsigned long>(m_idtVectorsSpin != nullptr ? m_idtVectorsSpin->value() : 64);
    const unsigned long flags = idtVectors == 0UL
        ? KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU
        : (KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU | KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES);

    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(false);
    }
    setStatusText(
        forceRefresh ? QStringLiteral("状态：正在刷新 R0 硬件证据...") : QStringLiteral("状态：正在查询 R0 硬件证据..."),
        KswordTheme::PrimaryBlueHex);

    QPointer<HardwareR0EvidencePage> safeThis(this);
    QRunnable* task = QRunnable::create([safeThis, ticket, flags, maxRows, idtVectors]() mutable
    {
        R0EvidenceQueryBundle bundle;
        const ksword::ark::DriverClient client;
        bundle.capabilityResult = client.queryDriverCapabilities();
        bundle.dynDataResult = client.queryDynDataCapabilities();
        bundle.integrityResult = client.queryKernelCpuIntegrity(flags, maxRows, idtVectors);

        if (safeThis == nullptr)
        {
            return;
        }

        QMetaObject::invokeMethod(safeThis.data(), [safeThis, ticket, bundle = std::move(bundle)]() mutable
        {
            if (safeThis == nullptr || safeThis->m_refreshTicket.load() != ticket)
            {
                return;
            }

            safeThis->m_refreshing.store(false);
            if (safeThis->m_refreshButton != nullptr)
            {
                safeThis->m_refreshButton->setEnabled(true);
            }

            safeThis->m_lastIntegrityResult = bundle.integrityResult;
            safeThis->m_lastCapabilityResult = bundle.capabilityResult;
            safeThis->m_lastDynDataResult = bundle.dynDataResult;
            safeThis->m_evidenceCache.clear();
            safeThis->m_evidenceCache.reserve(bundle.integrityResult.entries.size());
            for (const auto& row : bundle.integrityResult.entries)
            {
                if (isHardwareCpuEvidenceClass(row.evidenceClass))
                {
                    safeThis->m_evidenceCache.push_back(row);
                }
            }
            safeThis->rebuildEvidenceTable();
            safeThis->showSelectedEvidenceDetail();

            if (!bundle.integrityResult.io.ok)
            {
                const QString message = bundle.integrityResult.unsupported
                    ? QStringLiteral("状态：当前 R0 驱动未支持 CPU Integrity IOCTL")
                    : QStringLiteral("状态：R0 查询失败 %1").arg(QString::fromStdString(bundle.integrityResult.io.message));
                safeThis->setStatusText(message, QStringLiteral("#B23A3A"));
                return;
            }

            const QString capabilityText = bundle.capabilityResult.io.ok
                ? QStringLiteral("Cap:%1/%2")
                    .arg(bundle.capabilityResult.returnedFeatureCount)
                    .arg(bundle.capabilityResult.totalFeatureCount)
                : QStringLiteral("Cap不可用");
            const QString dynDataText = bundle.dynDataResult.io.ok
                ? QStringLiteral("Dyn:0x%1")
                    .arg(static_cast<qulonglong>(bundle.dynDataResult.capabilityMask), 0, 16)
                    .toUpper()
                : QStringLiteral("Dyn不可用");
            const QString statusText = QStringLiteral("状态：%1，证据 %2/%3，CPU %4，模块 %5，%6，%7，Last=%8")
                .arg(queryStatusText(bundle.integrityResult.queryStatus))
                .arg(safeThis->m_evidenceCache.size())
                .arg(bundle.integrityResult.totalCount)
                .arg(bundle.integrityResult.cpuCount)
                .arg(bundle.integrityResult.moduleCount)
                .arg(capabilityText)
                .arg(dynDataText)
                .arg(ntStatusText(bundle.integrityResult.lastStatus));
            safeThis->setStatusText(statusText, KswordTheme::PrimaryBlueHex);
        }, Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void HardwareR0EvidencePage::rebuildEvidenceTable()
{
    // 输入：读取本地缓存和过滤控件。
    // 处理：仅重绘表格，不触发任何 R0 调用。
    // 返回：无。
    if (m_evidenceTable == nullptr)
    {
        return;
    }

    const bool riskOnly = m_riskOnlyCheck != nullptr && m_riskOnlyCheck->isChecked();
    const QString filterText = m_filterEdit != nullptr ? m_filterEdit->text().trimmed() : QString();
    std::vector<std::size_t> visibleIndexes;
    visibleIndexes.reserve(m_evidenceCache.size());
    for (std::size_t index = 0; index < m_evidenceCache.size(); ++index)
    {
        const auto& row = m_evidenceCache[index];
        if (riskOnly && row.riskFlags == 0U)
        {
            continue;
        }
        if (!entryMatchesFilter(row, filterText))
        {
            continue;
        }
        visibleIndexes.push_back(index);
    }

    const QSignalBlocker blocker(m_evidenceTable);
    m_evidenceTable->setSortingEnabled(false);
    m_evidenceTable->setRowCount(static_cast<int>(visibleIndexes.size()));
    for (int rowIndex = 0; rowIndex < static_cast<int>(visibleIndexes.size()); ++rowIndex)
    {
        const std::size_t cacheIndex = visibleIndexes[static_cast<std::size_t>(rowIndex)];
        const auto& row = m_evidenceCache[cacheIndex];
        QTableWidgetItem* classItem = textItem(classText(row.evidenceClass));
        classItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Class), classItem);
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Cpu), textItem(cpuVectorText(row)));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Object), numericItem(hex64(row.objectAddress), row.objectAddress));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Target), numericItem(hex64(row.targetAddress), row.targetAddress));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Owner), textItem(
            QStringLiteral("%1 %2")
            .arg(wideToQString(row.ownerModule).isEmpty() ? QStringLiteral("<unknown>") : wideToQString(row.ownerModule))
            .arg(hex64(row.ownerModuleBase))));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Risk), textItem(riskText(row.riskFlags)));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Confidence), numericItem(QString::number(row.confidence), row.confidence));
        m_evidenceTable->setItem(rowIndex, columnIndex(R0EvidenceColumn::Detail), textItem(wideToQString(row.detail)));
    }

    if (m_evidenceTable->rowCount() > 0 && m_evidenceTable->currentRow() < 0)
    {
        m_evidenceTable->setCurrentCell(0, columnIndex(R0EvidenceColumn::Class));
    }
    m_evidenceTable->setSortingEnabled(true);
    ks::ui::RequestTableColumnAutoFit(m_evidenceTable);
}

void HardwareR0EvidencePage::showSelectedEvidenceDetail()
{
    // 输入：读取当前表格选中项。
    // 处理：通过 class 列 UserRole+1 的缓存索引展开详情。
    // 返回：无。
    if (m_detailEditor == nullptr || m_evidenceTable == nullptr)
    {
        return;
    }

    const int rowIndex = m_evidenceTable->currentRow();
    if (rowIndex < 0)
    {
        if (m_evidenceCache.empty())
        {
            m_detailEditor->setText(QStringLiteral("尚未返回 R0 CPU/MSR/IDT/GDT 证据。"));
        }
        else
        {
            m_detailEditor->setText(QStringLiteral("当前过滤条件下没有可见证据。"));
        }
        return;
    }

    QTableWidgetItem* classItem = m_evidenceTable->item(rowIndex, columnIndex(R0EvidenceColumn::Class));
    if (classItem == nullptr)
    {
        m_detailEditor->setText(QStringLiteral("当前行缺少缓存索引。"));
        return;
    }

    bool ok = false;
    const qulonglong cacheIndex = classItem->data(Qt::UserRole + 1).toULongLong(&ok);
    if (!ok || cacheIndex >= static_cast<qulonglong>(m_evidenceCache.size()))
    {
        m_detailEditor->setText(QStringLiteral("当前行缓存索引无效。"));
        return;
    }

    QString text;
    text += QStringLiteral("R0 查询摘要\n");
    text += capabilityText(m_lastCapabilityResult) + QStringLiteral("\n");
    text += dynDataText(m_lastDynDataResult) + QStringLiteral("\n");
    text += QStringLiteral("ProtocolVersion: %1\n").arg(m_lastIntegrityResult.version);
    text += QStringLiteral("QueryStatus: %1 (%2)\n").arg(queryStatusText(m_lastIntegrityResult.queryStatus)).arg(m_lastIntegrityResult.queryStatus);
    text += QStringLiteral("Flags: %1\n").arg(hex32(m_lastIntegrityResult.flags));
    text += QStringLiteral("SourceMask: %1 (%2)\n").arg(sourceMaskText(m_lastIntegrityResult.sourceMask), hex32(m_lastIntegrityResult.sourceMask));
    text += QStringLiteral("Total/Returned/Parsed: %1/%2/%3\n")
        .arg(m_lastIntegrityResult.totalCount)
        .arg(m_lastIntegrityResult.returnedCount)
        .arg(m_evidenceCache.size());
    text += QStringLiteral("CpuCount: %1\n").arg(m_lastIntegrityResult.cpuCount);
    text += QStringLiteral("ModuleCount: %1\n").arg(m_lastIntegrityResult.moduleCount);
    text += QStringLiteral("LastStatus: %1\n").arg(ntStatusText(m_lastIntegrityResult.lastStatus));
    text += QStringLiteral("IoMessage: %1\n\n").arg(QString::fromStdString(m_lastIntegrityResult.io.message));
    text += detailText(m_evidenceCache[static_cast<std::size_t>(cacheIndex)]);
    m_detailEditor->setText(text);
}

void HardwareR0EvidencePage::setStatusText(const QString& text, const QString& colorText)
{
    // 输入：状态文本和 CSS 颜色。
    // 处理：更新状态标签文本与样式；标签为空时忽略。
    // 返回：无。
    if (m_statusLabel == nullptr)
    {
        return;
    }
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(statusStyle(colorText));
}
