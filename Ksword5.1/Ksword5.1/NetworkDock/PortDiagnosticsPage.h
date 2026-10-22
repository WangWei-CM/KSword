#pragma once

#include "../ksword/network/network_port_diagnostics.h"
#include <QWidget>

class QLabel;
class QPushButton;
class QSpinBox;
class QComboBox;
class QTableWidget;
class CodeEditorWidget;
namespace ks::ui { class AsyncOperation; }

// 网络主页面的端口诊断页；查询与写操作走独立的值化后台请求，不借用页面 QObject。
class PortDiagnosticsPage final : public QWidget
{
public:
    explicit PortDiagnosticsPage(QWidget* parent = nullptr);
    ~PortDiagnosticsPage() override;

private:
    // 根据输入查询；输入变更先清除旧结果，避免对别的端口执行旧快照动作。
    void diagnose();
    void invalidateSnapshot();
    // 用户确认后按已复制的目标执行结束进程或 TCP 断开，再展示后台回读结果。
    void release(ks::network::PortReleaseAction action);
    // 仅应用值快照；更新表格、绑定结论和错误报告，不枚举进程。
    void showSnapshot(const ks::network::PortDiagnosticSnapshot& snapshot);
    void updateSelection();
    void setBusy(bool busy);
    // 稳定记录索引保存在单元格 UserRole，排序不会改变它与后台记录的对应关系。
    int selectedRecord() const;

    ks::ui::AsyncOperation* m_operation = nullptr; // 页面析构先关闭异步投递门禁。
    ks::ui::AsyncOperation* m_iconOperation = nullptr; // 图标异步回补，不阻塞诊断结果与操作状态。
    QSpinBox* m_port = nullptr;                    // 只接受 1..65535 的本地端口。
    QComboBox* m_protocol = nullptr;               // TCP/UDP 查询范围。
    QPushButton* m_diagnose = nullptr;             // 主动查询入口。
    QPushButton* m_release = nullptr;              // 明确结束占用进程的入口。
    QPushButton* m_disconnect = nullptr;           // 只对可终止的 IPv4 活动 TCP 行启用。
    QTableWidget* m_owners = nullptr;              // 紧凑占用证据表，五列。
    QTableWidget* m_probes = nullptr;              // 最多四条独占绑定结果。
    QLabel* m_details = nullptr;                  // 所选进程路径、远端和身份说明。
    QLabel* m_status = nullptr;                   // 诊断与动作结果摘要。
    CodeEditorWidget* m_report = nullptr;          // 仅错误时出现的可复制诊断报告。
    ks::network::PortDiagnosticSnapshot m_snapshot; // 当前表格对应的完整值快照。
    bool m_hasSnapshot = false;                   // 输入变化或初次进入时禁止执行旧动作。
    bool m_busy = false;                          // 查询/确认/动作期间统一禁用相关控件。
};
