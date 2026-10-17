#include "KernelServiceAuditUi.h"
#include "KernelThreadAuditTab.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/SegmentedControl.h"
#include "../UI/TablePresentation.h"
#include "../UI/ThemeBinding.h"
#include "../UI/ToolbarMetrics.h"
#include "../theme.h"

#include <QAction>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <algorithm>
#include <memory>

namespace ks::kernel::audit
{
    ModuleSnapshot QueryModules()
    {
        KernelThreadAuditTab::ModuleQueryStatus queryStatus{}; // 公共查询的能力/传输状态。
        ModuleSnapshot output; // 每轮独立快照，不共享可变 UI 缓存。
        const auto modules = KernelThreadAuditTab::queryKernelModules(&queryStatus, &output.status, nullptr);
        output.available = queryStatus == KernelThreadAuditTab::ModuleQueryStatus::Ok && !modules.empty();
        for (const auto& module : modules)
        {
            output.modules.push_back({module.baseAddress, module.imageSize,
                module.name.toLower().toStdString(), module.kernelImage});
        }
        return output;
    }

    QString CategoryText(const Category category)
    {
        switch (category)
        {
        case Category::Inline: return QStringLiteral("Inline Hook");
        case Category::Iat: return QStringLiteral("IAT");
        case Category::Eat: return QStringLiteral("EAT");
        case Category::Ssdt: return QStringLiteral("SSDT");
        case Category::ShadowSsdt: return QStringLiteral("ShadowSSDT");
        case Category::ObjectType: return QStringLiteral("ObjectType");
        case Category::Integrity: return ks::i18n::sourceText(QStringLiteral("驱动完整性"));
        }
        return {};
    }

    QString IssueText(const std::vector<QueryIssue>& issues)
    {
        QStringList lines; // 完整诊断与摘要分离，避免长错误挤占结果高度。
        for (const auto& issue : issues)
        {
            lines.push_back(QStringLiteral("%1: %2 (NTSTATUS=0x%3, Win32=%4)")
                .arg(issue.stage, ks::i18n::sourceText(issue.reason))
                .arg(static_cast<unsigned long>(issue.nativeStatus), 8, 16, QLatin1Char('0'))
                .arg(issue.win32));
        }
        return lines.join(QLatin1Char('\n'));
    }

    bool CheckQuery(std::vector<QueryIssue>& issues, const QString& stage, const bool transportOk,
        const bool complete, const long nativeStatus, const unsigned long win32)
    {
        if (!transportOk || !complete)
        {
            issues.push_back({stage, transportOk ? QStringLiteral("查询结果不完整")
                : QStringLiteral("查询失败"), nativeStatus, win32});
        }
        return transportOk;
    }

    void BindAuditLabel(QLabel* label, const bool secondary)
    {
        const QPointer<QLabel> safe(label); // 页面销毁后不继续访问旧标签。
        ks::ui::BindWidgetTheme(label, [safe, secondary]()
        {
            if (safe)
            {
                safe->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                    .arg(secondary ? KswordTheme::TextSecondaryHex() : KswordTheme::TextPrimaryHex()));
            }
        });
    }

    void InstallColumnPresets(QTableWidget* table, QHBoxLayout* toolbar,
        const std::vector<int>& overview, const std::vector<int>& evidence)
    {
        // A/B 仅保存列身份；表格、数据模型及快照操作条仍由原页面持有。
        auto* group = new QHBoxLayout();
        group->setContentsMargins(0, 0, 0, 0);
        group->setSpacing(0);
        auto* first = new QPushButton(QStringLiteral("A"), table->parentWidget());
        auto* second = new QPushButton(QStringLiteral("B"), table->parentWidget());
        first->setToolTip(ks::i18n::sourceText(QStringLiteral("概览列组")));
        second->setToolTip(ks::i18n::sourceText(QStringLiteral("地址与字节证据列组")));
        group->addWidget(first);
        group->addWidget(second);
        ks::ui::InstallSegmentedButtons(group, {first, second});
        ks::ui::NormalizeToolbarRow(group, 0);
        toolbar->insertLayout(2, group);
        ks::ui::ApplyFlatButtonTheme(first, ks::ui::FlatButtonTone::Neutral);
        ks::ui::ApplyFlatButtonTheme(second, ks::ui::FlatButtonTone::Neutral);
        const QPointer<QTableWidget> safe(table); // 菜单嵌套事件循环也可能关闭页面。
        const auto select = [first, second](const int preset)
        {
            ks::ui::SetSegmentedButtonSelected(first, preset == 0);
            ks::ui::SetSegmentedButtonSelected(second, preset == 1);
        };
        const auto apply = [safe, select](const std::vector<int>& visible, const int preset)
        {
            if (!safe)
            {
                return;
            }
            for (int column = 0; column < safe->columnCount(); ++column)
            {
                safe->setColumnHidden(column, std::find(visible.begin(), visible.end(), column) == visible.end());
            }
            select(preset);
        };
        QObject::connect(first, &QPushButton::clicked, table, [apply, overview]() { apply(overview, 0); });
        QObject::connect(second, &QPushButton::clicked, table, [apply, evidence]() { apply(evidence, 1); });
        // 自定义表头菜单必须使用不透明主题，并在手工改变时清除 A/B 选中态。
        table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table->horizontalHeader(), &QHeaderView::customContextMenuRequested, table,
            [safe, select](const QPoint& position)
        {
            if (!safe)
            {
                return;
            }
            QMenu menu; // 栈菜单不交给页面所有权，避免嵌套循环销毁父页时二次析构。
            menu.setStyleSheet(QStringLiteral("QMenu{background:%1;color:%2;}"
                "QMenu::item:selected{background:%3;color:%4;}QMenu::item:disabled{color:%5;}")
                .arg(KswordTheme::SurfaceHex(), KswordTheme::TextPrimaryHex(),
                    KswordTheme::PrimaryBlueHex, KswordTheme::OnAccentHex(), KswordTheme::TextSecondaryHex()));
            for (int column = 0; column < safe->columnCount(); ++column)
            {
                auto* action = menu.addAction(safe->horizontalHeaderItem(column)->text());
                action->setCheckable(true);
                action->setChecked(!safe->isColumnHidden(column));
                QObject::connect(action, &QAction::toggled, safe, [safe, select, column](const bool visible)
                {
                    if (safe)
                    {
                        safe->setColumnHidden(column, !visible);
                        select(-1);
                    }
                });
            }
            menu.exec(safe->horizontalHeader()->mapToGlobal(position));
        });
        ks::ui::ApplyTablePresentation(table);
        apply(overview, 0);
    }
}
