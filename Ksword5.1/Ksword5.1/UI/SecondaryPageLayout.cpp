#include "../Framework.h"
#include "./SecondaryPageLayout.h"
#include "./ThemeBinding.h"
#include "./FlatButtonTheme.h"
#include "./ToolbarMetrics.h"
#include "./PageTabMotion.h"
#include "../theme.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <algorithm>

namespace
{
    // 主题更新只替换自身块，保留页面的专用编辑器、提示色和其它局部规则。
    void SetOwnedStyle(QWidget* widget, const QString& rules)
    {
        const QString begin = QStringLiteral("/* ks_secondary_begin */");
        const QString end = QStringLiteral("/* ks_secondary_end */");
        QString style = widget->styleSheet(); // style 保存调用方已有的局部样式。
        const qsizetype first = style.indexOf(begin);
        const qsizetype last = first < 0 ? -1 : style.indexOf(end, first);
        if (first >= 0 && last >= first)
        {
            style.remove(first, last + end.size() - first);
        }
        const QString ownedBlock = begin + rules + end;
        widget->setProperty("ksword_secondary_style_block", ownedBlock);
        style = style.trimmed() + QLatin1Char('\n') + ownedBlock;
        if (widget->styleSheet() != style)
        {
            widget->setStyleSheet(style);
        }
    }

    // 已登记底栏只处理布局中的普通动作，不扫描其它页面或重置业务按钮状态/颜色。
    void styleFooterAction(QAbstractButton* button)
    {
        if (!qobject_cast<QPushButton*>(button) && !qobject_cast<QToolButton*>(button))
        {
            return;
        }
        ks::ui::NormalizeToolbarControl(button);
        QString remaining = button->styleSheet();
        remaining.remove(ks::ui::ToolbarControlGeometryStyle().trimmed());
        remaining.remove(ks::ui::ToolbarIconGeometryStyle().trimmed());
        if (button->property("ksword_flat_button_managed").toBool())
        {
            // managed标记可能在业务改成专用QSS后仍保留；只剔除实际共享块，不能抢回业务色。
            const QString begin = QStringLiteral("/*KSWORD_FLAT_BUTTON_BEGIN*/");
            const QString end = QStringLiteral("/*KSWORD_FLAT_BUTTON_END*/");
            const qsizetype first = remaining.indexOf(begin);
            const qsizetype last = first < 0 ? -1 : remaining.indexOf(end, first);
            if (first >= 0 && last >= first)
            {
                remaining.remove(first, last + end.size() - first);
            }
            if (button->property("ksword_flat_button_appearance").toInt()
                == static_cast<int>(ks::ui::FlatButtonAppearance::Auto)
                && remaining.trimmed().isEmpty())
            {
                ks::ui::ApplyFlatButtonTheme(button,
                    static_cast<ks::ui::FlatButtonTone>(button->property("ksword_flat_button_tone").toInt()),
                    ks::ui::FlatButtonAppearance::Solid);
            }
            return; // 显式Danger/Accent/Flat策略和其原信号由业务继续持有。
        }
        if (ks::ui::HasWidgetThemeBinding(button))
        {
            return;
        }
        if (remaining.trimmed().isEmpty())
        {
            ks::ui::ApplyFlatButtonTheme(button, ks::ui::FlatButtonTone::Neutral,
                ks::ui::FlatButtonAppearance::Solid);
        }
    }

    void styleFooterLayout(QLayout* layout)
    {
        if (!layout)
        {
            return;
        }
        for (int index = 0; index < layout->count(); ++index)
        {
            QLayoutItem* item = layout->itemAt(index);
            if (item->layout())
            {
                styleFooterLayout(item->layout());
            }
            else if (auto* box = qobject_cast<QDialogButtonBox*>(item->widget()))
            {
                for (auto* button : box->buttons()) styleFooterAction(button);
            }
            else
            {
                styleFooterAction(qobject_cast<QAbstractButton*>(item->widget()));
            }
        }
    }

    // 双栏只在容器宽度跨阈值时重新排列，避免每次内容刷新都重建表单。
    class SecondaryColumns final : public QWidget
    {
    public:
        SecondaryColumns(QWidget* primary, QWidget* secondary, QWidget* parent,
            int breakpoint, int primaryStretch, int secondaryStretch)
            : QWidget(parent), m_primary(primary), m_secondary(secondary),
              m_breakpoint(breakpoint), m_primaryStretch(primaryStretch),
              m_secondaryStretch(secondaryStretch), m_grid(new QGridLayout(this))
        {
            // 横向尺寸由宿主分配，内容高度仍保留，供页面内部滚动计算。
            setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_grid->setContentsMargins(0, 0, 0, 0);
            m_grid->setHorizontalSpacing(24);
            m_grid->setVerticalSpacing(12);
            primary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            secondary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            // 初次按低高度的双栏测量；拿到真实宽度后再收为单列，避免先撑高宽窗。
            arrange(true);
        }

        QSize minimumSizeHint() const override
        {
            return QSize(0, QWidget::minimumSizeHint().height());
        }

    protected:
        void resizeEvent(QResizeEvent* event) override
        {
            QWidget::resizeEvent(event);
            scheduleArrange();
        }

        void changeEvent(QEvent* event) override
        {
            QWidget::changeEvent(event);
            if (event->type() == QEvent::FontChange)
            {
                scheduleArrange();
            }
        }

    private:
        // 排列延后到当前布局计算结束，避免 resizeEvent 内嵌套修改布局。
        void scheduleArrange()
        {
            if (m_pending)
            {
                return;
            }
            m_pending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_pending = false;
                const int threshold = m_breakpoint * std::max(14, fontMetrics().height()) / 14;
                const bool wide = width() >= threshold;
                if (wide != m_wide)
                {
                    arrange(wide);
                }
            });
        }

        void arrange(bool wide)
        {
            m_grid->removeWidget(m_primary);
            m_grid->removeWidget(m_secondary);
            m_grid->addWidget(m_primary, 0, 0, Qt::AlignTop);
            m_grid->addWidget(m_secondary, wide ? 0 : 1, wide ? 1 : 0, Qt::AlignTop);
            m_grid->setColumnStretch(0, wide ? m_primaryStretch : 1);
            m_grid->setColumnStretch(1, wide ? m_secondaryStretch : 0);
            m_wide = wide;
            updateGeometry();
        }

        QWidget* m_primary; // 左侧或上方的主要配置区，由容器持有。
        QWidget* m_secondary; // 右侧或下方的辅助区，始终保持可见。
        int m_breakpoint; // 正常字号下切换为两列的宽度。
        int m_primaryStretch; // 宽布局左列的宽度权重。
        int m_secondaryStretch; // 宽布局右列的宽度权重。
        QGridLayout* m_grid; // 唯一布局，重新排列时不重建业务控件。
        bool m_wide = false; // 当前是否并列显示。
        bool m_pending = false; // 合并连续尺寸和字体变化。
    };
}

namespace ks::ui
{
    void StyleSecondaryWindow(QWidget* window)
    {
        if (window == nullptr || window->property("ksword_secondary_window").toBool())
        {
            return;
        }
        window->setProperty("ksword_secondary_window", true);
        window->setAttribute(Qt::WA_StyledBackground, true);
        // 部分页面在登记前已触发过旧弹窗兜底，先移除它拥有的尾块，避免两套样式竞争。
        const QString oldStyle = window->styleSheet();
        const qsizetype oldBlock = oldStyle.indexOf(QStringLiteral("/* KSWORD_GLOBAL_DIALOG_THEME_BEGIN */"));
        if (oldBlock >= 0)
        {
            window->setStyleSheet(oldStyle.left(oldBlock));
        }
        const QPointer<QWidget> target(window); // 窗口关闭后不再执行排队的主题刷新。
        BindWidgetTheme(window, [target]()
        {
            if (!target)
            {
                return;
            }
            // 显式窗口底面避免继承主窗口透明背景；输入与内容只用中性色区分。
            const QString rules = QStringLiteral(
                "QWidget[ksword_secondary_window=\"true\"]{background:%1;color:%2;}"
                "QWidget[ksword_secondary_window=\"true\"] QLabel{background:transparent;}"
                "QWidget[ksword_secondary_window=\"true\"] QLineEdit,"
                "QWidget[ksword_secondary_window=\"true\"] QComboBox,"
                "QWidget[ksword_secondary_window=\"true\"] QAbstractSpinBox{"
                "background:%3;color:%2;border:1px solid %4;border-radius:4px;}"
                "QWidget[ksword_secondary_window=\"true\"] QLineEdit:disabled{color:%5;}"
                "QWidget[ksword_secondary_window=\"true\"] QComboBox:disabled{color:%5;}"
                "QWidget[ksword_secondary_window=\"true\"] QAbstractSpinBox:disabled{color:%5;}"
                "QWidget[ksword_secondary_window=\"true\"] QLineEdit[ksword_page_search_style=\"true\"]{border:0;}"
                "QWidget[ksword_secondary_window=\"true\"] QScrollArea#ks_adaptive_page_scroll{border:0;background:transparent;}"
                "QWidget[ksword_secondary_window=\"true\"] QMenu{background:%3;color:%2;border:1px solid %4;}"
                "QWidget[ksword_secondary_window=\"true\"] QMenu::item:selected{background:%6;color:%7;}"
                "QWidget[ksword_secondary_window=\"true\"] QMenu::item:disabled{color:%5;}")
                .arg(KswordTheme::WindowColor().name(), KswordTheme::TextPrimaryColor().name(),
                    KswordTheme::ControlInputSurfaceColor().name(), KswordTheme::BorderColor().name(),
                    KswordTheme::TextDisabledColor().name(), KswordTheme::ControlAccentColor().name(),
                    KswordTheme::MaximumContrastMonochromeColor(KswordTheme::ControlAccentColor()).name());
            SetOwnedStyle(target, rules);
        });
    }

    void StyleSecondaryTabs(QTabWidget* tabs)
    {
        if (tabs == nullptr || tabs->property("ksword_secondary_tabs").toBool())
        {
            return;
        }
        tabs->setProperty("ksword_secondary_tabs", true);
        tabs->setTabPosition(QTabWidget::North);
        tabs->setDocumentMode(true);
        // 栈与标签栏不将隐藏页的最小高度传给窗口；每页保留自己的滚动语义。
        if (auto* stack = tabs->findChild<QStackedWidget*>())
        {
            stack->setMinimumSize(0, 0);
            stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        }
        tabs->setStyleSheet(tabs->styleSheet() + QStringLiteral(
            "QTabWidget[ksword_secondary_tabs=\"true\"]::pane{border:0;padding:0;margin:0;background:transparent;}"));
        QTabBar* bar = tabs->tabBar(); // 只绑定明确传入的栏，不修改全局或详情导航。
        if (bar->property("ksword_page_tabs_style").toBool())
        {
            bar->setStyleSheet(QString());
        }
        bar->setProperty("ksword_secondary_tabs", true);
        bar->setExpanding(false);
        bar->setUsesScrollButtons(true);
        InstallPageTabMotion(bar, []()
        {
            const QColor selected = KswordTheme::WindowColor();
            return PageTabMotionColors{selected, KswordTheme::EnsureTextContrast(
                KswordTheme::ControlAccentColor(), selected, 3.0)};
        });
        const QPointer<QTabBar> target(bar);
        BindWidgetTheme(bar, [target]()
        {
            if (!target)
            {
                return;
            }
            const QColor base = KswordTheme::WindowColor();
            const QString rules = QStringLiteral(
                "QTabBar[ksword_secondary_tabs=\"true\"]{background:transparent;border-bottom:1px solid %1;}"
                "QTabBar[ksword_secondary_tabs=\"true\"]::tab{background:transparent;color:%2;"
                "border:0;border-bottom:2px solid transparent;padding:8px 14px;margin:0;min-height:16px;}"
                "QTabBar[ksword_secondary_tabs=\"true\"]::tab:selected{color:%3;border-bottom-color:transparent;}"
                "QTabBar[ksword_secondary_tabs=\"true\"]::tab:hover{background:%4;color:%3;}"
                "QTabBar[ksword_secondary_tabs=\"true\"]::tab:disabled{color:%5;}")
                .arg(KswordTheme::BorderColor().name(), KswordTheme::TextSecondaryColor().name(),
                    KswordTheme::TextPrimaryColor().name(),
                    KswordTheme::BlendColors(base, KswordTheme::ControlAccentColor(), 16).name(),
                    KswordTheme::TextDisabledColor().name());
            SetOwnedStyle(target, rules + (target->property("ksword_settings_tabs").toBool()
                ? QStringLiteral("QTabBar[ksword_settings_tabs=\"true\"]::tab:selected{font-weight:600;}") : QString()));
        });
    }

    void StyleSecondarySection(QGroupBox* section)
    {
        if (section == nullptr || section->property("ksword_secondary_section").toBool())
        {
            return;
        }
        section->setProperty("ksword_secondary_section", true);
        const QPointer<QGroupBox> target(section);
        BindWidgetTheme(section, [target]()
        {
            if (!target)
            {
                return;
            }
            // 细线位于标题下方，正文没有卡片底色和额外侧边，信息密度由页面布局决定。
            const bool settings = target->property("ksword_settings_section").toBool();
            SetOwnedStyle(target, QStringLiteral(
                "QGroupBox[ksword_secondary_section=\"true\"]{background:transparent;"
                "border:0;border-top:1px solid %1;border-radius:0;margin-top:%3px;padding-top:%4px;}"
                "QGroupBox[ksword_secondary_section=\"true\"]::title{"
                "subcontrol-origin:margin;subcontrol-position:top left;left:0;top:0;"
                "padding:0;background:transparent;color:%2;font-weight:600;}")
                .arg(KswordTheme::BorderColor().name(), KswordTheme::TextSecondaryColor().name())
                .arg(settings ? 24 : 22).arg(8));
        });
    }

    void StyleSecondaryForm(QFormLayout* form, int labelWidth)
    {
        if (form == nullptr)
        {
            return;
        }
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(8);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        // 标签宽度统一但允许长翻译换行，不固定整个表单的高度。
        for (int row = 0; row < form->rowCount(); ++row)
        {
            QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole);
            auto* label = item == nullptr ? nullptr : qobject_cast<QLabel*>(item->widget());
            if (label != nullptr)
            {
                label->setMinimumWidth(labelWidth);
                label->setWordWrap(true);
            }
        }
        // 页面常先创建表单再逐行加入控件；延后一轮补齐标签宽度及直接单行输入高度。
        QTimer::singleShot(0, form, [form, labelWidth]()
        {
            for (int row = 0; row < form->rowCount(); ++row)
            {
                QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole);
                auto* label = labelItem == nullptr ? nullptr : qobject_cast<QLabel*>(labelItem->widget());
                if (label != nullptr)
                {
                    label->setMinimumWidth(labelWidth);
                    label->setWordWrap(true);
                }
                QLayoutItem* field = form->itemAt(row, QFormLayout::FieldRole);
                QWidget* control = field == nullptr ? nullptr : field->widget();
                if (qobject_cast<QLineEdit*>(control) || qobject_cast<QComboBox*>(control)
                    || qobject_cast<QAbstractSpinBox*>(control))
                {
                    NormalizeToolbarControl(control);
                }
            }
        });
    }

    void StyleSecondaryFooter(QWidget* footer)
    {
        if (footer == nullptr || footer->property("ksword_secondary_footer").toBool())
        {
            return;
        }
        footer->setProperty("ksword_secondary_footer", true);
        footer->setAttribute(Qt::WA_StyledBackground, true);
        if (footer->layout() != nullptr)
        {
            footer->layout()->setContentsMargins(16, 8, 16, 8);
            footer->layout()->setSpacing(8);
        }
        if (auto* box = qobject_cast<QDialogButtonBox*>(footer))
        {
            for (auto* button : box->buttons()) styleFooterAction(button);
        }
        else
        {
            styleFooterLayout(footer->layout());
        }
        // 常见构造顺序先登记底栏再加按钮；只补这一行的构造尾部，不建立全局遍历。
        QTimer::singleShot(0, footer, [footer]()
        {
            if (auto* box = qobject_cast<QDialogButtonBox*>(footer))
            {
                for (auto* button : box->buttons()) styleFooterAction(button);
            }
            else
            {
                styleFooterLayout(footer->layout());
            }
        });
        const QPointer<QWidget> target(footer);
        BindWidgetTheme(footer, [target]()
        {
            if (target)
            {
                SetOwnedStyle(target, QStringLiteral(
                    "QWidget[ksword_secondary_footer=\"true\"]{background:%1;border:0;border-top:1px solid %2;}")
                    .arg(KswordTheme::WindowColor().name(), KswordTheme::BorderColor().name()));
            }
        });
    }

    void StyleSecondaryButtonBox(QDialogButtonBox* buttons)
    {
        if (buttons == nullptr)
        {
            return;
        }
        StyleSecondaryFooter(buttons);
        // 保留默认、取消及业务角色；只规范按钮大小，不重新连接 accepted/rejected。
        for (QAbstractButton* button : buttons->buttons())
        {
            NormalizeToolbarControl(button);
            button->setMinimumWidth(std::max(76, button->minimumWidth()));
        }
    }

    void StyleSecondaryContentLayout(QLayout* layout)
    {
        if (layout != nullptr)
        {
            layout->setContentsMargins(16, 12, 16, 12);
            layout->setSpacing(12);
        }
    }

    QWidget* CreateSecondaryColumns(QWidget* primary, QWidget* secondary,
        QWidget* parent, int breakpoint, int primaryStretch, int secondaryStretch)
    {
        if (primary == nullptr || secondary == nullptr)
        {
            return nullptr;
        }
        return new SecondaryColumns(primary, secondary, parent, breakpoint, primaryStretch, secondaryStretch);
    }
}
