#pragma once

// ============================================================
// WindowChrome.h
// 作用：
// 1) 把主题色同步到所有“原生标题栏”顶层窗口（子窗口、对话框、浮动 Dock）；
// 2) 使用 DWM 属性染色标题栏/边框，解决系统默认白色标题栏与深色主题割裂问题；
// 3) 与自绘标题栏分工：无边框主窗口只明确接入圆角外形，不接管其标题栏颜色。
// ============================================================

class QApplication;
class QWidget;

namespace ks::ui
{
    // InstallWindowChrome 作用：
    // - 给 QApplication 安装顶层窗口标题栏主题器；
    // - 捕获后续显示的所有原生标题栏窗口并应用当前主题的标题栏颜色。
    // 参数 appInstance：当前 QApplication 实例；为空时忽略。
    // 返回值：无。
    void InstallWindowChrome(QApplication* appInstance);

    // RefreshAllWindowChrome 作用：
    // - 主题切换后重新染色当前所有已显示的顶层窗口标题栏；
    // - 让深浅色/主题色/主背景色变更立即反映到已打开窗口。
    // 参数：无。
    // 返回值：无。
    void RefreshAllWindowChrome();

    // ApplyWindowChrome 作用：
    // - 对单个顶层窗口应用当前主题的标题栏颜色；
    // - 窗口为空、非顶层、无边框或原生句柄未创建时静默忽略。
    // 参数 topLevelWidget：目标顶层窗口。
    // 返回值：无。
    void ApplyWindowChrome(QWidget* topLevelWidget);

    // InstallRoundedFramelessWindowFrame：明确登记自有无边框顶层窗口的8px圆角区域。
    // 构造结束前调用一次；透明/普通背景共用外形，最大化或全屏清除区域，保留原缩放命中。
    // 参数 topLevelWidget：自有窗口；空指针、非顶层或非无边框窗口忽略。返回值：无。
    void InstallRoundedFramelessWindowFrame(QWidget* topLevelWidget);
}
