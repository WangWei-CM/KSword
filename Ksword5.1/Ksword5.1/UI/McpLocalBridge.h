#pragma once

#include "./McpUiController.h"
#include <QPointer>
#include <QString>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QWidget;

namespace ks::ui
{
    // 主程序内建本地服务；调用方显式 start，close 在宿主拆除业务对象前执行。
    class McpLocalBridge final : public QObject
    {
    public:
        // 输入：宿主窗口与原有页面目录/导航回调；输出：尚未监听的桥实例。
        McpLocalBridge(QWidget* host, McpUiController::PageList list,
            McpUiController::PageOpen open);
        ~McpLocalBridge() override;

        // 监听同用户的当前 PID 管道；失败保留 lastError，不修改网络或全局配置。
        bool start();
        void close(); // 停止连接并使排队的 GUI 动作失效，支持重复调用。
        QString endpoint() const; // 返回当前实例的本地管道名。
        QString lastError() const; // 返回启动失败的诊断文本，仅供日志/诊断使用。
        // 只允许主程序内建代码登记特殊控件读取；不通过 IPC 接受类名或回调。
        void registerReadAdapter(const QByteArray& className, McpUiController::ReadAdapter adapter);

    private:
        void acceptConnection(QLocalSocket* socket); // 校验身份并绑定有界单请求会话。
        QPointer<QWidget> m_host; // 所有 UI 对象的宿主与生命周期边界。
        std::unique_ptr<McpUiController> m_controller; // GUI 线程中的固定控件方法适配器。
        QLocalServer* m_server = nullptr; // QObject 子对象，只有显式 start 后监听。
        QString m_endpoint; // PID 隔离的本地服务名称。
        QString m_lastError; // 启动错误的值快照，不保存令牌或用户输入。
        bool m_closed = false; // 宿主拆除后拒绝新请求与重启。
        int m_connections = 0; // 当前被本服务接纳的本地连接计数。
    };
}
