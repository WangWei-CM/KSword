#pragma once

// ============================================================
// WindowDock.h
// 作用说明：
// 1) 提供独立“窗口”模块入口，避免窗口功能挂在“其它”分组中；
// 2) 复用现有窗口管理实现（OtherDock）作为基础能力；
// 3) 让 MainWindow 以标准模块方式注册“窗口”Dock 页。
// ============================================================

#include "../OtherDock/OtherDock.h"

// ============================================================
// WindowDock
// 说明：
// - 当前类作为独立模块外壳，直接继承 OtherDock；
// - 后续若需要拆分窗口功能，可在本类中逐步接管具体实现。
// ============================================================
class WindowDock final : public OtherDock
{
public:
    // 构造函数：
    // - 作用：创建独立窗口管理模块实例；
    // - 参数 parent：Qt 父控件。
    explicit WindowDock(QWidget* parent = nullptr)
        : OtherDock(parent)
    {
    }

    // 析构函数：
    // - 作用：沿用基类析构逻辑，确保刷新线程与定时器安全释放。
    ~WindowDock() override = default;
};

