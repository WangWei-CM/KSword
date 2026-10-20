#include "../UI/StructuredFieldView.h"
#include "../UI/TableContextMenu.h"
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/FlatButtonTheme.h"
#include "HardwareR0EvidencePage.h"
#include "../UI/IntegrityRiskPresentation.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/AsyncUiDispatcher.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../theme.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/TableColumnAutoFit.h"
#include "../UI/DetailLayoutRegistry.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include "../UI/SegmentedControl.h"
#include <QRunnable>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QItemSelectionModel>
#include <QElapsedTimer>

#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <limits>
#include <array>
#include <memory>
#include <functional>
#include <string>

struct HardwareEvidenceRow
{
    QStringList cells;
    QStringList searchFields;
    quint32 riskFlags = 0;
    std::array<qulonglong, 4> numbers{};
    quint32 evidenceClass = 0;
};

struct HardwareEvidenceSnapshot
{
    std::vector<ksword::ark::DriverIntegrityEvidenceEntry> entries;
    std::vector<HardwareEvidenceRow> rows;
};

namespace
{
    // R0EvidenceColumn 作用：定义硬件 R0 证据表格列；返回值由 columnIndex 转成 int。
    enum class R0EvidenceColumn : int
    {
        Class = 0,
        Cpu,
        Source,
        Status,
        Object,
        Target,
        Owner,
        Risk,
        Confidence,
        Cr0,
        Cr4,
        Efer,
        MsrLstar,
        SysenterEip,
        IdtrBase,
        IdtrLimit,
        GdtrBase,
        GdtrLimit,
        Selector,
        Attributes,
        FieldMask,
        StatusFlags,
        RiskScore,
        Remark,
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

    QString friendlyHardwareIoMessage(const std::string& messageText)
    {
        // friendlyHardwareIoMessage：
        // - 输入：ArkDriverClient 的原始 io.message；
        // - 处理：把 IOCTL/协议错误转换为用户可读说明；
        // - 返回：适合详情区展示的短文本。
        if (messageText.empty())
        {
            return QStringLiteral("无额外驱动消息。");
        }
        const QString rawText = QString::fromStdString(messageText).trimmed();
        if (rawText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动接口调用失败或当前驱动版本不支持该硬件证据入口。");
        }
        if (rawText.contains(QStringLiteral("too small"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("entrySize"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动返回数据格式不完整，当前页只保留已解析出的证据。");
        }
        return rawText;
    }

    QString buildBlueButtonStyle()
    {
        // 纯色主题只接管颜色；保留本页按钮尺寸和业务选中状态。
        return ks::ui::BuildFlatButtonStyle(ks::ui::FlatButtonTone::Neutral)
            + QStringLiteral("QPushButton{border-radius:4px;padding:4px 10px;}");

    }

    QString buildBlueInputStyle()
    {
        // 输入：无。
        // 处理：按全局主题构造输入框样式。
        // 返回：stylesheet 文本。
        return QStringLiteral(
            "QLineEdit{border:1px solid %1;border-radius:4px;padding:4px 6px;color:%2;background:transparent;/* %3 */}"
            "QLineEdit:focus{border:1px solid %4;}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString buildHeaderStyle()
    {
        // 输入：无。
        // 处理：表头使用可读中性文字，主题强调色留给选区与风险提示。
        // 返回：stylesheet 文本。
        return QStringLiteral("QHeaderView::section{color:%1;font-weight:600;}")
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString statusStyle(const QString& colorText)
    {
        // 输入：颜色文本。
        // 处理：生成状态标签 CSS。
        // 返回：stylesheet 文本。
        return QStringLiteral("color:%1; font-weight:700;").arg(colorText);
    }

    QVector<int> r0EvidenceColumnGroupA()
    {
        // 输入：无。
        // 处理：定义 R0 证据默认 A 组，只保留定位异常所需的核心上下文。
        // 返回：列索引集合，调用方据此隐藏其它列。
        return QVector<int>{
            columnIndex(R0EvidenceColumn::Class),
            columnIndex(R0EvidenceColumn::Cpu),
            columnIndex(R0EvidenceColumn::Source),
            columnIndex(R0EvidenceColumn::Status),
            columnIndex(R0EvidenceColumn::Object),
            columnIndex(R0EvidenceColumn::Target),
            columnIndex(R0EvidenceColumn::Owner),
            columnIndex(R0EvidenceColumn::Risk),
            columnIndex(R0EvidenceColumn::Confidence)
        };
    }

    QVector<int> r0EvidenceColumnGroupB()
    {
        // 输入：无。
        // 处理：定义 R0 证据 B 组，重点展示控制寄存器和 MSR 入口字段。
        // 返回：列索引集合，允许与 A 组重复少量上下文列。
        return QVector<int>{
            columnIndex(R0EvidenceColumn::Class),
            columnIndex(R0EvidenceColumn::Cpu),
            columnIndex(R0EvidenceColumn::Cr0),
            columnIndex(R0EvidenceColumn::Cr4),
            columnIndex(R0EvidenceColumn::Efer),
            columnIndex(R0EvidenceColumn::MsrLstar),
            columnIndex(R0EvidenceColumn::SysenterEip)
        };
    }

    QVector<int> r0EvidenceColumnGroupC()
    {
        // 输入：无。
        // 处理：定义 R0 证据 C 组，重点展示描述符表和 IDT/GDT 诊断字段。
        // 返回：列索引集合，和其它列组互补以降低单视图拥挤度。
        return QVector<int>{
            columnIndex(R0EvidenceColumn::Class),
            columnIndex(R0EvidenceColumn::Cpu),
            columnIndex(R0EvidenceColumn::IdtrBase),
            columnIndex(R0EvidenceColumn::IdtrLimit),
            columnIndex(R0EvidenceColumn::GdtrBase),
            columnIndex(R0EvidenceColumn::GdtrLimit),
            columnIndex(R0EvidenceColumn::Selector),
            columnIndex(R0EvidenceColumn::Attributes),
            columnIndex(R0EvidenceColumn::StatusFlags)
        };
    }

    bool containsColumnIndex(const QVector<int>& columnGroup, const int columnIndexValue)
    {
        // 输入：列组和候选列号。
        // 处理：线性检查列号是否属于该组。
        // 返回：true 表示列应该显示。
        return std::find(columnGroup.begin(), columnGroup.end(), columnIndexValue) != columnGroup.end();
    }

    QString buildColumnPresetButtonStyle(const bool selected)
    {
        // 纯色主题只接管颜色；保留本页按钮尺寸和业务选中状态。
        return ks::ui::BuildFlatButtonStyle(selected ? ks::ui::FlatButtonTone::Accent : ks::ui::FlatButtonTone::Neutral)
            + QStringLiteral("QPushButton{min-width:24px;max-width:24px;padding:3px 0;border-radius:0;font-weight:700;}");

    }

    void updateColumnPresetButtons(
        QTableView* table,
        QPushButton* buttonA,
        QPushButton* buttonB,
        QPushButton* buttonC)
    {
        // 输入：目标表格和 A/B/C 按钮。
        // 处理：读取表格属性并刷新按钮着色；Custom 状态下三个按钮均不着色。
        // 返回：无。
        if (table == nullptr || buttonA == nullptr || buttonB == nullptr || buttonC == nullptr)
        {
            return;
        }

        const QString presetText = table->property("kswordColumnPreset").toString();
        ks::ui::SetSegmentedButtonSelected(buttonA, presetText == QStringLiteral("A"));
        ks::ui::SetSegmentedButtonSelected(buttonB, presetText == QStringLiteral("B"));
        ks::ui::SetSegmentedButtonSelected(buttonC, presetText == QStringLiteral("C"));
        buttonA->setStyleSheet(buildColumnPresetButtonStyle(presetText == QStringLiteral("A")));
        buttonB->setStyleSheet(buildColumnPresetButtonStyle(presetText == QStringLiteral("B")));
        buttonC->setStyleSheet(buildColumnPresetButtonStyle(presetText == QStringLiteral("C")));
    }

    void applyColumnPresetToTable(
        QTableView* table,
        const QVector<int>& columnGroup,
        const QString& presetText,
        QPushButton* buttonA,
        QPushButton* buttonB,
        QPushButton* buttonC)
    {
        // 输入：表格、列组、预设名以及 A/B/C 按钮。
        // 处理：按列组显示字段并更新当前预设；不改动行过滤条件。
        // 返回：无。
        if (table == nullptr)
        {
            return;
        }

        for (int columnIndexValue = 0; columnIndexValue < table->model()->columnCount(); ++columnIndexValue)
        {
            table->setColumnHidden(columnIndexValue, !containsColumnIndex(columnGroup, columnIndexValue));
        }
        table->setProperty("kswordColumnPreset", presetText);
        updateColumnPresetButtons(table, buttonA, buttonB, buttonC);
        ks::ui::RequestTableColumnAutoFit(table);
    }

    int visibleColumnCount(QTableView* table)
    {
        // 输入：目标表格。
        // 处理：统计当前未隐藏列，防止手动菜单隐藏最后一列。
        // 返回：可见列数量。
        if (table == nullptr)
        {
            return 0;
        }

        int count = 0;
        for (int columnIndexValue = 0; columnIndexValue < table->model()->columnCount(); ++columnIndexValue)
        {
            if (!table->isColumnHidden(columnIndexValue))
            {
                ++count;
            }
        }
        return count;
    }

    QPushButton* createColumnPresetButton(
        QWidget* parentWidget,
        const QString& buttonText,
        const QString& tooltipText)
    {
        // 输入：父控件、按钮文本和提示。
        // 处理：创建 A/B/C 紧凑按钮，按钮表面只显示单个字母。
        // 返回：由 Qt 父子树释放的按钮。
        QPushButton* button = new QPushButton(buttonText, parentWidget);
        button->setToolTip(tooltipText);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(buildColumnPresetButtonStyle(false));
        return button;
    }

    void installHeaderColumnMenu(
        QTableView* table,
        QPushButton* buttonA,
        QPushButton* buttonB,
        QPushButton* buttonC)
    {
        // 输入：目标表格和 A/B/C 按钮。
        // 处理：给表头安装显式主题样式的列显隐菜单；手动改列后进入 Custom 状态。
        // 返回：无。
        if (table == nullptr || table->horizontalHeader() == nullptr)
        {
            return;
        }

        QHeaderView* headerView = table->horizontalHeader();
        headerView->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(headerView, &QHeaderView::customContextMenuRequested, table, [table, headerView, buttonA, buttonB, buttonC](const QPoint& localPosition)
        {
            QMenu menu(table);
            menu.setStyleSheet(KswordTheme::ContextMenuStyle());
            for (int columnIndexValue = 0; columnIndexValue < table->model()->columnCount(); ++columnIndexValue)
            {
                const QString headerText = table->model()->headerData(
                    columnIndexValue, Qt::Horizontal).toString();
                QAction* columnAction = menu.addAction(headerText);
                columnAction->setCheckable(true);
                columnAction->setChecked(!table->isColumnHidden(columnIndexValue));
                columnAction->setData(columnIndexValue);
            }

            QAction* selectedAction = menu.exec(headerView->viewport()->mapToGlobal(localPosition));
            if (selectedAction == nullptr)
            {
                return;
            }

            const int columnIndexValue = selectedAction->data().toInt();
            const bool shouldShow = selectedAction->isChecked();
            if (!shouldShow && visibleColumnCount(table) <= 1)
            {
                table->setColumnHidden(columnIndexValue, false);
                return;
            }

            table->setColumnHidden(columnIndexValue, !shouldShow);
            table->setProperty("kswordColumnPreset", QStringLiteral("Custom"));
            updateColumnPresetButtons(table, buttonA, buttonB, buttonC);
            ks::ui::RequestTableColumnAutoFit(table);
        });
    }

    void installColumnPresetControls(
        QTableView* table,
        QPushButton* buttonA,
        QPushButton* buttonB,
        QPushButton* buttonC,
        const QVector<int>& groupA,
        const QVector<int>& groupB,
        const QVector<int>& groupC)
    {
        // 输入：表格、A/B/C 按钮和三组列定义。
        // 处理：绑定按钮切换、安装表头菜单，并默认应用 A 组。
        // 返回：无。
        if (table == nullptr || buttonA == nullptr || buttonB == nullptr || buttonC == nullptr)
        {
            return;
        }

        QObject::connect(buttonA, &QPushButton::clicked, table, [table, buttonA, buttonB, buttonC, groupA]()
        {
            applyColumnPresetToTable(table, groupA, QStringLiteral("A"), buttonA, buttonB, buttonC);
        });
        QObject::connect(buttonB, &QPushButton::clicked, table, [table, buttonA, buttonB, buttonC, groupB]()
        {
            applyColumnPresetToTable(table, groupB, QStringLiteral("B"), buttonA, buttonB, buttonC);
        });
        QObject::connect(buttonC, &QPushButton::clicked, table, [table, buttonA, buttonB, buttonC, groupC]()
        {
            applyColumnPresetToTable(table, groupC, QStringLiteral("C"), buttonA, buttonB, buttonC);
        });
        installHeaderColumnMenu(table, buttonA, buttonB, buttonC);
        applyColumnPresetToTable(table, groupA, QStringLiteral("A"), buttonA, buttonB, buttonC);
    }

    // classText / riskText 已收口到 UI/IntegrityRiskPresentation：本页不再自带一份，
    // 否则新增风险位（HIDDEN_HOOK 等）只补进别处时，这里的风险单元格会显示为空。
    using ks::ui::integrity::classText;
    using ks::ui::integrity::riskText;

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

    ks::ui::FieldDocument capabilityText(const ksword::ark::DriverCapabilitiesQueryResult& result)
    {
        // 输入：ArkDriverClient 驱动能力查询结果。
        // 处理：提取 protocol、feature、DynData 和错误摘要，用于状态栏和详情区。
        // 返回：单行可读文本。
        if (!result.io.ok)
        {
            { ks::ui::FieldDocument document;
        document.field(QStringLiteral("DriverCapabilities"), QStringLiteral("不可用"), true);
        return document; }
        }
        { ks::ui::FieldDocument document;
        document.field(QStringLiteral("DriverCapabilities"), QStringLiteral("version=%1 features=%2/%3 dynData=0x%4").arg(QStringLiteral("%1").arg(result.driverProtocolVersion).toUpper()).arg(QStringLiteral("%1").arg(result.returnedFeatureCount).toUpper()).arg(QStringLiteral("%1").arg(result.totalFeatureCount).toUpper()).arg(QStringLiteral("%1").arg(static_cast<qulonglong>(result.dynDataCapabilityMask), 0, 16).toUpper()));
        return document; }
    }

    ks::ui::FieldDocument dynDataText(const ksword::ark::DynDataCapabilitiesResult& result)
    {
        // 输入：ArkDriverClient DynData capability 查询结果。
        // 处理：提取 statusFlags 与 capabilityMask。
        // 返回：单行可读文本。
        if (!result.io.ok)
        {
            { ks::ui::FieldDocument document;
        document.field(QStringLiteral("DynDataCapabilities"), QStringLiteral("不可用"), true);
        return document; }
        }
        { ks::ui::FieldDocument document;
        document.field(QStringLiteral("DynDataCapabilities"), QStringLiteral("status=0x%1 capability=0x%2").arg(QStringLiteral("%1").arg(result.statusFlags, 8, 16, QChar('0')).toUpper()).arg(QStringLiteral("%1").arg(static_cast<qulonglong>(result.capabilityMask), 0, 16).toUpper()));
        return document; }
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

    QString integrityStatusFlagText(const std::uint32_t statusFlags)
    {
        // 输入：KSWORD_ARK_DRIVER_INTEGRITY_STATUS_FLAG_* 位集合。
        // 处理：展开 partial/unsupported/truncated/PDB-required 等状态，空集合显示 None。
        // 返回：表格和详情区可读状态标记。
        if (statusFlags == 0U)
        {
            return QStringLiteral("None");
        }
        QStringList parts;
        if ((statusFlags & KSWORD_ARK_DRIVER_INTEGRITY_STATUS_FLAG_PARTIAL) != 0U)
        {
            parts << QStringLiteral("Partial");
        }
        if ((statusFlags & KSWORD_ARK_DRIVER_INTEGRITY_STATUS_FLAG_UNSUPPORTED) != 0U)
        {
            parts << QStringLiteral("Unsupported");
        }
        if ((statusFlags & KSWORD_ARK_DRIVER_INTEGRITY_STATUS_FLAG_TRUNCATED) != 0U)
        {
            parts << QStringLiteral("Truncated");
        }
        if ((statusFlags & KSWORD_ARK_DRIVER_INTEGRITY_STATUS_FLAG_PDB_REQUIRED) != 0U)
        {
            parts << QStringLiteral("PdbRequired");
        }
        return parts.join(QStringLiteral("|"));
    }

    QString cpuEvidenceDetailValue(const QString& detailText, const QString& keyText)
    {
        // 输入：R0 CPU evidence detail 原文和稳定键名。
        // 处理：按 key=0x... 提取十六进制值，兼容分号/空格/句号分隔。
        // 返回：提取到的十六进制文本；不存在时返回空字符串。
        const QRegularExpression expression(
            QStringLiteral("%1\\s*=\\s*(0x[0-9A-Fa-f]+)")
                .arg(QRegularExpression::escape(keyText)),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = expression.match(detailText);
        if (!match.hasMatch())
        {
            return QString();
        }
        return match.captured(1).trimmed().toUpper();
    }

    QString cpuEvidenceIdtValue(const QString& detailText, const QString& keyText)
    {
        // 输入：R0 IDT 行 detail 和 gate/handler/selector/attr 键名。
        // 处理：提取 IDT[n] 详情中的十六进制字段。
        // 返回：提取到的值；不存在时返回空字符串。
        return cpuEvidenceDetailValue(detailText, keyText);
    }

    // CpuEvidenceParsedDetail 说明：
    // - 输入：来自 parseCpuEvidenceDetail 的拆分结果；
    // - 处理：字段名保持驱动 detail 中的语义；
    // - 返回行为：纯数据结构，无函数返回值。
    struct CpuEvidenceParsedDetail
    {
        QString cr0;
        QString cr4;
        QString efer;
        QString msrLstar;
        QString sysenterEip;
        QString idtrBase;
        QString idtrLimit;
        QString gdtrBase;
        QString gdtrLimit;
        QString selector;
        QString attributes;
        QString gate;
        QString handler;
    };

    CpuEvidenceParsedDetail parseCpuEvidenceDetail(const ksword::ark::DriverIntegrityEvidenceEntry& row)
    {
        // 输入：一条 DriverIntegrityEvidenceEntry。
        // 处理：只解析驱动源码固定输出的 CR/MSR/IDTR/GDTR/IDT 键值；
        // 返回：结构化字段，供表格列使用，未匹配字段保持空字符串。
        const QString rawDetailText = wideToQString(row.detail).trimmed();
        CpuEvidenceParsedDetail parsed;
        parsed.cr0 = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("CR0"));
        parsed.cr4 = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("CR4"));
        parsed.efer = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("EFER"));
        parsed.msrLstar = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("MSR_LSTAR"));
        parsed.sysenterEip = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("SYSENTER_EIP"));
        parsed.idtrBase = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("IDTR base"));
        parsed.idtrLimit = cpuEvidenceDetailValue(rawDetailText, QStringLiteral("limit"));

        const QRegularExpression gdtrExpression(
            QStringLiteral("GDTR\\s+base\\s*=\\s*(0x[0-9A-Fa-f]+)\\s+limit\\s*=\\s*(0x[0-9A-Fa-f]+)"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch gdtrMatch = gdtrExpression.match(rawDetailText);
        if (gdtrMatch.hasMatch())
        {
            parsed.gdtrBase = gdtrMatch.captured(1).trimmed().toUpper();
            parsed.gdtrLimit = gdtrMatch.captured(2).trimmed().toUpper();
        }

        parsed.selector = cpuEvidenceIdtValue(rawDetailText, QStringLiteral("selector"));
        parsed.attributes = cpuEvidenceIdtValue(rawDetailText, QStringLiteral("attr"));
        parsed.gate = cpuEvidenceIdtValue(rawDetailText, QStringLiteral("gate"));
        parsed.handler = cpuEvidenceIdtValue(rawDetailText, QStringLiteral("handler"));
        if (parsed.sysenterEip.isEmpty() &&
            row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY &&
            rawDetailText.contains(QStringLiteral("SYSENTER_EIP captured"), Qt::CaseInsensitive))
        {
            parsed.sysenterEip = hex64(row.targetAddress);
        }
        if (parsed.idtrBase.isEmpty() && row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE)
        {
            parsed.idtrBase = hex64(row.objectAddress);
        }
        if (parsed.gdtrBase.isEmpty() && row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE)
        {
            parsed.gdtrBase = hex64(row.targetAddress);
        }
        if (parsed.gate.isEmpty() && row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER)
        {
            parsed.gate = hex64(row.objectAddress);
        }
        if (parsed.handler.isEmpty() && row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER)
        {
            parsed.handler = hex64(row.targetAddress);
        }
        return parsed;
    }

    bool isStructuredCpuEvidenceDetail(
        const ksword::ark::DriverIntegrityEvidenceEntry& row,
        const QString& rawDetailText)
    {
        // 输入：证据行和原始 detail。
        // 处理：识别已经拆进表格列的驱动固定格式。
        // 返回：true 表示备注列不再重复整句原文。
        return rawDetailText.contains(QStringLiteral("CR0="), Qt::CaseInsensitive) ||
            rawDetailText.contains(QStringLiteral("MSR_LSTAR="), Qt::CaseInsensitive) ||
            rawDetailText.contains(QStringLiteral("IDTR base="), Qt::CaseInsensitive) ||
            rawDetailText.contains(QStringLiteral("IDT["), Qt::CaseInsensitive) ||
            row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE ||
            row.evidenceClass == KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY;
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

    QStringList evidenceSearchFields(const ksword::ark::DriverIntegrityEvidenceEntry& row,
        const CpuEvidenceParsedDetail& parsedDetail, const QString& classWord, const QString& riskWord)
    {
        return QStringList{
            classWord,
            cpuVectorText(row),
            sourceMaskText(row.sourceMask),
            queryStatusText(row.entryStatus),
            hex64(row.objectAddress),
            hex64(row.targetAddress),
            wideToQString(row.ownerModule),
            riskWord,
            hex32(row.fieldMask),
            hex32(row.statusFlags),
            integrityStatusFlagText(row.statusFlags),
            QString::number(row.riskScore),
            parsedDetail.cr0,
            parsedDetail.cr4,
            parsedDetail.efer,
            parsedDetail.msrLstar,
            parsedDetail.sysenterEip,
            parsedDetail.idtrBase,
            parsedDetail.idtrLimit,
            parsedDetail.gdtrBase,
            parsedDetail.gdtrLimit,
            parsedDetail.selector,
            parsedDetail.attributes,
            wideToQString(row.detail)
        };
    }

    ks::ui::FieldDocument detailText(const ksword::ark::DriverIntegrityEvidenceEntry& row)
    {
        // 输入：当前 R0 evidence 行。
        // 处理：展开所有关键协议字段，便于复制到调试记录。
        // 返回：多行详情文本。
        ks::ui::FieldDocument text;
        text.note(QStringLiteral("R0 硬件 / CPU 入口证据详情"));
        text.field(QStringLiteral("Class"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(classText(row.evidenceClass))).arg(QStringLiteral("%1").arg(row.evidenceClass)));
        text.field(QStringLiteral("CPU"), QStringLiteral("Group=%1 Processor=%2 Vector=%3").arg(QStringLiteral("%1").arg(row.processorGroup)).arg(QStringLiteral("%1").arg(row.processorNumber)).arg(QStringLiteral("%1").arg(row.vector)));
        text.field(QStringLiteral("ObjectAddress"), QStringLiteral("%1").arg(hex64(row.objectAddress)));
        text.field(QStringLiteral("TargetAddress"), QStringLiteral("%1").arg(hex64(row.targetAddress)));
        text.field(QStringLiteral("OwnerModule"), QStringLiteral("%1").arg(wideToQString(row.ownerModule)));
        text.field(QStringLiteral("OwnerModuleBase"), QStringLiteral("%1").arg(hex64(row.ownerModuleBase)));
        text.field(QStringLiteral("OwnerModuleSize"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(hex32(row.ownerModuleSize))).arg(QStringLiteral("%1").arg(row.ownerModuleSize)));
        text.field(QStringLiteral("SourceMask"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(sourceMaskText(row.sourceMask))).arg(QStringLiteral("%1").arg(hex32(row.sourceMask))));
        text.field(QStringLiteral("RiskFlags"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(riskText(row.riskFlags))).arg(QStringLiteral("%1").arg(hex32(row.riskFlags))));
        text.field(QStringLiteral("Confidence"), QStringLiteral("%1").arg(row.confidence));
        text.field(QStringLiteral("EntryStatus"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(queryStatusText(row.entryStatus))).arg(QStringLiteral("%1").arg(row.entryStatus)));
        text.field(QStringLiteral("StatusFlags"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(integrityStatusFlagText(row.statusFlags))).arg(QStringLiteral("%1").arg(hex32(row.statusFlags))));
        text.field(QStringLiteral("FieldMask"), QStringLiteral("%1").arg(hex32(row.fieldMask)));
        text.field(QStringLiteral("RiskScore"), QStringLiteral("%1").arg(row.riskScore));
        text.field(QStringLiteral("RangeState"), QStringLiteral("%1").arg(row.rangeState));
        text.field(QStringLiteral("Ordinal"), QStringLiteral("%1").arg(row.ordinal));
        text.field(QStringLiteral("Detail"), QStringLiteral("%1").arg(wideToQString(row.detail)));
        return text;
    }

    QString hardwareTableCellText(QTableView* table, const int rowIndex, const int columnIndex)
    {
        // hardwareTableCellText：
        // - 输入：表格、行号、列号；
        // - 处理：安全读取当前单元格；
        // - 返回：不存在时返回空字符串。
        if (table == nullptr)
        {
            return QString();
        }
        return table->model()->index(rowIndex, columnIndex).data().toString();
    }

    void copyHardwareTableCurrentRow(QTableView* table)
    {
        // copyHardwareTableCurrentRow：
        // - 输入：R0 硬件证据表；
        // - 处理：复制当前行 TSV；
        // - 返回：无，不触发额外 R0 查询或写操作。
        if (table == nullptr || QGuiApplication::clipboard() == nullptr)
        {
            return;
        }

        const int rowIndex = table->currentIndex().row();
        if (rowIndex < 0 || rowIndex >= table->model()->rowCount())
        {
            return;
        }

        QStringList fields;
        fields.reserve(table->model()->columnCount());
        for (int columnIndex = 0; columnIndex < table->model()->columnCount(); ++columnIndex)
        {
            fields.push_back(hardwareTableCellText(table, rowIndex, columnIndex));
        }
        QGuiApplication::clipboard()->setText(fields.join(QLatin1Char('\t')));
    }

    void installHardwareTableCopyMenu(QTableView* table)
    {
        // installHardwareTableCopyMenu：
        // - 输入：R0 硬件证据表；
        // - 处理：安装只读复制菜单；
        // - 返回：无，菜单只写剪贴板。
        if (table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QTableView::customContextMenuRequested, table, [table](const QPoint& localPosition)
        {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentIndex(clickedIndex);
                table->selectRow(clickedIndex.row());
            }

            QMenu contextMenu(table);
            contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
            QAction* copyRowAction = contextMenu.addAction(
                QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
                QStringLiteral("复制当前行"));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            copyRowAction->setEnabled(table->currentIndex().row() >= 0);
            if (contextMenu.exec(table->viewport()->mapToGlobal(localPosition)) == copyRowAction)
            {
                copyHardwareTableCurrentRow(table);
            }
        });
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
    struct EvidenceWords
    {
        QString languageId;
        QHash<quint32, QString> classes;
        QHash<quint32, QString> risks;
    };

    std::shared_ptr<const HardwareEvidenceSnapshot> buildEvidenceSnapshot(
        const ksword::ark::DriverIntegrityResult& result, const EvidenceWords& words)
    {
        auto snapshot = std::make_shared<HardwareEvidenceSnapshot>();
        for (const auto& row : result.entries)
        {
            if (!isHardwareCpuEvidenceClass(row.evidenceClass)) continue;
            const CpuEvidenceParsedDetail parsedDetail = parseCpuEvidenceDetail(row);
            const QString classWord = words.classes.value(row.evidenceClass);
            const QString riskWord = words.risks.value(row.riskFlags);
            const QString rawDetailText = wideToQString(row.detail).trimmed();
            QStringList remarkParts;
            if (!rawDetailText.isEmpty() && !isStructuredCpuEvidenceDetail(row, rawDetailText))
                remarkParts << rawDetailText;
            if (row.rangeState != 0U) remarkParts << QStringLiteral("rangeState=%1").arg(row.rangeState);
            if (row.ordinal != 0U && row.ordinal != std::numeric_limits<std::uint32_t>::max())
                remarkParts << QStringLiteral("ordinal=%1").arg(row.ordinal);
            HardwareEvidenceRow display;
            display.cells = QStringList{
                classWord,
                cpuVectorText(row),
                sourceMaskText(row.sourceMask),
                queryStatusText(row.entryStatus),
                hex64(row.objectAddress),
                hex64(row.targetAddress),
                QStringLiteral("%1 %2")
            .arg(wideToQString(row.ownerModule).isEmpty() ? QStringLiteral("<unknown>") : wideToQString(row.ownerModule))
            .arg(hex64(row.ownerModuleBase)),
                riskWord,
                QString::number(row.confidence),
                parsedDetail.cr0,
                parsedDetail.cr4,
                parsedDetail.efer,
                parsedDetail.msrLstar,
                parsedDetail.sysenterEip,
                parsedDetail.idtrBase,
                parsedDetail.idtrLimit,
                parsedDetail.gdtrBase,
                parsedDetail.gdtrLimit,
                parsedDetail.selector,
                parsedDetail.attributes,
                hex32(row.fieldMask),
                QStringLiteral("%1 (%2)")
            .arg(hex32(row.statusFlags))
            .arg(integrityStatusFlagText(row.statusFlags)),
                QString::number(row.riskScore),
                remarkParts.join(QStringLiteral("；"))
            };
            display.searchFields = evidenceSearchFields(row, parsedDetail, classWord, riskWord);
            display.riskFlags = row.riskFlags;
            display.evidenceClass = row.evidenceClass;
            display.numbers = { row.objectAddress, row.targetAddress, row.confidence, row.riskScore };
            snapshot->entries.push_back(row);
            snapshot->rows.push_back(std::move(display));
        }
        return snapshot;
    }

    // Freeze the few rendered risk/class words on the GUI thread in bounded batches.
    // Parsing/formatting and all row strings then run using values only.
    class EvidencePreparation final : public QObject
    {
    public:
        using Ready = std::function<void(R0EvidenceQueryBundle, std::shared_ptr<const HardwareEvidenceSnapshot>)>;
        EvidencePreparation(QObject* owner, std::shared_ptr<ks::ui::AsyncUiDispatcher> dispatcher,
            R0EvidenceQueryBundle bundle, Ready ready)
            : QObject(owner), m_bundle(std::move(bundle)), m_ready(std::move(ready)),
              m_dispatcher(std::move(dispatcher)) { pump(); }
    private:
        void pump()
        {
            const QString languageId = ks::i18n::LanguageManager::instance().currentLanguageId();
            if (m_words.languageId != languageId)
            {
                m_words = {};
                m_words.languageId = languageId;
                m_offset = 0;
            }
            QElapsedTimer budget;
            budget.start();
            int count = 0;
            while (m_offset < m_bundle.integrityResult.entries.size())
            {
                const auto& row = m_bundle.integrityResult.entries[m_offset++];
                if (isHardwareCpuEvidenceClass(row.evidenceClass))
                {
                    if (!m_words.classes.contains(row.evidenceClass))
                        m_words.classes.insert(row.evidenceClass, classText(row.evidenceClass));
                    if (!m_words.risks.contains(row.riskFlags))
                        m_words.risks.insert(row.riskFlags, riskText(row.riskFlags));
                }
                if (++count == 64 || budget.elapsed() >= 4) break;
            }
            if (m_offset < m_bundle.integrityResult.entries.size())
            {
                QTimer::singleShot(0, this, [this]() { pump(); });
                return;
            }
            const auto dispatcher = m_dispatcher;
            auto* task = QRunnable::create([dispatcher, bundle = std::move(m_bundle),
                words = std::move(m_words), ready = std::move(m_ready)]() mutable {
                auto snapshot = buildEvidenceSnapshot(bundle.integrityResult, words);
                dispatcher->post([bundle = std::move(bundle),
                    snapshot = std::move(snapshot), ready = std::move(ready)]() mutable {
                    ready(std::move(bundle), std::move(snapshot));
                });
            });
            QThreadPool::globalInstance()->start(task);
            deleteLater();
        }
        R0EvidenceQueryBundle m_bundle;
        EvidenceWords m_words;
        Ready m_ready;
        std::shared_ptr<ks::ui::AsyncUiDispatcher> m_dispatcher;
        std::size_t m_offset = 0;
    };

    class EvidenceModel final : public QAbstractTableModel
    {
    public:
        explicit EvidenceModel(QObject* parent) : QAbstractTableModel(parent) {}
        QStringList headers;
        mutable QString cachedLanguageId;
        mutable QHash<quint32, QString> classWords, riskWords;
        QString word(const HardwareEvidenceRow& row, bool risk) const
        {
            const QString languageId = ks::i18n::LanguageManager::instance().currentLanguageId();
            if (cachedLanguageId != languageId)
            {
                cachedLanguageId = languageId;
                classWords.clear();
                riskWords.clear();
            }
            auto& cache = risk ? riskWords : classWords;
            const quint32 key = risk ? row.riskFlags : row.evidenceClass;
            auto found = cache.constFind(key);
            if (found != cache.cend()) return found.value();
            const QString value = risk ? riskText(key) : classText(key);
            cache.insert(key, value);
            return value;
        }
        void refreshLanguage()
        {
            cachedLanguageId.clear();
            if (rowCount()) emit dataChanged(index(0,0), index(rowCount()-1,columnCount()-1), {Qt::DisplayRole});
            emit headerDataChanged(Qt::Horizontal, 0, columnCount()-1);
        }
        std::shared_ptr<const HardwareEvidenceSnapshot> snapshot;
        int rowCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() || !snapshot ? 0 : static_cast<int>(snapshot->rows.size()); }
        int columnCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() ? 0 : columnIndex(R0EvidenceColumn::Count); }
        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
        { return orientation == Qt::Horizontal && role == Qt::DisplayRole ? ks::i18n::displayText(headers.value(section)) : QVariant(); }
        const HardwareEvidenceRow* rowAt(int row) const
        { return snapshot && row >= 0 && row < rowCount() ? &snapshot->rows[static_cast<std::size_t>(row)] : nullptr; }
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
        {
            const auto* row = rowAt(index.row());
            if (!index.isValid() || row == nullptr || index.column() < 0 || index.column() >= columnCount()) return {};
            if (role == Qt::DisplayRole)
            {
                if (index.column() == columnIndex(R0EvidenceColumn::Class)) return word(*row, false);
                if (index.column() == columnIndex(R0EvidenceColumn::Risk)) return word(*row, true);
                return ks::i18n::sourceText(row->cells.value(index.column()));
            }
            if (role == Qt::TextAlignmentRole) return int(Qt::AlignVCenter | Qt::AlignLeft);
            if (role == Qt::UserRole + 1) return QVariant::fromValue<qulonglong>(index.row());
            if (role == Qt::UserRole)
            {
                const int column = index.column();
                const int slot = column == columnIndex(R0EvidenceColumn::Object) ? 0
                    : column == columnIndex(R0EvidenceColumn::Target) ? 1
                    : column == columnIndex(R0EvidenceColumn::Confidence) ? 2
                    : column == columnIndex(R0EvidenceColumn::RiskScore) ? 3 : -1;
                return slot < 0 ? QVariant() : QVariant::fromValue(row->numbers[slot]);
            }
            return ks::ui::integrity::riskRoleData(row->riskFlags, role);
        }
        void setSnapshot(std::shared_ptr<const HardwareEvidenceSnapshot> value)
        {
            beginResetModel();
            snapshot.swap(value);
            endResetModel();
            // Release a potentially large old string snapshot away from the event thread.
            if (value) QThreadPool::globalInstance()->start(QRunnable::create([old = std::move(value)]() {}));
        }
    };

    class EvidenceFilter final : public QSortFilterProxyModel
    {
    public:
        using QSortFilterProxyModel::QSortFilterProxyModel;
        QString query;
        bool riskOnly = false;
        void setValues(QString text, bool onlyRisk)
        {
            if (query == text && riskOnly == onlyRisk) return;
            query = std::move(text);
            riskOnly = onlyRisk;
            invalidateFilter();
        }
    protected:
        bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override
        {
            const auto* row = static_cast<const EvidenceModel*>(sourceModel())->rowAt(sourceRow);
            if (row == nullptr || (riskOnly && row->riskFlags == 0U)) return false;
            if (query.isEmpty()) return true;
            const auto* model = static_cast<const EvidenceModel*>(sourceModel());
            if (model->word(*row, false).contains(query, Qt::CaseInsensitive)
                || model->word(*row, true).contains(query, Qt::CaseInsensitive)) return true;
            for (int index = 0; index < row->searchFields.size(); ++index)
            {
                // Class/risk words depend on the current language; all other raw fields are canonical.
                if (index == 0 || index == 7) continue;
                if (row->searchFields[index].contains(query, Qt::CaseInsensitive)) return true;
            }
            return false;
        }
        bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
        {
            const QVariant a = left.data(Qt::UserRole), b = right.data(Qt::UserRole);
            return a.isValid() && b.isValid() ? a.toULongLong() < b.toULongLong()
                : QSortFilterProxyModel::lessThan(left, right);
        }
    };

}

HardwareR0EvidencePage::HardwareR0EvidencePage(QWidget* parent)
    : QWidget(parent), m_dispatcher(std::make_shared<ks::ui::AsyncUiDispatcher>(this))
{
    // 构造流程：先建 UI，再连接信号，最后延迟启动只读 R0 查询，避免阻塞 Dock 创建链路。
    initializeUi();
    initializeConnections();
    QTimer::singleShot(0, this, [this]()
    {
        refreshEvidenceAsync(false);
    });
}

HardwareR0EvidencePage::~HardwareR0EvidencePage()
{
    m_dispatcher->close(); // close receiver admission before any QWidget child/member is destroyed
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

    QHBoxLayout* presetLayout = new QHBoxLayout();
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(0);

    QPushButton* groupAButton = createColumnPresetButton(
        this,
        QStringLiteral("A"),
        QStringLiteral("显示默认精简列：类别、CPU、来源、状态、对象、Owner、风险和置信度。"));
    QPushButton* groupBButton = createColumnPresetButton(
        this,
        QStringLiteral("B"),
        QStringLiteral("显示 B 组精简列：CR0/CR4/EFER/MSR 入口字段。"));
    QPushButton* groupCButton = createColumnPresetButton(
        this,
        QStringLiteral("C"),
        QStringLiteral("显示 C 组精简列：IDTR/GDTR/Selector/Attr/StatusFlags。"));
    presetLayout->addWidget(groupAButton, 0);
    presetLayout->addWidget(groupBButton, 0);
    presetLayout->addWidget(groupCButton, 0);
    ks::ui::InstallSegmentedButtons(presetLayout, {groupAButton, groupBButton, groupCButton});

    m_refreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新R0证据"), this);
    m_refreshButton->setToolTip(QStringLiteral("刷新内核硬件证据（只读）。"));
    m_refreshButton->setStyleSheet(buildBlueButtonStyle());

    m_riskOnlyCheck = new QCheckBox(QStringLiteral("仅风险项"), this);
    m_riskOnlyCheck->setToolTip(QStringLiteral("只显示存在风险标记的硬件证据。"));

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(QStringLiteral("搜索 类别/CPU/来源/状态/Owner/风险/CR/MSR/IDTR/GDTR/FieldMask/StatusFlags"));
    ks::ui::StyleSearchField(m_filterEdit);

    m_maxRowsSpin = new QSpinBox(this);
    m_maxRowsSpin->setRange(64, 65536);
    m_maxRowsSpin->setValue(static_cast<int>(KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS));
    m_maxRowsSpin->setToolTip(QStringLiteral("限制单次查询返回的最大行数。"));

    m_idtVectorsSpin = new QSpinBox(this);
    m_idtVectorsSpin->setRange(0, static_cast<int>(KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS));
    m_idtVectorsSpin->setValue(64);
    m_idtVectorsSpin->setToolTip(QStringLiteral("每 CPU 展开的 IDT 向量数；0 表示不展开 IDT 向量行。"));

    m_statusLabel = new QLabel(QStringLiteral("状态：等待 R0 查询"), this);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->setStyleSheet(statusStyle(KswordTheme::TextSecondaryHex()));

    toolLayout->addLayout(presetLayout, 0);
    toolLayout->addWidget(m_refreshButton, 0);
    toolLayout->addWidget(m_riskOnlyCheck, 0);
    toolLayout->addWidget(new QLabel(QStringLiteral("行数:"), this), 0);
    toolLayout->addWidget(m_maxRowsSpin, 0);
    toolLayout->addWidget(new QLabel(QStringLiteral("IDT/CPU:"), this), 0);
    toolLayout->addWidget(m_idtVectorsSpin, 0);
    toolLayout->addWidget(m_filterEdit, 1);
    toolLayout->addWidget(m_statusLabel, 0);
    ks::ui::NormalizeToolbarRow(presetLayout, 0);
    ks::ui::NormalizeToolbarRow(toolLayout);
    m_rootLayout->addLayout(toolLayout, 0);

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    m_rootLayout->addWidget(splitter, 1);

    m_evidenceTable = new ks::ui::TableActionTableView(splitter);
    auto* model = new EvidenceModel(m_evidenceTable);
    auto* filter = new EvidenceFilter(m_evidenceTable);
    m_evidenceModel = model;
    m_filterModel = filter;
    filter->setSourceModel(model);
    m_evidenceTable->setModel(filter);
    m_evidenceTable->setProperty("ks_i18n_preserve_model_data", true);
    // CPU/MSR/描述符证据需要跨次快照对比，保留完整操作栏。
    ks::ui::SetTableActionBarMode(m_evidenceTable, ks::ui::TableActionBarMode::Full);
    model->headers = QStringList{
        QStringLiteral("类别"),
        QStringLiteral("CPU/Vector"),
        QStringLiteral("来源"),
        QStringLiteral("状态"),
        QStringLiteral("对象/寄存器"),
        QStringLiteral("目标/入口"),
        QStringLiteral("Owner模块"),
        QStringLiteral("风险"),
        QStringLiteral("置信度"),
        QStringLiteral("CR0"),
        QStringLiteral("CR4"),
        QStringLiteral("EFER"),
        QStringLiteral("MSR_LSTAR"),
        QStringLiteral("SYSENTER_EIP"),
        QStringLiteral("IDTR base"),
        QStringLiteral("IDTR limit"),
        QStringLiteral("GDTR base"),
        QStringLiteral("GDTR limit"),
        QStringLiteral("Selector"),
        QStringLiteral("Attr"),
        QStringLiteral("FieldMask"),
        QStringLiteral("StatusFlags"),
        QStringLiteral("RiskScore"),
        QStringLiteral("备注")
        };
    m_evidenceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_evidenceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_evidenceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_evidenceTable->setAlternatingRowColors(true);
    m_evidenceTable->setSortingEnabled(true);
    m_evidenceTable->verticalHeader()->setVisible(false);
    m_evidenceTable->horizontalHeader()->setStyleSheet(buildHeaderStyle());
    m_evidenceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_evidenceTable->horizontalHeader()->setSectionResizeMode(columnIndex(R0EvidenceColumn::Remark), QHeaderView::Stretch);
    installHardwareTableCopyMenu(m_evidenceTable);
    installColumnPresetControls(
        m_evidenceTable,
        groupAButton,
        groupBButton,
        groupCButton,
        r0EvidenceColumnGroupA(),
        r0EvidenceColumnGroupB(),
        r0EvidenceColumnGroupC());
    splitter->addWidget(m_evidenceTable);

    QWidget* detailPanel = new QWidget(splitter);
    QHBoxLayout* detailLayout = new QHBoxLayout(detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(8);

    m_detailEditor = new ks::ui::StructuredFieldView(detailPanel);

    m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("请选择一条 R0 CPU/MSR/IDT/GDT 证据查看详情。")));
    detailLayout->addWidget(m_detailEditor, 1);

    ks::ui::DetailLayoutRegistry::registerStructuredHost(
        m_evidenceTable, m_detailEditor, this);
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
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(120);
    connect(m_filterTimer, &QTimer::timeout, this, [this]() { rebuildEvidenceTable(); });
    connect(m_riskOnlyCheck, &QCheckBox::toggled, this, [this]() {
        m_filterTimer->stop();
        rebuildEvidenceTable();
    });
    connect(m_filterEdit, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_evidenceTable->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, [this]() { showSelectedEvidenceDetail(); });
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
    const auto dispatcher = m_dispatcher;
    QRunnable* task = QRunnable::create([dispatcher, safeThis, ticket, flags, maxRows, idtVectors]() mutable
    {
        R0EvidenceQueryBundle bundle;
        const ksword::ark::DriverClient client;
        bundle.capabilityResult = client.queryDriverCapabilities();
        bundle.dynDataResult = client.queryDynDataCapabilities();
        bundle.integrityResult = client.queryKernelCpuIntegrity(flags, maxRows, idtVectors);

        dispatcher->post([safeThis, dispatcher, ticket, bundle = std::move(bundle)]() mutable
        {
            if (safeThis == nullptr)
            {
                return;
            }

            new EvidencePreparation(safeThis.data(), dispatcher, std::move(bundle),
                [safeThis, ticket](R0EvidenceQueryBundle ready,
                    std::shared_ptr<const HardwareEvidenceSnapshot> snapshot) mutable {
                    if (safeThis == nullptr) return;
                    safeThis->applyEvidenceQueryResults(ticket, std::move(ready.capabilityResult),
                        std::move(ready.dynDataResult), std::move(ready.integrityResult), std::move(snapshot));
                });
        });
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void HardwareR0EvidencePage::applyEvidenceQueryResults(
    const std::uint64_t ticket,
    ksword::ark::DriverCapabilitiesQueryResult capabilityResult,
    ksword::ark::DynDataCapabilitiesResult dynDataResult,
    ksword::ark::DriverIntegrityResult integrityResult,
    std::shared_ptr<const HardwareEvidenceSnapshot> snapshot)
{
    if (m_refreshTicket.load() != ticket)
    {
        return;
    }

    if (ks::ui::IsTableUiCommitBlockedByContextMenu({m_evidenceTable}))
    {
        const QPointer<HardwareR0EvidencePage> safeThis(this);
        ks::ui::DeferTableUiCommitIfContextMenuOpen(
            this,
            QStringLiteral("hardware-r0-evidence-snapshot-apply"),
            {m_evidenceTable},
            [safeThis,
                ticket,
                capabilityResult = std::move(capabilityResult),
                dynDataResult = std::move(dynDataResult),
                integrityResult = std::move(integrityResult),
                snapshot = std::move(snapshot)]() mutable
            {
                if (!safeThis.isNull())
                {
                    safeThis->applyEvidenceQueryResults(
                        ticket,
                        std::move(capabilityResult),
                        std::move(dynDataResult),
                        std::move(integrityResult), std::move(snapshot));
                }
            });
        return;
    }

    m_refreshing.store(false);
    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(true);
    }

    m_lastIntegrityResult = std::move(integrityResult);
    m_lastCapabilityResult = std::move(capabilityResult);
    m_lastDynDataResult = std::move(dynDataResult);
    const QSignalBlocker selectionBlocker(m_evidenceTable->selectionModel());
    m_evidenceSnapshot = std::move(snapshot);
    ks::ui::DetailLayoutRegistry::prepareDataRebuild(m_detailEditor);
    static_cast<EvidenceModel*>(m_evidenceModel)->setSnapshot(m_evidenceSnapshot);
    ks::ui::RequestTableColumnAutoFit(m_evidenceTable); // once per accepted query, not per keystroke
    rebuildEvidenceTable();
    showSelectedEvidenceDetail();

    if (!m_lastIntegrityResult.io.ok)
    {
        const QString message = m_lastIntegrityResult.unsupported
            ? QStringLiteral("状态：当前 R0 驱动未支持 CPU Integrity IOCTL")
            : QStringLiteral("状态：R0 查询不可用：%1")
                .arg(friendlyHardwareIoMessage(m_lastIntegrityResult.io.message));
        setStatusText(message, KswordTheme::ErrorColor().name(QColor::HexRgb));
        return;
    }

    const QString capabilityText = m_lastCapabilityResult.io.ok
        ? QStringLiteral("Cap:%1/%2")
            .arg(m_lastCapabilityResult.returnedFeatureCount)
            .arg(m_lastCapabilityResult.totalFeatureCount)
        : QStringLiteral("Cap不可用");
    const QString dynDataText = m_lastDynDataResult.io.ok
        ? QStringLiteral("Dyn:0x%1")
            .arg(static_cast<qulonglong>(m_lastDynDataResult.capabilityMask), 0, 16)
            .toUpper()
        : QStringLiteral("Dyn不可用");
    const QString statusText =
        QStringLiteral("状态：%1，证据 %2/%3，CPU %4，模块 %5，%6，%7，Last=%8")
        .arg(queryStatusText(m_lastIntegrityResult.queryStatus))
        .arg(m_evidenceSnapshot->entries.size())
        .arg(m_lastIntegrityResult.totalCount)
        .arg(m_lastIntegrityResult.cpuCount)
        .arg(m_lastIntegrityResult.moduleCount)
        .arg(capabilityText)
        .arg(dynDataText)
        .arg(ntStatusText(m_lastIntegrityResult.lastStatus));
    setStatusText(statusText, KswordTheme::PrimaryBlueHex);
}

void HardwareR0EvidencePage::rebuildEvidenceTable()
{
    if (m_evidenceTable == nullptr) return;
    const QPointer<HardwareR0EvidencePage> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("hardware-r0-filter"),
        {m_evidenceTable}, [guard]() { if (guard != nullptr) guard->rebuildEvidenceTable(); })) return;
    const bool riskOnly = m_riskOnlyCheck != nullptr && m_riskOnlyCheck->isChecked();
    const QString query = m_filterEdit != nullptr ? m_filterEdit->text().trimmed() : QString();
    static_cast<EvidenceFilter*>(m_filterModel)->setValues(query, riskOnly);
    if (!m_evidenceTable->currentIndex().isValid() && m_filterModel->rowCount() > 0)
        m_evidenceTable->setCurrentIndex(m_filterModel->index(0, columnIndex(R0EvidenceColumn::Class)));
    showSelectedEvidenceDetail();
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

    const int rowIndex = m_evidenceTable->currentIndex().row();
    if (rowIndex < 0)
    {
        if ((!m_evidenceSnapshot || m_evidenceSnapshot->entries.empty()))
        {
            m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("尚未返回 R0 CPU/MSR/IDT/GDT 证据。")));
        }
        else
        {
            m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("当前过滤条件下没有可见证据。")));
        }
        return;
    }

    const QModelIndex classIndex = m_filterModel->index(rowIndex, columnIndex(R0EvidenceColumn::Class));
    bool ok = false;
    const qulonglong cacheIndex = classIndex.data(Qt::UserRole + 1).toULongLong(&ok);
    if (!ok || cacheIndex >= static_cast<qulonglong>(m_evidenceSnapshot->entries.size()))
    {
        m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("当前行缓存索引无效。")));
        return;
    }

    ks::ui::FieldDocument text;
    text.note(QStringLiteral("R0 查询摘要"));
    text.nodes += capabilityText(m_lastCapabilityResult).nodes;
    text.nodes += dynDataText(m_lastDynDataResult).nodes;
    text.field(QStringLiteral("ProtocolVersion"), QStringLiteral("%1").arg(m_lastIntegrityResult.version));
    text.field(QStringLiteral("QueryStatus"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(queryStatusText(m_lastIntegrityResult.queryStatus))).arg(QStringLiteral("%1").arg(m_lastIntegrityResult.queryStatus)));
    text.field(QStringLiteral("Flags"), QStringLiteral("%1").arg(hex32(m_lastIntegrityResult.flags)));
    text.field(QStringLiteral("SourceMask"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(sourceMaskText(m_lastIntegrityResult.sourceMask))).arg(QStringLiteral("%1").arg(hex32(m_lastIntegrityResult.sourceMask))));
    text.field(QStringLiteral("ResponseFieldFlags"), QStringLiteral("%1").arg(hex32(m_lastIntegrityResult.fieldFlags)));
    text.field(QStringLiteral("ResponseStatusFlags"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(integrityStatusFlagText(m_lastIntegrityResult.statusFlags))).arg(QStringLiteral("%1").arg(hex32(m_lastIntegrityResult.statusFlags))));
    text.field(QStringLiteral("Total/Returned/Parsed"), QStringLiteral("%1/%2/%3").arg(QStringLiteral("%1").arg(m_lastIntegrityResult.totalCount)).arg(QStringLiteral("%1").arg(m_lastIntegrityResult.returnedCount)).arg(QStringLiteral("%1").arg(m_evidenceSnapshot->entries.size())));
    text.field(QStringLiteral("CpuCount"), QStringLiteral("%1").arg(m_lastIntegrityResult.cpuCount));
    text.field(QStringLiteral("ModuleCount"), QStringLiteral("%1").arg(m_lastIntegrityResult.moduleCount));
    text.field(QStringLiteral("LastStatus"), QStringLiteral("%1").arg(ntStatusText(m_lastIntegrityResult.lastStatus)));
    text.field(QStringLiteral("驱动返回说明"), QStringLiteral("%1").arg(friendlyHardwareIoMessage(m_lastIntegrityResult.io.message)));
    text.nodes += detailText(m_evidenceSnapshot->entries[static_cast<std::size_t>(cacheIndex)]).nodes;
    m_detailEditor->setDocument(text);
}

void HardwareR0EvidencePage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_evidenceModel != nullptr)
    {
        static_cast<EvidenceModel*>(m_evidenceModel)->refreshLanguage();
        m_filterModel->invalidate();
        rebuildEvidenceTable();
    }
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
