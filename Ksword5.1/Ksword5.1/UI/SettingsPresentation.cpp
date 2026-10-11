#include "../Framework.h"
#include "./SettingsPresentation.h"
#include "./ThemeBinding.h"
#include "../theme.h"
#include <QAbstractButton>
#include <QApplication>
#include <QFontDatabase>
#include <QPointer>
#include <QTabBar>
#include <algorithm>

namespace ks::ui
{
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
            owner->setStyleSheet(rules);
        }, ThemePalettePolicy::PreserveLocal);
    }

    void StyleSettingsColorButton(QAbstractButton* button)
    {
        if (button == nullptr)
        {
            return;
        }
        button->setProperty("ksword_theme_icon_managed", true); // 色块是未应用配置的数据原色。
        QFont code = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        code.setPointSizeF(std::max(10.5, button->font().pointSizeF()));
        button->setFont(code);
        const QPointer<QAbstractButton> target(button);
        BindWidgetTheme(button, [target]()
        {
            if (target)
            {
                const QColor input = KswordTheme::BlendColors(KswordTheme::WindowColor(), KswordTheme::SurfaceAltColor(), 70);
                target->setStyleSheet(QStringLiteral(
                    "QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:5px;padding:3px 8px;}"
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
                target->setStyleSheet(QStringLiteral(
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
