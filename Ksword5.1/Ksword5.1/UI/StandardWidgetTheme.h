#pragma once

class QApplication;

namespace ks::ui
{
    // 应用启动时安装标准基础主题；普通新控件自动接入，无需逐页调用样式函数。
    // 页面仍负责业务语义、布局类型和表格操作模式，不从文字猜测危险动作。
    void InstallStandardWidgetTheme(QApplication* application);

    // 专业控件可在首次Polish之前明确保留内部度量/绘制；全局颜色基线仍适用。
    inline constexpr const char* StandardThemeExemptProperty = "ksword_standard_theme_exempt";
}
