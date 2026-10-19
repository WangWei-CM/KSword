// KernelShadowComparisonTab.cpp
// 作用：内核 Shadow 比较页实现——逐条对比 SSDT / ShadowSSDT
// 当前值与基线（期望宿主模块），高亮被修改或重定向的服务项。

#include "KernelShadowComparisonTab.h"
#include "KernelDock.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ThemeBinding.h"
#include "../UI/TablePresentation.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <thread>
#include <algorithm>

using ksword::kernel_dock_internal::kernelText;
namespace audit = ks::kernel::audit;

namespace
{
    // 比较结果表格列序号。
    enum CompareColumn : int
    {
        ColIndex = 0,       // 服务号列。
        ColServiceName,     // 函数名列。
        ColAddress,         // 当前地址列。
        ColTableValue,      // 服务表原始值列。
        ColModule,          // 所属模块列。
        ColStatus,          // 比对状态列（正常/可疑）。
        ColCount
    };

    // readOnlyItem：创建不可编辑的表格单元格。
    QTableWidgetItem* readOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    // 比较对象下拉值定义。
    enum CompareTarget : int
    {
        TargetSsdt = 0,        // 系统服务描述符表。
        TargetShadowSsdt = 1   // 影子系统服务描述符表。
    };


}

// ----------------------------------------------------------------
// 构造和析构
// ----------------------------------------------------------------

KernelShadowComparisonTab::KernelShadowComparisonTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
}

KernelShadowComparisonTab::~KernelShadowComparisonTab()
{
    // 通知后台线程尽快退出。
    if (m_cancelFlag != nullptr)
    {
        m_cancelFlag->store(true);
    }
}

// ----------------------------------------------------------------
// showEvent：首次显示时设置初始提示。
// ----------------------------------------------------------------

void KernelShadowComparisonTab::showEvent(QShowEvent* event)
{
    ks::ui::PrepareWidgetThemeBindingsForShow(this); // 首帧不沿用尚未消费的旧主题。
    QWidget::showEvent(event);
    if (!m_firstShown)
    {
        m_firstShown = true;
        m_statusLabel->setText(
            kernelText(
                "kernel.shadow_comparison.status.idle",
                QStringLiteral("选择比较对象后点击开始")));
    }
}

// ----------------------------------------------------------------
// initializeUi：构建工具栏和比较结果表格。
// ----------------------------------------------------------------

void KernelShadowComparisonTab::initializeUi()
{
    // 根布局——6px 统一边距和间距。
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    // --- 工具栏 ---
    auto* toolbar = new QHBoxLayout();

    // 比较对象选择。
    m_targetCombo = new QComboBox(this);
    ks::ui::StylePrimaryCombo(m_targetCombo);
    m_targetCombo->addItem(QStringLiteral("SSDT"));
    m_targetCombo->addItem(QStringLiteral("ShadowSSDT"));

    // 开始比较按钮（播放图标）。
    m_scanButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/process_start.svg")),
        QString(),
        this);
    m_scanButton->setToolTip(
        kernelText(
            "kernel.shadow_comparison.scan.tooltip",
            QStringLiteral("扫描所选服务表并逐条比对所属模块")));
    KswordTheme::ApplyCompactIconButtonMetrics(m_scanButton);

    // 取消按钮（停止图标）。
    m_cancelButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/process_stop.svg")),
        QString(),
        this);
    m_cancelButton->setToolTip(
        kernelText(
            "kernel.shadow_comparison.cancel.tooltip",
            QStringLiteral("取消比较")));
    KswordTheme::ApplyCompactIconButtonMetrics(m_cancelButton);
    m_cancelButton->setEnabled(false);

    // 只看可疑条目复选框。
    m_suspiciousOnlyCheck = new QCheckBox(
        ks::i18n::sourceText(QStringLiteral("只看可疑或未验证")),
        this);
    m_suspiciousOnlyCheck->setToolTip(
        ks::i18n::sourceText(QStringLiteral("勾选后隐藏已验证正常的服务，保留可疑和未验证条目")));

    // 文本过滤输入框。
    m_filterEdit = new QLineEdit(this);
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(
        kernelText(
            "kernel.shadow_comparison.filter.placeholder",
            QStringLiteral("按函数名或模块名过滤")));

    // 汇总标签。
    m_summaryLabel = new QLabel(this);
    audit::BindAuditLabel(m_summaryLabel, false);

    // 状态标签。
    m_statusLabel = new QLabel(this);
    audit::BindAuditLabel(m_statusLabel, true);

    // 将控件加入工具栏并归一化行高。
    toolbar->addWidget(m_targetCombo);
    toolbar->addWidget(m_scanButton);
    toolbar->addWidget(m_cancelButton);
    toolbar->addWidget(m_suspiciousOnlyCheck);
    toolbar->addWidget(m_filterEdit, 1);

    ks::ui::NormalizeToolbarRow(toolbar);
    rootLayout->addLayout(toolbar);
    // 状态在独立自然高度行换行，长译文不挤压工具控件或撑宽窄窗口。
    auto* statusRow = new QHBoxLayout();
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusRow->addWidget(m_summaryLabel, 1);
    statusRow->addWidget(m_statusLabel, 1);
    rootLayout->addLayout(statusRow);

    // --- 结果表格 ---
    m_resultTable = new ks::ui::VisibleTableWidget(this);
    // 完整操作条用于比较结果的复制和导出。
    ks::ui::SetTableActionBarMode(
        m_resultTable, ks::ui::TableActionBarMode::Full);
    m_resultTable->setColumnCount(ColCount);
    m_resultTable->setHorizontalHeaderLabels({
        kernelText("kernel.shadow_comparison.col.index",
                   QStringLiteral("序号")),
        kernelText("kernel.shadow_comparison.col.service_name",
                   QStringLiteral("函数名")),
        kernelText("kernel.shadow_comparison.col.address",
                   QStringLiteral("当前地址")),
        kernelText("kernel.shadow_comparison.col.table_value",
                   QStringLiteral("表值")),
        kernelText("kernel.shadow_comparison.col.module",
                   QStringLiteral("所属模块")),
        kernelText("kernel.shadow_comparison.col.status",
                   QStringLiteral("状态"))
    });

    // 表格基础样式。
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->horizontalHeader()->setResizeContentsPrecision(100);
    m_resultTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    rootLayout->addWidget(m_resultTable, 1);
    audit::InstallColumnPresets(m_resultTable, toolbar,
        {ColIndex, ColServiceName, ColModule, ColStatus},
        {ColServiceName, ColAddress, ColTableValue, ColStatus});
    // 换肤仅更新现有状态文字颜色，不触发服务枚举或模型重建。
    const QPointer<QTableWidget> themedTable(m_resultTable);
    ks::ui::BindWidgetTheme(m_resultTable, [themedTable]()
    {
        if (!themedTable)
        {
            return;
        }
        ks::ui::ApplyTablePresentation(themedTable);
        for (int row = 0; row < themedTable->rowCount(); ++row)
        {
            auto* item = themedTable->item(row, ColStatus);
            if (item != nullptr)
            {
                const auto ownership = static_cast<audit::Ownership>(item->data(Qt::UserRole).toInt());
                item->setForeground(ownership == audit::Ownership::Unexpected
                    ? QBrush(KswordTheme::ErrorColor()) : QBrush(KswordTheme::TextPrimaryColor()));
            }
        }
    });

    // --- 信号连接 ---
    connect(m_scanButton, &QPushButton::clicked,
            this, [this]() { startComparison(); });
    connect(m_cancelButton, &QPushButton::clicked,
            this, [this]() { cancelComparison(); });
    // 过滤器变更时重新填表。
    connect(m_suspiciousOnlyCheck, &QCheckBox::toggled,
            this, [this](bool) { populateTable(); });
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { populateTable(); });
}

// ----------------------------------------------------------------
// startComparison：按选定目标在后台线程扫描服务表。
// ----------------------------------------------------------------

void KernelShadowComparisonTab::startComparison()
{
    if (m_scanning)
    {
        return;
    }
    m_scanning = true;
    m_targetCombo->setEnabled(false);
    m_scanButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_entries.clear();
    m_resultTable->setRowCount(0);
    m_summaryLabel->clear();
    m_summaryLabel->setToolTip(QString());
    m_statusLabel->setToolTip(QString());

    m_cancelFlag = std::make_shared<std::atomic_bool>(false);

    m_statusLabel->setText(
        kernelText(
            "kernel.shadow_comparison.status.scanning",
            QStringLiteral("正在扫描……")));

    // 记录用户选择的比较对象。
    const int target = m_targetCombo->currentIndex();
    const QPointer<KernelShadowComparisonTab> safeThis(this);
    auto cancelFlag = m_cancelFlag;

    std::thread([safeThis, cancelFlag, target]()
    {
        std::vector<CompareEntry> results;
        std::vector<audit::QueryIssue> issues; // 保留传输失败和未知覆盖原因。

        // 栈构造 DriverClient。
        const ksword::ark::DriverClient client;

        // 根据目标选择调用 SSDT 或 ShadowSSDT。
        const bool isShadow = (target == TargetShadowSsdt);
        const QString stage = isShadow ? QStringLiteral("ShadowSSDT") : QStringLiteral("SSDT");
        const auto before = audit::QueryModules(); // 不信任服务行的导出源模块名。

        // 发起 IOCTL。
        auto result = isShadow
            ? client.enumerateShadowSsdt()
            : client.enumerateSsdt(0UL);

        const auto after = audit::QueryModules(); // 同一模块前后身份一致才允许判断正常。
        if (audit::CheckQuery(issues, stage, result.io.ok,
            result.serviceTableBase != 0 && result.serviceCountFromTable != 0
                && result.entries.size() == result.returnedCount
                && result.returnedCount >= result.totalCount, result.io.ntStatus, result.io.win32Error))
        {
            // 遍历所有服务表条目。
            for (const auto& e : result.entries)
            {
                if (cancelFlag->load())
                {
                    break;
                }

                CompareEntry entry;
                entry.serviceIndex = e.serviceIndex;
                entry.serviceName = QString::fromStdString(e.serviceName);
                entry.serviceRoutineAddress = e.serviceRoutineAddress;
                entry.currentTableValue = e.currentTableValue;
                const auto owner = audit::ResolveServiceOwner(e.serviceRoutineAddress,
                    (e.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_TABLE_ADDRESS_VALID) != 0,
                    isShadow, before, after);
                entry.moduleName = QString::fromStdString(owner.moduleName);
                entry.ownership = owner.ownership;
                results.push_back(std::move(entry));
            }
        }

        if (!before.available || !after.available
            || std::any_of(results.begin(), results.end(), [](const CompareEntry& entry)
                { return entry.ownership == audit::Ownership::Unknown; }))
        {
            issues.push_back({stage, QStringLiteral("模块范围不可用或已变化"), after.status, 0});
        }
        const bool wasCancelled = cancelFlag->load();

        // 回到 UI 线程提交结果。
        QMetaObject::invokeMethod(qApp,
            [safeThis, data = std::move(results), issues = std::move(issues), wasCancelled]()
            {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_entries = std::move(data);
                safeThis->m_scanning = false;
                safeThis->m_targetCombo->setEnabled(true);
                safeThis->m_scanButton->setEnabled(true);
                safeThis->m_cancelButton->setEnabled(false);
                safeThis->populateTable();

                // 更新状态。
                if (wasCancelled)
                {
                    safeThis->m_statusLabel->setText(
                        kernelText(
                            "kernel.shadow_comparison.status.cancelled",
                            QStringLiteral("已取消")));
                }
                else
                {
                    safeThis->m_statusLabel->setText(
                        kernelText(
                            "kernel.shadow_comparison.status.done",
                            QStringLiteral("比较完成")));
                }

                if (!wasCancelled && !issues.empty())
                {
                    safeThis->m_statusLabel->setText(ks::i18n::sourceText(
                        QStringLiteral("未完成：%1 个查询失败或不完整")).arg(issues.size()));
                }
                safeThis->m_statusLabel->setToolTip(audit::IssueText(issues));
                // 未验证单独计数，零可疑不代表全部正常。
                int suspicious = 0;
                int unknown = 0;
                for (const auto& e : safeThis->m_entries)
                {
                    if (e.ownership == audit::Ownership::Unexpected)
                    {
                        ++suspicious;
                    }
                    if (e.ownership == audit::Ownership::Unknown)
                    {
                        ++unknown;
                    }
                }
                const int total =
                    static_cast<int>(safeThis->m_entries.size());
                safeThis->m_summaryLabel->setText(
                    ks::i18n::sourceText(QStringLiteral("共 %1 项，可疑 %2 项，未验证 %3 项"))
                        .arg(total).arg(suspicious).arg(unknown));
                safeThis->m_summaryLabel->setToolTip(issues.empty() ? QString()
                    : ks::i18n::sourceText(QStringLiteral("当前风险统计仅包含已获取的结果")));
            });
    }).detach();
}

// ----------------------------------------------------------------
// cancelComparison：请求取消后台扫描。
// ----------------------------------------------------------------

void KernelShadowComparisonTab::cancelComparison()
{
    if (m_cancelFlag != nullptr)
    {
        m_cancelFlag->store(true);
    }
}

// ----------------------------------------------------------------
// populateTable：按过滤条件填充比较结果表。
// ----------------------------------------------------------------

void KernelShadowComparisonTab::populateTable()
{
    m_resultTable->setRowCount(0);

    const bool suspiciousOnly = m_suspiciousOnlyCheck->isChecked();
    const QString textFilter = m_filterEdit->text().trimmed();

    int row = 0;
    for (const auto& entry : m_entries)
    {
        // 只看可疑——跳过正常条目。
        if (suspiciousOnly && entry.ownership == audit::Ownership::Expected)
        {
            continue;
        }
        // 文本过滤——匹配函数名或模块名。
        if (!textFilter.isEmpty()
            && !entry.serviceName.contains(textFilter, Qt::CaseInsensitive)
            && !entry.moduleName.contains(textFilter, Qt::CaseInsensitive))
        {
            continue;
        }

        // 插入行。
        m_resultTable->insertRow(row);
        m_resultTable->setItem(row, ColIndex,
            readOnlyItem(QString::number(entry.serviceIndex)));
        m_resultTable->setItem(row, ColServiceName,
            readOnlyItem(entry.serviceName));
        m_resultTable->setItem(row, ColAddress,
            readOnlyItem(hex64(entry.serviceRoutineAddress)));
        m_resultTable->setItem(row, ColTableValue,
            readOnlyItem(hex64(entry.currentTableValue)));
        m_resultTable->setItem(row, ColModule,
            readOnlyItem(entry.moduleName));

        // 状态列——可疑条目标红。
        const QString statusText = entry.ownership == audit::Ownership::Unknown
            ? ks::i18n::sourceText(QStringLiteral("未验证"))
            : entry.ownership == audit::Ownership::Unexpected
                ? kernelText("kernel.shadow_comparison.status.suspicious", QStringLiteral("可疑"))
                : kernelText("kernel.shadow_comparison.status.normal", QStringLiteral("正常"));
        auto* statusItem = readOnlyItem(statusText);
        statusItem->setData(Qt::UserRole, static_cast<int>(entry.ownership)); // 换肤使用稳定状态。
        if (entry.ownership == audit::Ownership::Unexpected)
        {
            statusItem->setForeground(KswordTheme::ErrorColor());
        }
        m_resultTable->setItem(row, ColStatus, statusItem);
        ++row;
    }
}

// ----------------------------------------------------------------
// hex64：格式化 64 位值为 "0x" 前缀十六进制。
// ----------------------------------------------------------------

QString KernelShadowComparisonTab::hex64(const std::uint64_t value)
{
    return QStringLiteral("0x%1")
        .arg(value, 16, 16, QLatin1Char('0'));
}
