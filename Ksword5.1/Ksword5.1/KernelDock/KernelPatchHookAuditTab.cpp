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

    // SSDT 正常模块名前缀列表，不在此列的视为可疑。
    const QStringList kSsdtExpectedModules = {
        QStringLiteral("ntoskrnl"),
        QStringLiteral("ntkrnlmp"),
        QStringLiteral("ntkrnlpa"),
        QStringLiteral("ntkrpamp")
    };

    // ShadowSSDT 正常模块名前缀列表。
    const QStringList kShadowSsdtExpectedModules = {
        QStringLiteral("win32k"),
        QStringLiteral("win32kfull"),
        QStringLiteral("win32kbase"),
        QStringLiteral("cdd")
    };

    // isExpectedSsdtModule：判断模块名是否属于 SSDT 或 ShadowSSDT 的合法宿主。
    // - 输入 name：模块名字符串；expected：期望前缀列表。
    // - 返回：true 表示该模块在期望范围内。
    bool isExpectedModule(const QString& name, const QStringList& expected)
    {
        if (name.isEmpty())
        {
            return true;
        }
        const QString lower = name.toLower();
        for (const auto& prefix : expected)
        {
            if (lower.startsWith(prefix))
            {
                return true;
            }
        }
        return false;
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
        kernelText("kernel.patch_hook_audit.filter.all_categories",
                   QStringLiteral("全部类别")));
    m_categoryFilterCombo->addItem(QStringLiteral("Inline Hook"));
    m_categoryFilterCombo->addItem(QStringLiteral("IAT"));
    m_categoryFilterCombo->addItem(QStringLiteral("EAT"));
    m_categoryFilterCombo->addItem(QStringLiteral("SSDT"));
    m_categoryFilterCombo->addItem(QStringLiteral("ShadowSSDT"));
    m_categoryFilterCombo->addItem(QStringLiteral("ObjectType"));
    m_categoryFilterCombo->addItem(
        kernelText("kernel.patch_hook_audit.filter.integrity",
                   QStringLiteral("驱动完整性")));

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
    m_summaryLabel->setStyleSheet(
        QStringLiteral("font-weight:600;color:%1;")
            .arg(KswordTheme::TextPrimaryHex()));

    // 状态标签——显示扫描进度或空闲提示。
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
            .arg(KswordTheme::TextSecondaryHex()));

    // 将控件加入工具栏并归一化行高。
    toolbar->addWidget(m_scanButton);
    toolbar->addWidget(m_cancelButton);
    toolbar->addWidget(m_categoryFilterCombo);
    toolbar->addWidget(m_riskFilterCombo);
    toolbar->addWidget(m_filterEdit, 1);
    toolbar->addWidget(m_summaryLabel);
    toolbar->addWidget(m_statusLabel);
    ks::ui::NormalizeToolbarRow(toolbar);
    rootLayout->addLayout(toolbar);

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

        // 依次执行各子扫描——中途检查取消标志。
        if (!cancelFlag->load())
        {
            scanInlineHooks(results, cancelFlag);
        }
        if (!cancelFlag->load())
        {
            scanIatEatHooks(results, cancelFlag);
        }
        if (!cancelFlag->load())
        {
            scanSsdtEntries(results, cancelFlag);
        }
        if (!cancelFlag->load())
        {
            scanObjectTypeProcedures(results, cancelFlag);
        }
        if (!cancelFlag->load())
        {
            scanDriverIntegrity(results, cancelFlag);
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
            [safeThis, data = std::move(results), wasCancelled]()
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
    const int categoryIndex = m_categoryFilterCombo->currentIndex();
    const QString categoryFilter =
        (categoryIndex > 0) ? m_categoryFilterCombo->currentText() : QString();
    const int riskIndex = m_riskFilterCombo->currentIndex();
    const QString riskFilter =
        (riskIndex > 0) ? m_riskFilterCombo->currentText() : QString();
    const QString textFilter = m_filterEdit->text().trimmed();

    // 过滤并填表。
    int row = 0;
    for (const auto& entry : m_allEntries)
    {
        // 类别过滤。
        if (!categoryFilter.isEmpty() && entry.category != categoryFilter)
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
                               readOnlyItem(entry.category));
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
                               readOnlyItem(entry.detail));
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
    const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (cancel->load())
    {
        return;
    }

    // 栈构造 DriverClient——无单例。
    const ksword::ark::DriverClient client;
    auto result = client.scanInlineHooks();

    // IOCTL 失败则静默跳过。
    if (!result.io.ok)
    {
        return;
    }

    // 遍历结果，过滤出可疑条目。
    for (const auto& e : result.entries)
    {
        if (cancel->load())
        {
            return;
        }
        // CLEAN 和 READ_FAILED 不报。
        if (e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN
            || e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED)
        {
            continue;
        }

        AuditEntry entry;
        entry.category = QStringLiteral("Inline Hook");
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
    const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;
    auto result = client.enumerateIatEatHooks();

    if (!result.io.ok)
    {
        return;
    }

    for (const auto& e : result.entries)
    {
        if (cancel->load())
        {
            return;
        }
        if (e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN
            || e.status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED)
        {
            continue;
        }

        AuditEntry entry;
        // IAT 和 EAT 分别标注类别。
        entry.category =
            (e.hookClass == KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT)
                ? QStringLiteral("EAT")
                : QStringLiteral("IAT");
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
    const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;

    // --- SSDT ---
    {
        auto result = client.enumerateSsdt(0UL);
        if (result.io.ok)
        {
            for (const auto& e : result.entries)
            {
                if (cancel->load())
                {
                    return;
                }
                const QString modName = QString::fromStdString(e.moduleName);
                // 属于预期内核模块则跳过。
                if (isExpectedModule(modName, kSsdtExpectedModules))
                {
                    continue;
                }

                AuditEntry entry;
                entry.category = QStringLiteral("SSDT");
                entry.moduleName = modName;
                entry.targetName = QString::fromStdString(e.serviceName);
                entry.address = e.serviceRoutineAddress;
                entry.riskLevel = QStringLiteral("Critical");
                entry.detail = QStringLiteral("#%1").arg(e.serviceIndex);
                out.push_back(std::move(entry));
            }
        }
    }

    if (cancel->load())
    {
        return;
    }

    // --- ShadowSSDT ---
    {
        auto result = client.enumerateShadowSsdt();
        if (result.io.ok)
        {
            for (const auto& e : result.entries)
            {
                if (cancel->load())
                {
                    return;
                }
                const QString modName = QString::fromStdString(e.moduleName);
                if (isExpectedModule(modName, kShadowSsdtExpectedModules))
                {
                    continue;
                }

                AuditEntry entry;
                entry.category = QStringLiteral("ShadowSSDT");
                entry.moduleName = modName;
                entry.targetName = QString::fromStdString(e.serviceName);
                entry.address = e.serviceRoutineAddress;
                entry.riskLevel = QStringLiteral("Critical");
                entry.detail = QStringLiteral("#%1").arg(e.serviceIndex);
                out.push_back(std::move(entry));
            }
        }
    }
}

// ----------------------------------------------------------------
// scanObjectTypeProcedures：扫描 ObjectType 方法指针完整性。
// riskFlags 非零的条目视为可疑。
// ----------------------------------------------------------------

void KernelPatchHookAuditTab::scanObjectTypeProcedures(
    std::vector<AuditEntry>& out,
    const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;
    auto result = client.enumObjectTypeProcedures();

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
        entry.category = QStringLiteral("ObjectType");
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
    const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (cancel->load())
    {
        return;
    }

    const ksword::ark::DriverClient client;

    // queryDriverIntegrity 聚合 DriverObject/LDR/FastIo/CPU/IDT 证据。
    auto result = client.queryDriverIntegrity();

    if (!result.io.ok || result.unsupported)
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
        entry.category =
            kernelText("kernel.patch_hook_audit.filter.integrity",
                       QStringLiteral("驱动完整性"));
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
