#pragma once

// ============================================================
// GlobalDialogTheme.h
// 作用：
// 1) 在 UI 层集中处理普通 QDialog / QInputDialog 的主题背景；
// 2) 避免各业务模块反复给临时弹窗手工 setStyleSheet；
// 3) 与 UI/ThemedMessageBox 分工，消息框仍由专用主题器处理。
// ============================================================

#include <QApplication>

namespace ks::ui
{
    // InstallGlobalDialogTheme 作用：
    // - 给 QApplication 安装普通弹窗全局主题器；
    // - 捕获后续显示的 QInputDialog、QDialog 派生弹窗并补齐不透明背景。
    // 参数 appInstance：当前 QApplication 实例；为空时忽略。
    // 返回值：无。
    void InstallGlobalDialogTheme(QApplication* appInstance);

    // RefreshGlobalDialogTheme 作用：
    // - 主题切换后重新刷新当前已经打开的普通弹窗；
    // - 让深色/浅色模式变更立即反映到现有窗口。
    // 参数：无。
    // 返回值：无。
    void RefreshGlobalDialogTheme();
}
