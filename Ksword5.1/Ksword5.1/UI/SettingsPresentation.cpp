#include "../Framework.h"
#include "./SettingsPresentation.h"
#include "./ThemeBinding.h"
#include "../theme.h"
#include <QAbstractButton>
#include <QApplication>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QTabBar>
#include <QTimer>
#include <algorithm>

namespace
{
    // 完整最终规则逐值比较：语言/重复palette事件不使同一设置子树再次repolish。
    // 不依赖主题代次，避免遗漏显式局部规则被替换后的重新接管。
    void applySettingsRules(QWidget* widget, const QString& rules)
    {
        if (widget->styleSheet() != rules) widget->setStyleSheet(rules);
    }

    // 只追踪已登记的设置表单。字体或复合控件尺寸变化后，同步左右两列的视觉中心。
    class SettingsFormMetrics final : public QObject
    {
    public:
        explicit SettingsFormMetrics(QFormLayout* form) : QObject(form), m_form(form)
        {
            if (form->parentWidget())
            {
                form->parentWidget()->installEventFilter(this);
            }
            schedule();
        }

    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
                || event->type() == QEvent::LayoutRequest)
            {
                schedule();
            }
            return false;
        }

    private:
        // 合并 Qt 样式/布局事件，避免在布局计算中再次改变几何。
        void schedule()
        {
            if (m_pending)
            {
                return;
            }
            m_pending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_pending = false;
                apply();
            });
        }

        void apply()
        {
            const int labelWidth = m_form->property("ksword_settings_label_width").toInt();
            for (int row = 0; row < m_form->rowCount(); ++row)
            {
                QLayoutItem* labelItem = m_form->itemAt(row, QFormLayout::LabelRole);
                QLayoutItem* fieldItem = m_form->itemAt(row, QFormLayout::FieldRole);
                auto* label = labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr;
                QWidget* field = fieldItem ? fieldItem->widget() : nullptr;
                // 跨行说明不强加控件行高；长标签仍可按实际宽度换行。
                if (!label || label->text().isEmpty() || !field)
                {
                    continue;
                }
                label->installEventFilter(this);
                field->installEventFilter(this);
                label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                label->setWordWrap(true);
                if (label->minimumWidth() != labelWidth)
                {
                    label->setMinimumWidth(labelWidth);
                }
                // 行内文字按同一字号测量，右侧的原生复选框也占据完整的单行高度。
                const int rowHeight = std::max({28, label->fontMetrics().height() + 12,
                    field->fontMetrics().height() + 12});
                if (field->minimumHeight() != rowHeight)
                {
                    field->setMinimumHeight(rowHeight);
                }
                // 不能将复合行的无宽度 sizeHint 固化为最小高度：可换行说明的建议
                // 高度可能很大。实际换行与整行高度交给 QFormLayout 的 heightForWidth。
                if (label->minimumHeight() != rowHeight)
                {
                    label->setMinimumHeight(rowHeight);
                }
            }
        }

        QFormLayout* m_form; // 布局拥有此对象，不跨设置页保存状态。
        bool m_pending = false; // 每轮只执行一次尺寸同步。
    };
}

namespace ks::ui
{
    void StyleSettingsForm(QFormLayout* form, int labelWidth)
    {
        if (!form)
        {
            return;
        }
        form->setProperty("ksword_settings_label_width", labelWidth);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(4);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        if (!form->property("ksword_settings_form_metrics").toBool())
        {
            form->setProperty("ksword_settings_form_metrics", true);
            new SettingsFormMetrics(form);
        }
    }

    void ApplySettingsPresentation(QWidget* settings)
    {
        if (settings == nullptr)
        {
            return;
        }
        QFont body = QApplication::font(); // 保留用户配置家族，设置正文只提高阅读字号下限。
        if (body.pixelSize() > 0)
        {
            body.setPixelSize(std::max(14, body.pixelSize()));
        }
        else
        {
            body.setPointSizeF(std::max(10.5, body.pointSizeF()));
        }
        body.setWeight(QFont::Normal);
        settings->setFont(body);
        if (settings->property("ksword_settings_content").toBool())
        {
            return;
        }
        settings->setProperty("ksword_settings_content", true);
        const QPointer<QWidget> owner(settings);
        BindWidgetTheme(settings, [owner]()
        {
            if (!owner)
            {
                return;
            }
            // 输入面比窗口稍亮，边界使用低饱和中性色；不再用大片亮灰块分隔每一行。
            const QColor input = KswordTheme::BlendColors(KswordTheme::WindowColor(), KswordTheme::SurfaceAltColor(), 70);
            QString rules = QStringLiteral(
                "QWidget[ksword_settings_content=\"true\"]{background:transparent;color:__TEXT__; }"
                // 表单正文与控件文案同级，避免全局按钮半粗体让右列显得更重。
                "QWidget[ksword_settings_content=\"true\"] QLabel,"
                "QWidget[ksword_settings_content=\"true\"] QAbstractButton,"
                "QWidget[ksword_settings_content=\"true\"] QLineEdit,"
                "QWidget[ksword_settings_content=\"true\"] QComboBox,"
                "QWidget[ksword_settings_content=\"true\"] QAbstractSpinBox{font-weight:400;}"
                "QWidget[ksword_settings_content=\"true\"] QLineEdit,"
                "QWidget[ksword_settings_content=\"true\"] QComboBox,"
                "QWidget[ksword_settings_content=\"true\"] QAbstractSpinBox{"
                "background:__INPUT__;color:__TEXT__;border:1px solid __BORDER__;border-radius:5px;}"
                "QWidget[ksword_settings_content=\"true\"] QComboBox::drop-down{background:transparent;border:0;}"
                "QWidget[ksword_settings_content=\"true\"] QLineEdit:disabled{color:__DISABLED__; }"
                "QWidget[ksword_settings_content=\"true\"] QComboBox:disabled{color:__DISABLED__; }"
                "QWidget[ksword_settings_content=\"true\"] QLabel[ksword_settings_hint=\"true\"]{color:__SECONDARY__; }"
                "QWidget[ksword_settings_content=\"true\"] QLabel:disabled{color:__DISABLED__; }");
            rules.replace(QStringLiteral("__INPUT__"), input.name());
            rules.replace(QStringLiteral("__TEXT__"), KswordTheme::TextPrimaryColor().name());
            rules.replace(QStringLiteral("__BORDER__"), KswordTheme::BorderColor().name());
            rules.replace(QStringLiteral("__DISABLED__"), KswordTheme::TextDisabledColor().name());
            rules.replace(QStringLiteral("__SECONDARY__"), KswordTheme::EnsureTextContrast(
                KswordTheme::TextSecondaryColor(), KswordTheme::WindowColor(), 4.5).name());
            applySettingsRules(owner, rules);
        }, ThemePalettePolicy::PreserveLocal);
    }

    void StyleSettingsColorButton(QAbstractButton* button)
    {
        if (button == nullptr)
        {
            return;
        }
        button->setProperty("ksword_theme_icon_managed", true); // 色块是未应用配置的数据原色。
        QFont code = button->font(); // 色值保留等宽家族，但不另行放大字号或加重。
        code.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
        code.setWeight(QFont::Normal);
        button->setFont(code);
        const QPointer<QAbstractButton> target(button);
        BindWidgetTheme(button, [target]()
        {
            if (target)
            {
                const QColor input = KswordTheme::BlendColors(KswordTheme::WindowColor(), KswordTheme::SurfaceAltColor(), 70);
                applySettingsRules(target, QStringLiteral(
                    "QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:5px;padding:3px 8px;font-weight:400;}"
                    "QPushButton:hover{border-color:%4;}"
                    "QPushButton:disabled{color:%5;}")
                    .arg(input.name(), KswordTheme::TextPrimaryColor().name(), KswordTheme::BorderColor().name(),
                        KswordTheme::ControlAccentColor().name(), KswordTheme::TextDisabledColor().name()));
            }
        }, ThemePalettePolicy::PreserveLocal);
    }

    void StyleSettingsNavigation(QTabBar* bar, const QFont& bodyFont)
    {
        if (bar == nullptr)
        {
            return;
        }
        QFont navigation = bodyFont; // 导航比正文略大，保留字号单位和用户字体家族。
        if (navigation.pixelSize() > 0)
        {
            navigation.setPixelSize(std::max(16, navigation.pixelSize() + 2));
        }
        else
        {
            navigation.setPointSizeF(std::max(11.5, navigation.pointSizeF() + 1.0));
        }
        bar->setProperty("ksword_settings_tabs", true);
        bar->setFont(navigation);
    }

    void StyleSettingsLinkButton(QAbstractButton* button)
    {
        if (button == nullptr)
        {
            return;
        }
        const QPointer<QAbstractButton> target(button);
        BindWidgetTheme(button, [target]()
        {
            if (target)
            {
                const QColor base = KswordTheme::WindowColor();
                applySettingsRules(target, QStringLiteral(
                    "QAbstractButton{background:transparent;color:%1;border:0;padding:3px 6px;font-weight:400;}"
                    "QAbstractButton:hover{background:%2;border-radius:4px;}"
                    "QAbstractButton:disabled{color:%3;}")
                    .arg(KswordTheme::EnsureTextContrast(KswordTheme::PrimaryAccentColor(), base, 4.5).name(),
                        KswordTheme::BlendColors(base, KswordTheme::ControlAccentColor(), 20).name(),
                        KswordTheme::TextDisabledColor().name()));
            }
        }, ThemePalettePolicy::PreserveLocal);
    }
}
