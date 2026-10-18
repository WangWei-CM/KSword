// KernelPatchHookAuditTab.cpp
// 作用：统一内核修改审计页实现——后台扫描所有 hook/patch 类型并汇总展示。

#include "KernelPatchHookAuditTab.h"
#include "KernelDock.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ThemeBinding.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
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

#include <algorithm>
#include <thread>

using ksword::kernel_dock_internal::kernelText;
namespace audit = ks::kernel::audit;

namespace
{
    // 结果表格列序号。
    enum AuditColumn : int
    {
        ColCategory = 0,    // 检测类别列。
        ColModule,          // 模块列。
        ColTarget,          // 目标标识列。
        ColAddress,         // 当前地址列。
        ColExpected,        // 期望地址列。
        ColRisk,            // 风险等级列。
        ColCurrentBytes,    // 当前字节列。
        ColBaselineBytes,   // 基线字节列。
        ColDetail,          // 详情列。
        ColCount
    };

    // readOnlyItem：创建不可编辑的表格单元格。
    // - 输入 text：单元格文本。
    // - 返回：带只读标志的 QTableWidgetItem。
    QTableWidgetItem* readOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }


}

// ----------------------------------------------------------------
// 构造和析构
// ----------------------------------------------------------------

KernelPatchHookAuditTab::KernelPatchHookAuditTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
}

KernelPatchHookAuditTab::~KernelPatchHookAuditTab()
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

void KernelPatchHookAuditTab::showEvent(QShowEvent* event)
{
    ks::ui::PrepareWidgetThemeBindingsForShow(this); // 首次绘制前消费本页主题绑定。
    QWidget::showEvent(event);
    if (!m_firstShown)
    {
        m_firstShown = true;
        m_statusLabel->setText(
            kernelText(
                "kernel.patch_hook_audit.status.idle",
                QStringLiteral("就绪：点击开始审计")));
    }
}

// ----------------------------------------------------------------
// initializeUi：构建工具栏、过滤器和结果表格。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::initializeUi()
{
    // 根布局——6px 统一边距和间距。
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    // --- 工具栏 ---
    auto* toolbar = new QHBoxLayout();

    // 开始审计按钮（播放图标）。
    m_scanButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/process_start.svg")),
        QString(),
        this);
    m_scanButton->setToolTip(
        kernelText(
            "kernel.patch_hook_audit.scan.tooltip",
            QStringLiteral("一键扫描 Inline Hook、IAT/EAT、SSDT、ObjectType 和驱动完整性")));
    KswordTheme::ApplyCompactIconButtonMetrics(m_scanButton);

    // 取消按钮（停止图标）。
    m_cancelButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/process_stop.svg")),
        QString(),
        this);
    m_cancelButton->setToolTip(
        kernelText(
            "kernel.patch_hook_audit.cancel.tooltip",
            QStringLiteral("取消正在进行的审计")));
    KswordTheme::ApplyCompactIconButtonMetrics(m_cancelButton);
    m_cancelButton->setEnabled(false);

    // 类别过滤下拉框。
    m_categoryFilterCombo = new QComboBox(this);
    ks::ui::StylePrimaryCombo(m_categoryFilterCombo);
    m_categoryFilterCombo->addItem(
        kernelText("kernel.patch_hook_audit.filter.all_categories", QStringLiteral("全部类别")), -1);
    for (const auto category : {audit::Category::Inline, audit::Category::Iat, audit::Category::Eat,
        audit::Category::Ssdt, audit::Category::ShadowSsdt, audit::Category::ObjectType, audit::Category::Integrity})
    {
        m_categoryFilterCombo->addItem(audit::CategoryText(category), static_cast<int>(category));
    }

    // 风险等级过滤下拉框。
    m_riskFilterCombo = new QComboBox(this);
    ks::ui::StylePrimaryCombo(m_riskFilterCombo);
    m_riskFilterCombo->addItem(
        kernelText("kernel.patch_hook_audit.filter.all_risks",
                   QStringLiteral("全部风险")));
    m_riskFilterCombo->addItem(QStringLiteral("Critical"));
    m_riskFilterCombo->addItem(QStringLiteral("Warning"));
    m_riskFilterCombo->addItem(QStringLiteral("Info"));

    // 文本过滤输入框。
    m_filterEdit = new QLineEdit(this);
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(
        kernelText(
            "kernel.patch_hook_audit.filter.placeholder",
            QStringLiteral("按模块名或函数名过滤")));

    // 汇总标签——显示各风险等级计数。
    m_summaryLabel = new QLabel(this);
    audit::BindAuditLabel(m_summaryLabel, false);

    // 状态标签——显示扫描进度或空闲提示。
    m_statusLabel = new QLabel(this);
    audit::BindAuditLabel(m_statusLabel, true);

    // 将控件加入工具栏并归一化行高。
    toolbar->addWidget(m_scanButton);
    toolbar->addWidget(m_cancelButton);
    toolbar->addWidget(m_categoryFilterCombo);
    toolbar->addWidget(m_riskFilterCombo);
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
    // 统一审计结果需要留存和导出。
    ks::ui::SetTableActionBarMode(
        m_resultTable, ks::ui::TableActionBarMode::Full);
    m_resultTable->setColumnCount(ColCount);
    m_resultTable->setHorizontalHeaderLabels({
        kernelText("kernel.patch_hook_audit.col.category",
                   QStringLiteral("类别")),
        kernelText("kernel.patch_hook_audit.col.module",
                   QStringLiteral("模块")),
        kernelText("kernel.patch_hook_audit.col.target",
                   QStringLiteral("目标")),
        kernelText("kernel.patch_hook_audit.col.address",
                   QStringLiteral("当前地址")),
        kernelText("kernel.patch_hook_audit.col.expected",
                   QStringLiteral("期望地址")),
        kernelText("kernel.patch_hook_audit.col.risk",
                   QStringLiteral("风险")),
        kernelText("kernel.patch_hook_audit.col.current_bytes",
                   QStringLiteral("当前字节")),
        kernelText("kernel.patch_hook_audit.col.baseline_bytes",
                   QStringLiteral("基线字节")),
        kernelText("kernel.patch_hook_audit.col.detail",
                   QStringLiteral("详情"))
    });

    // 表格基础样式：行选择、只读、交替行色、隐藏行号。
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    rootLayout->addWidget(m_resultTable, 1);
    // 概览和地址/字节证据为互补精简列组，保留原模型与快照宿主。
    audit::InstallColumnPresets(m_resultTable, toolbar,
        {ColCategory, ColModule, ColTarget, ColRisk, ColDetail},
        {ColTarget, ColAddress, ColExpected, ColCurrentBytes, ColBaselineBytes});

    // --- 信号连接 ---
    connect(m_scanButton, &QPushButton::clicked,
            this, [this]() { startFullAudit(); });
    connect(m_cancelButton, &QPushButton::clicked,
            this, [this]() { cancelAudit(); });
    // 过滤器变更时重新填表。
    connect(m_categoryFilterCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { populateTable(); });
    connect(m_riskFilterCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { populateTable(); });
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { populateTable(); });
}

// ----------------------------------------------------------------
// startFullAudit：在后台线程启动全类型扫描。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::startFullAudit()
{
    if (m_scanning)
    {
        return;
    }
    m_scanning = true;
    m_scanButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_allEntries.clear();
    m_resultTable->setRowCount(0);
    m_summaryLabel->clear();
    m_summaryLabel->setToolTip(QString());
    m_statusLabel->setToolTip(QString());

    // 创建取消标志——后台线程持有共享所有权。
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);

    m_statusLabel->setText(
        kernelText(
            "kernel.patch_hook_audit.status.scanning",
            QStringLiteral("正在扫描……")));

    // QPointer 防止 widget 被提前销毁。
    const QPointer<KernelPatchHookAuditTab> safeThis(this);
    auto cancelFlag = m_cancelFlag;

    std::thread([safeThis, cancelFlag]()
    {
        std::vector<AuditEntry> results;
        std::vector<audit::QueryIssue> issues; // 保留子查询失败和覆盖缺口。

        // 依次执行各子扫描——中途检查取消标志。
        if (!cancelFlag->load())
        {
            scanInlineHooks(results, cancelFlag, issues);
        }
        if (!cancelFlag->load())
        {
            scanIatEatHooks(results, cancelFlag, issues);
        }
        if (!cancelFlag->load())
        {
            scanSsdtEntries(results, cancelFlag, issues);
        }
        if (!cancelFlag->load())
        {
            scanObjectTypeProcedures(results, cancelFlag, issues);
        }
        if (!cancelFlag->load())
        {
            scanDriverIntegrity(results, cancelFlag, issues);
        }

        // 按风险等级排序：Critical 在前。
        std::stable_sort(results.begin(), results.end(),
            [](const AuditEntry& a, const AuditEntry& b)
            {
                return riskSortKey(a.riskLevel) < riskSortKey(b.riskLevel);
            });

        const bool wasCancelled = cancelFlag->load();

        // 回到 UI 线程提交结果。
        QMetaObject::invokeMethod(qApp,
            [safeThis, data = std::move(results), issues = std::move(issues), wasCancelled]()
            {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_allEntries = std::move(data);
                safeThis->m_scanning = false;
                safeThis->m_scanButton->setEnabled(true);
                safeThis->m_cancelButton->setEnabled(false);
                safeThis->populateTable();

                // 更新状态文本。
                if (wasCancelled)
                {
                    safeThis->m_statusLabel->setText(
                        kernelText(
                            "kernel.patch_hook_audit.status.cancelled",
                            QStringLiteral("已取消")));
                }
                else
                {
                    safeThis->m_statusLabel->setText(
                        kernelText(
                            "kernel.patch_hook_audit.status.done",
                            QStringLiteral("扫描完成")));
                }

                // 失败/部分结果仍保留已有证据，但不能显示成全部完成。
                if (!wasCancelled && !issues.empty())
                {
                    safeThis->m_statusLabel->setText(ks::i18n::sourceText(
                        QStringLiteral("未完成：%1 个查询失败或不完整")).arg(issues.size()));
                }
                safeThis->m_statusLabel->setToolTip(audit::IssueText(issues));
                safeThis->m_summaryLabel->setToolTip(issues.empty() ? QString()
                    : ks::i18n::sourceText(QStringLiteral("当前风险统计仅包含已获取的结果")));
                // 汇总各风险等级计数。
                int critical = 0;
                int warning = 0;
                int info = 0;
                for (const auto& e : safeThis->m_allEntries)
                {
                    if (e.riskLevel == QStringLiteral("Critical"))
                    {
                        ++critical;
                    }
                    else if (e.riskLevel == QStringLiteral("Warning"))
                    {
                        ++warning;
                    }
                    else
                    {
                        ++info;
                    }
                }
                safeThis->m_summaryLabel->setText(
                    kernelText(
                        "kernel.patch_hook_audit.summary",
                        QStringLiteral("Critical: %1  Warning: %2  Info: %3"))
                        .arg(critical).arg(warning).arg(info));
            });
    }).detach();
}

// ----------------------------------------------------------------
// cancelAudit：请求取消后台扫描。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::cancelAudit()
{
    if (m_cancelFlag != nullptr)
    {
        m_cancelFlag->store(true);
    }
}

// ----------------------------------------------------------------
// populateTable：按当前过滤条件将 m_allEntries 填入表格。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::populateTable()
{
    m_resultTable->setRowCount(0);

    // 读取过滤条件。
    const int categoryFilter = m_categoryFilterCombo->currentData().toInt(); // 稳定业务值。
    const int riskIndex = m_riskFilterCombo->currentIndex();
    const QString riskFilter =
        (riskIndex > 0) ? m_riskFilterCombo->currentText() : QString();
    const QString textFilter = m_filterEdit->text().trimmed();

    // 过滤并填表。
    int row = 0;
    for (const auto& entry : m_allEntries)
    {
        // 类别过滤。
        if (categoryFilter >= 0 && static_cast<int>(entry.category) != categoryFilter)
        {
            continue;
        }
        // 风险过滤。
        if (!riskFilter.isEmpty() && entry.riskLevel != riskFilter)
        {
            continue;
        }
        // 文本过滤——匹配模块名或目标名。
        if (!textFilter.isEmpty()
            && !entry.moduleName.contains(textFilter, Qt::CaseInsensitive)
            && !entry.targetName.contains(textFilter, Qt::CaseInsensitive))
        {
            continue;
        }

        // 通过过滤，插入行。
        m_resultTable->insertRow(row);
        m_resultTable->setItem(row, ColCategory,
                               readOnlyItem(audit::CategoryText(entry.category)));
        m_resultTable->setItem(row, ColModule,
                               readOnlyItem(entry.moduleName));
        m_resultTable->setItem(row, ColTarget,
                               readOnlyItem(entry.targetName));
        m_resultTable->setItem(row, ColAddress,
                               readOnlyItem(entry.address != 0
                                   ? hex64(entry.address)
                                   : QString()));
        m_resultTable->setItem(row, ColExpected,
                               readOnlyItem(entry.expected != 0
                                   ? hex64(entry.expected)
                                   : QString()));
        m_resultTable->setItem(row, ColRisk,
                               readOnlyItem(entry.riskLevel));
        m_resultTable->setItem(row, ColCurrentBytes,
                               readOnlyItem(entry.currentBytes));
        m_resultTable->setItem(row, ColBaselineBytes,
                               readOnlyItem(entry.baselineBytes));
        m_resultTable->setItem(row, ColDetail,
                               readOnlyItem(ks::i18n::sourceText(entry.detail)));
        ++row;
    }
}

// ================================================================
// 子扫描函数——均为 static，不访问成员，可在后台线程安全调用。
// ================================================================

// ----------------------------------------------------------------
// scanInlineHooks：扫描内核 Inline Hook。
// 将 status 非 CLEAN 的条目视为可疑并追加到 out。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::scanInlineHooks(
    std::vector<AuditEntry>& out,
    const std::shared_ptr<std::atomic_bool>& cancel,
    std::vector<audit::QueryIssue>& issues)
{
    if (cancel->load())
    {
        return;
    }

    // 栈构造 DriverClient——无单例。
    const ksword::ark::DriverClient client;
    auto result = client.scanInlineHooks();
    if (!audit::CheckQuery(issues, QStringLiteral("Inline Hook"), result.io.ok,
        result.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN && result.lastStatus >= 0
            && result.entries.size() == result.returnedCount
            && result.returnedCount >= result.totalCount, result.lastStatus, result.io.win32Error))
    {
        return;
    }

    if (std::any_of(result.entries.begin(), result.entries.end(), [](const auto& entry)
        { return entry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED; }))
    {
        issues.push_back({QStringLiteral("Inline Hook"), QStringLiteral("查询结果不完整"), result.lastStatus, 0});
    }
    for (const auto& e : result.entries)
    {
        if (cancel->load())
        {
            return;
        }
        // CLEAN 不报告；读取失败保留覆盖缺口和参考行。
        if (e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN)
        {
            continue;
        }

        AuditEntry entry;
        entry.category = audit::Category::Inline;
        entry.moduleName = QString::fromStdWString(e.moduleName);
        entry.targetName = QString::fromStdString(e.functionName);
        entry.address = e.targetAddress;
        entry.expected = e.functionAddress;
        entry.currentBytes = bytesToHex(e.currentBytes);
        entry.baselineBytes = bytesToHex(e.expectedBytes);

        // 风险分级：SUSPICIOUS 和 PATCHED 视为 Critical，其余 Warning。
        if (e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS
            || e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED)
        {
            entry.riskLevel = QStringLiteral("Critical");
        }
        else
        {
            entry.riskLevel = QStringLiteral("Warning");
        }

        // 目标模块名作为详情补充。
        entry.detail = QString::fromStdWString(e.targetModuleName);
        out.push_back(std::move(entry));
    }
}

// ----------------------------------------------------------------
// scanIatEatHooks：扫描内核 IAT/EAT 重定向。
// status 非 CLEAN 的条目视为可疑。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::scanIatEatHooks(
    std::vector<AuditEntry>& out,
    const std::shared_ptr<std::atomic_bool>& cancel,
    std::vector<audit::QueryIssue>& issues)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;
    auto result = client.enumerateIatEatHooks();
    if (!audit::CheckQuery(issues, QStringLiteral("IAT/EAT"), result.io.ok,
        result.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN && result.lastStatus >= 0
            && result.entries.size() == result.returnedCount
            && result.returnedCount >= result.totalCount, result.lastStatus, result.io.win32Error))
    {
        return;
    }

    if (std::any_of(result.entries.begin(), result.entries.end(), [](const auto& entry)
        { return entry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED; }))
    {
        issues.push_back({QStringLiteral("IAT/EAT"), QStringLiteral("查询结果不完整"), result.lastStatus, 0});
    }
    for (const auto& e : result.entries)
    {
        if (cancel->load())
        {
            return;
        }
        if (e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN)
        {
            continue;
        }

        AuditEntry entry;
        // IAT 和 EAT 分别标注类别。
        entry.category =
            (e.hookClass == KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT)
                ? audit::Category::Eat
                : audit::Category::Iat;
        entry.moduleName = QString::fromStdWString(e.moduleName);
        entry.targetName = QString::fromStdString(e.functionName);
        entry.address = e.currentTarget;
        entry.expected = e.expectedTarget;

        // 风险分级——与 Inline Hook 一致。
        if (e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS
            || e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED)
        {
            entry.riskLevel = QStringLiteral("Critical");
        }
        else
        {
            entry.riskLevel = QStringLiteral("Warning");
        }

        // 导入来源模块 + 目标模块。
        entry.detail = QString::fromStdWString(e.importModuleName)
            + QStringLiteral(" -> ")
            + QString::fromStdWString(e.targetModuleName);
        out.push_back(std::move(entry));
    }
}

// ----------------------------------------------------------------
// scanSsdtEntries：扫描 SSDT 和 ShadowSSDT。
// 服务例程不属于期望内核模块的条目视为可疑。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::scanSsdtEntries(
    std::vector<AuditEntry>& out,
    const std::shared_ptr<std::atomic_bool>& cancel,
    std::vector<audit::QueryIssue>& issues)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;
    for (const bool shadow : {false, true})
    {
        if (cancel->load())
        {
            return;
        }
        const QString stage = shadow ? QStringLiteral("ShadowSSDT") : QStringLiteral("SSDT");
        const auto before = audit::QueryModules(); // 查询前冻结加载范围。
        const auto result = shadow ? client.enumerateShadowSsdt() : client.enumerateSsdt(0UL);
        const auto after = audit::QueryModules(); // 查询后排除卸载/重载后的旧范围。
        if (!audit::CheckQuery(issues, stage, result.io.ok,
            result.serviceTableBase != 0 && result.serviceCountFromTable != 0
                && result.entries.size() == result.returnedCount
                && result.returnedCount >= result.totalCount, result.io.ntStatus, result.io.win32Error))
        {
            continue;
        }
        bool unknown = false; // 无法定位的服务必须保留未知状态。
        for (const auto& source : result.entries)
        {
            if (cancel->load())
            {
                return;
            }
            const auto owner = audit::ResolveServiceOwner(source.serviceRoutineAddress,
                (source.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_TABLE_ADDRESS_VALID) != 0,
                shadow, before, after);
            if (owner.ownership == audit::Ownership::Expected)
            {
                continue;
            }
            AuditEntry entry; // 导出源模块名不参与目标地址归属。
            entry.category = shadow ? audit::Category::ShadowSsdt : audit::Category::Ssdt;
            entry.moduleName = QString::fromStdString(owner.moduleName);
            entry.targetName = QString::fromStdString(source.serviceName);
            entry.address = source.serviceRoutineAddress;
            entry.riskLevel = owner.ownership == audit::Ownership::Unknown
                ? QStringLiteral("Info") : QStringLiteral("Critical");
            entry.detail = owner.ownership == audit::Ownership::Unknown
                ? QStringLiteral("未验证") : QStringLiteral("#%1").arg(source.serviceIndex);
            unknown = unknown || owner.ownership == audit::Ownership::Unknown;
            out.push_back(std::move(entry));
        }
        if (unknown || !before.available || !after.available)
        {
            issues.push_back({stage, QStringLiteral("模块范围不可用或已变化"), after.status, 0});
        }
    }
}

// ----------------------------------------------------------------
// scanObjectTypeProcedures：扫描 ObjectType 方法指针完整性。
// riskFlags 非零的条目视为可疑。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::scanObjectTypeProcedures(
    std::vector<AuditEntry>& out,
    const std::shared_ptr<std::atomic_bool>& cancel,
    std::vector<audit::QueryIssue>& issues)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;
    auto result = client.enumObjectTypeProcedures();
    if (!audit::CheckQuery(issues, QStringLiteral("ObjectType"), result.io.ok && !result.unsupported,
        !result.truncated && !result.skippedTypes && result.lastStatus >= 0
            && result.entries.size() == result.returnedCount
            && result.status == KSWORD_ARK_OBJECT_TYPE_TABLE_STATUS_OK
            && result.layoutState == KSWORD_ARK_OBJTYPE_LAYOUT_VALIDATED,
        result.lastStatus, result.io.win32Error))
    {
        return;
    }
    if (result.layoutState != KSWORD_ARK_OBJTYPE_LAYOUT_VALIDATED)
    {
        return; // 未验证的结构不得据此给出确定的劫持结论。
    }


    // 响应级状态不影响逐条判断——只要有 entries 就处理。
    for (const auto& e : result.entries)
    {
        if (cancel->load())
        {
            return;
        }
        // 只报告有风险标志的条目。
        if (e.riskFlags == KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE)
        {
            continue;
        }

        AuditEntry entry;
        entry.category = audit::Category::ObjectType;
        entry.moduleName = QString::fromStdWString(e.ownerModule);
        entry.targetName = QString::fromStdWString(e.typeName);
        entry.address = e.targetAddress;

        // 有 detour 跳板的视为 Critical，其余 Warning。
        if (e.detourTargetAddress != 0
            || (e.riskFlags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_HIDDEN_HOOK) != 0)
        {
            entry.riskLevel = QStringLiteral("Critical");
        }
        else
        {
            entry.riskLevel = QStringLiteral("Warning");
        }

        // 节名和 detour 落点作为详情。
        entry.detail = QString::fromStdWString(e.sectionName);
        if (e.detourTargetAddress != 0)
        {
            entry.detail += QStringLiteral(" detour=")
                + hex64(e.detourTargetAddress);
        }
        out.push_back(std::move(entry));
    }
}

// ----------------------------------------------------------------
// scanDriverIntegrity：查询驱动 / IDT 完整性证据。
// riskFlags 非零的证据行追加。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::scanDriverIntegrity(
    std::vector<AuditEntry>& out,
    const std::shared_ptr<std::atomic_bool>& cancel,
    std::vector<audit::QueryIssue>& issues)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;

    // queryDriverIntegrity 聚合 DriverObject/LDR/FastIo/CPU/IDT 证据。
    auto result = client.queryDriverIntegrity();

    if (!audit::CheckQuery(issues, QStringLiteral("Driver integrity"), result.io.ok && !result.unsupported,
        result.queryStatus == KSWORD_ARK_DRIVER_INTEGRITY_STATUS_OK && result.statusFlags == 0
            && result.entries.size() == result.returnedCount
            && result.returnedCount >= result.totalCount && result.lastStatus >= 0,
        result.lastStatus, result.io.win32Error))
    {
        return;
    }

    for (const auto& e : result.entries)
    {
        if (cancel->load())
        {
            return;
        }
        // 只报告有风险标志的行。
        if (e.riskFlags == KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE)
        {
            continue;
        }

        AuditEntry entry;
        entry.category = audit::Category::Integrity;
        entry.moduleName = QString::fromStdWString(e.ownerModule);
        entry.address = e.targetAddress;

        // 高风险标志 Critical，其余 Warning。
        const bool highRisk =
            (e.riskFlags & (KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE
                | KSWORD_ARK_DRIVER_INTEGRITY_RISK_HIDDEN_HOOK
                | KSWORD_ARK_DRIVER_INTEGRITY_RISK_PROC_DETOUR
                | KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED)) != 0;
        entry.riskLevel = highRisk
            ? QStringLiteral("Critical")
            : QStringLiteral("Warning");

        entry.detail = QString::fromStdWString(e.detail);
        out.push_back(std::move(entry));
    }
}

// ================================================================
// 工具函数
// ================================================================

// hex64：格式化 64 位值为 "0x" 前缀十六进制。
QString KernelPatchHookAuditTab::hex64(const std::uint64_t value)
{
    return QStringLiteral("0x%1")
        .arg(value, 16, 16, QLatin1Char('0'));
}

// bytesToHex：字节向量转为空格分隔的十六进制（最多显示 16 字节）。
QString KernelPatchHookAuditTab::bytesToHex(
    const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return {};
    }
    QString result;
    const int limit = std::min(static_cast<int>(bytes.size()), 16);
    for (int i = 0; i < limit; ++i)
    {
        if (i > 0)
        {
            result += QLatin1Char(' ');
        }
        result += QStringLiteral("%1")
            .arg(bytes[i], 2, 16, QLatin1Char('0'));
    }
    // 超过 16 字节时追加省略号。
    if (static_cast<int>(bytes.size()) > limit)
    {
        result += QStringLiteral("...");
    }
    return result;
}

// riskSortKey：Critical=0, Warning=1, Info=2。
int KernelPatchHookAuditTab::riskSortKey(const QString& risk)
{
    if (risk == QStringLiteral("Critical"))
    {
        return 0;
    }
    if (risk == QStringLiteral("Warning"))
    {
        return 1;
    }
    return 2;
}
