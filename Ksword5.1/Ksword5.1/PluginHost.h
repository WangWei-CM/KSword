#pragma once

// ============================================================
// PluginHost.h
// 作用：
// - 提供 Ksword GUI 到独立插件命令行入口的唯一桥接；
// - GUI 读取插件清单，但绝不加载第三方插件代码或 DLL；
// - 文件、进程和网络入口共用同一目标上下文与菜单构建逻辑。
// ============================================================

#include <QString>

#include <QtGlobal>

class QMenu;
class QWidget;

namespace ks::plugin_host
{
    enum class TargetKind
    {
        File,
        Process,
        Network,
    };

    struct InvocationContext
    {
        TargetKind targetKind = TargetKind::File;
        QString filePath;
        quint32 processId = 0;
        QString processName;
    };

    // populateTargetMenu：
    // - 从 plugin\<id>\plugin.json 发现已安装插件；
    // - 只展示声明支持当前目标类型的插件，菜单层级始终为“插件 → <插件名>”；
    // - 选择动作后由 Ksword 直接用 QProcess 启动独立入口。
    void populateTargetMenu(QMenu* menu, QWidget* owner, const InvocationContext& context);

    // showPluginManager：
    // - 打开非模态插件管理窗口；
    // - 管理本地插件，并从商城执行许可证确认、校验和一键安装。
    void showPluginManager(QWidget* owner);
}
