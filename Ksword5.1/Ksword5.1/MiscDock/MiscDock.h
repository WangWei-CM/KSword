#pragma once

// ============================================================
// MiscDock.h
// 作用：
// 1) 提供“杂项”总入口页；
// 2) 通过内部 Tab 承载子功能模块；
// 3) 当前首个子模块为“引导（Windows Boot Editor）”。
// ============================================================

#include "../Framework.h"

#include <QWidget>

class QTabWidget;
class QVBoxLayout;
class BootEditorTab;
namespace ks::misc
{
    class DiskEditorTab;
}

class MiscDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：创建“杂项”根布局与内部 Tab。
    // - 参数 parent：Qt 父控件。
    explicit MiscDock(QWidget* parent = nullptr);
    ~MiscDock() override = default;

private:
    // initializeUi：
    // - 作用：初始化“杂项”页的控件树；
    // - 调用：构造函数中调用一次。
    void initializeUi();

private:
    QVBoxLayout* m_rootLayout = nullptr;      // m_rootLayout：杂项页根布局。
    QTabWidget* m_mainTabWidget = nullptr;    // m_mainTabWidget：杂项页内部 Tab 容器。
    BootEditorTab* m_bootEditorTab = nullptr; // m_bootEditorTab：引导编辑器页组件。
    ks::misc::DiskEditorTab* m_diskEditorTab = nullptr; // m_diskEditorTab：磁盘编辑器页组件。
};
