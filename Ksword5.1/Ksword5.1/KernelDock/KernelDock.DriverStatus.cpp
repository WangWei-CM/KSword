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
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

namespace
{
    // DriverSummaryColumn and DriverCapabilityColumn keep table layout explicit.
    enum class DriverSummaryColumn : int { Name = 0, Value, Count };
    enum class DriverCapabilityColumn : int { Feature = 0, State, Policy, RequiredDyn, PresentDyn, Dependency, Reason, Count };

    struct CapabilityDisplay
    {
        std::uint64_t mask = 0;
        const char* name = nullptr;
        const wchar_t* title = nullptr;
    };

    struct PolicyDisplay
    {
        std::uint32_t mask = 0;
        const char* name = nullptr;
        const wchar_t* title = nullptr;
    };

    // kDynCapabilities：
    // - 作用：列出所有可由 R0 DynData 暴露的 capability bit；
    // - 处理逻辑：驱动状态页、能力详情和筛选都复用该表。
    constexpr std::array<CapabilityDisplay, 12> kDynCapabilities{ {
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
        { KSW_CAP_WSL_LXCORE_FIELDS, "KSW_CAP_WSL_LXCORE_FIELDS", L"WSL/lxcore 字段" },
        { KSW_CAP_ETW_GUID_FIELDS, "KSW_CAP_ETW_GUID_FIELDS", L"ETW GUID/Registration 字段" }
    } };

    // kSecurityPolicies lists every policy bit currently surfaced by Phase 1.
    constexpr std::array<PolicyDisplay, 6> kSecurityPolicies{ {
        { KSWORD_ARK_SECURITY_POLICY_FLAG_ACTIVE, "POLICY_ACTIVE", L"安全策略启用" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_MUTATING_ACTIONS, "ALLOW_MUTATING_ACTIONS", L"允许进程修改动作" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_FILE_DELETE, "ALLOW_FILE_DELETE", L"允许文件删除" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_CALLBACK_CONTROL, "ALLOW_CALLBACK_CONTROL", L"允许回调控制" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_PROCESS_PROTECTION, "ALLOW_PROCESS_PROTECTION", L"允许进程保护修改" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_KERNEL_SNAPSHOTS, "ALLOW_KERNEL_SNAPSHOTS", L"允许内核快照" }
    } };

    QString blueButtonStyle() { return KswordTheme::ThemedButtonStyle(); }

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

    QString headerStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString itemSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}").arg(KswordTheme::PrimaryBlueHex);
    }

    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString stringToQString(const std::string& valueText)
    {
        return QString::fromUtf8(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    QString formatHex32(const std::uint32_t value) { return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper(); }
    QString formatHex64(const std::uint64_t value) { return QStringLiteral("0x%1").arg(value, 16, 16, QChar('0')).toUpper(); }
    QString formatNtStatus(const long value) { return formatHex32(static_cast<std::uint32_t>(value)); }
    QString boolText(const bool value) { return value ? QStringLiteral("是") : QStringLiteral("否"); }
    bool flagEnabled(const std::uint32_t flags, const std::uint32_t flag) { return (flags & flag) == flag; }

    QString capabilityNames(const std::uint64_t mask)
    {
        QStringList names;
        for (const CapabilityDisplay& item : kDynCapabilities)
        {
            if ((mask & item.mask) == item.mask)
            {
                names << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(item.name)).arg(QString::fromWCharArray(item.title));
            }
        }
        return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
    }

    QString policyNames(const std::uint32_t mask)
    {
        QStringList names;
        for (const PolicyDisplay& item : kSecurityPolicies)
        {
            if ((mask & item.mask) == item.mask)
            {
                names << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(item.name)).arg(QString::fromWCharArray(item.title));
            }
        }
        return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
    }

    QString statusBadges(const KernelDriverStatusSummary& summary)
    {
        QStringList badges;
        badges << (summary.driverLoaded ? QStringLiteral("Driver Loaded") : QStringLiteral("Driver Missing"));
        if (!summary.protocolOk) { badges << QStringLiteral("Protocol Mismatch"); }
        if (summary.dynDataMissing) { badges << QStringLiteral("DynData Missing"); }
        if (summary.limited) { badges << QStringLiteral("Limited"); }
        return badges.join(QStringLiteral(", "));
    }

    QString dynDataStatusText(const std::uint32_t flags)
    {
        QStringList parts;
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_INITIALIZED)) { parts << QStringLiteral("Initialized"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE)) { parts << QStringLiteral("NtosActive"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE)) { parts << QStringLiteral("LxcoreActive"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE)) { parts << QStringLiteral("ExtraActive"); }
        return parts.isEmpty() ? QStringLiteral("None") : parts.join(QStringLiteral(", "));
    }

    QString featureFlagText(const std::uint32_t flags)
    {
        QStringList parts;
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA)) { parts << QStringLiteral("Requires DynData"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_MUTATING)) { parts << QStringLiteral("Mutating"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY)) { parts << QStringLiteral("Kernel Only"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_READ_ONLY)) { parts << QStringLiteral("Read Only"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_POLICY_GATED)) { parts << QStringLiteral("Policy Gated"); }
        return parts.isEmpty() ? QStringLiteral("None") : parts.join(QStringLiteral(", "));
    }

    QString stateText(const std::uint32_t state, const QString& fallbackText)
    {
        switch (state)
        {
        case KSWORD_ARK_FEATURE_STATE_AVAILABLE: return QStringLiteral("Available");
        case KSWORD_ARK_FEATURE_STATE_UNAVAILABLE: return QStringLiteral("Unavailable");
        case KSWORD_ARK_FEATURE_STATE_DEGRADED: return QStringLiteral("Degraded");
        case KSWORD_ARK_FEATURE_STATE_DENIED_BY_POLICY: return QStringLiteral("Denied by policy");
        default: return safeText(fallbackText, QStringLiteral("Unknown"));
        }
    }

    QBrush stateBrush(const std::uint32_t state)
    {
        if (state == KSWORD_ARK_FEATURE_STATE_AVAILABLE) { return QBrush(QColor(QStringLiteral("#3A8F3A"))); }
        if (state == KSWORD_ARK_FEATURE_STATE_DEGRADED) { return QBrush(QColor(QStringLiteral("#D77A00"))); }
        if (state == KSWORD_ARK_FEATURE_STATE_DENIED_BY_POLICY) { return QBrush(QColor(QStringLiteral("#7A4DB3"))); }
        return QBrush(QColor(QStringLiteral("#B23A3A")));
    }

    void appendSummaryRow(QTableWidget* table, const QString& nameText, const QString& valueText)
    {
        if (table == nullptr) { return; }
        const int row = table->rowCount();
        table->insertRow(row);
        auto* nameItem = new QTableWidgetItem(nameText);
        auto* valueItem = new QTableWidgetItem(valueText);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, static_cast<int>(DriverSummaryColumn::Name), nameItem);
        table->setItem(row, static_cast<int>(DriverSummaryColumn::Value), valueItem);
    }

    void setReadonlyItem(QTableWidget* table, const int row, const DriverCapabilityColumn column, QTableWidgetItem* item)
    {
        if (table == nullptr || item == nullptr) { delete item; return; }
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, static_cast<int>(column), item);
    }

    QString buildCapabilityDetail(const KernelDriverCapabilityEntry& entry, const KernelDriverStatusSummary& summary)
    {
        return QStringLiteral(
            "功能: %1\nFeatureId: %2\n状态: %3\n功能标志: %4 (%5)\n\n"
            "依赖字段: %6\n状态原因: %7\n\n"
            "所需安全策略: %8 (%9)\n被拒绝策略位: %10 (%11)\n"
            "所需 DynData capability: %12 (%13)\n已满足 DynData capability: %14 (%15)\n"
            "全局 DynData capability: %16 (%17)\n\n驱动状态: %18\n最近 R0 错误: %19 / %20 / %21")
            .arg(safeText(entry.featureNameText)).arg(entry.featureId).arg(stateText(entry.state, entry.stateNameText))
            .arg(formatHex32(entry.flags)).arg(featureFlagText(entry.flags))
            .arg(safeText(entry.dependencyText, QStringLiteral("None"))).arg(safeText(entry.reasonText, QStringLiteral("Feature is available.")))
            .arg(formatHex32(entry.requiredPolicyFlags)).arg(policyNames(entry.requiredPolicyFlags))
            .arg(formatHex32(entry.deniedPolicyFlags)).arg(policyNames(entry.deniedPolicyFlags))
            .arg(formatHex64(entry.requiredDynDataMask)).arg(capabilityNames(entry.requiredDynDataMask))
            .arg(formatHex64(entry.presentDynDataMask)).arg(capabilityNames(entry.presentDynDataMask))
            .arg(formatHex64(summary.dynDataCapabilityMask)).arg(capabilityNames(summary.dynDataCapabilityMask))
            .arg(statusBadges(summary)).arg(formatNtStatus(summary.lastErrorStatus))
            .arg(safeText(summary.lastErrorSourceText, QStringLiteral("None")))
            .arg(safeText(summary.lastErrorSummaryText, QStringLiteral("None")));
    }

    QString buildDriverStatusReport(const KernelDriverStatusSummary& summary, const std::vector<KernelDriverCapabilityEntry>& rows)
    {
        QStringList lines;
        lines << QStringLiteral("Ksword Driver Capability Diagnostic Report");
        lines << QStringLiteral("Status: %1").arg(statusBadges(summary));
        lines << QStringLiteral("QueryOk: %1").arg(boolText(summary.queryOk));
        lines << QStringLiteral("IoMessage: %1").arg(safeText(summary.ioMessageText));
        lines << QStringLiteral("CapabilityProtocolVersion: %1").arg(summary.version);
        lines << QStringLiteral("DriverProtocolVersion: %1").arg(formatHex32(summary.driverProtocolVersion));
        lines << QStringLiteral("ExpectedDriverProtocolVersion: %1").arg(formatHex32(KSWORD_ARK_DRIVER_PROTOCOL_VERSION));
        lines << QStringLiteral("StatusFlags: %1").arg(formatHex32(summary.statusFlags));
        lines << QStringLiteral("SecurityPolicyFlags: %1 (%2)").arg(formatHex32(summary.securityPolicyFlags)).arg(policyNames(summary.securityPolicyFlags));
        lines << QStringLiteral("DynDataStatusFlags: %1 (%2)").arg(formatHex32(summary.dynDataStatusFlags)).arg(dynDataStatusText(summary.dynDataStatusFlags));
        lines << QStringLiteral("DynDataCapabilityMask: %1 (%2)").arg(formatHex64(summary.dynDataCapabilityMask)).arg(capabilityNames(summary.dynDataCapabilityMask));
        lines << QStringLiteral("LastError: %1 / %2 / %3").arg(formatNtStatus(summary.lastErrorStatus)).arg(safeText(summary.lastErrorSourceText, QStringLiteral("None"))).arg(safeText(summary.lastErrorSummaryText, QStringLiteral("None")));
        lines << QStringLiteral("FeatureCount: returned=%1 total=%2").arg(summary.returnedFeatureCount).arg(summary.totalFeatureCount);
        lines << QStringLiteral("\nFeatures:");
        for (const KernelDriverCapabilityEntry& entry : rows)
        {
            lines << QStringLiteral("%1\t%2\tpolicy=%3\tdynRequired=%4\tdynPresent=%5\t%6\t%7")
                .arg(safeText(entry.featureNameText)).arg(stateText(entry.state, entry.stateNameText))
                .arg(formatHex32(entry.requiredPolicyFlags)).arg(formatHex64(entry.requiredDynDataMask))
                .arg(formatHex64(entry.presentDynDataMask)).arg(safeText(entry.dependencyText, QStringLiteral("None")))
                .arg(safeText(entry.reasonText, QStringLiteral("None")));
        }
        return lines.join(QStringLiteral("\n"));
    }

    void populateSummaryTable(QTableWidget* table, const KernelDriverStatusSummary& summary, const std::size_t visibleRows)
    {
        if (table == nullptr) { return; }
        table->setSortingEnabled(false);
        table->setRowCount(0);
        appendSummaryRow(table, QStringLiteral("状态栏"), statusBadges(summary));
        appendSummaryRow(table, QStringLiteral("Driver Loaded"), boolText(summary.driverLoaded));
        appendSummaryRow(table, QStringLiteral("Protocol OK"), boolText(summary.protocolOk));
        appendSummaryRow(table, QStringLiteral("DynData Missing"), boolText(summary.dynDataMissing));
        appendSummaryRow(table, QStringLiteral("Limited"), boolText(summary.limited));
        appendSummaryRow(table, QStringLiteral("能力协议版本"), QString::number(summary.version));
        appendSummaryRow(table, QStringLiteral("驱动协议版本"), formatHex32(summary.driverProtocolVersion));
        appendSummaryRow(table, QStringLiteral("期望协议版本"), formatHex32(KSWORD_ARK_DRIVER_PROTOCOL_VERSION));
        appendSummaryRow(table, QStringLiteral("状态位"), formatHex32(summary.statusFlags));
        appendSummaryRow(table, QStringLiteral("安全策略位"), QStringLiteral("%1 (%2)").arg(formatHex32(summary.securityPolicyFlags)).arg(policyNames(summary.securityPolicyFlags)));
        appendSummaryRow(table, QStringLiteral("DynData 状态位"), QStringLiteral("%1 (%2)").arg(formatHex32(summary.dynDataStatusFlags)).arg(dynDataStatusText(summary.dynDataStatusFlags)));
        appendSummaryRow(table, QStringLiteral("DynData 能力位"), QStringLiteral("%1 (%2)").arg(formatHex64(summary.dynDataCapabilityMask)).arg(capabilityNames(summary.dynDataCapabilityMask)));
        appendSummaryRow(table, QStringLiteral("功能数"), QStringLiteral("显示 %1 / 返回 %2 / 总计 %3").arg(visibleRows).arg(summary.returnedFeatureCount).arg(summary.totalFeatureCount));
        appendSummaryRow(table, QStringLiteral("最近错误"), QStringLiteral("%1 / %2 / %3").arg(formatNtStatus(summary.lastErrorStatus)).arg(safeText(summary.lastErrorSourceText, QStringLiteral("None"))).arg(safeText(summary.lastErrorSummaryText, QStringLiteral("None"))));
        appendSummaryRow(table, QStringLiteral("R3 IO"), safeText(summary.ioMessageText));
    }

    bool shouldShowCapability(const KernelDriverCapabilityEntry& entry, const QString& filter)
    {
        if (filter.isEmpty()) { return true; }
        return entry.featureNameText.contains(filter, Qt::CaseInsensitive) ||
            stateText(entry.state, entry.stateNameText).contains(filter, Qt::CaseInsensitive) ||
            entry.dependencyText.contains(filter, Qt::CaseInsensitive) ||
            entry.reasonText.contains(filter, Qt::CaseInsensitive) ||
            featureFlagText(entry.flags).contains(filter, Qt::CaseInsensitive) ||
            policyNames(entry.requiredPolicyFlags).contains(filter, Qt::CaseInsensitive) ||
            capabilityNames(entry.requiredDynDataMask).contains(filter, Qt::CaseInsensitive) ||
            formatHex64(entry.requiredDynDataMask).contains(filter, Qt::CaseInsensitive);
    }

    bool queryDriverStatusSnapshot(KernelDriverStatusSummary& summaryOut, std::vector<KernelDriverCapabilityEntry>& rowsOut)
    {
        ksword::ark::DriverClient client;
        const ksword::ark::DriverCapabilitiesQueryResult queryResult = client.queryDriverCapabilities();

        summaryOut = KernelDriverStatusSummary{};
        rowsOut.clear();
        summaryOut.queryOk = queryResult.io.ok;
        summaryOut.driverLoaded = queryResult.io.ok || queryResult.io.win32Error != ERROR_FILE_NOT_FOUND;
        summaryOut.ioMessageText = QString::fromStdString(queryResult.io.message);

        if (!queryResult.io.ok)
        {
            summaryOut.protocolOk = false;
            summaryOut.dynDataMissing = true;
            summaryOut.limited = true;
            return false;
        }

        summaryOut.version = queryResult.version;
        summaryOut.driverProtocolVersion = queryResult.driverProtocolVersion;
        summaryOut.statusFlags = queryResult.statusFlags;
        summaryOut.securityPolicyFlags = queryResult.securityPolicyFlags;
        summaryOut.dynDataStatusFlags = queryResult.dynDataStatusFlags;
        summaryOut.lastErrorStatus = queryResult.lastErrorStatus;
        summaryOut.totalFeatureCount = queryResult.totalFeatureCount;
        summaryOut.returnedFeatureCount = queryResult.returnedFeatureCount;
        summaryOut.dynDataCapabilityMask = queryResult.dynDataCapabilityMask;
        summaryOut.lastErrorSourceText = stringToQString(queryResult.lastErrorSource);
        summaryOut.lastErrorSummaryText = stringToQString(queryResult.lastErrorSummary);
        summaryOut.driverLoaded = flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DRIVER_LOADED);
        summaryOut.protocolOk = flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_PROTOCOL_OK) &&
            summaryOut.driverProtocolVersion == KSWORD_ARK_DRIVER_PROTOCOL_VERSION;
        summaryOut.dynDataMissing = flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_MISSING) ||
            !flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_ACTIVE);
        summaryOut.limited = flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_LIMITED) || !summaryOut.protocolOk;

        rowsOut.reserve(queryResult.entries.size());
        for (const ksword::ark::DriverFeatureCapabilityEntry& sourceEntry : queryResult.entries)
        {
            KernelDriverCapabilityEntry row{};
            row.featureId = sourceEntry.featureId;
            row.state = sourceEntry.state;
            row.flags = sourceEntry.flags;
            row.requiredPolicyFlags = sourceEntry.requiredPolicyFlags;
            row.deniedPolicyFlags = sourceEntry.deniedPolicyFlags;
            row.requiredDynDataMask = sourceEntry.requiredDynDataMask;
            row.presentDynDataMask = sourceEntry.presentDynDataMask;
            row.featureNameText = stringToQString(sourceEntry.featureName);
            row.stateNameText = stateText(sourceEntry.state, stringToQString(sourceEntry.stateName));
            row.dependencyText = stringToQString(sourceEntry.dependencyText);
            row.reasonText = stringToQString(sourceEntry.reasonText);
            row.detailText = buildCapabilityDetail(row, summaryOut);
            rowsOut.push_back(std::move(row));
        }

        return summaryOut.queryOk && summaryOut.protocolOk;
    }
}

void KernelDock::initializeDriverStatusTab()
{
    if (m_driverStatusPage == nullptr || m_driverStatusLayout != nullptr) { return; }

    m_driverStatusLayout = new QVBoxLayout(m_driverStatusPage);
    m_driverStatusLayout->setContentsMargins(4, 4, 4, 4);
    m_driverStatusLayout->setSpacing(6);

    m_driverStatusToolLayout = new QHBoxLayout();
    m_driverStatusToolLayout->setContentsMargins(0, 0, 0, 0);
    m_driverStatusToolLayout->setSpacing(6);

    m_refreshDriverStatusButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_driverStatusPage);
    m_refreshDriverStatusButton->setToolTip(QStringLiteral("刷新 KswordARK 驱动状态、协议、安全策略和能力矩阵"));
    m_refreshDriverStatusButton->setStyleSheet(blueButtonStyle());
    m_refreshDriverStatusButton->setFixedWidth(34);

    m_copyDriverStatusReportButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制诊断"), m_driverStatusPage);
    m_copyDriverStatusReportButton->setToolTip(QStringLiteral("复制统一驱动状态和能力矩阵诊断报告"));
    m_copyDriverStatusReportButton->setStyleSheet(blueButtonStyle());

    m_driverStatusFilterEdit = new QLineEdit(m_driverStatusPage);
    m_driverStatusFilterEdit->setPlaceholderText(QStringLiteral("按功能/状态/策略/DynData capability/依赖字段筛选"));
    m_driverStatusFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤驱动能力矩阵"));
    m_driverStatusFilterEdit->setClearButtonEnabled(true);
    m_driverStatusFilterEdit->setStyleSheet(blueInputStyle());

    m_driverStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_driverStatusPage);
    m_driverStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_driverStatusToolLayout->addWidget(m_refreshDriverStatusButton, 0);
    m_driverStatusToolLayout->addWidget(m_copyDriverStatusReportButton, 0);
    m_driverStatusToolLayout->addWidget(m_driverStatusFilterEdit, 1);
    m_driverStatusToolLayout->addWidget(m_driverStatusLabel, 0);
    m_driverStatusLayout->addLayout(m_driverStatusToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_driverStatusPage);
    m_driverStatusLayout->addWidget(verticalSplitter, 1);

    m_driverStatusSummaryTable = new QTableWidget(verticalSplitter);
    m_driverStatusSummaryTable->setColumnCount(static_cast<int>(DriverSummaryColumn::Count));
    m_driverStatusSummaryTable->setHorizontalHeaderLabels(QStringList{ QStringLiteral("项目"), QStringLiteral("值") });
    m_driverStatusSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_driverStatusSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_driverStatusSummaryTable->setAlternatingRowColors(true);
    m_driverStatusSummaryTable->setStyleSheet(itemSelectionStyle());
    m_driverStatusSummaryTable->setCornerButtonEnabled(false);
    m_driverStatusSummaryTable->verticalHeader()->setVisible(false);
    m_driverStatusSummaryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_driverStatusSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_driverStatusSummaryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(DriverSummaryColumn::Value), QHeaderView::Stretch);
    m_driverStatusSummaryTable->setColumnWidth(static_cast<int>(DriverSummaryColumn::Name), 220);

    QSplitter* lowerSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);
    m_driverCapabilityTable = new QTableWidget(lowerSplitter);
    m_driverCapabilityTable->setColumnCount(static_cast<int>(DriverCapabilityColumn::Count));
    m_driverCapabilityTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("功能"), QStringLiteral("状态"), QStringLiteral("策略"),
        QStringLiteral("所需DynData"), QStringLiteral("已满足DynData"), QStringLiteral("依赖字段"), QStringLiteral("原因") });
    m_driverCapabilityTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_driverCapabilityTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_driverCapabilityTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_driverCapabilityTable->setAlternatingRowColors(true);
    m_driverCapabilityTable->setStyleSheet(itemSelectionStyle());
    m_driverCapabilityTable->setCornerButtonEnabled(false);
    m_driverCapabilityTable->verticalHeader()->setVisible(false);
    m_driverCapabilityTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_driverCapabilityTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_driverCapabilityTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(DriverCapabilityColumn::Feature), QHeaderView::Stretch);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::State), 140);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::RequiredDyn), 180);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::PresentDyn), 180);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::Dependency), 280);

    m_driverCapabilityDetailEditor = new CodeEditorWidget(lowerSplitter);
    m_driverCapabilityDetailEditor->setReadOnly(true);
    m_driverCapabilityDetailEditor->setText(QStringLiteral("请选择一条驱动功能能力查看依赖字段和诊断详情。"));

    verticalSplitter->setStretchFactor(0, 2);
    verticalSplitter->setStretchFactor(1, 5);
    lowerSplitter->setStretchFactor(0, 3);
    lowerSplitter->setStretchFactor(1, 2);

    connect(m_refreshDriverStatusButton, &QPushButton::clicked, this, [this]() { refreshDriverStatusAsync(); });
    connect(m_copyDriverStatusReportButton, &QPushButton::clicked, this, [this]() {
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard != nullptr)
        {
            clipboard->setText(buildDriverStatusReport(m_driverStatusSummary, m_driverCapabilityRows));
            m_driverStatusLabel->setText(QStringLiteral("状态：诊断报告已复制"));
        }
    });
    connect(m_driverStatusFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildDriverCapabilityTable(filterText.trimmed());
    });
    connect(m_driverCapabilityTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showDriverCapabilityDetailByCurrentRow();
    });
}

void KernelDock::refreshDriverStatusAsync()
{
    if (m_driverStatusRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] 驱动状态刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshDriverStatusButton->setEnabled(false);
    m_driverStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_driverStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        KernelDriverStatusSummary summary;
        std::vector<KernelDriverCapabilityEntry> rows;
        const bool success = queryDriverStatusSnapshot(summary, rows);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, summary = std::move(summary), rows = std::move(rows)]() mutable {
            if (guardThis == nullptr) { return; }

            guardThis->m_driverStatusRefreshRunning.store(false);
            guardThis->m_refreshDriverStatusButton->setEnabled(true);
            guardThis->m_driverStatusSummary = std::move(summary);
            guardThis->m_driverCapabilityRows = std::move(rows);
            populateSummaryTable(guardThis->m_driverStatusSummaryTable, guardThis->m_driverStatusSummary, guardThis->m_driverCapabilityRows.size());
            guardThis->rebuildDriverCapabilityTable(guardThis->m_driverStatusFilterEdit->text().trimmed());

            const QString badges = statusBadges(guardThis->m_driverStatusSummary);
            if (!success)
            {
                guardThis->m_driverStatusLabel->setText(QStringLiteral("状态：%1").arg(badges));
                guardThis->m_driverStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_driverCapabilityDetailEditor->setText(buildDriverStatusReport(guardThis->m_driverStatusSummary, guardThis->m_driverCapabilityRows));
                return;
            }

            const std::size_t unavailableCount = static_cast<std::size_t>(std::count_if(
                guardThis->m_driverCapabilityRows.begin(),
                guardThis->m_driverCapabilityRows.end(),
                [](const KernelDriverCapabilityEntry& entry) { return entry.state != KSWORD_ARK_FEATURE_STATE_AVAILABLE; }));
            guardThis->m_driverStatusLabel->setText(QStringLiteral("状态：%1，功能 %2 项，受限 %3 项")
                .arg(badges).arg(guardThis->m_driverCapabilityRows.size()).arg(unavailableCount));
            guardThis->m_driverStatusLabel->setStyleSheet(statusLabelStyle(unavailableCount == 0U ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));

            if (guardThis->m_driverCapabilityTable->rowCount() > 0)
            {
                guardThis->m_driverCapabilityTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_driverCapabilityDetailEditor->setText(QStringLiteral("当前筛选条件下没有驱动能力记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildDriverCapabilityTable(const QString& filterKeyword)
{
    if (m_driverCapabilityTable == nullptr) { return; }

    m_driverCapabilityTable->setSortingEnabled(false);
    m_driverCapabilityTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_driverCapabilityRows.size(); ++sourceIndex)
    {
        const KernelDriverCapabilityEntry& entry = m_driverCapabilityRows[sourceIndex];
        if (!shouldShowCapability(entry, filterKeyword)) { continue; }

        const int row = m_driverCapabilityTable->rowCount();
        m_driverCapabilityTable->insertRow(row);
        auto* featureItem = new QTableWidgetItem(safeText(entry.featureNameText));
        auto* stateItem = new QTableWidgetItem(stateText(entry.state, entry.stateNameText));
        auto* policyItem = new QTableWidgetItem(formatHex32(entry.requiredPolicyFlags));
        auto* requiredItem = new QTableWidgetItem(formatHex64(entry.requiredDynDataMask));
        auto* presentItem = new QTableWidgetItem(formatHex64(entry.presentDynDataMask));
        auto* dependencyItem = new QTableWidgetItem(safeText(entry.dependencyText, QStringLiteral("None")));
        auto* reasonItem = new QTableWidgetItem(safeText(entry.reasonText, QStringLiteral("None")));

        featureItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        stateItem->setForeground(stateBrush(entry.state));
        if (entry.deniedPolicyFlags != 0U) { policyItem->setForeground(QBrush(QColor(QStringLiteral("#B23A3A")))); }
        if (entry.requiredDynDataMask != 0ULL && entry.presentDynDataMask != entry.requiredDynDataMask)
        {
            presentItem->setForeground(QBrush(QColor(QStringLiteral("#B23A3A"))));
        }

        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Feature, featureItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::State, stateItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Policy, policyItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::RequiredDyn, requiredItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::PresentDyn, presentItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Dependency, dependencyItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Reason, reasonItem);
    }

    m_driverCapabilityTable->setSortingEnabled(true);
    populateSummaryTable(m_driverStatusSummaryTable, m_driverStatusSummary, static_cast<std::size_t>(m_driverCapabilityTable->rowCount()));
}

bool KernelDock::currentDriverCapabilitySourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_driverCapabilityTable == nullptr) { return false; }
    const int currentRow = m_driverCapabilityTable->currentRow();
    if (currentRow < 0) { return false; }

    QTableWidgetItem* featureItem = m_driverCapabilityTable->item(currentRow, static_cast<int>(DriverCapabilityColumn::Feature));
    if (featureItem == nullptr) { return false; }

    sourceIndexOut = static_cast<std::size_t>(featureItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_driverCapabilityRows.size();
}

const KernelDriverCapabilityEntry* KernelDock::currentDriverCapabilityEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentDriverCapabilitySourceIndex(sourceIndex)) { return nullptr; }
    return &m_driverCapabilityRows[sourceIndex];
}

void KernelDock::showDriverCapabilityDetailByCurrentRow()
{
    if (m_driverCapabilityDetailEditor == nullptr) { return; }

    const KernelDriverCapabilityEntry* entry = currentDriverCapabilityEntry();
    if (entry == nullptr)
    {
        m_driverCapabilityDetailEditor->setText(buildDriverStatusReport(m_driverStatusSummary, m_driverCapabilityRows));
        return;
    }

    m_driverCapabilityDetailEditor->setText(QStringLiteral(
        "%1\n\n当前状态摘要:\n  %2\n  SecurityPolicy: %3 (%4)\n  DynDataStatus: %5 (%6)\n  DynDataCapability: %7 (%8)")
        .arg(entry->detailText)
        .arg(statusBadges(m_driverStatusSummary))
        .arg(formatHex32(m_driverStatusSummary.securityPolicyFlags)).arg(policyNames(m_driverStatusSummary.securityPolicyFlags))
        .arg(formatHex32(m_driverStatusSummary.dynDataStatusFlags)).arg(dynDataStatusText(m_driverStatusSummary.dynDataStatusFlags))
        .arg(formatHex64(m_driverStatusSummary.dynDataCapabilityMask)).arg(capabilityNames(m_driverStatusSummary.dynDataCapabilityMask)));
}
