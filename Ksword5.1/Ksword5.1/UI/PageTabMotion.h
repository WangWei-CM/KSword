#pragma once

#include <QColor>
#include <functional>

class QTabBar;

namespace ks::ui
{
    // 普通导航只移动中性选中面和强调细线；文本及图标由原 QTabBar 继续绘制。
    struct PageTabMotionColors
    {
        QColor selected; // 当前栏实际选中底面。
        QColor marker; // 已按选中底面校准的强调标记。
    };

    // 对明确登记的原栏安装一次动画；颜色回调仅求主题 token，不访问业务页面或模型。
    void InstallPageTabMotion(QTabBar* bar, std::function<PageTabMotionColors()> colors);
}
