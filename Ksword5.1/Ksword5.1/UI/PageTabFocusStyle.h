#pragma once

class QTabBar;

namespace ks::ui
{
    // 普通共享页签按输入来源显示焦点：鼠标隐藏，键盘保留；不改变焦点策略。
    void InstallMouseAwareTabFocus(QTabBar* bar);
}
