#pragma once
class QWidget;
class QAbstractButton;
class QTabBar;
class QFont;

namespace ks::ui
{
    // 设置内容专用阅读层级，不改变应用字体配置；热切换仍保留用户选择的字体家族。
    void ApplySettingsPresentation(QWidget* settings);
    void StyleSettingsNavigation(QTabBar* bar, const QFont& bodyFont);
    // 调色按钮保留数据色块，复原动作作为轻文字链接；仅接管明确登记的控件。
    void StyleSettingsColorButton(QAbstractButton* button);
    void StyleSettingsLinkButton(QAbstractButton* button);
}
