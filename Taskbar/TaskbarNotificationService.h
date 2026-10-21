#pragma once

#include "TaskbarEarthquakeClient.h"

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>

class QWidget;

// Only the required UTF-16 preview and scan position survive between turns.
struct TaskbarClipboardPreviewState
{
    QString text;
    qsizetype offset = 0;
    bool pendingSpace = false;
    bool truncated = false;
    bool done = false;
    bool valid = false;
};

// TaskbarNotificationKind 区分三类普通通知，以便由设置页独立启停。
enum class TaskbarNotificationKind
{
    Clipboard, // 系统剪贴板文字变化。
    Device,    // USB、磁盘或卷的接入。
    Earthquake // 多源实时地震预警。
};

// TaskbarNotificationView 是所有屏幕共享的当前展示通知快照。
struct TaskbarNotificationView
{
    TaskbarNotificationKind kind = TaskbarNotificationKind::Clipboard; // 通知来源类型。
    QString source;            // 紧凑来源文本。
    QString title;             // 通知标题。
    QString body;              // 通知正文。
    bool earthquake = false;   // true 时 Taskbar 立即进入红色警报主题。
};

// TaskbarNotificationService 统一监听系统消息、维护普通通知 FIFO 队列并控制地震最高优先级。
// 它只创建一个隐藏消息窗口，因此多个 Taskbar 屏幕窗口始终显示相同通知内容。
class TaskbarNotificationService : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    // 构造函数：接收全局地震客户端；安装剪贴板/设备监听并启动通知调度时钟。
    explicit TaskbarNotificationService(TaskbarEarthquakeClient* earthquakeClient, QObject* parent = nullptr);

    // 析构函数：注销 Win32 监听、销毁隐藏窗口并停止音频播放器。
    ~TaskbarNotificationService() override;

    // currentNotification：无输入；返回全局当前通知，空标题代表无通知。
    TaskbarNotificationView currentNotification() const;

    // hasVisibleNotification：无输入；返回当前是否应以通知取代中央频谱和时钟。
    bool hasVisibleNotification() const;

    // earthquakeActive：无输入；返回是否至少有一条活动真实或测试地震预警。
    bool earthquakeActive() const;

    // clipboardNotificationsEnabled：无输入；返回剪贴板文字通知开关。
    bool clipboardNotificationsEnabled() const;

    // deviceNotificationsEnabled：无输入；返回设备接入通知开关。
    bool deviceNotificationsEnabled() const;

    // earthquakeNotificationsEnabled：无输入；返回地震预警开关。
    bool earthquakeNotificationsEnabled() const;

    // notificationDurationSeconds：无输入；返回普通消息完整滞留秒数。
    int notificationDurationSeconds() const;

    // setClipboardNotificationsEnabled：设置剪贴板文字变化通知开关并持久化。
    void setClipboardNotificationsEnabled(bool enabled);

    // setDeviceNotificationsEnabled：设置设备接入通知开关并持久化。
    void setDeviceNotificationsEnabled(bool enabled);

    // setEarthquakeNotificationsEnabled：设置地震预警开关并持久化。
    void setEarthquakeNotificationsEnabled(bool enabled);

    // setNotificationDurationSeconds：设置普通消息完整滞留秒数并持久化，范围为 1 到 60 秒。
    void setNotificationDurationSeconds(int seconds);

    // injectTestEarthquake：无输入；请求地震客户端插播一个本地测试预警。
    void injectTestEarthquake();

    // sourceStatuses：无输入；返回地震客户端的最新连接状态，供设置窗口展示。
    QList<TaskbarEarthquakeSourceStatus> sourceStatuses() const;

    // nativeEventFilter：接收专属隐藏窗口的 Win32 剪贴板与设备消息。
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    // presentationChanged：当前通知或地震警报态变化；所有 Taskbar 窗口据此同步中央区域。
    void presentationChanged();

    // settingsChanged：任何持久化通知设置变化后发出，供设置窗口刷新复选框。
    void settingsChanged();

    // sourceStatusesChanged：地震来源连接状态变化后发出，供设置窗口刷新诊断文本。
    void sourceStatusesChanged();

private slots:
    // advancePresentation：由短周期时钟驱动，负责普通队列轮播和地震状态刷新。
    void advancePresentation();

    // refreshEarthquakePresentation：地震活动状态变化时立即接管或释放普通通知队列。
    void refreshEarthquakePresentation();

private:
    // handleNativeMessage：处理隐藏窗口收到的 WM_CLIPBOARDUPDATE 与 WM_DEVICECHANGE。
    bool handleNativeMessage(void* message, qintptr* result);

    // enqueueClipboardText：在 GUI 线程读取并规范化剪贴板 Unicode 文本，然后加入普通队列。
    void enqueueClipboardText();
    void processClipboardPreviewChunk();
    void finishClipboardPreview(quint64 ticket, const QString& text);

    // handleDeviceChange：把符合条件且未重复的设备消息加入普通队列。
    void handleDeviceChange(quintptr wParam, qintptr lParam);

    // enqueueNotification：把一条普通消息追加到 FIFO 队列，队列过长时丢弃最旧项。
    void enqueueNotification(const TaskbarNotificationView& notification, quint64 pendingClipboard = 0);

    // updateCurrentNormalNotification：切换至下一条普通通知；不会影响地震优先级。
    void updateCurrentNormalNotification();

    // normalNotificationDurationMilliseconds：把设置的滞留秒数转换为包含淡入淡出预算的计时长度。
    qint64 normalNotificationDurationMilliseconds() const;

    // loadSettings：从 Taskbar 专属 QSettings 读取三类通知开关。
    void loadSettings();

    // saveSettings：把三类通知开关写入 Taskbar 专属 QSettings。
    void saveSettings() const;

    // registerDeviceNotifications：对隐藏窗口注册 USB、磁盘、卷的设备消息。
    void registerDeviceNotifications();

    // unregisterDeviceNotifications：注销此前注册的所有设备消息句柄。
    void unregisterDeviceNotifications();

    TaskbarEarthquakeClient* m_earthquakeClient; // 进程唯一地震接收器，不由服务拥有生命周期。
    QTimer m_tickTimer;                           // 100ms 轮播和地震状态刷新时钟。
    TaskbarNotificationView m_currentNotification; // 所有屏幕共享的当前展示内容。
    struct QueuedNotification { TaskbarNotificationView notification; quint64 clipboardTicket = 0; };
    struct ClipboardJob { quint64 ticket = 0; quint32 sequence = 0; TaskbarClipboardPreviewState state; };
    QList<QueuedNotification> m_queue;       // 等待展示的剪贴板和设备接入消息。
    QList<ClipboardJob> m_clipboardJobs;
    QTimer m_clipboardTimer;
    quint64 m_nextClipboardTicket = 0;
    qint64 m_currentNormalStartedMs = 0;          // 当前普通通知的单调时钟开始值。
    qint64 m_currentNormalDurationMs = 6000;      // 当前消息时长，包含 1 秒的中央区域过渡预算。
    int m_notificationDurationSeconds = 5;        // 普通消息正文完整滞留秒数，设置页可调整。
    bool m_earthquakeActive = false;              // 地震优先模式，true 时普通队列计时暂停。
    bool m_clipboardNotificationsEnabled = true;  // 剪贴板文字通知开关。
    bool m_deviceNotificationsEnabled = true;     // 设备热插拔通知开关。
    bool m_earthquakeNotificationsEnabled = true; // 地震预警显示和音频开关。
    QWidget* m_messageWindow = nullptr;           // 唯一隐藏原生窗口，承担剪贴板/设备广播监听。
    QList<void*> m_deviceNotificationHandles;     // RegisterDeviceNotificationW 返回的句柄。
    QString m_lastDeviceNotificationKey;          // 设备消息 500ms 去重键。
    qint64 m_lastDeviceNotificationMs = 0;        // 上次设备消息的单调时钟。
};
