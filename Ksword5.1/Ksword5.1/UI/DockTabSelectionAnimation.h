#pragma once

class QPainter;

namespace ads
{
    class CDockAreaTabBar;
    class CDockWidgetTab;
}

namespace ks::ui
{
    // 为原 ADS 标签栏安装选中底面/下划线动画；幂等，不改变 Dock 或页面对象。
    // tabBar 为主导航或浮动导航的原标签栏；输入为空时忽略，无返回值。
    void InstallDockTabSelectionAnimation(ads::CDockAreaTabBar* tabBar);

    // 由原标签工厂在基类绘制后调用，整面覆盖静态 QSS 标记再绘制当前动画帧。
    // painter 使用 tab 的坐标；返回 true 后调用方跳过旧 hover/选中标记补绘。
    // 未登记、已移出原标签栏或不可绘制时返回 false，保留原生静态绘制。
    bool PaintAnimatedDockTabSelection(ads::CDockWidgetTab* tab, QPainter& painter);
}
