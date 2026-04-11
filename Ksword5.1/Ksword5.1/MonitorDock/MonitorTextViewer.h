#pragma once

// ============================================================
// MonitorTextViewer.h
// 作用：
// 1) 提供监控模块统一的只读文本查看窗口；
// 2) 复用 CodeEditorWidget，展示 ETW/WMI/进程定向监控等文本详情；
// 3) 避免每个 Dock 各自重复实现“文本详情弹窗”。
// ============================================================

#include <QString>

class QWidget;

namespace monitor_text_viewer
{
    // showReadOnlyTextWindow：
    // - 作用：弹出非模态的只读文本编辑器窗口；
    // - 调用：监控模块查看 ETW 详情、事件原始文本、导出前预览等场景复用；
    // - 传入 parentWidget：父窗口；
    // - 传入 titleText：窗口标题；
    // - 传入 contentText：正文内容；
    // - 传入 virtualPathText：虚拟路径/来源标签，可为空；
    // - 传出：无，直接显示窗口。
    void showReadOnlyTextWindow(
        QWidget* parentWidget,
        const QString& titleText,
        const QString& contentText,
        const QString& virtualPathText = QString());
}
