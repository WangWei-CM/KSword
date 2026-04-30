#include "KernelDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <thread>
#include <utility>

namespace
{
    // DynDataColumn：
    // - 作用：定义动态偏移字段表列索引；
    // - 使用 enum class 避免魔法数字扩散到渲染与读取逻辑。
    enum class DynDataColumn : int
    {
        Field = 0,
        Offset,
        Status,
        Source,
        Feature,
        Capability,
        Count
    };

    // SummaryColumn：
    // - 作用：定义摘要表两列布局；
    // - Field/Value 模式便于添加 R0 诊断项。
    enum class SummaryColumn : int
    {
        Name = 0,
        Value,
        Count
    };

    // CapabilityDisplay：
    // - 作用：把 capability bit 与 UI 文案绑定；
    // - 处理逻辑：刷新摘要和字段详情时复用同一张表。
    struct CapabilityDisplay
    {
        std::uint64_t mask = 0;      // mask：KSW_CAP_* 单 bit。
        const char* name = nullptr;  // name：英文稳定名称。
        const wchar_t* title = nullptr; // title：中文功能说明。
    };

    // kCapabilities：
    // - 作用：枚举 Phase 0 暴露的全部 capability；
    // - 返回行为：由 helper 函数格式化为摘要、详情或缺失列表。
    constexpr std::array<CapabilityDisplay, 11> kCapabilities{ {
        { KSW_CAP_DYN_NTOS_ACTIVE, "KSW_CAP_DYN_NTOS_ACTIVE", L"ntoskrnl profile 已激活" },
        { KSW_CAP_DYN_LXCORE_ACTIVE, "KSW_CAP_DYN_LXCORE_ACTIVE", L"lxcore profile 已激活" },
        { KSW_CAP_OBJECT_TYPE_FIELDS, "KSW_CAP_OBJECT_TYPE_FIELDS", L"对象类型字段" },
        { KSW_CAP_HANDLE_TABLE_DECODE, "KSW_CAP_HANDLE_TABLE_DECODE", L"句柄表解码" },
        { KSW_CAP_PROCESS_OBJECT_TABLE, "KSW_CAP_PROCESS_OBJECT_TABLE", L"进程 ObjectTable" },
        { KSW_CAP_THREAD_STACK_FIELDS, "KSW_CAP_THREAD_STACK_FIELDS", L"线程栈字段" },
        { KSW_CAP_THREAD_IO_COUNTERS, "KSW_CAP_THREAD_IO_COUNTERS", L"线程 I/O 计数" },
        { KSW_CAP_ALPC_FIELDS, "KSW_CAP_ALPC_FIELDS", L"ALPC 字段" },
        { KSW_CAP_SECTION_CONTROL_AREA, "KSW_CAP_SECTION_CONTROL_AREA", L"Section/ControlArea" },
        { KSW_CAP_PROCESS_PROTECTION_PATCH, "KSW_CAP_PROCESS_PROTECTION_PATCH", L"进程保护修改" },
        { KSW_CAP_WSL_LXCORE_FIELDS, "KSW_CAP_WSL_LXCORE_FIELDS", L"WSL/lxcore 字段" }
    } };

    // blueButtonStyle：
    // - 输入：无；
    // - 处理：读取全局主题按钮样式；
    // - 返回：可直接 setStyleSheet 的按钮样式文本。
    QString blueButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    // blueInputStyle：
    // - 输入：无；
    // - 处理：使用主题色拼接 QLineEdit 样式；
    // - 返回：筛选框样式文本。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // headerStyle：
    // - 输入：无；
    // - 处理：使用主题色拼接表头样式；
    // - 返回：可直接应用到 QHeaderView 的样式文本。
    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // itemSelectionStyle：
    // - 输入：无；
    // - 处理：统一表格选中态颜色；
    // - 返回：表格 selection 样式文本。
    QString itemSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // statusLabelStyle：
    // - 输入 colorHex：目标文字颜色；
    // - 处理：拼接 QLabel 样式；
    // - 返回：加粗状态文本样式。
    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    // safeText：
    // - 输入 valueText/fallbackText：待展示文本和兜底文本；
    // - 处理：去除首尾空白后判断是否为空；
    // - 返回：非空原文或兜底占位符。
    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    // stringToQString：
    // - 输入 valueText：ArkDriverClient 返回的 UTF-8/ANSI 小字符串；
    // - 处理：按 UTF-8 转换，兼容 ASCII 字段名；
    // - 返回：Qt 展示字符串。
    QString stringToQString(const std::string& valueText)
    {
        return QString::fromUtf8(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    // wideStringToQString：
    // - 输入 valueText：ArkDriverClient 返回的宽字符串；
    // - 处理：按 wchar_t 数组转换；
    // - 返回：Qt 展示字符串。
    QString wideStringToQString(const std::wstring& valueText)
    {
        return QString::fromWCharArray(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    // formatHex32：
    // - 输入 value：32 位数值；
    // - 处理：补零并使用大写十六进制；
    // - 返回：0xXXXXXXXX 格式文本。
    QString formatHex32(const std::uint32_t value)
    {
        return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper();
    }

    // formatHex64：
    // - 输入 value：64 位数值；
    // - 处理：补零并使用大写十六进制；
    // - 返回：0xXXXXXXXXXXXXXXXX 格式文本。
    QString formatHex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1").arg(value, 16, 16, QChar('0')).toUpper();
    }

    // formatNtStatus：
    // - 输入 statusValue：NTSTATUS signed long；
    // - 处理：保留底层 32 bit 原样展示；
    // - 返回：0xXXXXXXXX 格式文本。
    QString formatNtStatus(const long statusValue)
    {
        return formatHex32(static_cast<std::uint32_t>(statusValue));
    }

    // formatOffset：
    // - 输入 offsetValue：字段偏移；
    // - 处理：识别 DynData 不可用哨兵；
    // - 返回：可读偏移文本或 <不可用>。
    QString formatOffset(const std::uint32_t offsetValue)
    {
        if (offsetValue == 0xFFFFFFFFU || offsetValue == 0x0000FFFFU)
        {
            return QStringLiteral("<不可用>");
        }
        return QStringLiteral("0x%1").arg(offsetValue, 4, 16, QChar('0')).toUpper();
    }

    // fieldPresent：
    // - 输入 flags/offset：R0 字段 flags 和偏移值；
    // - 处理：同时检查 PRESENT bit 与不可用哨兵；
    // - 返回：true 表示字段可用。
    bool fieldPresent(const std::uint32_t flags, const std::uint32_t offset)
    {
        return (flags & KSW_DYN_FIELD_FLAG_PRESENT) != 0U &&
            offset != 0xFFFFFFFFU &&
            offset != 0x0000FFFFU;
    }

    // statusFlagEnabled：
    // - 输入 flags/flag：状态位图和目标 bit；
    // - 处理：按位检测；
    // - 返回：true 表示目标状态启用。
    bool statusFlagEnabled(const std::uint32_t flags, const std::uint32_t flag)
    {
        return (flags & flag) == flag;
    }

    // boolText：
    // - 输入 enabled：布尔状态；
    // - 处理：转换为中文；
    // - 返回：“是”或“否”。
    QString boolText(const bool enabled)
    {
        return enabled ? QStringLiteral("是") : QStringLiteral("否");
    }

    // moduleClassText：
    // - 输入 classId：KSW_DYN_PROFILE_CLASS_*；
    // - 处理：转换为 UI 可读文本；
    // - 返回：profile class 文案。
    QString moduleClassText(const std::uint32_t classId)
    {
        switch (classId)
        {
        case KSW_DYN_PROFILE_CLASS_NTOSKRNL:
            return QStringLiteral("ntoskrnl");
        case KSW_DYN_PROFILE_CLASS_NTKRLA57:
            return QStringLiteral("ntkrla57");
        case KSW_DYN_PROFILE_CLASS_LXCORE:
            return QStringLiteral("lxcore");
        default:
            return QStringLiteral("unknown(%1)").arg(classId);
        }
    }

    // sourceText：
    // - 输入 source：KSW_DYN_FIELD_SOURCE_*；
    // - 处理：转换为 UI 可读文本；
    // - 返回：字段来源文案。
    QString sourceText(const std::uint32_t source)
    {
        switch (source)
        {
        case KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER:
            return QStringLiteral("System Informer");
        case KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN:
            return QStringLiteral("Ksword runtime pattern");
        case KSW_DYN_FIELD_SOURCE_KSWORD_EXTRA_TABLE:
            return QStringLiteral("Ksword extra table");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // capabilityNames：
    // - 输入 mask：能力位图；
    // - 处理：遍历能力表并拼接命中名称；
    // - 返回：逗号分隔名称；无命中返回 None。
    QString capabilityNames(const std::uint64_t mask)
    {
        QStringList names;
        for (const CapabilityDisplay& capability : kCapabilities)
        {
            if ((mask & capability.mask) == capability.mask)
            {
                names << QString::fromLatin1(capability.name);
            }
        }
        return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
    }

    // capabilityReport：
    // - 输入 mask：能力位图；
    // - 处理：逐项列出启用/禁用；
    // - 返回：多行报告文本。
    QString capabilityReport(const std::uint64_t mask)
    {
        QStringList lines;
        for (const CapabilityDisplay& capability : kCapabilities)
        {
            const bool enabled = (mask & capability.mask) == capability.mask;
            lines << QStringLiteral("%1 [%2] %3")
                .arg(enabled ? QStringLiteral("[ON]") : QStringLiteral("[OFF]"))
                .arg(QString::fromLatin1(capability.name))
                .arg(QString::fromWCharArray(capability.title));
        }
        return lines.join(QStringLiteral("\n"));
    }

    // disabledCapabilitySummary：
    // - 输入 mask：能力位图；
    // - 处理：收集未启用能力中文名；
    // - 返回：缺失能力摘要。
    QString disabledCapabilitySummary(const std::uint64_t mask)
    {
        QStringList disabledItems;
        for (const CapabilityDisplay& capability : kCapabilities)
        {
            if ((mask & capability.mask) != capability.mask)
            {
                disabledItems << QString::fromWCharArray(capability.title);
            }
        }
        return disabledItems.isEmpty() ? QStringLiteral("无") : disabledItems.join(QStringLiteral("、"));
    }

    // convertModuleIdentity：
    // - 输入 source：ArkDriverClient 模块身份；
    // - 处理：转换到 KernelDock 内部模型；
    // - 返回：KernelDynDataModuleIdentity 值对象。
    KernelDynDataModuleIdentity convertModuleIdentity(const ksword::ark::ArkDynModuleIdentity& source)
    {
        KernelDynDataModuleIdentity result{};
        result.present = source.present;
        result.classId = source.classId;
        result.machine = source.machine;
        result.timeDateStamp = source.timeDateStamp;
        result.sizeOfImage = source.sizeOfImage;
        result.imageBase = source.imageBase;
        result.moduleNameText = wideStringToQString(source.moduleName);
        return result;
    }

    // moduleDetailText：
    // - 输入 title/source：模块标题和身份结构；
    // - 处理：格式化模块 identity；
    // - 返回：多行诊断文本。
    QString moduleDetailText(const QString& title, const KernelDynDataModuleIdentity& source)
    {
        if (!source.present)
        {
            return QStringLiteral("%1: <未加载或未识别>").arg(title);
        }

        return QStringLiteral(
            "%1:\n"
            "  ModuleName: %2\n"
            "  Class: %3 (%4)\n"
            "  Machine: %5\n"
            "  TimeDateStamp: %6\n"
            "  SizeOfImage: %7\n"
            "  ImageBase: %8")
            .arg(title)
            .arg(safeText(source.moduleNameText))
            .arg(moduleClassText(source.classId))
            .arg(source.classId)
            .arg(formatHex32(source.machine))
            .arg(formatHex32(source.timeDateStamp))
            .arg(formatHex32(source.sizeOfImage))
            .arg(formatHex64(source.imageBase));
    }

    // appendSummaryRow：
    // - 输入 table/name/value：摘要表、字段名和值；
    // - 处理：追加只读行；
    // - 返回：无。
    void appendSummaryRow(QTableWidget* table, const QString& nameText, const QString& valueText)
    {
        if (table == nullptr)
        {
            return;
        }

        const int rowIndex = table->rowCount();
        table->insertRow(rowIndex);

        auto* nameItem = new QTableWidgetItem(nameText);
        auto* valueItem = new QTableWidgetItem(valueText);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);

        table->setItem(rowIndex, static_cast<int>(SummaryColumn::Name), nameItem);
        table->setItem(rowIndex, static_cast<int>(SummaryColumn::Value), valueItem);
    }

    // setReadonlyItem：
    // - 输入 table/row/column/item：目标表、行列和 item；
    // - 处理：去掉可编辑 flag 后放入表格；
    // - 返回：无。
    void setReadonlyItem(QTableWidget* table, const int rowIndex, const DynDataColumn column, QTableWidgetItem* item)
    {
        if (table == nullptr || item == nullptr)
        {
            delete item;
            return;
        }

        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(rowIndex, static_cast<int>(column), item);
    }

    // buildFieldDetail：
    // - 输入 entry/summary：字段行和当前摘要；
    // - 处理：生成详情面板文本，包含能力依赖和全局状态；
    // - 返回：多行详情文本。
    QString buildFieldDetail(const KernelDynDataFieldEntry& entry, const KernelDynDataSummary& summary)
    {
        return QStringLiteral(
            "字段名: %1\n"
            "字段ID: %2\n"
            "偏移: %3\n"
            "状态: %4\n"
            "来源: %5\n"
            "功能: %6\n"
            "字段标志: %7\n"
            "字段能力位: %8\n"
            "字段能力名: %9\n\n"
            "当前全局能力位: %10\n"
            "当前未启用能力: %11\n\n"
            "R0不可用原因: %12")
            .arg(safeText(entry.fieldNameText))
            .arg(entry.fieldId)
            .arg(formatOffset(entry.offset))
            .arg(safeText(entry.statusText))
            .arg(safeText(entry.sourceNameText))
            .arg(safeText(entry.featureNameText))
            .arg(formatHex32(entry.flags))
            .arg(formatHex64(entry.capabilityMask))
            .arg(capabilityNames(entry.capabilityMask))
            .arg(formatHex64(summary.capabilityMask))
            .arg(disabledCapabilitySummary(summary.capabilityMask))
            .arg(safeText(summary.unavailableReasonText));
    }

    // buildDynDataReport：
    // - 输入 summary/rows：摘要和字段行；
    // - 处理：拼出可复制的完整诊断报告；
    // - 返回：多行报告文本。
    QString buildDynDataReport(const KernelDynDataSummary& summary, const std::vector<KernelDynDataFieldEntry>& rows)
    {
        QStringList lines;
        lines << QStringLiteral("Ksword DynData Diagnostic Report");
        lines << QStringLiteral("StatusQueryOk: %1").arg(boolText(summary.statusQueryOk));
        lines << QStringLiteral("FieldsQueryOk: %1").arg(boolText(summary.fieldsQueryOk));
        lines << QStringLiteral("StatusFlags: %1").arg(formatHex32(summary.statusFlags));
        lines << QStringLiteral("CapabilityMask: %1").arg(formatHex64(summary.capabilityMask));
        lines << QStringLiteral("SystemInformerDataVersion: %1").arg(summary.systemInformerDataVersion);
        lines << QStringLiteral("SystemInformerDataLength: %1").arg(summary.systemInformerDataLength);
        lines << QStringLiteral("LastStatus: %1").arg(formatNtStatus(summary.lastStatus));
        lines << QStringLiteral("MatchedProfileClass: %1").arg(moduleClassText(summary.matchedProfileClass));
        lines << QStringLiteral("MatchedProfileOffset: %1").arg(formatHex32(summary.matchedProfileOffset));
        lines << QStringLiteral("MatchedFieldsId: %1").arg(summary.matchedFieldsId);
        lines << QStringLiteral("UnavailableReason: %1").arg(safeText(summary.unavailableReasonText));
        lines << moduleDetailText(QStringLiteral("ntoskrnl"), summary.ntoskrnl);
        lines << moduleDetailText(QStringLiteral("lxcore"), summary.lxcore);
        lines << QStringLiteral("");
        lines << QStringLiteral("Capabilities:");
        lines << capabilityReport(summary.capabilityMask);
        lines << QStringLiteral("");
        lines << QStringLiteral("Fields:");
        for (const KernelDynDataFieldEntry& entry : rows)
        {
            lines << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6")
                .arg(safeText(entry.fieldNameText))
                .arg(formatOffset(entry.offset))
                .arg(safeText(entry.statusText))
                .arg(safeText(entry.sourceNameText))
                .arg(safeText(entry.featureNameText))
                .arg(formatHex64(entry.capabilityMask));
        }
        return lines.join(QStringLiteral("\n"));
    }

    // populateSummaryTable：
    // - 输入 table/summary/visibleRows：摘要表、摘要数据和当前字段总数；
    // - 处理：重建两列表格；
    // - 返回：无。
    void populateSummaryTable(QTableWidget* table, const KernelDynDataSummary& summary, const std::size_t visibleRows)
    {
        if (table == nullptr)
        {
            return;
        }

        table->setSortingEnabled(false);
        table->setRowCount(0);
        appendSummaryRow(table, QStringLiteral("DynData 初始化"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_INITIALIZED)));
        appendSummaryRow(table, QStringLiteral("ntoskrnl profile"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("lxcore profile"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("Ksword runtime offset"), boolText(statusFlagEnabled(summary.statusFlags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE)));
        appendSummaryRow(table, QStringLiteral("System Informer 版本"), QString::number(summary.systemInformerDataVersion));
        appendSummaryRow(table, QStringLiteral("System Informer 数据长度"), QString::number(summary.systemInformerDataLength));
        appendSummaryRow(table, QStringLiteral("LastStatus"), formatNtStatus(summary.lastStatus));
        appendSummaryRow(table, QStringLiteral("MatchedProfileClass"), QStringLiteral("%1 (%2)").arg(moduleClassText(summary.matchedProfileClass)).arg(summary.matchedProfileClass));
        appendSummaryRow(table, QStringLiteral("MatchedProfileOffset"), formatHex32(summary.matchedProfileOffset));
        appendSummaryRow(table, QStringLiteral("MatchedFieldsId"), QString::number(summary.matchedFieldsId));
        appendSummaryRow(table, QStringLiteral("CapabilityMask"), formatHex64(summary.capabilityMask));
        appendSummaryRow(table, QStringLiteral("字段总数/当前返回"), QStringLiteral("%1 / %2").arg(summary.fieldCount).arg(visibleRows));
        appendSummaryRow(table, QStringLiteral("禁用能力"), disabledCapabilitySummary(summary.capabilityMask));
        appendSummaryRow(table, QStringLiteral("不可用原因"), safeText(summary.unavailableReasonText));
        appendSummaryRow(table, QStringLiteral("Status IO"), safeText(summary.statusIoMessageText));
        appendSummaryRow(table, QStringLiteral("Fields IO"), safeText(summary.fieldsIoMessageText));
        appendSummaryRow(table, QStringLiteral("ntoskrnl"), moduleDetailText(QStringLiteral("ntoskrnl"), summary.ntoskrnl).replace(QStringLiteral("\n"), QStringLiteral("; ")));
        appendSummaryRow(table, QStringLiteral("lxcore"), moduleDetailText(QStringLiteral("lxcore"), summary.lxcore).replace(QStringLiteral("\n"), QStringLiteral("; ")));
        table->setSortingEnabled(false);
    }

    // shouldShowField：
    // - 输入 entry/filterKeyword：字段行和筛选关键字；
    // - 处理：在字段名、偏移、来源、功能、状态和能力名中匹配；
    // - 返回：true 表示该行应显示。
    bool shouldShowField(const KernelDynDataFieldEntry& entry, const QString& filterKeyword)
    {
        if (filterKeyword.isEmpty())
        {
            return true;
        }

        return entry.fieldNameText.contains(filterKeyword, Qt::CaseInsensitive) ||
            formatOffset(entry.offset).contains(filterKeyword, Qt::CaseInsensitive) ||
            entry.statusText.contains(filterKeyword, Qt::CaseInsensitive) ||
            entry.sourceNameText.contains(filterKeyword, Qt::CaseInsensitive) ||
            entry.featureNameText.contains(filterKeyword, Qt::CaseInsensitive) ||
            capabilityNames(entry.capabilityMask).contains(filterKeyword, Qt::CaseInsensitive);
    }

    // queryDynDataSnapshot：
    // - 输入 summaryOut/rowsOut：输出摘要和字段缓存；
    // - 处理：通过 ArkDriverClient 查询三个 DynData IOCTL 并转换模型；
    // - 返回：true 表示至少 status 和 fields 查询都成功。
    bool queryDynDataSnapshot(KernelDynDataSummary& summaryOut, std::vector<KernelDynDataFieldEntry>& rowsOut)
    {
        ksword::ark::DriverClient client;
        const ksword::ark::DynDataStatusResult statusResult = client.queryDynDataStatus();
        const ksword::ark::DynDataFieldsResult fieldsResult = client.queryDynDataFields();
        const ksword::ark::DynDataCapabilitiesResult capabilitiesResult = client.queryDynDataCapabilities();

        summaryOut = KernelDynDataSummary{};
        rowsOut.clear();

        summaryOut.statusQueryOk = statusResult.io.ok;
        summaryOut.fieldsQueryOk = fieldsResult.io.ok;
        summaryOut.statusIoMessageText = QString::fromStdString(statusResult.io.message);
        summaryOut.fieldsIoMessageText = QString::fromStdString(fieldsResult.io.message);

        if (statusResult.io.ok)
        {
            summaryOut.statusFlags = statusResult.statusFlags;
            summaryOut.systemInformerDataVersion = statusResult.systemInformerDataVersion;
            summaryOut.systemInformerDataLength = statusResult.systemInformerDataLength;
            summaryOut.lastStatus = statusResult.lastStatus;
            summaryOut.matchedProfileClass = statusResult.matchedProfileClass;
            summaryOut.matchedProfileOffset = statusResult.matchedProfileOffset;
            summaryOut.matchedFieldsId = statusResult.matchedFieldsId;
            summaryOut.fieldCount = statusResult.fieldCount;
            summaryOut.capabilityMask = statusResult.capabilityMask;
            summaryOut.ntoskrnl = convertModuleIdentity(statusResult.ntoskrnl);
            summaryOut.lxcore = convertModuleIdentity(statusResult.lxcore);
            summaryOut.unavailableReasonText = wideStringToQString(statusResult.unavailableReason);
        }

        if (capabilitiesResult.io.ok)
        {
            summaryOut.capabilityMask = capabilitiesResult.capabilityMask;
            summaryOut.statusFlags = capabilitiesResult.statusFlags != 0U ? capabilitiesResult.statusFlags : summaryOut.statusFlags;
        }

        if (fieldsResult.io.ok)
        {
            rowsOut.reserve(fieldsResult.entries.size());
            for (const ksword::ark::DynDataFieldEntry& sourceEntry : fieldsResult.entries)
            {
                KernelDynDataFieldEntry row{};
                row.fieldId = sourceEntry.fieldId;
                row.flags = sourceEntry.flags;
                row.source = sourceEntry.source;
                row.offset = sourceEntry.offset;
                row.capabilityMask = sourceEntry.capabilityMask;
                row.fieldNameText = stringToQString(sourceEntry.fieldName);
                row.sourceNameText = !sourceEntry.sourceName.empty()
                    ? stringToQString(sourceEntry.sourceName)
                    : sourceText(sourceEntry.source);
                row.featureNameText = stringToQString(sourceEntry.featureName);
                row.statusText = fieldPresent(row.flags, row.offset)
                    ? QStringLiteral("可用")
                    : (row.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U
                        ? QStringLiteral("缺失(必需)")
                        : QStringLiteral("缺失(可选)");
                row.detailText = buildFieldDetail(row, summaryOut);
                rowsOut.push_back(row);
            }
        }

        return summaryOut.statusQueryOk && summaryOut.fieldsQueryOk;
    }
}

void KernelDock::initializeDynDataTab()
{
    if (m_dynDataPage == nullptr || m_dynDataLayout != nullptr)
    {
        return;
    }

    // 顶层布局：工具栏 + 上下分割区域。上半部分摘要，下半部分字段表和详情。
    m_dynDataLayout = new QVBoxLayout(m_dynDataPage);
    m_dynDataLayout->setContentsMargins(4, 4, 4, 4);
    m_dynDataLayout->setSpacing(6);

    m_dynDataToolLayout = new QHBoxLayout();
    m_dynDataToolLayout->setContentsMargins(0, 0, 0, 0);
    m_dynDataToolLayout->setSpacing(6);

    m_refreshDynDataButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_dynDataPage);
    m_refreshDynDataButton->setToolTip(QStringLiteral("刷新 R0 DynData 状态和字段表"));
    m_refreshDynDataButton->setStyleSheet(blueButtonStyle());
    m_refreshDynDataButton->setFixedWidth(34);

    m_copyDynDataReportButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制诊断"), m_dynDataPage);
    m_copyDynDataReportButton->setToolTip(QStringLiteral("复制 DynData 状态、能力和字段列表到剪贴板"));
    m_copyDynDataReportButton->setStyleSheet(blueButtonStyle());

    m_dynDataFilterEdit = new QLineEdit(m_dynDataPage);
    m_dynDataFilterEdit->setPlaceholderText(QStringLiteral("按字段名/偏移/状态/来源/功能/capability 筛选"));
    m_dynDataFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤动态偏移字段表"));
    m_dynDataFilterEdit->setClearButtonEnabled(true);
    m_dynDataFilterEdit->setStyleSheet(blueInputStyle());

    m_dynDataStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_dynDataPage);
    m_dynDataStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_dynDataKernelBadge = new QLabel(m_dynDataPage);
    m_dynDataKernelBadge->setToolTip(QStringLiteral("Kernel/R0 数据来源标识"));
    m_dynDataKernelBadge->setPixmap(QPixmap(QStringLiteral(":/Image/kernel_badge.png")).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_dynDataKernelBadge->setFixedSize(24, 24);

    m_dynDataToolLayout->addWidget(m_refreshDynDataButton, 0);
    m_dynDataToolLayout->addWidget(m_copyDynDataReportButton, 0);
    m_dynDataToolLayout->addWidget(m_dynDataFilterEdit, 1);
    m_dynDataToolLayout->addWidget(m_dynDataKernelBadge, 0);
    m_dynDataToolLayout->addWidget(m_dynDataStatusLabel, 0);
    m_dynDataLayout->addLayout(m_dynDataToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_dynDataPage);
    m_dynDataLayout->addWidget(verticalSplitter, 1);

    m_dynDataSummaryTable = new QTableWidget(verticalSplitter);
    m_dynDataSummaryTable->setColumnCount(static_cast<int>(SummaryColumn::Count));
    m_dynDataSummaryTable->setHorizontalHeaderLabels(QStringList{ QStringLiteral("项目"), QStringLiteral("值") });
    m_dynDataSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_dynDataSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dynDataSummaryTable->setAlternatingRowColors(true);
    m_dynDataSummaryTable->setStyleSheet(itemSelectionStyle());
    m_dynDataSummaryTable->setCornerButtonEnabled(false);
    m_dynDataSummaryTable->verticalHeader()->setVisible(false);
    m_dynDataSummaryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_dynDataSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_dynDataSummaryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(SummaryColumn::Value), QHeaderView::Stretch);
    m_dynDataSummaryTable->setColumnWidth(static_cast<int>(SummaryColumn::Name), 220);
    m_dynDataSummaryTable->setToolTip(QStringLiteral("DynData 精确匹配、模块身份和 capability 摘要"));

    QSplitter* lowerSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    m_dynDataFieldTable = new QTableWidget(lowerSplitter);
    m_dynDataFieldTable->setColumnCount(static_cast<int>(DynDataColumn::Count));
    m_dynDataFieldTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("字段"),
        QStringLiteral("偏移"),
        QStringLiteral("状态"),
        QStringLiteral("来源"),
        QStringLiteral("功能"),
        QStringLiteral("Capability")
        });
    m_dynDataFieldTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_dynDataFieldTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_dynDataFieldTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dynDataFieldTable->setAlternatingRowColors(true);
    m_dynDataFieldTable->setStyleSheet(itemSelectionStyle());
    m_dynDataFieldTable->setCornerButtonEnabled(false);
    m_dynDataFieldTable->verticalHeader()->setVisible(false);
    m_dynDataFieldTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_dynDataFieldTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_dynDataFieldTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(DynDataColumn::Field), QHeaderView::Stretch);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Offset), 100);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Status), 110);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Source), 180);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Feature), 180);
    m_dynDataFieldTable->setColumnWidth(static_cast<int>(DynDataColumn::Capability), 180);

    m_dynDataDetailEditor = new CodeEditorWidget(lowerSplitter);
    m_dynDataDetailEditor->setReadOnly(true);
    m_dynDataDetailEditor->setText(QStringLiteral("请选择一条动态偏移字段查看详情。"));

    verticalSplitter->setStretchFactor(0, 2);
    verticalSplitter->setStretchFactor(1, 5);
    lowerSplitter->setStretchFactor(0, 3);
    lowerSplitter->setStretchFactor(1, 2);

    // 信号连接：刷新、筛选、当前行详情和报告复制都在本页内部完成。
    connect(m_refreshDynDataButton, &QPushButton::clicked, this, [this]() {
        refreshDynDataAsync();
    });
    connect(m_copyDynDataReportButton, &QPushButton::clicked, this, [this]() {
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard != nullptr)
        {
            clipboard->setText(buildDynDataReport(m_dynDataSummary, m_dynDataRows));
            m_dynDataStatusLabel->setText(QStringLiteral("状态：诊断报告已复制"));
        }
    });
    connect(m_dynDataFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildDynDataFieldTable(filterText.trimmed());
    });
    connect(m_dynDataFieldTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showDynDataDetailByCurrentRow();
    });
}

void KernelDock::refreshDynDataAsync()
{
    if (m_dynDataRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] DynData 刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshDynDataButton->setEnabled(false);
    m_dynDataStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_dynDataStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        KernelDynDataSummary summary;
        std::vector<KernelDynDataFieldEntry> rows;
        const bool success = queryDynDataSnapshot(summary, rows);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, summary = std::move(summary), rows = std::move(rows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_dynDataRefreshRunning.store(false);
            guardThis->m_refreshDynDataButton->setEnabled(true);
            guardThis->m_dynDataSummary = std::move(summary);
            guardThis->m_dynDataRows = std::move(rows);

            populateSummaryTable(
                guardThis->m_dynDataSummaryTable,
                guardThis->m_dynDataSummary,
                guardThis->m_dynDataRows.size());
            guardThis->rebuildDynDataFieldTable(guardThis->m_dynDataFilterEdit->text().trimmed());

            const std::size_t missingRequiredCount = static_cast<std::size_t>(
                std::count_if(
                    guardThis->m_dynDataRows.begin(),
                    guardThis->m_dynDataRows.end(),
                    [](const KernelDynDataFieldEntry& entry) {
                        return (entry.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U &&
                            !fieldPresent(entry.flags, entry.offset);
                    }));

            if (!success)
            {
                guardThis->m_dynDataStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_dynDataStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_dynDataDetailEditor->setText(buildDynDataReport(guardThis->m_dynDataSummary, guardThis->m_dynDataRows));
            }
            else
            {
                const bool ntosActive = statusFlagEnabled(guardThis->m_dynDataSummary.statusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE);
                guardThis->m_dynDataStatusLabel->setText(
                    QStringLiteral("状态：%1，字段 %2 项，缺失必需 %3 项")
                    .arg(ntosActive ? QStringLiteral("ntos profile 已命中") : QStringLiteral("ntos profile 未命中"))
                    .arg(guardThis->m_dynDataRows.size())
                    .arg(missingRequiredCount));
                guardThis->m_dynDataStatusLabel->setStyleSheet(
                    statusLabelStyle(ntosActive && missingRequiredCount == 0U ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));

                if (guardThis->m_dynDataFieldTable->rowCount() > 0)
                {
                    guardThis->m_dynDataFieldTable->setCurrentCell(0, 0);
                }
                else
                {
                    guardThis->m_dynDataDetailEditor->setText(QStringLiteral("当前筛选条件下没有动态偏移字段。"));
                }
            }

            kLogEvent doneEvent;
            info << doneEvent
                << "[KernelDock] DynData 刷新完成, success="
                << success
                << ", fields="
                << guardThis->m_dynDataRows.size()
                << ", caps="
                << formatHex64(guardThis->m_dynDataSummary.capabilityMask)
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildDynDataFieldTable(const QString& filterKeyword)
{
    if (m_dynDataFieldTable == nullptr)
    {
        return;
    }

    m_dynDataFieldTable->setSortingEnabled(false);
    m_dynDataFieldTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_dynDataRows.size(); ++sourceIndex)
    {
        const KernelDynDataFieldEntry& entry = m_dynDataRows[sourceIndex];
        if (!shouldShowField(entry, filterKeyword))
        {
            continue;
        }

        const int rowIndex = m_dynDataFieldTable->rowCount();
        m_dynDataFieldTable->insertRow(rowIndex);

        auto* fieldItem = new QTableWidgetItem(safeText(entry.fieldNameText));
        auto* offsetItem = new QTableWidgetItem(formatOffset(entry.offset));
        auto* statusItem = new QTableWidgetItem(safeText(entry.statusText));
        auto* sourceItem = new QTableWidgetItem(safeText(entry.sourceNameText));
        auto* featureItem = new QTableWidgetItem(safeText(entry.featureNameText));
        auto* capabilityItem = new QTableWidgetItem(formatHex64(entry.capabilityMask));

        fieldItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        if (!fieldPresent(entry.flags, entry.offset))
        {
            statusItem->setForeground(QBrush((entry.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U
                ? QColor(QStringLiteral("#B23A3A"))
                : KswordTheme::WarningAccentColor()));
        }
        else
        {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#3A8F3A"))));
        }

        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Field, fieldItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Offset, offsetItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Status, statusItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Source, sourceItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Feature, featureItem);
        setReadonlyItem(m_dynDataFieldTable, rowIndex, DynDataColumn::Capability, capabilityItem);
    }

    m_dynDataFieldTable->setSortingEnabled(true);
}

bool KernelDock::currentDynDataFieldSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;

    if (m_dynDataFieldTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_dynDataFieldTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* fieldItem = m_dynDataFieldTable->item(currentRow, static_cast<int>(DynDataColumn::Field));
    if (fieldItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(fieldItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_dynDataRows.size();
}

const KernelDynDataFieldEntry* KernelDock::currentDynDataFieldEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentDynDataFieldSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_dynDataRows[sourceIndex];
}

void KernelDock::showDynDataDetailByCurrentRow()
{
    if (m_dynDataDetailEditor == nullptr)
    {
        return;
    }

    const KernelDynDataFieldEntry* entry = currentDynDataFieldEntry();
    if (entry == nullptr)
    {
        m_dynDataDetailEditor->setText(buildDynDataReport(m_dynDataSummary, m_dynDataRows));
        return;
    }

    m_dynDataDetailEditor->setText(QStringLiteral(
        "%1\n\n"
        "模块身份:\n"
        "%2\n\n"
        "%3\n\n"
        "Capability 状态:\n"
        "%4")
        .arg(entry->detailText)
        .arg(moduleDetailText(QStringLiteral("ntoskrnl"), m_dynDataSummary.ntoskrnl))
        .arg(moduleDetailText(QStringLiteral("lxcore"), m_dynDataSummary.lxcore))
        .arg(capabilityReport(m_dynDataSummary.capabilityMask)));
}
