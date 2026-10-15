#pragma once
#include <QObject>
#include <QList>
#include <functional>
#include <memory>
#include "../../../shared/usermode/KswordPageNavigation.h"

class QWidget;
namespace ads { class CDockWidget; }

namespace ks::ui
{
    // 主程序提供实际 Tab 目录与页面激活，Taskbar 只呈现目录，不持有跨进程控件地址。
    class PageNavigationBridge final : public QObject
    {
    public:
        using DockList = std::function<QList<ads::CDockWidget*>()>;
        using DockAction = std::function<void(ads::CDockWidget*)>;
        PageNavigationBridge(QWidget* host, DockList list, DockAction prepare, DockAction activate,
            DockAction nestedPrepare = {});
        ~PageNavigationBridge() override;
        void setReady(); // 一级 Dock 框架创建完成后才发布 IPC 入口。
        bool handleMessage(const COPYDATASTRUCT* data, HWND sender);
    private:
        struct State;
        std::unique_ptr<State> m_state; // 目录生成、弱引用路由和取消代次的唯一所有者。
    };
}
