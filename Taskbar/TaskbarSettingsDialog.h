#pragma once

#include <QDialog>

class QCheckBox;
class QHideEvent;
class QShowEvent;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class TaskbarNotificationService;

// TaskbarSettingsDialog 提供 Taskbar 专属的通知开关、地震源诊断和测试预警入口。
// 所有屏幕共用一个非模态实例，避免多显示器分别修改出不一致的设置。
class TaskbarSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    // 构造函数：传入全局通知服务；建立设置控件并订阅其状态变化信号。
    explicit TaskbarSettingsDialog(TaskbarNotificationService* notificationService, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    // applyClipboardSetting：把复选框状态同步至全局通知服务。
    void applyClipboardSetting(bool enabled);

    // applyDeviceSetting：把复选框状态同步至全局通知服务。
    void applyDeviceSetting(bool enabled);

    // applyEarthquakeSetting：把复选框状态同步至全局通知服务。
    void applyEarthquakeSetting(bool enabled);

    // applyNotificationDuration：把消息滞留秒数同步至全局通知服务。
    void applyNotificationDuration(int seconds);

    // refreshFromService：读取全局设置，避免从其它显示器打开的窗口状态陈旧。
    void refreshFromService();

    // refreshSourceDiagnostics：刷新多源连接状态文字。
    void refreshSourceDiagnostics();
    void scheduleSourceDiagnostics();

    // restartTaskbar：启动等待当前 PID 的接替实例，并退出本进程释放 AppBar。
    void restartTaskbar();

private:
    // m_notificationService 是全局服务，不由本对话框拥有或销毁。
    TaskbarNotificationService* m_notificationService;

    // 三个复选框对应需要从 WindowsMarker 保留的通知功能。
    QCheckBox* m_clipboardCheckBox;
    QCheckBox* m_deviceCheckBox;
    QCheckBox* m_earthquakeCheckBox;
    // m_notificationDurationSpinBox 设置每条普通消息正文的完整滞留时间。
    QSpinBox* m_notificationDurationSpinBox;

    // m_sourceStatusLabel 展示 WebSocket 源连接情况，便于排查网络或服务端不可用问题。
    QLabel* m_sourceStatusLabel;

    // m_testEarthquakeButton 立即插播本地测试地震，用于验证红色警报态和队列暂停。
    QPushButton* m_testEarthquakeButton;

    // m_restartTaskbarButton 启动 PID 感知的接替实例，使旧进程先释放所有 AppBar 资源。
    QPushButton* m_restartTaskbarButton;

    // m_refreshTimer 在窗口显示期间周期刷新连接诊断，不修改业务状态。
    QTimer* m_refreshTimer;
    bool m_sourceDiagnosticsDirty = true;
    bool m_sourceDiagnosticsPending = false;
};
