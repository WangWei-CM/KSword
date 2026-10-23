#include "TaskbarSettingsDialog.h"

#include "TaskbarNotificationService.h"
#include "TaskbarRestartCoordinator.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

TaskbarSettingsDialog::TaskbarSettingsDialog(TaskbarNotificationService* notificationService, QWidget* parent)
    : QDialog(parent)
    , m_notificationService(notificationService)
    , m_clipboardCheckBox(nullptr)
    , m_deviceCheckBox(nullptr)
    , m_earthquakeCheckBox(nullptr)
    , m_notificationDurationSpinBox(nullptr)
    , m_sourceStatusLabel(nullptr)
    , m_testEarthquakeButton(nullptr)
    , m_restartTaskbarButton(nullptr)
    , m_refreshTimer(nullptr)
{
    // 对话框为非模态工具窗口，集中服务所有屏幕的 Taskbar，而不单独保存每屏设置。
    setWindowTitle(QStringLiteral("Taskbar 通知设置"));
    setWindowFlags(windowFlags() | Qt::Tool);
    setModal(false);
    setMinimumWidth(430);
    setStyleSheet(R"(
        QDialog { background: #111820; color: #dff8ff; }
        QGroupBox { border: 1px solid #31566a; margin-top: 12px; padding: 10px; color: #91dfff; }
        QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 4px; }
        QCheckBox { spacing: 7px; padding: 3px; color: #dff8ff; }
        QCheckBox:disabled { color: #6f8790; }
        QLabel { color: #dff8ff; }
        QSpinBox { background: #0b1015; border: 1px solid #4fbed9; border-radius: 3px; padding: 3px 6px; color: #e9fbff; }
        QSpinBox:focus { border-color: #91dfff; }
        QPushButton { background: #153542; border: 1px solid #4fbed9; border-radius: 3px; padding: 6px 10px; color: #e9fbff; }
        QPushButton:hover { background: #1b4a5d; }
        QLabel#sourceStatus { color: #a8c9d2; background: #0b1015; border: 1px solid #263e49; padding: 8px; }
    )");

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(14, 14, 14, 14);
    rootLayout->setSpacing(10);

    QGroupBox* notificationGroup = new QGroupBox(QStringLiteral("通知"), this);
    QVBoxLayout* notificationLayout = new QVBoxLayout(notificationGroup);
    notificationLayout->setSpacing(5);
    m_clipboardCheckBox = new QCheckBox(QStringLiteral("剪贴板文字变化"), notificationGroup);
    m_deviceCheckBox = new QCheckBox(QStringLiteral("设备接入"), notificationGroup);
    m_earthquakeCheckBox = new QCheckBox(QStringLiteral("地震预警"), notificationGroup);
    notificationLayout->addWidget(m_clipboardCheckBox);
    notificationLayout->addWidget(m_deviceCheckBox);
    notificationLayout->addWidget(m_earthquakeCheckBox);

    // 消息滞留时间使用秒为单位，范围与服务层一致并在配置文件中持久化。
    QHBoxLayout* durationLayout = new QHBoxLayout();
    QLabel* durationLabel = new QLabel(QStringLiteral("消息滞留时间"), notificationGroup);
    m_notificationDurationSpinBox = new QSpinBox(notificationGroup);
    m_notificationDurationSpinBox->setRange(1, 60);
    m_notificationDurationSpinBox->setSuffix(QStringLiteral(" 秒"));
    m_notificationDurationSpinBox->setToolTip(QStringLiteral("设置每条普通消息正文完整显示的时间，范围为 1 到 60 秒。"));
    durationLayout->addWidget(durationLabel);
    durationLayout->addWidget(m_notificationDurationSpinBox);
    durationLayout->addStretch(1);
    notificationLayout->addLayout(durationLayout);
    rootLayout->addWidget(notificationGroup);

    QGroupBox* earthquakeGroup = new QGroupBox(QStringLiteral("地震预警诊断"), this);
    QVBoxLayout* earthquakeLayout = new QVBoxLayout(earthquakeGroup);
    m_sourceStatusLabel = new QLabel(earthquakeGroup);
    m_sourceStatusLabel->setObjectName(QStringLiteral("sourceStatus"));
    m_sourceStatusLabel->setWordWrap(true);
    m_sourceStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    earthquakeLayout->addWidget(m_sourceStatusLabel);

    QHBoxLayout* testLayout = new QHBoxLayout();
    testLayout->addStretch(1);
    m_testEarthquakeButton = new QPushButton(QStringLiteral("插播测试地震预警"), earthquakeGroup);
    m_testEarthquakeButton->setToolTip(QStringLiteral("立即显示十秒测试预警，用于验证红色闪烁主题和队列抢占。"));
    testLayout->addWidget(m_testEarthquakeButton);
    earthquakeLayout->addLayout(testLayout);
    rootLayout->addWidget(earthquakeGroup);

    QGroupBox* taskbarGroup = new QGroupBox(QStringLiteral("Taskbar"), this);
    QVBoxLayout* taskbarLayout = new QVBoxLayout(taskbarGroup);
    QLabel* restartDescriptionLabel = new QLabel(
        QStringLiteral("切换系统输出设备后，可重启 Taskbar 重新建立音频采集。"),
        taskbarGroup);
    restartDescriptionLabel->setWordWrap(true);
    taskbarLayout->addWidget(restartDescriptionLabel);

    QHBoxLayout* restartLayout = new QHBoxLayout();
    restartLayout->addStretch(1);
    m_restartTaskbarButton = new QPushButton(QStringLiteral("重启 Taskbar"), taskbarGroup);
    m_restartTaskbarButton->setIcon(QIcon(QStringLiteral(":/Icon/Resource/svg/system/refresh_1_line.svg")));
    m_restartTaskbarButton->setToolTip(
        QStringLiteral("退出当前 Taskbar，等待一秒释放 AppBar 资源后自动重新启动。"));
    restartLayout->addWidget(m_restartTaskbarButton);
    taskbarLayout->addLayout(restartLayout);
    rootLayout->addWidget(taskbarGroup);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
    rootLayout->addWidget(buttons);

    // 复选框直接驱动唯一全局服务，任意屏幕打开对话框改变的结果都会同步至全部窗口。
    connect(m_clipboardCheckBox, &QCheckBox::toggled, this, &TaskbarSettingsDialog::applyClipboardSetting);
    connect(m_deviceCheckBox, &QCheckBox::toggled, this, &TaskbarSettingsDialog::applyDeviceSetting);
    connect(m_earthquakeCheckBox, &QCheckBox::toggled, this, &TaskbarSettingsDialog::applyEarthquakeSetting);
    connect(m_notificationDurationSpinBox, qOverload<int>(&QSpinBox::valueChanged), this,
        &TaskbarSettingsDialog::applyNotificationDuration);
    connect(m_testEarthquakeButton, &QPushButton::clicked, this, [this]() {
        if (m_notificationService != nullptr)
        {
            m_notificationService->injectTestEarthquake();
        }
    });
    connect(m_restartTaskbarButton, &QPushButton::clicked, this, &TaskbarSettingsDialog::restartTaskbar);

    if (m_notificationService != nullptr)
    {
        connect(m_notificationService, &TaskbarNotificationService::settingsChanged, this,
            &TaskbarSettingsDialog::refreshFromService);
        connect(m_notificationService, &TaskbarNotificationService::sourceStatusesChanged, this,
            &TaskbarSettingsDialog::scheduleSourceDiagnostics);
    }

    // 对话框可见时每秒刷新一次连接状态，既有实时诊断也不会给 WebSocket 线程增加轮询负担。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, &TaskbarSettingsDialog::refreshSourceDiagnostics);
    refreshFromService();
}

void TaskbarSettingsDialog::showEvent(QShowEvent* event)
{
    refreshFromService();
    refreshSourceDiagnostics();
    m_refreshTimer->start();
    QDialog::showEvent(event);
}

void TaskbarSettingsDialog::hideEvent(QHideEvent* event)
{
    m_refreshTimer->stop();
    m_sourceDiagnosticsDirty = true;
    QDialog::hideEvent(event);
}

void TaskbarSettingsDialog::scheduleSourceDiagnostics()
{
    m_sourceDiagnosticsDirty = true;
    if (!isVisible() || m_sourceDiagnosticsPending)
    {
        return;
    }
    m_sourceDiagnosticsPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_sourceDiagnosticsPending = false;
        if (m_sourceDiagnosticsDirty)
        {
            refreshSourceDiagnostics();
        }
    });
}

void TaskbarSettingsDialog::applyClipboardSetting(bool enabled)
{
    // 复选框状态只写入全局服务，持久化细节由服务统一处理。
    if (m_notificationService != nullptr)
    {
        m_notificationService->setClipboardNotificationsEnabled(enabled);
    }
}

void TaskbarSettingsDialog::applyDeviceSetting(bool enabled)
{
    // 设备监听持续保留，但关闭开关后收到的事件不会进入展示队列。
    if (m_notificationService != nullptr)
    {
        m_notificationService->setDeviceNotificationsEnabled(enabled);
    }
}

void TaskbarSettingsDialog::applyEarthquakeSetting(bool enabled)
{
    // 关闭地震开关会立即退出红色警报展示，但接收客户端仍维持连接以便重新开启时快速恢复。
    if (m_notificationService != nullptr)
    {
        m_notificationService->setEarthquakeNotificationsEnabled(enabled);
    }
}

void TaskbarSettingsDialog::applyNotificationDuration(int seconds)
{
    // 仅把设置值交给全局服务，配置文件写入和当前轮播计时由服务统一处理。
    if (m_notificationService != nullptr)
    {
        m_notificationService->setNotificationDurationSeconds(seconds);
    }
}

void TaskbarSettingsDialog::refreshFromService()
{
    // 阻断信号以避免 setChecked 在刷新阶段又写回同一个 QSettings 值。
    if (m_notificationService == nullptr)
    {
        return;
    }
    const QSignalBlocker clipboardBlocker(m_clipboardCheckBox);
    const QSignalBlocker deviceBlocker(m_deviceCheckBox);
    const QSignalBlocker earthquakeBlocker(m_earthquakeCheckBox);
    const QSignalBlocker durationBlocker(m_notificationDurationSpinBox);
    m_clipboardCheckBox->setChecked(m_notificationService->clipboardNotificationsEnabled());
    m_deviceCheckBox->setChecked(m_notificationService->deviceNotificationsEnabled());
    m_earthquakeCheckBox->setChecked(m_notificationService->earthquakeNotificationsEnabled());
    m_notificationDurationSpinBox->setValue(m_notificationService->notificationDurationSeconds());
}

void TaskbarSettingsDialog::refreshSourceDiagnostics()
{
    // 隐藏时只保留脏状态；网络接收和通知业务继续运行。
    if (!isVisible())
    {
        m_sourceDiagnosticsDirty = true;
        return;
    }
    m_sourceDiagnosticsDirty = false;
    const auto setDiagnosticsText = [this](const QString& text) {
        if (m_sourceStatusLabel->text() != text)
        {
            m_sourceStatusLabel->setText(text);
        }
    };
    // 将所有来源的状态压缩成多行文本，连接成功后显示最近包时间和已测得的 RTT。
    if (m_notificationService == nullptr)
    {
        setDiagnosticsText(QStringLiteral("地震预警服务不可用。"));
        return;
    }

    const QList<TaskbarEarthquakeSourceStatus> statuses = m_notificationService->sourceStatuses();
    if (statuses.isEmpty())
    {
        setDiagnosticsText(QStringLiteral("正在初始化地震预警来源。"));
        return;
    }

    QStringList lines;
    for (const TaskbarEarthquakeSourceStatus& status : statuses)
    {
        QString state;
        if (!status.enabled)
        {
            state = QStringLiteral("未启用");
        }
        else if (!status.connected)
        {
            state = status.everConnected ? QStringLiteral("重连中") : QStringLiteral("未连接");
        }
        else if (status.lastMessageAgeMs == 0)
        {
            state = QStringLiteral("已连接，等待数据");
        }
        else
        {
            state = QStringLiteral("已连接，%1 秒前收到").arg(status.lastMessageAgeMs / 1000);
            if (status.latencyMs > 0)
            {
                state += QStringLiteral("，延迟 %1 ms").arg(status.latencyMs);
            }
        }
        lines.push_back(QStringLiteral("%1: %2").arg(status.name, state));
    }
    setDiagnosticsText(lines.join(QLatin1Char('\n')));
}

void TaskbarSettingsDialog::restartTaskbar()
{
    // 仅在接替实例已成功启动时退出；接替实例会等待本进程完全释放 AppBar。
    if (!TaskbarRestartCoordinator::scheduleAfterCurrentProcessExit())
    {
        return;
    }

    // 防止用户重复点击产生多个新实例，并用文字反馈已开始执行重启。
    m_restartTaskbarButton->setEnabled(false);
    m_restartTaskbarButton->setText(QStringLiteral("正在重启..."));
    QCoreApplication::quit();
}
