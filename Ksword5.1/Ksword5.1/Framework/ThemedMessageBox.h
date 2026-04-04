#pragma once

#include <QApplication>

namespace ks::ui
{
    // InstallGlobalMessageBoxTheme 作用：
    // - 给 QApplication 安装全局 QMessageBox 主题器；
    // - 让后续所有 QMessageBox 在显示时自动套用统一深浅色与按钮样式。
    // 调用方式：main.cpp 在 QApplication 构造完成后调用一次。
    // 参数 appInstance：当前应用对象；为空时忽略。
    // 返回值：无。
    void InstallGlobalMessageBoxTheme(QApplication* appInstance);

    // RefreshGlobalMessageBoxTheme 作用：
    // - 在主题切换后重新刷新当前已打开的 QMessageBox；
    // - 解决深色/浅色切换时旧消息框残留旧配色的问题。
    // 调用方式：MainWindow::applyAppearanceSettings 完成全局 palette 更新后调用。
    // 参数：无。
    // 返回值：无。
    void RefreshGlobalMessageBoxTheme();
}
