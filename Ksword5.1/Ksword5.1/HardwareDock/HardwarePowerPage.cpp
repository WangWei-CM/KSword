#include "HardwarePowerPage.h"
#include <QList>
#include "../UI/ToolbarMetrics.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ParameterWorkspace.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TableInteractionSupport.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <PowrProf.h>

#pragma comment(lib, "PowrProf.lib")

struct HardwarePowerSchemeSnapshot
{
    struct Scheme { QString name; QByteArray guid; bool active = false; };
    std::vector<Scheme> schemes;
    QByteArray activeGuid;
    DWORD enumerationError = ERROR_SUCCESS;
    ULONG enumerationErrorIndex = 0;
};

struct HardwarePowerTransactionRequest
{
    enum class Kind { Scheme, Cpu, Restore };
    Kind kind = Kind::Cpu;
    QByteArray schemeGuid;
    bool hasRestoreCpu = false;
    KSWORD_ARK_CPU_POWER_RESPONSE restoreCpu{};
    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST cpu{};
};

struct HardwarePowerTransactionResult
{
    HardwarePowerTransactionRequest::Kind kind = HardwarePowerTransactionRequest::Kind::Cpu;
    bool invalidGuid = false;
    DWORD schemeError = ERROR_SUCCESS;
    bool schemeApplied = false;
    bool schemesRead = false;
    HardwarePowerSchemeSnapshot schemes;
    bool cpuRead = false;
    ksword::ark::CpuPowerResult cpu;
    bool controlAttempted = false;
    unsigned long preflightFailure = KSWORD_ARK_CPU_POWER_FAILURE_NONE;
    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST request{};
    ksword::ark::CpuPowerResult control;
};

namespace
{
    // powerText：读取电源页 context key，并保留中文源文本作为回退。
    QString powerText(const QString& key, const QString& sourceText)
    {
        return ks::i18n::contextText(key, sourceText);
    }

    // ntStatusText：把 R0 NTSTATUS 固定显示为八位十六进制。
    QString ntStatusText(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<quint32>(status), 8, 16, QChar('0'))
            .toUpper();
    }

    // rawMsrText：把 64 位 MSR 固定显示为十六位十六进制。
    QString rawMsrText(const unsigned long long value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    // addSnapshotRow：向两列表写入一条不可编辑摘要。
    void addSnapshotRow(
        QTableWidget* table,
        const QString& name,
        const QString& value)
    {
        if (table == nullptr)
        {
            return;
        }
        const int row = table->rowCount();
        table->insertRow(row);
        auto* nameItem = new QTableWidgetItem(name);
        auto* valueItem = new QTableWidgetItem(value);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, nameItem);
        table->setItem(row, 1, valueItem);
    }

    // powerSchemeFriendlyName：读取 Windows 电源方案本地化名称。
    QString powerSchemeFriendlyName(const GUID& schemeGuid)
    {
        DWORD nameBytes = 0UL;
        DWORD error = ::PowerReadFriendlyName(
            nullptr,
            &schemeGuid,
            nullptr,
            nullptr,
            nullptr,
            &nameBytes);
        if (error != ERROR_SUCCESS || nameBytes < sizeof(wchar_t))
        {
            return QString();
        }

        std::vector<UCHAR> nameBuffer(nameBytes, 0U);
        error = ::PowerReadFriendlyName(
            nullptr,
            &schemeGuid,
            nullptr,
            nullptr,
            nameBuffer.data(),
            &nameBytes);
        if (error != ERROR_SUCCESS)
        {
            return QString();
        }
        return QString::fromWCharArray(
            reinterpret_cast<const wchar_t*>(nameBuffer.data())).trimmed();
    }

    // guidBytes：把 GUID 复制到 QVariant 可保存的拥有型 QByteArray。
    QByteArray guidBytes(const GUID& guid)
    {
        return QByteArray(
            reinterpret_cast<const char*>(&guid),
            static_cast<int>(sizeof(guid)));
    }

    // guidFromBytes：严格校验长度后还原 GUID。
    bool guidFromBytes(const QByteArray& bytes, GUID* guidOut)
    {
        if (guidOut == nullptr || bytes.size() != static_cast<int>(sizeof(GUID)))
        {
            return false;
        }
        std::memcpy(guidOut, bytes.constData(), sizeof(*guidOut));
        return true;
    }

    // cpuPowerFailureReasonText：把 R0 failureReason 转为含请求值的可操作诊断。
    QString cpuPowerFailureReasonText(
        const unsigned long failureReason,
        const KSWORD_ARK_CPU_POWER_CONTROL_REQUEST& request,
        const KSWORD_ARK_CPU_POWER_RESPONSE& snapshot)
    {
        switch (failureReason)
        {
        case KSWORD_ARK_CPU_POWER_FAILURE_REQUEST_HEADER:
            return powerText(
                QStringLiteral("hardware.power.error.reason.request_header"),
                QStringLiteral("请求头、操作标志或 UI 确认标志无效。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_SAFETY_POLICY:
            return powerText(
                QStringLiteral("hardware.power.error.reason.safety_policy"),
                QStringLiteral("统一 R0 安全策略拒绝了本次 CPU 电源修改。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_VENDOR:
            return powerText(
                QStringLiteral("hardware.power.error.reason.vendor"),
                QStringLiteral("当前 CPU 厂商不支持此 R0 调节路径。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_POWER_CAPABILITY:
            return powerText(
                QStringLiteral("hardware.power.error.reason.power_capability"),
                QStringLiteral("RAPL 功耗限制不可写、字段不可读，或已被 BIOS/固件锁定。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_POWER_ABSOLUTE_RANGE:
            return powerText(
                QStringLiteral("hardware.power.error.reason.power_absolute"),
                QStringLiteral("PL1=%1 W、PL2=%2 W 必须大于 0 且均不超过 1000 W。"))
                .arg(request.pl1Milliwatts / 1000.0, 0, 'f', 3)
                .arg(request.pl2Milliwatts / 1000.0, 0, 'f', 3);
        case KSWORD_ARK_CPU_POWER_FAILURE_POWER_PLATFORM_MAXIMUM:
            return powerText(
                QStringLiteral("hardware.power.error.reason.power_maximum"),
                QStringLiteral("PL1=%1 W、PL2=%2 W 超过 CPU 报告的平台上限 %3 W。"))
                .arg(request.pl1Milliwatts / 1000.0, 0, 'f', 3)
                .arg(request.pl2Milliwatts / 1000.0, 0, 'f', 3)
                .arg(snapshot.packageMaximumPowerMilliwatts / 1000.0, 0, 'f', 3);
        case KSWORD_ARK_CPU_POWER_FAILURE_POWER_PLATFORM_MINIMUM:
            return powerText(
                QStringLiteral("hardware.power.error.reason.power_minimum"),
                QStringLiteral("PL1=%1 W、PL2=%2 W 低于 CPU 报告的平台下限 %3 W。"))
                .arg(request.pl1Milliwatts / 1000.0, 0, 'f', 3)
                .arg(request.pl2Milliwatts / 1000.0, 0, 'f', 3)
                .arg(snapshot.packageMinimumPowerMilliwatts / 1000.0, 0, 'f', 3);
        case KSWORD_ARK_CPU_POWER_FAILURE_POWER_BOOLEAN:
            return powerText(
                QStringLiteral("hardware.power.error.reason.power_boolean"),
                QStringLiteral("PL1/PL2 enable 或 clamp 字段不是有效布尔值。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_TURBO_CAPABILITY:
            return powerText(
                QStringLiteral("hardware.power.error.reason.turbo"),
                QStringLiteral("CPU 未提供可写的 Intel Turbo 控制字段。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_HWP_CAPABILITY:
            return powerText(
                QStringLiteral("hardware.power.error.reason.hwp_capability"),
                QStringLiteral("HWP 未启用，或 HWP capability/request 字段不可读。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_HWP_ORDER:
            return powerText(
                QStringLiteral("hardware.power.error.reason.hwp_order"),
                QStringLiteral("HWP 最小性能 %1 不能高于最大性能 %2。"))
                .arg(request.hwpMinimumPerformance)
                .arg(request.hwpMaximumPerformance);
        case KSWORD_ARK_CPU_POWER_FAILURE_HWP_DESIRED_RANGE:
            return powerText(
                QStringLiteral("hardware.power.error.reason.hwp_desired"),
                QStringLiteral("HWP 期望性能 %1 必须为 0（自动），或位于最小值 %2 与最大值 %3 之间。"))
                .arg(request.hwpDesiredPerformance)
                .arg(request.hwpMinimumPerformance)
                .arg(request.hwpMaximumPerformance);
        case KSWORD_ARK_CPU_POWER_FAILURE_HWP_PLATFORM_RANGE:
            return powerText(
                QStringLiteral("hardware.power.error.reason.hwp_platform"),
                QStringLiteral("HWP 请求 Min=%1 Max=%2 Desired=%3 超出 CPU 报告的范围 %4 到 %5。"))
                .arg(request.hwpMinimumPerformance)
                .arg(request.hwpMaximumPerformance)
                .arg(request.hwpDesiredPerformance)
                .arg(snapshot.hwpLowestPerformance)
                .arg(snapshot.hwpHighestPerformance);
        case KSWORD_ARK_CPU_POWER_FAILURE_HWP_EPP:
            return powerText(
                QStringLiteral("hardware.power.error.reason.hwp_epp"),
                QStringLiteral("该 CPU 未声明 HWP EPP 能力，EPP 字段不能修改。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_TURBO_RATIO:
            return powerText(
                QStringLiteral("hardware.power.error.reason.turbo_ratio"),
                QStringLiteral("Turbo Ratio 不可编程，寄存器不可读，或倍率不在 1 到 255 之间。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_STALE_SNAPSHOT:
            return powerText(
                QStringLiteral("hardware.power.error.stale"),
                QStringLiteral("设置在提交前已被固件、Windows 或其他工具改变，请刷新后重试。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_PROCESSOR_APPLY:
            return powerText(
                QStringLiteral("hardware.power.error.reason.processor_apply"),
                QStringLiteral("至少一个逻辑处理器的 MSR 写入或回读校验失败。"));
        case KSWORD_ARK_CPU_POWER_FAILURE_PERF_CONTROL:
            return powerText(
                QStringLiteral("hardware.power.error.reason.perf_control"),
                QStringLiteral("请求倍频不可编程、IA32_PERF_CTL 不可读，或倍率不在 1 到 255 之间。"));
        default:
            return powerText(
                QStringLiteral("hardware.power.error.reason.unknown"),
                QStringLiteral("R0 未提供已知失败原因，reason=%1。"))
                .arg(failureReason);
        }
    }

    // validateCpuPowerRequestForUi：在弹出确认框前阻止必然被 R0 拒绝的关系错误。
    unsigned long cpuPowerRequestFailureReason(
        const KSWORD_ARK_CPU_POWER_CONTROL_REQUEST& request,
        const KSWORD_ARK_CPU_POWER_RESPONSE& snapshot)
    {
        if ((request.applyFlags &
            KSWORD_ARK_CPU_POWER_APPLY_POWER_LIMITS) != 0UL)
        {
            if (request.pl1Milliwatts == 0UL ||
                request.pl2Milliwatts == 0UL ||
                request.pl1Milliwatts >
                    KSWORD_ARK_CPU_POWER_ABSOLUTE_MAX_MILLIWATTS ||
                request.pl2Milliwatts >
                    KSWORD_ARK_CPU_POWER_ABSOLUTE_MAX_MILLIWATTS)
            {
                return KSWORD_ARK_CPU_POWER_FAILURE_POWER_ABSOLUTE_RANGE;
            }
            if (snapshot.packageMaximumPowerMilliwatts != 0UL &&
                (request.pl1Milliwatts >
                    snapshot.packageMaximumPowerMilliwatts ||
                 request.pl2Milliwatts >
                    snapshot.packageMaximumPowerMilliwatts))
            {
                return KSWORD_ARK_CPU_POWER_FAILURE_POWER_PLATFORM_MAXIMUM;
            }
            if (snapshot.packageMinimumPowerMilliwatts != 0UL &&
                (request.pl1Milliwatts <
                    snapshot.packageMinimumPowerMilliwatts ||
                 request.pl2Milliwatts <
                    snapshot.packageMinimumPowerMilliwatts))
            {
                return KSWORD_ARK_CPU_POWER_FAILURE_POWER_PLATFORM_MINIMUM;
            }
        }

        if ((request.applyFlags & KSWORD_ARK_CPU_POWER_APPLY_HWP) != 0UL)
        {
            if (request.hwpMinimumPerformance >
                request.hwpMaximumPerformance)
            {
                return KSWORD_ARK_CPU_POWER_FAILURE_HWP_ORDER;
            }
            if (request.hwpDesiredPerformance != 0UL &&
                (request.hwpDesiredPerformance <
                    request.hwpMinimumPerformance ||
                 request.hwpDesiredPerformance >
                    request.hwpMaximumPerformance))
            {
                return KSWORD_ARK_CPU_POWER_FAILURE_HWP_DESIRED_RANGE;
            }
            if ((snapshot.hwpLowestPerformance != 0UL &&
                    request.hwpMinimumPerformance <
                        snapshot.hwpLowestPerformance) ||
                (snapshot.hwpHighestPerformance != 0UL &&
                    (request.hwpMaximumPerformance >
                        snapshot.hwpHighestPerformance ||
                     request.hwpDesiredPerformance >
                        snapshot.hwpHighestPerformance)))
            {
                return KSWORD_ARK_CPU_POWER_FAILURE_HWP_PLATFORM_RANGE;
            }
        }
        return KSWORD_ARK_CPU_POWER_FAILURE_NONE;
    }

    QString validateCpuPowerRequestForUi(
        const KSWORD_ARK_CPU_POWER_CONTROL_REQUEST& request,
        const KSWORD_ARK_CPU_POWER_RESPONSE& snapshot)
    {
        const unsigned long reason = cpuPowerRequestFailureReason(request, snapshot);
        return reason == KSWORD_ARK_CPU_POWER_FAILURE_NONE ? QString()
            : cpuPowerFailureReasonText(reason, request, snapshot);
    }
}

HardwarePowerPage::HardwarePowerPage(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    m_queryOperation = new ks::ui::AsyncOperation(this);
    m_queryOperation->setStateChangedCallback([this]() { updateControlAvailability(); });
}

HardwarePowerPage::~HardwarePowerPage()
{
    m_queryOperation->close();
}

void HardwarePowerPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_loadedOnce)
    {
        m_loadedOnce = true;
        refreshAll();
    }
}

void HardwarePowerPage::initializeUi()
{
    auto& language = ks::i18n::LanguageManager::instance();
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    auto* workspace = new ks::ui::ParameterWorkspace(this, 480); // 调节参数滚动，能力表保持可见。
    rootLayout->addWidget(workspace);

    auto* titleLabel = new QLabel(QStringLiteral("CPU 电源与性能调节"), this);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:700;color:%1;")
            .arg(KswordTheme::TextPrimaryHex()));
    language.bindText(
        titleLabel,
        QStringLiteral("hardware.power.title"),
        QStringLiteral("CPU 电源与性能调节"));
    // 调节口径原为标题下的常驻说明，改挂标题 tooltip，需要时悬停可见。
    language.bindToolTip(
        titleLabel,
        QStringLiteral("hardware.power.title.tooltip"),
        QStringLiteral("R0 调节仅写入已探测的 Intel RAPL/HWP/Turbo/请求倍频白名单字段，不绕过 BIOS/微码锁，也不提供任意 MSR 写入。"));
    workspace->parameters()->addWidget(titleLabel);

    m_statusLabel = new QLabel(
        powerText(
            QStringLiteral("hardware.power.status.initial"),
            QStringLiteral("状态：尚未刷新。")),
        this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-weight:600;color:%1;").arg(KswordTheme::PrimaryBlueHex));
    workspace->results()->addWidget(m_statusLabel);

    QWidget* contentWidget = workspace; // 原控件继续由页面持有，由框架接管布局。
    auto* contentLayout = workspace->parameters();

    auto* schemeGroup = new QGroupBox(QStringLiteral("Windows 电源方案"), contentWidget);
    language.bindText(
        schemeGroup,
        QStringLiteral("hardware.power.scheme.group"),
        QStringLiteral("Windows 电源方案"));
    auto* schemeLayout = new QGridLayout(schemeGroup);
    ks::ui::StyleParameterSection(schemeGroup);
    auto* schemeLabel = new QLabel(QStringLiteral("当前/目标方案"), schemeGroup);
    language.bindText(
        schemeLabel,
        QStringLiteral("hardware.power.scheme.label"),
        QStringLiteral("当前/目标方案"));
    m_powerSchemeCombo = new QComboBox(schemeGroup);
    ks::ui::StylePrimaryCombo(m_powerSchemeCombo);
    m_applyPowerSchemeButton = new QPushButton(QStringLiteral("应用电源方案"), schemeGroup);
    language.bindText(
        m_applyPowerSchemeButton,
        QStringLiteral("hardware.power.scheme.apply"),
        QStringLiteral("应用电源方案"));
    m_refreshAllButton = new QPushButton(QStringLiteral("刷新全部"), schemeGroup);
    language.bindText(
        m_refreshAllButton,
        QStringLiteral("hardware.power.refresh"),
        QStringLiteral("刷新全部"));
    m_restoreInitialStateButton = new QPushButton(
        QStringLiteral("一键还原首次状态"),
        schemeGroup);
    language.bindText(
        m_restoreInitialStateButton,
        QStringLiteral("hardware.power.restore"),
        QStringLiteral("一键还原首次状态"));
    m_restoreInitialStateButton->setToolTip(
        QStringLiteral("把功耗墙、倍频、Turbo 等所有设置一键恢复到本页刚打开时的原始状态"));
    schemeLayout->addWidget(schemeLabel, 0, 0);
    schemeLayout->addWidget(m_powerSchemeCombo, 1, 0);
    schemeLayout->addWidget(m_applyPowerSchemeButton, 2, 0);
    schemeLayout->setColumnStretch(0, 1);
    contentLayout->addWidget(schemeGroup, 0);

    auto* snapshotGroup = new QGroupBox(QStringLiteral("CPU 能力与当前状态"), contentWidget);
    language.bindText(
        snapshotGroup,
        QStringLiteral("hardware.power.snapshot.group"),
        QStringLiteral("CPU 能力与当前状态"));
    auto* snapshotLayout = new QVBoxLayout(snapshotGroup);
    ks::ui::StyleParameterSection(snapshotGroup);
    m_snapshotTable = new ks::ui::VisibleTableWidget(snapshotGroup);
    // 当前能力键值摘要位于调节表单内，保留右键复制，不挤占调节空间。
    ks::ui::SetTableActionBarMode(m_snapshotTable, ks::ui::TableActionBarMode::None);
    m_snapshotTable->setColumnCount(2);
    m_snapshotTable->setHorizontalHeaderLabels({
        powerText(QStringLiteral("hardware.power.table.item"), QStringLiteral("项目")),
        powerText(QStringLiteral("hardware.power.table.value"), QStringLiteral("当前值")) });
    m_snapshotTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_snapshotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_snapshotTable->setAlternatingRowColors(true);
    m_snapshotTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_snapshotTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_snapshotTable->verticalHeader()->setVisible(false);
    m_snapshotTable->setMinimumHeight(160);
    snapshotLayout->addWidget(m_snapshotTable, 1);
    workspace->results()->addWidget(snapshotGroup, 1);

    auto* raplGroup = new QGroupBox(QStringLiteral("RAPL 功耗墙（PL1 / PL2）"), contentWidget);
    language.bindText(
        raplGroup,
        QStringLiteral("hardware.power.rapl.group"),
        QStringLiteral("RAPL 功耗墙（PL1 / PL2）"));
    auto* raplLayout = new QGridLayout(raplGroup);
    ks::ui::StyleParameterSection(raplGroup);
    auto* pl1Label = new QLabel(QStringLiteral("PL1 长时功耗"), raplGroup);
    auto* pl2Label = new QLabel(QStringLiteral("PL2 短时功耗"), raplGroup);
    language.bindText(pl1Label, QStringLiteral("hardware.power.rapl.pl1"), QStringLiteral("PL1 长时功耗"));
    language.bindText(pl2Label, QStringLiteral("hardware.power.rapl.pl2"), QStringLiteral("PL2 短时功耗"));
    m_pl1Spin = new QDoubleSpinBox(raplGroup);
    m_pl2Spin = new QDoubleSpinBox(raplGroup);
    for (QDoubleSpinBox* spin : { m_pl1Spin, m_pl2Spin })
    {
        spin->setDecimals(3);
        spin->setRange(0.001, 1000.0);
        spin->setSingleStep(1.0);
        spin->setSuffix(QStringLiteral(" W"));
    }
    m_pl1EnableCheck = new QCheckBox(QStringLiteral("启用 PL1"), raplGroup);
    m_pl1ClampCheck = new QCheckBox(QStringLiteral("允许 PL1 Clamp"), raplGroup);
    m_pl2EnableCheck = new QCheckBox(QStringLiteral("启用 PL2"), raplGroup);
    m_pl2ClampCheck = new QCheckBox(QStringLiteral("允许 PL2 Clamp"), raplGroup);
    language.bindText(m_pl1EnableCheck, QStringLiteral("hardware.power.rapl.pl1_enable"), QStringLiteral("启用 PL1"));
    language.bindText(m_pl1ClampCheck, QStringLiteral("hardware.power.rapl.pl1_clamp"), QStringLiteral("允许 PL1 Clamp"));
    language.bindText(m_pl2EnableCheck, QStringLiteral("hardware.power.rapl.pl2_enable"), QStringLiteral("启用 PL2"));
    language.bindText(m_pl2ClampCheck, QStringLiteral("hardware.power.rapl.pl2_clamp"), QStringLiteral("允许 PL2 Clamp"));
    m_applyPowerLimitsButton = new QPushButton(QStringLiteral("应用 PL1 / PL2"), raplGroup);
    m_raisePowerLimitsButton = new QPushButton(QStringLiteral("解除软件功耗墙（平台上限）"), raplGroup);
    language.bindText(m_applyPowerLimitsButton, QStringLiteral("hardware.power.rapl.apply"), QStringLiteral("应用 PL1 / PL2"));
    language.bindText(m_raisePowerLimitsButton, QStringLiteral("hardware.power.rapl.raise"), QStringLiteral("解除软件功耗墙（平台上限）"));
    // 写入边界并入这个按钮：值会不会被别人改回去，是按下去之前该知道的事。
    m_applyPowerLimitsButton->setToolTip(
        QStringLiteral("把上面填写的 PL1（长时）/PL2（短时）功耗上限写入 CPU，单位瓦。MSR lock 置位后本页不会尝试清除；固件、Windows 电源管理或其他调校工具可能随时重写这些值。AMD 型号相关 SMU/PBO 暂不写入。"));
    m_raisePowerLimitsButton->setToolTip(
        QStringLiteral("把功耗上限拉到平台允许的最高值，相当于解除软件功耗限制；可能升温降频，请谨慎使用"));
    raplLayout->addWidget(pl1Label, 0, 0);
    // PL1/PL2 各自连续排列，开关跟随本项功耗值，避免四列表单横向溢出。
    raplLayout->addWidget(m_pl1Spin, 1, 0);
    raplLayout->addWidget(m_pl1EnableCheck, 2, 0);
    raplLayout->addWidget(m_pl1ClampCheck, 3, 0);
    raplLayout->addWidget(pl2Label, 4, 0);
    raplLayout->addWidget(m_pl2Spin, 5, 0);
    raplLayout->addWidget(m_pl2EnableCheck, 6, 0);
    raplLayout->addWidget(m_pl2ClampCheck, 7, 0);
    raplLayout->addWidget(m_applyPowerLimitsButton, 8, 0);
    raplLayout->addWidget(m_raisePowerLimitsButton, 9, 0);
    contentLayout->addWidget(raplGroup, 0);

    auto* turboGroup = new QGroupBox(QStringLiteral("Turbo 与超频倍率"), contentWidget);
    language.bindText(turboGroup, QStringLiteral("hardware.power.turbo.group"), QStringLiteral("Turbo 与超频倍率"));
    auto* turboLayout = new QGridLayout(turboGroup);
    ks::ui::StyleParameterSection(turboGroup);
    m_turboEnableCheck = new QCheckBox(QStringLiteral("启用 Intel Turbo Boost"), turboGroup);
    language.bindText(m_turboEnableCheck, QStringLiteral("hardware.power.turbo.enable"), QStringLiteral("启用 Intel Turbo Boost"));
    m_applyTurboButton = new QPushButton(QStringLiteral("应用 Turbo 开关"), turboGroup);
    language.bindText(m_applyTurboButton, QStringLiteral("hardware.power.turbo.apply"), QStringLiteral("应用 Turbo 开关"));
    m_applyTurboButton->setToolTip(QStringLiteral("打开或关闭 Intel 睿频加速（Turbo Boost）"));
    auto* ratioLabel = new QLabel(QStringLiteral("全档位 Turbo Ratio"), turboGroup);
    language.bindText(ratioLabel, QStringLiteral("hardware.power.turbo.ratio"), QStringLiteral("全档位 Turbo Ratio"));
    m_turboRatioSpin = new QSpinBox(turboGroup);
    m_turboRatioSpin->setRange(1, 255);
    m_turboRatioSpin->setSuffix(QStringLiteral(" x"));
    m_applyTurboRatioButton = new QPushButton(QStringLiteral("应用超频倍率"), turboGroup);
    language.bindText(m_applyTurboRatioButton, QStringLiteral("hardware.power.turbo.ratio_apply"), QStringLiteral("应用超频倍率"));
    m_applyTurboRatioButton->setToolTip(
        QStringLiteral("把设定的全核睿频倍率写入 CPU（超频操作，风险较高，可能导致不稳定或死机）"));
    turboLayout->addWidget(m_turboEnableCheck, 0, 0);
    turboLayout->addWidget(m_applyTurboButton, 1, 0);
    turboLayout->addWidget(ratioLabel, 2, 0);
    turboLayout->addWidget(m_turboRatioSpin, 3, 0);
    turboLayout->addWidget(m_applyTurboRatioButton, 4, 0);
    auto* requestedMultiplierLabel = new QLabel(
        QStringLiteral("请求倍频（IA32_PERF_CTL）"),
        turboGroup);
    language.bindText(
        requestedMultiplierLabel,
        QStringLiteral("hardware.power.turbo.requested_multiplier"),
        QStringLiteral("请求倍频（IA32_PERF_CTL）"));
    m_requestedMultiplierSpin = new QSpinBox(turboGroup);
    m_requestedMultiplierSpin->setRange(1, 255);
    m_requestedMultiplierSpin->setSuffix(QStringLiteral(" x"));
    m_applyRequestedMultiplierButton = new QPushButton(
        QStringLiteral("应用请求倍频"),
        turboGroup);
    language.bindText(
        m_applyRequestedMultiplierButton,
        QStringLiteral("hardware.power.turbo.requested_multiplier_apply"),
        QStringLiteral("应用请求倍频"));
    m_applyRequestedMultiplierButton->setToolTip(
        QStringLiteral("向 CPU 写入请求的运行倍频（IA32_PERF_CTL 寄存器）"));
    requestedMultiplierLabel->setWordWrap(true);
    turboLayout->addWidget(requestedMultiplierLabel, 5, 0);
    turboLayout->addWidget(m_requestedMultiplierSpin, 6, 0);
    turboLayout->addWidget(m_applyRequestedMultiplierButton, 7, 0);
    contentLayout->addWidget(turboGroup, 0);

    auto* hwpGroup = new QGroupBox(QStringLiteral("Intel Speed Shift / HWP"), contentWidget);
    language.bindText(hwpGroup, QStringLiteral("hardware.power.hwp.group"), QStringLiteral("Intel Speed Shift / HWP"));
    auto* hwpLayout = new QGridLayout(hwpGroup);
    ks::ui::StyleParameterSection(hwpGroup);
    auto* hwpMinimumLabel = new QLabel(QStringLiteral("最小性能"), hwpGroup);
    auto* hwpMaximumLabel = new QLabel(QStringLiteral("最大性能"), hwpGroup);
    auto* hwpDesiredLabel = new QLabel(QStringLiteral("期望性能（0=自动）"), hwpGroup);
    auto* hwpEppLabel = new QLabel(QStringLiteral("EPP（0=性能，255=节能）"), hwpGroup);
    language.bindText(hwpMinimumLabel, QStringLiteral("hardware.power.hwp.minimum"), QStringLiteral("最小性能"));
    language.bindText(hwpMaximumLabel, QStringLiteral("hardware.power.hwp.maximum"), QStringLiteral("最大性能"));
    language.bindText(hwpDesiredLabel, QStringLiteral("hardware.power.hwp.desired"), QStringLiteral("期望性能（0=自动）"));
    language.bindText(hwpEppLabel, QStringLiteral("hardware.power.hwp.epp"), QStringLiteral("EPP（0=性能，255=节能）"));
    m_hwpMinimumSpin = new QSpinBox(hwpGroup);
    m_hwpMaximumSpin = new QSpinBox(hwpGroup);
    m_hwpDesiredSpin = new QSpinBox(hwpGroup);
    m_hwpEppSpin = new QSpinBox(hwpGroup);
    for (QSpinBox* spin : { m_hwpMinimumSpin, m_hwpMaximumSpin, m_hwpDesiredSpin, m_hwpEppSpin })
    {
        spin->setRange(0, 255);
    }
    m_applyHwpButton = new QPushButton(QStringLiteral("应用到全部逻辑处理器"), hwpGroup);
    language.bindText(m_applyHwpButton, QStringLiteral("hardware.power.hwp.apply"), QStringLiteral("应用到全部逻辑处理器"));
    m_applyHwpButton->setToolTip(
        QStringLiteral("把上方 Speed Shift/HWP 的最小、最大、期望性能与能效偏好应用到所有 CPU 逻辑核心"));
    hwpLayout->addWidget(hwpMinimumLabel, 0, 0);
    // HWP 从两组横排字段收为单列，各字段的取值范围和业务含义不变。
    hwpLayout->addWidget(m_hwpMinimumSpin, 1, 0);
    hwpLayout->addWidget(hwpMaximumLabel, 2, 0);
    hwpLayout->addWidget(m_hwpMaximumSpin, 3, 0);
    hwpDesiredLabel->setWordWrap(true);
    hwpLayout->addWidget(hwpDesiredLabel, 4, 0);
    hwpLayout->addWidget(m_hwpDesiredSpin, 5, 0);
    hwpEppLabel->setWordWrap(true);
    hwpLayout->addWidget(hwpEppLabel, 6, 0);
    hwpLayout->addWidget(m_hwpEppSpin, 7, 0);
    hwpLayout->addWidget(m_applyHwpButton, 8, 0);
    contentLayout->addWidget(hwpGroup, 0);

    // 风险说明改为常驻文字：每次点击“应用”时仍会弹出确认框，
    // 不再要求先勾选确认框才能解锁按钮（重复门槛，且忘记勾选只会得到一句报错）。
    auto* riskNoticeLabel = new QLabel(
        QStringLiteral("注意：提高功耗/倍率可能导致过热、降频、数据错误、死机或硬件寿命下降；请先保存工作并准备好恢复方案。"),
        contentWidget);
    riskNoticeLabel->setWordWrap(true);
    riskNoticeLabel->setStyleSheet(
        QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
    language.bindText(
        riskNoticeLabel,
        QStringLiteral("hardware.power.risk_notice"),
        QStringLiteral("注意：提高功耗/倍率可能导致过热、降频、数据错误、死机或硬件寿命下降；请先保存工作并准备好恢复方案。"));
    contentLayout->insertWidget(1, riskNoticeLabel); // 风险始终位于参数前部，不收入折叠区。

    // 四个调节组保持原控件与参数语义，各组按自然高度纵向排列。
    for (QGridLayout* groupLayout : { schemeLayout, raplLayout, turboLayout, hwpLayout })
    {
        groupLayout->setHorizontalSpacing(8);
        groupLayout->setVerticalSpacing(8);
    }
    const QList<QWidget*> controls { m_powerSchemeCombo, m_applyPowerSchemeButton, m_refreshAllButton,
        m_restoreInitialStateButton, m_pl1Spin, m_pl2Spin, m_applyPowerLimitsButton, m_raisePowerLimitsButton,
        m_applyTurboButton, m_turboRatioSpin, m_applyTurboRatioButton, m_requestedMultiplierSpin,
        m_applyRequestedMultiplierButton, m_hwpMinimumSpin, m_hwpMaximumSpin, m_hwpDesiredSpin, m_hwpEppSpin,
        m_applyHwpButton };
    for (QWidget* control : controls)
    {
        ks::ui::NormalizeToolbarControl(control);
    }
    // 数值参数此前只登记高度；显式接入输入底面与热主题，和方案下拉保持一致。
    const QList<QWidget*> parameterInputs { m_pl1Spin, m_pl2Spin, m_turboRatioSpin,
        m_requestedMultiplierSpin, m_hwpMinimumSpin, m_hwpMaximumSpin,
        m_hwpDesiredSpin, m_hwpEppSpin };
    for (QWidget* input : parameterInputs)
    {
        ks::ui::StyleStandaloneInput(input);
    }
    // 刷新/还原是全页动作，固定在参数栏底部；分组应用仍邻近其参数。
    auto* pageActions = new QWidget(workspace);
    auto* pageActionLayout = new QVBoxLayout(pageActions);
    pageActionLayout->setContentsMargins(0, 0, 0, 0);
    pageActionLayout->setSpacing(8);
    pageActionLayout->addWidget(m_refreshAllButton);
    pageActionLayout->addWidget(m_restoreInitialStateButton);
    workspace->actions()->addWidget(pageActions, 1);
    workspace->finish();
    updateControlAvailability();
}

void HardwarePowerPage::initializeConnections()
{
    connect(m_refreshAllButton, &QPushButton::clicked, this, [this]() {
        refreshAll();
    });
    connect(m_applyPowerSchemeButton, &QPushButton::clicked, this, [this]() {
        applySelectedPowerScheme();
    });
    connect(m_restoreInitialStateButton, &QPushButton::clicked, this, [this]() {
        restoreInitialState();
    });
    connect(m_applyPowerLimitsButton, &QPushButton::clicked, this, [this]() {
        sendControlRequest(
            KSWORD_ARK_CPU_POWER_APPLY_POWER_LIMITS,
            powerText(
                QStringLiteral("hardware.power.confirm.rapl"),
                QStringLiteral("即将修改全部处理器封装的 PL1/PL2。过高值可能超过散热和供电能力，过低值可能导致严重限频。是否继续？")));
    });
    connect(m_raisePowerLimitsButton, &QPushButton::clicked, this, [this]() {
        raisePowerLimitsToPlatformMaximum();
    });
    connect(m_applyTurboButton, &QPushButton::clicked, this, [this]() {
        sendControlRequest(
            KSWORD_ARK_CPU_POWER_APPLY_TURBO,
            powerText(
                QStringLiteral("hardware.power.confirm.turbo"),
                QStringLiteral("即将在全部逻辑处理器修改 Intel Turbo Boost 开关。该值可能被固件或 Windows 重写。是否继续？")));
    });
    connect(m_applyTurboRatioButton, &QPushButton::clicked, this, [this]() {
        sendControlRequest(
            KSWORD_ARK_CPU_POWER_APPLY_TURBO_RATIO,
            powerText(
                QStringLiteral("hardware.power.confirm.ratio"),
                QStringLiteral("即将把 MSR_TURBO_RATIO_LIMIT 中所有已实现档位改为同一倍率。超出 CPU、主板或散热能力可能立即死机或产生计算错误。是否继续？")));
    });
    connect(m_applyRequestedMultiplierButton, &QPushButton::clicked, this, [this]() {
        sendControlRequest(
            KSWORD_ARK_CPU_POWER_APPLY_PERF_CONTROL,
            powerText(
                QStringLiteral("hardware.power.confirm.requested_multiplier"),
                QStringLiteral("即将在全部逻辑处理器修改 IA32_PERF_CTL 请求倍频。Speed Shift、固件或微码可能限制或忽略该请求；过高倍率可能导致过热、死机或计算错误。是否继续？")));
    });
    connect(m_applyHwpButton, &QPushButton::clicked, this, [this]() {
        sendControlRequest(
            KSWORD_ARK_CPU_POWER_APPLY_HWP,
            powerText(
                QStringLiteral("hardware.power.confirm.hwp"),
                QStringLiteral("即将在全部逻辑处理器修改 HWP min/max/desired/EPP。设置可能与 Windows 电源策略竞争。是否继续？")));
    });
}

namespace { HardwarePowerSchemeSnapshot collectPowerSchemeSnapshot(const ks::ui::AsyncOperationToken* token); }

void HardwarePowerPage::refreshAll()
{
    updateControlAvailability();
    if (m_queryOperation->isBusy() || m_confirming || m_restorePreparing) return;
    struct Result { HardwarePowerSchemeSnapshot schemes; ksword::ark::CpuPowerResult cpu; };
    m_queryPending = true;
    setStatus(powerText(QStringLiteral("hardware.power.status.refreshing"),
        QStringLiteral("状态：正在刷新 Windows 电源方案与 R0 CPU 电源快照...")));
    const auto consume = [this](const Result& result) {
        m_queryPending = false;
        applyPowerSchemesSnapshot(result.schemes);
        applySnapshotToUi(result.cpu);
    };
    m_queryOperation->submit<int, Result>(0, [](int, const ks::ui::AsyncOperationToken& token) {
        Result result;
        result.schemes = collectPowerSchemeSnapshot(&token);
        if (!token.isCanceled())
        {
            const ksword::ark::DriverClient client;
            result.cpu = client.queryCpuPowerState();
        }
        return result;
    }, [this, consume](const Result& result, std::uint64_t ticket) {
        const QPointer<HardwarePowerPage> guard(this);
        if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("hardware-power-query"),
            {m_snapshotTable}, [guard, result, consume, ticket]() {
                if (guard == nullptr || !guard->m_queryOperation->isCurrent(ticket)) return;
                consume(result);
                guard->m_queryOperation->completeDeferred(ticket);
            })) return false;
        consume(result);
        return true;
    }, [this](std::exception_ptr failure) {
        m_queryPending = false;
        ksword::ark::CpuPowerResult result;
        try { if (failure) std::rethrow_exception(failure); }
        catch (const std::exception& error) { result.io.message = error.what(); }
        catch (...) {}
        applySnapshotToUi(result);
    });
}

namespace
{
    HardwarePowerSchemeSnapshot collectPowerSchemeSnapshot(const ks::ui::AsyncOperationToken* token)
    {
        HardwarePowerSchemeSnapshot result;
        GUID* activeGuid = nullptr;
        const DWORD activeError = ::PowerGetActiveScheme(nullptr, &activeGuid);


        for (ULONG schemeIndex = 0UL;; ++schemeIndex)
        {
            if (token != nullptr && token->isCanceled()) break;
            GUID schemeGuid{};
            DWORD guidBytesCount = sizeof(schemeGuid);
            const DWORD enumerateError = ::PowerEnumerate(
                nullptr,
                nullptr,
                nullptr,
                ACCESS_SCHEME,
                schemeIndex,
                reinterpret_cast<UCHAR*>(&schemeGuid),
                &guidBytesCount);
            if (enumerateError == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (enumerateError != ERROR_SUCCESS || guidBytesCount != sizeof(schemeGuid))
            {
                result.enumerationError = enumerateError != ERROR_SUCCESS ? enumerateError : ERROR_INVALID_DATA;
                result.enumerationErrorIndex = schemeIndex;
                break;
            }

            result.schemes.push_back({ powerSchemeFriendlyName(schemeGuid), guidBytes(schemeGuid),
                activeError == ERROR_SUCCESS && activeGuid != nullptr && ::IsEqualGUID(*activeGuid, schemeGuid) });
        }
        if (activeError == ERROR_SUCCESS && activeGuid != nullptr) result.activeGuid = guidBytes(*activeGuid);
        if (activeGuid != nullptr) ::LocalFree(activeGuid);
        return result;
    }
}


namespace
{
    unsigned long restorableCpuApplyFlags(
        bool hasSnapshot, const KSWORD_ARK_CPU_POWER_RESPONSE& snapshot,
        bool hasRestoreSnapshot, const KSWORD_ARK_CPU_POWER_RESPONSE& restoreSnapshot)
{
    if (!hasSnapshot || !hasRestoreSnapshot ||
        snapshot.vendor != KSWORD_ARK_CPU_POWER_VENDOR_INTEL ||
        restoreSnapshot.vendor != KSWORD_ARK_CPU_POWER_VENDOR_INTEL)
    {
        return 0UL;
    }

    unsigned long applyFlags = 0UL;
    if ((snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_PACKAGE_POWER_PROGRAMMABLE) != 0ULL &&
        (snapshot.responseFlags &
            KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_POWER_LIMIT_LOCKED) == 0UL &&
        (snapshot.fieldFlags &
            (KSWORD_ARK_CPU_POWER_FIELD_RAPL_UNIT |
             KSWORD_ARK_CPU_POWER_FIELD_PACKAGE_POWER_LIMIT)) ==
            (KSWORD_ARK_CPU_POWER_FIELD_RAPL_UNIT |
             KSWORD_ARK_CPU_POWER_FIELD_PACKAGE_POWER_LIMIT) &&
        (restoreSnapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_PACKAGE_POWER_LIMIT) != 0UL)
    {
        applyFlags |= KSWORD_ARK_CPU_POWER_APPLY_POWER_LIMITS;
    }
    if ((snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_TURBO_CONTROL) != 0ULL &&
        (snapshot.fieldFlags & KSWORD_ARK_CPU_POWER_FIELD_MISC_ENABLE) != 0UL &&
        (restoreSnapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_MISC_ENABLE) != 0UL)
    {
        applyFlags |= KSWORD_ARK_CPU_POWER_APPLY_TURBO;
    }
    if ((snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_HWP_ENABLED) != 0ULL &&
        (snapshot.fieldFlags &
            (KSWORD_ARK_CPU_POWER_FIELD_HWP_CAPABILITIES |
             KSWORD_ARK_CPU_POWER_FIELD_HWP_REQUEST)) ==
            (KSWORD_ARK_CPU_POWER_FIELD_HWP_CAPABILITIES |
             KSWORD_ARK_CPU_POWER_FIELD_HWP_REQUEST) &&
        (restoreSnapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_HWP_REQUEST) != 0UL)
    {
        applyFlags |= KSWORD_ARK_CPU_POWER_APPLY_HWP;
    }
    if ((snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_TURBO_RATIO_PROGRAMMABLE) != 0ULL &&
        (snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_TURBO_RATIO_LIMIT) != 0UL &&
        (restoreSnapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_TURBO_RATIO_LIMIT) != 0UL)
    {
        applyFlags |= KSWORD_ARK_CPU_POWER_APPLY_TURBO_RATIO;
    }
    if ((snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_PERF_CONTROL_PROGRAMMABLE) != 0ULL &&
        (snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_PERF_CONTROL) != 0UL &&
        (restoreSnapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_PERF_CONTROL) != 0UL &&
        restoreSnapshot.requestedMultiplier > 0UL &&
        restoreSnapshot.requestedMultiplier <= 0xFFUL)
    {
        applyFlags |= KSWORD_ARK_CPU_POWER_APPLY_PERF_CONTROL;
    }
    return applyFlags;
}

    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST buildRestoreControlRequest(
        const unsigned long applyFlags, const KSWORD_ARK_CPU_POWER_RESPONSE& snapshot,
        const KSWORD_ARK_CPU_POWER_RESPONSE& restoreSnapshot)
{
    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST request{};
    request.size = sizeof(request);
    request.version = KSWORD_ARK_CPU_POWER_PROTOCOL_VERSION;
    request.applyFlags = applyFlags;
    request.requestFlags = KSWORD_ARK_CPU_POWER_REQUEST_FLAG_UI_CONFIRMED |
        KSWORD_ARK_CPU_POWER_REQUEST_FLAG_REQUIRE_CURRENT;
    if ((applyFlags & KSWORD_ARK_CPU_POWER_APPLY_TURBO_RATIO) != 0UL)
    {
        request.requestFlags |=
            KSWORD_ARK_CPU_POWER_REQUEST_FLAG_TURBO_RATIO_ARRAY;
    }

    request.pl1Milliwatts = restoreSnapshot.pl1Milliwatts;
    request.pl2Milliwatts = restoreSnapshot.pl2Milliwatts;
    request.pl1Enabled = restoreSnapshot.pl1Enabled;
    request.pl1ClampEnabled = restoreSnapshot.pl1ClampEnabled;
    request.pl2Enabled = restoreSnapshot.pl2Enabled;
    request.pl2ClampEnabled = restoreSnapshot.pl2ClampEnabled;
    request.turboEnabled = restoreSnapshot.turboEnabled;
    request.hwpMinimumPerformance =
        restoreSnapshot.hwpMinimumPerformance;
    request.hwpMaximumPerformance =
        restoreSnapshot.hwpMaximumPerformance;
    request.hwpDesiredPerformance =
        restoreSnapshot.hwpDesiredPerformance;
    request.hwpEnergyPerformancePreference =
        (snapshot.capabilityFlags & KSWORD_ARK_CPU_POWER_CAP_HWP_EPP) != 0ULL
        ? restoreSnapshot.hwpEnergyPerformancePreference
        : snapshot.hwpEnergyPerformancePreference;
    request.requestedMultiplier = restoreSnapshot.requestedMultiplier;
    for (unsigned long ratioIndex = 0UL;
        ratioIndex < KSWORD_ARK_CPU_POWER_TURBO_RATIO_COUNT;
        ++ratioIndex)
    {
        request.turboRatios[ratioIndex] =
            restoreSnapshot.turboRatios[ratioIndex];
        if (request.turboRatio == 0UL && request.turboRatios[ratioIndex] != 0UL)
        {
            request.turboRatio = request.turboRatios[ratioIndex];
        }
    }

    request.expectedPackagePowerLimit = snapshot.msrPackagePowerLimit;
    request.expectedMiscEnable = snapshot.msrMiscEnable;
    request.expectedHwpRequest = snapshot.msrHwpRequest;
    request.expectedTurboRatioLimit = snapshot.msrTurboRatioLimit;
    request.expectedPerfControl = snapshot.msrPerfControl;
    return request;
}

    bool validCpuPowerSnapshot(const ksword::ark::CpuPowerResult& result)
    {
        return !result.unsupported && result.io.ok &&
            result.io.bytesReturned >= sizeof(KSWORD_ARK_CPU_POWER_RESPONSE) &&
            result.response.size >= sizeof(KSWORD_ARK_CPU_POWER_RESPONSE) &&
            result.response.version == KSWORD_ARK_CPU_POWER_PROTOCOL_VERSION;
    }

    HardwarePowerTransactionResult runPowerTransaction(const HardwarePowerTransactionRequest& request)
    {
        HardwarePowerTransactionResult result;
        result.kind = request.kind;
        if (request.kind != HardwarePowerTransactionRequest::Kind::Cpu && !request.schemeGuid.isEmpty())
        {
            GUID scheme{};
            if (!guidFromBytes(request.schemeGuid, &scheme))
            {
                result.invalidGuid = true;
                return result;
            }
            result.schemeError = ::PowerSetActiveScheme(nullptr, &scheme);
            if (result.schemeError != ERROR_SUCCESS) return result;
            result.schemeApplied = true;
            result.schemes = collectPowerSchemeSnapshot(nullptr);
            result.schemesRead = true;
        }
        if (request.kind == HardwarePowerTransactionRequest::Kind::Scheme) return result;

        const ksword::ark::DriverClient client;
        result.request = request.cpu;
        if (request.kind == HardwarePowerTransactionRequest::Kind::Restore)
        {
            if (!request.hasRestoreCpu) return result;
            // Windows may have changed the CPU request. Use this fresh raw value
            // for REQUIRE_CURRENT while preserving the first restore target.
            result.cpu = client.queryCpuPowerState();
            result.cpuRead = true;
            const unsigned long flags = restorableCpuApplyFlags(
                validCpuPowerSnapshot(result.cpu), result.cpu.response, true, request.restoreCpu);
            if (flags == 0UL) return result;
            result.request = buildRestoreControlRequest(flags, result.cpu.response, request.restoreCpu);
            result.preflightFailure = cpuPowerRequestFailureReason(result.request, result.cpu.response);
            if (result.preflightFailure != KSWORD_ARK_CPU_POWER_FAILURE_NONE) return result;
        }
        result.control = client.controlCpuPower(result.request);
        result.controlAttempted = true;
        result.cpu = client.queryCpuPowerState();
        result.cpuRead = true;
        return result;
    }
}



void HardwarePowerPage::applyPowerSchemesSnapshot(const HardwarePowerSchemeSnapshot& snapshot)
{
    if (m_powerSchemeCombo == nullptr) return;
    m_powerSchemeEnumerationError = snapshot.enumerationError == ERROR_SUCCESS ? QString()
        : powerText(QStringLiteral("hardware.power.error.scheme_enumerate"),
            QStringLiteral("Windows 电源方案枚举未完成（索引 %1，Win32=%2）；已保留此前读取的方案，请刷新重试。"))
            .arg(snapshot.enumerationErrorIndex).arg(snapshot.enumerationError);
    m_powerSchemeCombo->clear();
    int activeIndex = -1;
    for (const auto& scheme : snapshot.schemes)
    {
        QString name = scheme.name;
        if (name.isEmpty()) name = powerText(QStringLiteral("hardware.power.scheme.unnamed"),
            QStringLiteral("未命名电源方案"));
        if (scheme.active) name += powerText(QStringLiteral("hardware.power.scheme.active_suffix"),
            QStringLiteral("（当前）"));
        m_powerSchemeCombo->addItem(name, scheme.guid);
        if (scheme.active) activeIndex = m_powerSchemeCombo->count()-1;
    }
    if (m_restorePowerSchemeGuid.isEmpty() && !snapshot.activeGuid.isEmpty())
        m_restorePowerSchemeGuid = snapshot.activeGuid;
    if (activeIndex >= 0) m_powerSchemeCombo->setCurrentIndex(activeIndex);
    updateControlAvailability();
    setStatus(m_statusText, m_statusIsError);
}

void HardwarePowerPage::applySelectedPowerScheme()
{
    updateControlAvailability();
    if (m_queryOperation->isBusy() || m_confirming || m_restorePreparing) return;
    if (m_powerSchemeCombo == nullptr || m_powerSchemeCombo->currentIndex() < 0)
    {
        QMessageBox::critical(this,
            powerText(QStringLiteral("hardware.power.error.title"), QStringLiteral("电源调节失败")),
            powerText(QStringLiteral("hardware.power.error.no_scheme"), QStringLiteral("没有可应用的 Windows 电源方案。")));
        return;
    }
    HardwarePowerTransactionRequest request;
    request.kind = HardwarePowerTransactionRequest::Kind::Scheme;
    request.schemeGuid = m_powerSchemeCombo->currentData().toByteArray();
    GUID scheme{};
    if (!guidFromBytes(request.schemeGuid, &scheme))
    {
        QMessageBox::critical(this,
            powerText(QStringLiteral("hardware.power.error.title"), QStringLiteral("电源调节失败")),
            powerText(QStringLiteral("hardware.power.error.scheme_data"), QStringLiteral("所选电源方案 GUID 数据无效。")));
        return;
    }
    submitPowerTransaction(request);
}

void HardwarePowerPage::restoreInitialState()
{
    updateControlAvailability();
    if (m_queryOperation->isBusy() || m_confirming || m_restorePreparing) return;
    if (m_restorePowerSchemeGuid.isEmpty() && !m_hasRestoreCpuSnapshot)
    {
        QMessageBox::critical(this,
            powerText(QStringLiteral("hardware.power.error.title"), QStringLiteral("电源调节失败")),
            powerText(QStringLiteral("hardware.power.error.no_restore"),
                QStringLiteral("尚未捕获可还原的首次状态，请先刷新。")));
        return;
    }
    m_restorePreparing = true;
    if (!m_hasRestoreCpuSnapshot)
    {
        confirmRestoreInitialState();
        return;
    }
    // Refresh capabilities before confirming, without admitting another action.
    m_queryPending = true;
    setStatus(powerText(QStringLiteral("hardware.power.status.refreshing"),
        QStringLiteral("状态：正在刷新 Windows 电源方案与 R0 CPU 电源快照...")));
    const auto consume = [this](const ksword::ark::CpuPowerResult& result) {
        m_queryPending = false;
        applySnapshotToUi(result);
        QTimer::singleShot(0, this, [this]() { confirmRestoreInitialState(); });
    };
    const auto ticket = m_queryOperation->submit<int, ksword::ark::CpuPowerResult>(0,
        [](int, const ks::ui::AsyncOperationToken&) {
            return ksword::ark::DriverClient().queryCpuPowerState();
        }, [this, consume](const ksword::ark::CpuPowerResult& result, std::uint64_t ticket) {
            const QPointer<HardwarePowerPage> guard(this);
            if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this,
                QStringLiteral("hardware-power-restore-query"), {m_snapshotTable},
                [guard, result, consume, ticket]() {
                    if (guard == nullptr || !guard->m_queryOperation->isCurrent(ticket)) return;
                    consume(result);
                    guard->m_queryOperation->completeDeferred(ticket);
                })) return false;
            consume(result);
            return true;
        }, [this](std::exception_ptr) {
            m_queryPending = false;
            m_restorePreparing = false;
            m_hasSnapshot = false;
            setStatus(powerText(QStringLiteral("hardware.power.status.query_dropped"),
                QStringLiteral("状态：电源查询已结束但未收到结果，请刷新。")), true);
        });
    if (ticket == 0)
    {
        m_queryPending = false;
        m_restorePreparing = false;
        reportPowerTransactionUnavailable(false);
    }
    updateControlAvailability();
}

void HardwarePowerPage::confirmRestoreInitialState()
{
    if (!m_restorePreparing || m_queryOperation->isBusy()) return;
    const unsigned long flags = restorableCpuApplyFlags();
    const QString validation = flags == 0UL ? QString()
        : validateCpuPowerRequestForUi(buildRestoreControlRequest(flags), m_snapshot);
    if (!validation.isEmpty())
    {
        m_restorePreparing = false;
        updateControlAvailability();
        setStatus(validation, true);
        QMessageBox::critical(this,
            powerText(QStringLiteral("hardware.power.error.title"), QStringLiteral("电源调节失败")), validation);
        return;
    }
    HardwarePowerTransactionRequest request;
    request.kind = HardwarePowerTransactionRequest::Kind::Restore;
    request.schemeGuid = m_restorePowerSchemeGuid;
    request.hasRestoreCpu = m_hasRestoreCpuSnapshot;
    request.restoreCpu = m_restoreCpuSnapshot;
    m_confirming = true;
    updateControlAvailability();
    const QPointer<HardwarePowerPage> guard(this);
    const int answer = QMessageBox::warning(this,
        powerText(QStringLiteral("hardware.power.confirm.restore_title"), QStringLiteral("确认一键还原")),
        powerText(QStringLiteral("hardware.power.confirm.restore"),
            QStringLiteral("即将恢复本页首次有效刷新时捕获的 Windows 电源方案，以及当前仍可写的 PL1/PL2、Turbo、HWP、Turbo Ratio 和请求倍频。固件或 Windows 仍可能再次重写这些值。是否继续？")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (guard == nullptr) return;
    m_confirming = false;
    m_restorePreparing = false;
    if (answer == QMessageBox::Yes) submitPowerTransaction(request);
    updateControlAvailability();
}



void HardwarePowerPage::applySnapshotToUi(const ksword::ark::CpuPowerResult& result)
{
    m_hasSnapshot = false;
    m_snapshotTable->clearContents();
    m_snapshotTable->setRowCount(0);
    m_snapshotTable->setHorizontalHeaderLabels({
        powerText(QStringLiteral("hardware.power.table.item"), QStringLiteral("项目")),
        powerText(QStringLiteral("hardware.power.table.value"), QStringLiteral("当前值")) });
    if (result.unsupported)
    {
        setStatus(
            powerText(
                QStringLiteral("hardware.power.status.old_driver"),
                QStringLiteral("状态：当前 KswordARK 驱动版本尚未集成 CPU 电源 IOCTL。")),
            true);
        updateControlAvailability();
        return;
    }
    if (!result.io.ok ||
        result.io.bytesReturned < sizeof(KSWORD_ARK_CPU_POWER_RESPONSE) ||
        result.response.size < sizeof(KSWORD_ARK_CPU_POWER_RESPONSE) ||
        result.response.version != KSWORD_ARK_CPU_POWER_PROTOCOL_VERSION)
    {
        setStatus(
            powerText(
                QStringLiteral("hardware.power.status.query_failed"),
                QStringLiteral("状态：R0 CPU 电源快照读取失败：%1"))
                .arg(QString::fromStdString(result.io.message)),
            true);
        updateControlAvailability();
        return;
    }

    m_snapshot = result.response;
    m_hasSnapshot = true;
    // 首次有效协议快照是还原基线；控制成功后的刷新不能覆盖它。
    if (!m_hasRestoreCpuSnapshot)
    {
        m_restoreCpuSnapshot = m_snapshot;
        m_hasRestoreCpuSnapshot = true;
    }
    const QString vendorText = QString::fromLatin1(m_snapshot.vendorId).trimmed();
    const QString brandText = QString::fromLatin1(m_snapshot.brandText).simplified();
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.cpu"), QStringLiteral("处理器")),
        QStringLiteral("%1 | %2").arg(vendorText, brandText));
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.identity"), QStringLiteral("身份 / 拓扑")),
        powerText(
            QStringLiteral("hardware.power.value.identity"),
            QStringLiteral("Family %1 Model %2 Stepping %3 | 逻辑处理器 %4 | 组 %5"))
            .arg(m_snapshot.family)
            .arg(m_snapshot.model)
            .arg(m_snapshot.stepping)
            .arg(m_snapshot.logicalProcessorCount)
            .arg(m_snapshot.processorGroupCount));

    QStringList capabilityParts;
    const auto appendCapability = [&capabilityParts](
        const unsigned long long mask,
        const unsigned long long available,
        const QString& text) {
        if ((available & mask) != 0ULL)
        {
            capabilityParts.append(text);
        }
    };
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_RAPL,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.rapl"), QStringLiteral("RAPL")));
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_PACKAGE_POWER_PROGRAMMABLE,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.power_write"), QStringLiteral("PL1/PL2 Write")));
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_TURBO_CONTROL,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.turbo"), QStringLiteral("Turbo")));
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_TURBO_RATIO_PROGRAMMABLE,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.turbo_ratio"), QStringLiteral("Turbo Ratio")));
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_PERF_CONTROL_PROGRAMMABLE,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.requested_multiplier"), QStringLiteral("请求倍频")));
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_HWP_ENABLED,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.hwp"), QStringLiteral("HWP")));
    appendCapability(
        KSWORD_ARK_CPU_POWER_CAP_HWP_EPP,
        m_snapshot.capabilityFlags,
        powerText(QStringLiteral("hardware.power.cap.hwp_epp"), QStringLiteral("HWP EPP")));
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.capabilities"), QStringLiteral("可用能力")),
        capabilityParts.isEmpty()
            ? powerText(QStringLiteral("hardware.power.value.none"), QStringLiteral("无可写 R0 能力"))
            : capabilityParts.join(QStringLiteral(", ")));

    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.rapl_units"), QStringLiteral("RAPL 单位")),
        powerText(
            QStringLiteral("hardware.power.value.rapl_units"),
            QStringLiteral("%1 μW / unit | %2 ns / time unit"))
            .arg(m_snapshot.powerUnitMicrowatts)
            .arg(m_snapshot.timeUnitNanoseconds));
    addSnapshotRow(
        m_snapshotTable,
        QStringLiteral("PL1 / PL2"),
        powerText(
            QStringLiteral("hardware.power.value.limits"),
            QStringLiteral("PL1 %1 W（启用=%2 Clamp=%3） | PL2 %4 W（启用=%5 Clamp=%6）"))
            .arg(m_snapshot.pl1Milliwatts / 1000.0, 0, 'f', 3)
            .arg(m_snapshot.pl1Enabled)
            .arg(m_snapshot.pl1ClampEnabled)
            .arg(m_snapshot.pl2Milliwatts / 1000.0, 0, 'f', 3)
            .arg(m_snapshot.pl2Enabled)
            .arg(m_snapshot.pl2ClampEnabled));
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.sku_range"), QStringLiteral("SKU 功耗范围")),
        powerText(
            QStringLiteral("hardware.power.value.sku_range"),
            QStringLiteral("TDP %1 W | 最小 %2 W | 最大 %3 W | Lock=%4"))
            .arg(m_snapshot.packageTdpMilliwatts / 1000.0, 0, 'f', 3)
            .arg(m_snapshot.packageMinimumPowerMilliwatts / 1000.0, 0, 'f', 3)
            .arg(m_snapshot.packageMaximumPowerMilliwatts / 1000.0, 0, 'f', 3)
            .arg((m_snapshot.responseFlags & KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_POWER_LIMIT_LOCKED) != 0UL ? 1 : 0));
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.turbo"), QStringLiteral("Turbo / Ratio")),
        powerText(
            QStringLiteral("hardware.power.value.turbo"),
            QStringLiteral("Turbo=%1 | Non-Turbo=%2x | 请求=%3x | 当前=%4x | Ratio MSR=%5"))
            .arg(m_snapshot.turboEnabled)
            .arg(m_snapshot.maximumNonTurboRatio)
            .arg(m_snapshot.requestedMultiplier)
            .arg(m_snapshot.currentMultiplier)
            .arg(rawMsrText(m_snapshot.msrTurboRatioLimit)));
    addSnapshotRow(
        m_snapshotTable,
        QStringLiteral("HWP"),
        powerText(
            QStringLiteral("hardware.power.value.hwp"),
            QStringLiteral("能力 高=%1 保证=%2 高效=%3 低=%4 | 请求 Min=%5 Max=%6 Desired=%7 EPP=%8"))
            .arg(m_snapshot.hwpHighestPerformance)
            .arg(m_snapshot.hwpGuaranteedPerformance)
            .arg(m_snapshot.hwpMostEfficientPerformance)
            .arg(m_snapshot.hwpLowestPerformance)
            .arg(m_snapshot.hwpMinimumPerformance)
            .arg(m_snapshot.hwpMaximumPerformance)
            .arg(m_snapshot.hwpDesiredPerformance)
            .arg(m_snapshot.hwpEnergyPerformancePreference));
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.raw"), QStringLiteral("原始 MSR")),
        QStringLiteral("0x610=%1 | 0x1A0=%2 | 0x774=%3 | 0x198=%4 | 0x199=%5")
            .arg(rawMsrText(m_snapshot.msrPackagePowerLimit))
            .arg(rawMsrText(m_snapshot.msrMiscEnable))
            .arg(rawMsrText(m_snapshot.msrHwpRequest))
            .arg(rawMsrText(m_snapshot.msrPerfStatus))
            .arg(rawMsrText(m_snapshot.msrPerfControl)));
    addSnapshotRow(
        m_snapshotTable,
        powerText(QStringLiteral("hardware.power.row.status"), QStringLiteral("R0 状态")),
        powerText(
            QStringLiteral("hardware.power.value.r0_status"),
            QStringLiteral("NTSTATUS=%1 | field=0x%2 | flags=0x%3 | reason=%4"))
            .arg(ntStatusText(m_snapshot.lastStatus))
            .arg(m_snapshot.fieldFlags, 8, 16, QChar('0'))
            .arg(m_snapshot.responseFlags, 8, 16, QChar('0'))
            .arg(m_snapshot.failureReason)
            .toUpper());

    const double platformMinimumWatts = m_snapshot.packageMinimumPowerMilliwatts > 0UL
        ? m_snapshot.packageMinimumPowerMilliwatts / 1000.0
        : 0.001;
    double platformMaximumWatts = m_snapshot.packageMaximumPowerMilliwatts > 0UL
        ? m_snapshot.packageMaximumPowerMilliwatts / 1000.0
        : 1000.0;
    platformMaximumWatts = std::max({
        platformMaximumWatts,
        m_snapshot.pl1Milliwatts / 1000.0,
        m_snapshot.pl2Milliwatts / 1000.0,
        platformMinimumWatts });
    m_pl1Spin->setRange(platformMinimumWatts, platformMaximumWatts);
    m_pl2Spin->setRange(platformMinimumWatts, platformMaximumWatts);
    if (m_snapshot.pl1Milliwatts > 0UL)
    {
        m_pl1Spin->setValue(m_snapshot.pl1Milliwatts / 1000.0);
    }
    if (m_snapshot.pl2Milliwatts > 0UL)
    {
        m_pl2Spin->setValue(m_snapshot.pl2Milliwatts / 1000.0);
    }
    m_pl1EnableCheck->setChecked(m_snapshot.pl1Enabled != 0UL);
    m_pl1ClampCheck->setChecked(m_snapshot.pl1ClampEnabled != 0UL);
    m_pl2EnableCheck->setChecked(m_snapshot.pl2Enabled != 0UL);
    m_pl2ClampCheck->setChecked(m_snapshot.pl2ClampEnabled != 0UL);
    m_turboEnableCheck->setChecked(m_snapshot.turboEnabled != 0UL);

    int displayedRatio = 0;
    for (const unsigned long ratio : m_snapshot.turboRatios)
    {
        if (ratio != 0UL)
        {
            displayedRatio = static_cast<int>(ratio);
        }
    }
    if (displayedRatio > 0)
    {
        m_turboRatioSpin->setValue(displayedRatio);
    }
    if (m_snapshot.requestedMultiplier > 0UL)
    {
        m_requestedMultiplierSpin->setValue(
            static_cast<int>(m_snapshot.requestedMultiplier));
    }
    const int hwpLowest = m_snapshot.hwpLowestPerformance != 0UL
        ? static_cast<int>(m_snapshot.hwpLowestPerformance)
        : 0;
    const int hwpHighest = m_snapshot.hwpHighestPerformance != 0UL
        ? std::max(hwpLowest, static_cast<int>(m_snapshot.hwpHighestPerformance))
        : 255;
    m_hwpMinimumSpin->setRange(hwpLowest, hwpHighest);
    m_hwpMaximumSpin->setRange(hwpLowest, hwpHighest);
    m_hwpDesiredSpin->setRange(0, hwpHighest);
    m_hwpMinimumSpin->setValue(static_cast<int>(m_snapshot.hwpMinimumPerformance));
    m_hwpMaximumSpin->setValue(static_cast<int>(m_snapshot.hwpMaximumPerformance));
    m_hwpDesiredSpin->setValue(static_cast<int>(m_snapshot.hwpDesiredPerformance));
    m_hwpEppSpin->setValue(static_cast<int>(m_snapshot.hwpEnergyPerformancePreference));

    if ((m_snapshot.responseFlags & KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_UNSUPPORTED_VENDOR) != 0UL)
    {
        setStatus(
            powerText(
                QStringLiteral("hardware.power.status.vendor_unsupported"),
                QStringLiteral("状态：该 CPU 厂商暂不支持 R0 调节；Windows 电源方案仍可使用。AMD SMU/PBO 不会按 Intel MSR 方式写入。")),
            true);
    }
    else if ((m_snapshot.responseFlags & KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_POWER_LIMIT_LOCKED) != 0UL)
    {
        setStatus(
            powerText(
                QStringLiteral("hardware.power.status.locked"),
                QStringLiteral("状态：CPU 快照已刷新；RAPL 功耗限制已被 BIOS/固件锁定，本页不会尝试清除 lock 位。")),
            true);
    }
    else if ((m_snapshot.responseFlags & KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_PARTIAL_MSR) != 0UL)
    {
        setStatus(
            powerText(
                QStringLiteral("hardware.power.status.partial"),
                QStringLiteral("状态：CPU 快照部分可用；不可读 MSR 已禁用对应控件。R0=%1"))
                .arg(ntStatusText(m_snapshot.lastStatus)),
            true);
    }
    else
    {
        setStatus(powerText(
            QStringLiteral("hardware.power.status.ready"),
            QStringLiteral("状态：CPU 电源快照已刷新；仅能力探测通过的控件可用。")));
    }
    updateControlAvailability();
}

void HardwarePowerPage::updateControlAvailability()
{
    const bool operationBusy = m_queryOperation != nullptr && m_queryOperation->isBusy();
    if (m_transactionPending && !operationBusy)
    {
        m_transactionPending = false;
        reportPowerTransactionUnavailable(true);
    }
    if (m_queryPending && !operationBusy)
    {
        m_queryPending = false;
        m_restorePreparing = false;
        m_hasSnapshot = false;
        setStatus(powerText(QStringLiteral("hardware.power.status.query_dropped"),
            QStringLiteral("状态：电源查询已结束但未收到结果，请刷新。")), true);
    }
    const bool busy = operationBusy || m_confirming || m_restorePreparing;
    m_refreshAllButton->setEnabled(!busy);
    const bool hasSchemes = m_powerSchemeCombo->count() > 0;
    m_powerSchemeCombo->setEnabled(!busy && hasSchemes);
    m_applyPowerSchemeButton->setEnabled(!busy && hasSchemes);
    if (busy)
    {
        const QList<QWidget*> controls { m_restoreInitialStateButton, m_pl1Spin, m_pl2Spin,
            m_pl1EnableCheck, m_pl1ClampCheck, m_pl2EnableCheck, m_pl2ClampCheck,
            m_applyPowerLimitsButton, m_raisePowerLimitsButton, m_turboEnableCheck, m_applyTurboButton,
            m_turboRatioSpin, m_applyTurboRatioButton, m_requestedMultiplierSpin,
            m_applyRequestedMultiplierButton, m_hwpMinimumSpin, m_hwpMaximumSpin,
            m_hwpDesiredSpin, m_hwpEppSpin, m_applyHwpButton };
        for (QWidget* control : controls) control->setEnabled(false);
        return;
    }

    const bool hasIntelSnapshot = m_hasSnapshot &&
        m_snapshot.vendor == KSWORD_ARK_CPU_POWER_VENDOR_INTEL;
    const bool powerLimitLocked = m_hasSnapshot &&
        (m_snapshot.responseFlags &
            KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_POWER_LIMIT_LOCKED) != 0UL;
    const bool canProgramPowerLimits = hasIntelSnapshot &&
        !powerLimitLocked &&
        (m_snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_PACKAGE_POWER_PROGRAMMABLE) != 0ULL &&
        (m_snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_PACKAGE_POWER_LIMIT) != 0UL;
    const bool canControlTurbo = hasIntelSnapshot &&
        (m_snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_TURBO_CONTROL) != 0ULL &&
        (m_snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_MISC_ENABLE) != 0UL;
    const bool canProgramTurboRatio = hasIntelSnapshot &&
        (m_snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_TURBO_RATIO_PROGRAMMABLE) != 0ULL &&
        (m_snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_TURBO_RATIO_LIMIT) != 0UL;
    const bool canProgramRequestedMultiplier = hasIntelSnapshot &&
        (m_snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_PERF_CONTROL_PROGRAMMABLE) != 0ULL &&
        (m_snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_PERF_CONTROL) != 0UL;
    const bool canControlHwp = hasIntelSnapshot &&
        (m_snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_HWP_ENABLED) != 0ULL &&
        (m_snapshot.fieldFlags &
            KSWORD_ARK_CPU_POWER_FIELD_HWP_REQUEST) != 0UL;
    const bool canControlHwpEpp = canControlHwp &&
        (m_snapshot.capabilityFlags &
            KSWORD_ARK_CPU_POWER_CAP_HWP_EPP) != 0ULL;
    const bool hasPlatformMaximum = canProgramPowerLimits &&
        m_snapshot.packageMaximumPowerMilliwatts > 0UL &&
        (m_snapshot.responseFlags &
            KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_PLATFORM_MAX_UNKNOWN) == 0UL;

    for (QWidget* widget : {
            static_cast<QWidget*>(m_pl1Spin),
            static_cast<QWidget*>(m_pl2Spin),
            static_cast<QWidget*>(m_pl1EnableCheck),
            static_cast<QWidget*>(m_pl1ClampCheck),
            static_cast<QWidget*>(m_pl2EnableCheck),
            static_cast<QWidget*>(m_pl2ClampCheck) })
    {
        if (widget != nullptr)
        {
            widget->setEnabled(canProgramPowerLimits);
        }
    }
    m_applyPowerLimitsButton->setEnabled(canProgramPowerLimits);
    m_raisePowerLimitsButton->setEnabled(hasPlatformMaximum);

    m_turboEnableCheck->setEnabled(canControlTurbo);
    m_applyTurboButton->setEnabled(canControlTurbo);
    m_turboRatioSpin->setEnabled(canProgramTurboRatio);
    m_applyTurboRatioButton->setEnabled(canProgramTurboRatio);
    m_requestedMultiplierSpin->setEnabled(canProgramRequestedMultiplier);
    m_applyRequestedMultiplierButton->setEnabled(
        canProgramRequestedMultiplier);

    m_hwpMinimumSpin->setEnabled(canControlHwp);
    m_hwpMaximumSpin->setEnabled(canControlHwp);
    m_hwpDesiredSpin->setEnabled(canControlHwp);
    m_hwpEppSpin->setEnabled(canControlHwpEpp);
    m_applyHwpButton->setEnabled(canControlHwp);

    const bool hasRestoreBaseline = !m_restorePowerSchemeGuid.isEmpty() ||
        m_hasRestoreCpuSnapshot;
    m_restoreInitialStateButton->setEnabled(hasRestoreBaseline);
}

void HardwarePowerPage::setStatus(const QString& text, const bool isError)
{
    if (m_statusLabel == nullptr)
    {
        return;
    }
    m_statusText = text;
    m_statusIsError = isError;
    m_statusLabel->setText(m_powerSchemeEnumerationError.isEmpty()
        ? text : text + QLatin1Char('\n') + m_powerSchemeEnumerationError);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-weight:600;color:%1;")
            .arg(isError || !m_powerSchemeEnumerationError.isEmpty()
                ? KswordTheme::ErrorHex()
                : KswordTheme::PrimaryBlueHex));
}

KSWORD_ARK_CPU_POWER_CONTROL_REQUEST HardwarePowerPage::buildControlRequest(
    const unsigned long applyFlags) const
{
    const auto wattsToMilliwatts = [](const double watts) {
        const double boundedWatts = std::clamp(watts, 0.001, 1000.0);
        return static_cast<unsigned long>(
            std::llround(boundedWatts * 1000.0));
    };

    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST request{};
    request.size = sizeof(request);
    request.version = KSWORD_ARK_CPU_POWER_PROTOCOL_VERSION;
    request.applyFlags = applyFlags;
    request.requestFlags = KSWORD_ARK_CPU_POWER_REQUEST_FLAG_UI_CONFIRMED |
        KSWORD_ARK_CPU_POWER_REQUEST_FLAG_REQUIRE_CURRENT;
    request.pl1Milliwatts = wattsToMilliwatts(m_pl1Spin->value());
    request.pl2Milliwatts = wattsToMilliwatts(m_pl2Spin->value());
    request.pl1Enabled = m_pl1EnableCheck->isChecked() ? 1UL : 0UL;
    request.pl1ClampEnabled = m_pl1ClampCheck->isChecked() ? 1UL : 0UL;
    request.pl2Enabled = m_pl2EnableCheck->isChecked() ? 1UL : 0UL;
    request.pl2ClampEnabled = m_pl2ClampCheck->isChecked() ? 1UL : 0UL;
    request.turboEnabled = m_turboEnableCheck->isChecked() ? 1UL : 0UL;
    request.hwpMinimumPerformance =
        static_cast<unsigned long>(m_hwpMinimumSpin->value());
    request.hwpMaximumPerformance =
        static_cast<unsigned long>(m_hwpMaximumSpin->value());
    request.hwpDesiredPerformance =
        static_cast<unsigned long>(m_hwpDesiredSpin->value());
    request.hwpEnergyPerformancePreference =
        static_cast<unsigned long>(m_hwpEppSpin->value());
    request.turboRatio =
        static_cast<unsigned long>(m_turboRatioSpin->value());
    request.requestedMultiplier =
        static_cast<unsigned long>(m_requestedMultiplierSpin->value());
    for (unsigned long ratioIndex = 0UL;
        ratioIndex < KSWORD_ARK_CPU_POWER_TURBO_RATIO_COUNT;
        ++ratioIndex)
    {
        request.turboRatios[ratioIndex] = request.turboRatio;
    }
    request.expectedPackagePowerLimit = m_snapshot.msrPackagePowerLimit;
    request.expectedMiscEnable = m_snapshot.msrMiscEnable;
    request.expectedHwpRequest = m_snapshot.msrHwpRequest;
    request.expectedTurboRatioLimit = m_snapshot.msrTurboRatioLimit;
    request.expectedPerfControl = m_snapshot.msrPerfControl;
    return request;
}

unsigned long HardwarePowerPage::restorableCpuApplyFlags() const
{
    return ::restorableCpuApplyFlags(m_hasSnapshot, m_snapshot,
        m_hasRestoreCpuSnapshot, m_restoreCpuSnapshot);
}

KSWORD_ARK_CPU_POWER_CONTROL_REQUEST HardwarePowerPage::buildRestoreControlRequest(
    const unsigned long applyFlags) const
{
    return ::buildRestoreControlRequest(applyFlags, m_snapshot, m_restoreCpuSnapshot);
}

bool HardwarePowerPage::presentControlResult(
    const KSWORD_ARK_CPU_POWER_CONTROL_REQUEST& request,
    const ksword::ark::CpuPowerResult& result)
{
    const QString errorTitle = powerText(QStringLiteral("hardware.power.error.title"),
        QStringLiteral("电源调节失败"));
    const bool responseValid =
        result.io.bytesReturned >= sizeof(KSWORD_ARK_CPU_POWER_RESPONSE) &&
        result.response.size >= sizeof(KSWORD_ARK_CPU_POWER_RESPONSE) &&
        result.response.version == KSWORD_ARK_CPU_POWER_PROTOCOL_VERSION;
    if (!result.io.ok || !responseValid || result.response.lastStatus != 0L)
    {
        QString detail = QString::fromStdString(result.io.message);
        if (responseValid &&
            (result.response.responseFlags &
                KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_STALE_SNAPSHOT) != 0UL)
        {
            detail = powerText(
                QStringLiteral("hardware.power.error.stale"),
                QStringLiteral("设置在提交前已被固件、Windows 或其他工具改变，请刷新后重试。"));
        }
        else if (responseValid &&
            (result.response.responseFlags &
                KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_WRITE_PARTIAL) != 0UL)
        {
            detail = powerText(
                QStringLiteral("hardware.power.error.partial_write"),
                QStringLiteral("部分逻辑处理器写入或回读失败；请立即刷新并检查当前值。R0=%1"))
                .arg(ntStatusText(result.response.lastStatus));
        }
        else if (responseValid &&
            result.response.failureReason !=
                KSWORD_ARK_CPU_POWER_FAILURE_NONE)
        {
            detail = cpuPowerFailureReasonText(
                result.response.failureReason,
                request,
                result.response) + QStringLiteral("\n") + detail;
        }
        const QString message = powerText(
            QStringLiteral("hardware.power.error.control"),
            QStringLiteral("CPU 电源设置未完整应用：%1"))
            .arg(detail);
        setStatus(message, true);
        QMessageBox::critical(this, errorTitle, message);
        return false;
    }

    setStatus(powerText(
        QStringLiteral("hardware.power.status.applied"),
        QStringLiteral("状态：CPU 电源设置已写入并回读验证，已更新逻辑处理器 %1 个。"))
        .arg(result.response.updatedProcessorCount));
    return true;
}

void HardwarePowerPage::reportPowerTransactionUnavailable(bool accepted)
{
    if (accepted) m_hasSnapshot = false;
    setStatus(accepted
        ? powerText(QStringLiteral("hardware.power.status.receipt_missing"),
            QStringLiteral("状态：电源操作已结束但未收到完整回执，请刷新当前状态，勿自动重发。"))
        : powerText(QStringLiteral("hardware.power.status.not_accepted"),
            QStringLiteral("状态：当前忙碌或正在退出，未接受新的电源操作。")), true);
}

void HardwarePowerPage::submitPowerTransaction(const HardwarePowerTransactionRequest& request)
{
    updateControlAvailability();
    if (m_queryOperation->isBusy() || m_confirming || m_restorePreparing) return;
    setStatus(powerText(QStringLiteral("hardware.power.status.transaction"),
        QStringLiteral("状态：正在应用电源操作并读取当前状态…")));
    const auto ticket = m_queryOperation->submitTransaction<HardwarePowerTransactionRequest,
        HardwarePowerTransactionResult>(request, runPowerTransaction,
        [this](const HardwarePowerTransactionResult& result, std::uint64_t ticket) {
            const QPointer<HardwarePowerPage> guard(this);
            if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this,
                QStringLiteral("hardware-power-transaction"), {m_snapshotTable},
                [guard, result, ticket]() {
                    if (guard == nullptr || !guard->m_queryOperation->isCurrent(ticket)) return;
                    guard->consumePowerTransaction(result);
                    if (guard != nullptr) guard->m_queryOperation->completeDeferred(ticket);
                })) return false;
            consumePowerTransaction(result);
            return true;
        }, [this](std::exception_ptr) {
            m_transactionPending = false;
            reportPowerTransactionUnavailable(true);
        });
    if (ticket == 0)
    {
        m_transactionPending = false;
        reportPowerTransactionUnavailable(false);
    }
    else m_transactionPending = true;
    updateControlAvailability();
}

void HardwarePowerPage::consumePowerTransaction(const HardwarePowerTransactionResult& result)
{
    m_transactionPending = false;
    if (result.schemesRead) applyPowerSchemesSnapshot(result.schemes);
    if (result.cpuRead) applySnapshotToUi(result.cpu);
    const bool restore = result.kind == HardwarePowerTransactionRequest::Kind::Restore;
    const QString errorTitle = powerText(QStringLiteral("hardware.power.error.title"), QStringLiteral("电源调节失败"));
    if (result.invalidGuid || result.schemeError != ERROR_SUCCESS)
    {
        const QString message = result.invalidGuid
            ? (restore
                ? powerText(QStringLiteral("hardware.power.error.restore_scheme_data"), QStringLiteral("首次捕获的 Windows 电源方案 GUID 数据无效。"))
                : powerText(QStringLiteral("hardware.power.error.scheme_data"), QStringLiteral("所选电源方案 GUID 数据无效。")))
            : (restore
                ? powerText(QStringLiteral("hardware.power.error.restore_scheme_apply"), QStringLiteral("还原首次 Windows 电源方案失败，Win32=%1。"))
                : powerText(QStringLiteral("hardware.power.error.scheme_apply"), QStringLiteral("应用 Windows 电源方案失败，Win32=%1。"))).arg(result.schemeError);
        setStatus(message, true);
        QMessageBox::critical(this, errorTitle, message);
        return;
    }
    if (result.preflightFailure != KSWORD_ARK_CPU_POWER_FAILURE_NONE)
    {
        const QString message = cpuPowerFailureReasonText(result.preflightFailure, result.request, result.cpu.response);
        setStatus(message, true);
        QMessageBox::critical(this, errorTitle, message);
        return;
    }
    if (result.controlAttempted && !presentControlResult(result.request, result.control)) return;
    if (result.kind == HardwarePowerTransactionRequest::Kind::Scheme)
    {
        setStatus(powerText(QStringLiteral("hardware.power.status.scheme_applied"), QStringLiteral("状态：Windows 电源方案已应用。")));
    }
    else if (restore)
    {
        if (result.schemeApplied && result.controlAttempted)
            setStatus(powerText(QStringLiteral("hardware.power.status.restored"), QStringLiteral("状态：已还原首次捕获的 Windows 电源方案和当前可写 CPU 设置。")));
        else if (result.schemeApplied)
            setStatus(powerText(QStringLiteral("hardware.power.status.restored_scheme_only"), QStringLiteral("状态：已还原首次捕获的 Windows 电源方案；当前没有可写的 CPU 还原字段。")));
        else if (result.controlAttempted)
            setStatus(powerText(QStringLiteral("hardware.power.status.restored_cpu_only"), QStringLiteral("状态：已还原首次捕获的当前可写 CPU 设置。")));
        else
        {
            const QString message = powerText(QStringLiteral("hardware.power.error.restore_unavailable"), QStringLiteral("首次状态已捕获，但当前没有可执行的还原目标。"));
            setStatus(message, true);
            QMessageBox::critical(this, errorTitle, message);
        }
    }
}

void HardwarePowerPage::sendControlRequest(
    const unsigned long applyFlags, const QString& confirmationText)
{
    updateControlAvailability();
    if (m_queryOperation->isBusy() || m_confirming || m_restorePreparing) return;
    const QString errorTitle = powerText(QStringLiteral("hardware.power.error.title"), QStringLiteral("电源调节失败"));
    if (!m_hasSnapshot)
    {
        QMessageBox::critical(this, errorTitle,
            powerText(QStringLiteral("hardware.power.error.no_snapshot"),
                QStringLiteral("没有可用于并发校验的 CPU 电源快照，请先刷新。")));
        return;
    }
    HardwarePowerTransactionRequest request;
    request.cpu = buildControlRequest(applyFlags);
    const QString validation = validateCpuPowerRequestForUi(request.cpu, m_snapshot);
    if (!validation.isEmpty())
    {
        setStatus(validation, true);
        QMessageBox::critical(this, errorTitle, validation);
        return;
    }
    m_confirming = true;
    updateControlAvailability();
    const QPointer<HardwarePowerPage> guard(this);
    const int answer = QMessageBox::warning(this,
        powerText(QStringLiteral("hardware.power.confirm.title"), QStringLiteral("确认 CPU 电源修改")),
        confirmationText, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (guard == nullptr) return;
    m_confirming = false;
    if (answer == QMessageBox::Yes) submitPowerTransaction(request);
    updateControlAvailability();
}

void HardwarePowerPage::raisePowerLimitsToPlatformMaximum()
{
    updateControlAvailability();
    if (m_queryOperation->isBusy() || m_confirming || m_restorePreparing) return;
    if (!m_hasSnapshot ||
        m_snapshot.packageMaximumPowerMilliwatts == 0UL ||
        (m_snapshot.responseFlags &
            KSWORD_ARK_CPU_POWER_RESPONSE_FLAG_PLATFORM_MAX_UNKNOWN) != 0UL)
    {
        QMessageBox::critical(
            this,
            powerText(
                QStringLiteral("hardware.power.error.title"),
                QStringLiteral("电源调节失败")),
            powerText(
                QStringLiteral("hardware.power.error.no_platform_max"),
                QStringLiteral("CPU 未报告可信的平台功耗上限，无法执行一键提升。仍可在允许范围内手动设置 PL1/PL2。")));
        return;
    }

    const double maximumWatts =
        m_snapshot.packageMaximumPowerMilliwatts / 1000.0;
    m_pl1Spin->setValue(maximumWatts);
    m_pl2Spin->setValue(maximumWatts);
    m_pl1EnableCheck->setChecked(true);
    m_pl2EnableCheck->setChecked(true);
    m_pl1ClampCheck->setChecked(false);
    m_pl2ClampCheck->setChecked(false);
    sendControlRequest(
        KSWORD_ARK_CPU_POWER_APPLY_POWER_LIMITS,
        powerText(
            QStringLiteral("hardware.power.confirm.raise"),
            QStringLiteral("即将把 PL1/PL2 提升到 CPU 报告的平台最大值 %1 W，并关闭 Clamp。此操作不清除 BIOS/MSR lock，也不保证主板、散热或固件允许持续运行。是否继续？"))
            .arg(maximumWatts, 0, 'f', 3));
}
