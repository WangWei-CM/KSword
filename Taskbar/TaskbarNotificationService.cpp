#include "TaskbarNotificationService.h"
#include "../Ksword5.1/Ksword5.1/UI/AsyncUiDispatcher.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <process.h>
#include <thread>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QSettings>
#include <QPointer>
#include <algorithm>
#include <limits>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dbt.h>

#include <shellapi.h>

namespace
{
    // 普通消息计时包含中央区域淡出和淡入各 500ms 的动画预算，正文仍按设置完整滞留。
    constexpr qint64 kNotificationTransitionBudgetMs = 1000;
    constexpr int kDefaultNotificationDurationSeconds = 5;
    constexpr int kMinimumNotificationDurationSeconds = 1;
    constexpr int kMaximumNotificationDurationSeconds = 60;
    constexpr qint64 kDeviceDeduplicationMs = 500;
    constexpr int kMaximumQueuedNotifications = 32;

    // 三个 GUID 沿用 WindowsMarker 的局部定义，避免引入 SDK 宏定义冲突。
    const GUID kUsbDeviceInterfaceGuid =
        { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };
    const GUID kDiskDeviceInterfaceGuid =
        { 0x53F56307, 0xB6BF, 0x11D0, { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };
    const GUID kVolumeDeviceInterfaceGuid =
        { 0x53F5630D, 0xB6BF, 0x11D0, { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };

    // monotonicMilliseconds 返回单调时钟，避免系统校时造成通知轮播突然跳过。
    qint64 monotonicMilliseconds()
    {
        return static_cast<qint64>(::GetTickCount64());
    }

    // truncateText 用 Unicode 字符长度限制剪贴板正文，避免超长内容压垮 32px 高度任务栏。
    QString truncateText(const QString& text, int maximumCharacters)
    {
        if (text.size() <= maximumCharacters)
        {
            return text;
        }
        return text.left(maximumCharacters) + QStringLiteral("...");
    }

    constexpr qsizetype kClipboardPreviewCharacters = 120;
    constexpr qsizetype kClipboardInputBudget = 8192;

    // Exactly QString::simplified()+left(120), with bounded input/output work.
    // The borrowed native buffer is used only while the clipboard is open/locked.
    void consumeClipboardPreview(TaskbarClipboardPreviewState& state,
        const wchar_t* source, qsizetype available, qsizetype budget)
    {
        if (state.done || source == nullptr || budget <= 0) return;
        state.text.reserve(kClipboardPreviewCharacters);
        qsizetype consumed = 0;
        while (state.offset < available && consumed < budget)
        {
            const QChar character(static_cast<ushort>(source[state.offset++]));
            ++consumed;
            if (character.isNull())
            {
                state.done = state.valid = true;
                return;
            }
            if (character.isSpace())
            {
                if (!state.text.isEmpty()) state.pendingSpace = true;
                continue;
            }
            if (state.pendingSpace)
            {
                if (state.text.size() == kClipboardPreviewCharacters)
                {
                    state.done = state.valid = state.truncated = true;
                    return;
                }
                state.text.append(QChar(u' '));
                state.pendingSpace = false;
            }
            if (state.text.size() == kClipboardPreviewCharacters)
            {
                state.done = state.valid = state.truncated = true;
                return;
            }
            state.text.append(character);
        }
        // A malformed buffer without NUL is never read beyond GlobalSize.
        if (state.offset >= available) state.done = true;
    }

    // deviceVolumeDescription 把 DBT_DEVTYP_VOLUME 位图转换为用户可读的盘符列表。
    QString deviceVolumeDescription(ULONG unitMask)
    {
        QStringList drives;
        for (int index = 0; index < 26; ++index)
        {
            if ((unitMask & (1u << index)) != 0)
            {
                drives.push_back(QStringLiteral("%1:").arg(QChar(u'A' + index)));
            }
        }
        return drives.isEmpty() ? QStringLiteral("卷") : QStringLiteral("卷 %1").arg(drives.join(QStringLiteral(", ")));
    }

    // deviceArrivalType 根据广播类型和接口类 GUID 为接入通知提供可靠的设备类型前缀。
    QString deviceArrivalType(LPARAM lParam)
    {
        if (lParam == 0)
        {
            return QStringLiteral("设备");
        }

        const DEV_BROADCAST_HDR* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
        if (header->dbch_devicetype == DBT_DEVTYP_VOLUME)
        {
            return QStringLiteral("存储卷");
        }
        if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
        {
            const DEV_BROADCAST_DEVICEINTERFACE_W* device =
                reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lParam);
            if (::IsEqualGUID(device->dbcc_classguid, kUsbDeviceInterfaceGuid))
            {
                return QStringLiteral("USB 设备");
            }
            if (::IsEqualGUID(device->dbcc_classguid, kDiskDeviceInterfaceGuid))
            {
                return QStringLiteral("磁盘");
            }
            if (::IsEqualGUID(device->dbcc_classguid, kVolumeDeviceInterfaceGuid))
            {
                return QStringLiteral("存储卷");
            }
        }

        return QStringLiteral("设备");
    }

    // describeDeviceArrival 将接入广播中的盘符或接口路径转成紧凑单行正文。
    QString describeDeviceArrival(LPARAM lParam)
    {
        if (lParam == 0)
        {
            return {};
        }

        const DEV_BROADCAST_HDR* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
        if (header->dbch_devicetype == DBT_DEVTYP_VOLUME)
        {
            const DEV_BROADCAST_VOLUME* volume = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lParam);
            return deviceVolumeDescription(volume->dbcv_unitmask);
        }
        if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
        {
            const DEV_BROADCAST_DEVICEINTERFACE_W* device =
                reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lParam);
            return truncateText(QString::fromWCharArray(device->dbcc_name), 96);
        }

        return {};
    }
}


// This receiver owns only values and its native window. Service destruction closes
// the dispatcher and cooperatively cancels requests without waiting for an external
// delayed-render provider. At most one receiver and 32 queued values exist per service.
struct TaskbarClipboardReceiverState
{
    struct Request
    {
        quint64 ticket = 0;
        quint32 sequence = 0;
        std::shared_ptr<std::atomic_bool> canceled;
        std::function<void(const QString&)> complete;
    };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Request> pending;
    bool stopping = false;
};

namespace
{
    QString readClipboardPreview(HWND window, const TaskbarClipboardReceiverState::Request& request)
    {
        TaskbarClipboardPreviewState preview;
        while (!preview.done && !request.canceled->load(std::memory_order_acquire))
        {
            bool available = false;
            if (window != nullptr && ::GetClipboardSequenceNumber() == request.sequence && ::OpenClipboard(window) != FALSE)
            {
                struct Close { ~Close() { ::CloseClipboard(); } } close;
                // GetClipboardData can synchronously wait for another application's
                // WM_RENDERFORMAT. It runs only on this receiver's native thread.
                const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
                if (!request.canceled->load(std::memory_order_acquire)
                    && ::GetClipboardSequenceNumber() == request.sequence)
                {
                    const SIZE_T bytes = data != nullptr ? ::GlobalSize(data) : 0;
                    const wchar_t* source = data != nullptr ? static_cast<const wchar_t*>(::GlobalLock(data)) : nullptr;
                    if (source != nullptr)
                    {
                        struct Unlock { HANDLE data; ~Unlock() { ::GlobalUnlock(data); } } unlock {data};
                        const qsizetype characters = static_cast<qsizetype>(std::min<SIZE_T>(
                            bytes / sizeof(wchar_t), static_cast<SIZE_T>((std::numeric_limits<qsizetype>::max)())));
                        consumeClipboardPreview(preview,source,characters,kClipboardInputBudget);
                        available = true;
                    }
                }
            }
            if (!available) return {};
            // No HGLOBAL or borrowed pointer survives this turn. Release the clipboard
            // between chunks so long leading whitespace cannot monopolize ownership.
            std::this_thread::yield();
        }
        if (request.canceled->load(std::memory_order_acquire) || !preview.valid) return {};
        return preview.text + (preview.truncated ? QStringLiteral("...") : QString());
    }

    unsigned __stdcall runClipboardReceiver(void* parameter)
    {
        std::unique_ptr<std::shared_ptr<TaskbarClipboardReceiverState>> argument(
            static_cast<std::shared_ptr<TaskbarClipboardReceiverState>*>(parameter));
        const auto state = *argument;
        argument.reset();
        // A native message-only owner avoids constructing or accessing Qt widgets
        // from the receiver. Its creation/destruction and all clipboard APIs share a thread.
        const HWND window = ::CreateWindowExW(0,L"STATIC",nullptr,0,0,0,0,0,
            HWND_MESSAGE,nullptr,::GetModuleHandleW(nullptr),nullptr);
        struct Destroy { HWND window; ~Destroy() { if (window != nullptr) ::DestroyWindow(window); } } destroy {window};
        for (;;)
        {
            TaskbarClipboardReceiverState::Request request;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->ready.wait(lock,[&]() { return state->stopping || !state->pending.empty(); });
                if (state->stopping) return 0;
                request = std::move(state->pending.front()); state->pending.pop_front();
            }
            if (request.canceled->load(std::memory_order_acquire)) continue;
            QString text;
            try { text = readClipboardPreview(window,request); }
            catch (...) { text.clear(); }
            if (!request.canceled->load(std::memory_order_acquire)) request.complete(text);
        }
    }
}

TaskbarNotificationService::TaskbarNotificationService(TaskbarEarthquakeClient* earthquakeClient, QObject* parent)
    : QObject(parent)
    , m_earthquakeClient(earthquakeClient)
{
    m_clipboardDispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(this);
    // 读取一次持久化开关；这些设置是进程级，因此所有屏幕窗口天然共享同一份配置。
    loadSettings();
    if (m_earthquakeClient != nullptr)
    {
        m_earthquakeClient->setAlertAudioEnabled(m_earthquakeNotificationsEnabled);
    }

    // 创建一个不可见原生窗口作为唯一广播接收点，防止每个屏幕 Taskbar 重复排队同一事件。
    m_messageWindow = new QWidget();
    m_messageWindow->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    m_messageWindow->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_messageWindow->setAttribute(Qt::WA_NativeWindow, true);
    m_messageWindow->setGeometry(-10000, -10000, 1, 1);
    const HWND messageWindowHandle = reinterpret_cast<HWND>(m_messageWindow->winId());
    if (messageWindowHandle != nullptr)
    {
        ::AddClipboardFormatListener(messageWindowHandle);
        registerDeviceNotifications();
    }
    qApp->installNativeEventFilter(this);

    // 高优先级地震状态变化直接抢占普通队列；来源状态仅更新设置窗口诊断。
    if (m_earthquakeClient != nullptr)
    {
        connect(m_earthquakeClient, &TaskbarEarthquakeClient::activeWarningsChanged, this,
            &TaskbarNotificationService::refreshEarthquakePresentation);
        connect(m_earthquakeClient, &TaskbarEarthquakeClient::sourceStatusesChanged, this,
            &TaskbarNotificationService::sourceStatusesChanged);
    }

    // 100ms 周期兼顾测试预警到期、网络事件超时和两秒通知轮播，无需后台计时线程。
    m_tickTimer.setInterval(100);
    connect(&m_tickTimer, &QTimer::timeout, this, &TaskbarNotificationService::advancePresentation);
    m_tickTimer.start();
    refreshEarthquakePresentation();
}

TaskbarNotificationService::~TaskbarNotificationService()
{
    m_clipboardDispatcher->close();
    for (const auto& job : m_clipboardJobs) job.canceled->store(true,std::memory_order_release);
    if (m_clipboardReceiver != nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(m_clipboardReceiver->mutex);
            m_clipboardReceiver->stopping = true;
            m_clipboardReceiver->pending.clear();
        }
        m_clipboardReceiver->ready.notify_one();
    }
    // The value-owned receiver releases its native window after an outstanding OS
    // provider returns. It cannot access this QObject, and GUI teardown never joins it.
    // 销毁时先移除原生过滤器，随后注销通知句柄，避免退出过程中回调到已析构服务。
    qApp->removeNativeEventFilter(this);
    unregisterDeviceNotifications();
    if (m_messageWindow != nullptr)
    {
        const HWND messageWindowHandle = reinterpret_cast<HWND>(m_messageWindow->winId());
        if (messageWindowHandle != nullptr)
        {
            ::RemoveClipboardFormatListener(messageWindowHandle);
        }
        delete m_messageWindow;
        m_messageWindow = nullptr;
    }
}

TaskbarNotificationView TaskbarNotificationService::currentNotification() const
{
    // 返回值是轻量 QString 值对象副本，调用者无需同步服务内部队列。
    return m_currentNotification;
}

bool TaskbarNotificationService::hasVisibleNotification() const
{
    // 只要地震或普通通知有标题，Taskbar 中央区域就应显示通知而非时间和频谱。
    return m_earthquakeActive || !m_currentNotification.title.isEmpty();
}

bool TaskbarNotificationService::earthquakeActive() const
{
    // 地震状态独立于当前正文，确保多个来源刷新时背景主题保持稳定。
    return m_earthquakeActive;
}

bool TaskbarNotificationService::clipboardNotificationsEnabled() const
{
    return m_clipboardNotificationsEnabled;
}

bool TaskbarNotificationService::deviceNotificationsEnabled() const
{
    return m_deviceNotificationsEnabled;
}

bool TaskbarNotificationService::earthquakeNotificationsEnabled() const
{
    return m_earthquakeNotificationsEnabled;
}

int TaskbarNotificationService::notificationDurationSeconds() const
{
    // 返回值是设置页使用的普通消息正文滞留秒数，不包含中央区域过渡动画。
    return m_notificationDurationSeconds;
}

void TaskbarNotificationService::setClipboardNotificationsEnabled(bool enabled)
{
    // 仅状态真正变化时写盘和发信号，避免设置窗口刷新导致无意义循环。
    if (m_clipboardNotificationsEnabled == enabled)
    {
        return;
    }
    m_clipboardNotificationsEnabled = enabled;
    if (!enabled)
    {
        for (const auto& job : m_clipboardJobs) job.canceled->store(true,std::memory_order_release);
        m_clipboardJobs.clear();
        m_queue.removeIf([](const QueuedNotification& entry) { return entry.clipboardTicket != 0; });
        if (!m_earthquakeActive && m_currentNotification.title.isEmpty())
            QTimer::singleShot(0, this, &TaskbarNotificationService::updateCurrentNormalNotification);
    }
    saveSettings();
    emit settingsChanged();
}

void TaskbarNotificationService::setDeviceNotificationsEnabled(bool enabled)
{
    if (m_deviceNotificationsEnabled == enabled)
    {
        return;
    }
    m_deviceNotificationsEnabled = enabled;
    saveSettings();
    emit settingsChanged();
}

void TaskbarNotificationService::setEarthquakeNotificationsEnabled(bool enabled)
{
    if (m_earthquakeNotificationsEnabled == enabled)
    {
        return;
    }
    m_earthquakeNotificationsEnabled = enabled;
    if (m_earthquakeClient != nullptr)
    {
        m_earthquakeClient->setAlertAudioEnabled(enabled);
    }
    saveSettings();
    refreshEarthquakePresentation();
    emit settingsChanged();
}

void TaskbarNotificationService::setNotificationDurationSeconds(int seconds)
{
    // 输入秒数先限制在设置页约定范围内，避免异常配置让消息永久占据任务栏。
    const int boundedSeconds = qBound(kMinimumNotificationDurationSeconds,
        seconds, kMaximumNotificationDurationSeconds);
    if (m_notificationDurationSeconds == boundedSeconds)
    {
        return;
    }

    m_notificationDurationSeconds = boundedSeconds;
    m_currentNormalDurationMs = normalNotificationDurationMilliseconds();
    saveSettings();
    emit settingsChanged();
}

void TaskbarNotificationService::injectTestEarthquake()
{
    // 测试按钮仍经由唯一地震客户端，因此所有屏幕同时进入完全相同的警报状态。
    if (m_earthquakeClient != nullptr)
    {
        m_earthquakeClient->injectTestWarning();
    }
}

QList<TaskbarEarthquakeSourceStatus> TaskbarNotificationService::sourceStatuses() const
{
    // 无客户端时返回空列表，设置窗口据此显示连接功能不可用而不是伪造状态。
    return m_earthquakeClient != nullptr ? m_earthquakeClient->sourceStatuses()
        : QList<TaskbarEarthquakeSourceStatus>();
}

bool TaskbarNotificationService::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    // Qt Windows 后端以 windows_generic_MSG 交付 MSG；其它平台或事件类型直接放行。
    if (eventType != QByteArrayLiteral("windows_generic_MSG") && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))
    {
        return false;
    }
    return handleNativeMessage(message, result);
}

bool TaskbarNotificationService::handleNativeMessage(void* message, qintptr* result)
{
    MSG* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage == nullptr || m_messageWindow == nullptr)
    {
        return false;
    }

    const HWND messageWindowHandle = reinterpret_cast<HWND>(m_messageWindow->winId());
    if (nativeMessage->hwnd != messageWindowHandle)
    {
        return false;
    }

    if (nativeMessage->message == WM_CLIPBOARDUPDATE && m_clipboardNotificationsEnabled)
    {
        enqueueClipboardText();
    }
    else if (nativeMessage->message == WM_DEVICECHANGE)
    {
        handleDeviceChange(nativeMessage->wParam, nativeMessage->lParam);
    }

    if (result != nullptr)
    {
        *result = 0;
    }
    return false;
}

void TaskbarNotificationService::enqueueClipboardText()
{
    const quint32 sequence = ::GetClipboardSequenceNumber();
    if (sequence == 0) return;
    ClipboardJob job;
    job.ticket = ++m_nextClipboardTicket;
    job.sequence = sequence;
    m_clipboardJobs.push_back(job);
    TaskbarNotificationView notification;
    notification.kind = TaskbarNotificationKind::Clipboard;
    notification.source = QStringLiteral("剪贴板");
    notification.title = QStringLiteral("剪贴板新内容");
    const QPointer<TaskbarNotificationService> guard(this);
    enqueueNotification(notification, job.ticket);
    if (guard == nullptr) return;
    processClipboardPreviewChunk();
}

void TaskbarNotificationService::processClipboardPreviewChunk()
{
    if (m_clipboardJobs.isEmpty()) return;
    if (m_clipboardReceiver == nullptr)
    {
        auto state = std::make_shared<TaskbarClipboardReceiverState>();
        auto argument = std::make_unique<std::shared_ptr<TaskbarClipboardReceiverState>>(state);
        const uintptr_t handle = ::_beginthreadex(nullptr,0,&runClipboardReceiver,argument.get(),0,nullptr);
        if (handle == 0)
        {
            // No worker accepted these replaceable previews. Release their FIFO tickets.
            const auto jobs = m_clipboardJobs;
            const QPointer<TaskbarNotificationService> guard(this);
            for (const auto& job : jobs) { finishClipboardPreview(job.ticket,{}); if (guard == nullptr) return; }
            return;
        }
        argument.release();
        ::CloseHandle(reinterpret_cast<HANDLE>(handle));
        m_clipboardReceiver = std::move(state);
    }
    const QPointer<TaskbarNotificationService> guard(this);
    const auto dispatcher = m_clipboardDispatcher;
    {
        std::lock_guard<std::mutex> lock(m_clipboardReceiver->mutex);
        auto& queue = m_clipboardReceiver->pending;
        queue.erase(std::remove_if(queue.begin(),queue.end(),[](const auto& request) {
            return request.canceled->load(std::memory_order_acquire);
        }),queue.end());
        for (auto& job : m_clipboardJobs)
        {
            if (job.submitted) continue;
            TaskbarClipboardReceiverState::Request request;
            request.ticket=job.ticket; request.sequence=job.sequence; request.canceled=job.canceled;
            request.complete=[guard,dispatcher,ticket=job.ticket](const QString& text) {
                dispatcher->post([guard,ticket,text]() { if (guard) guard->finishClipboardPreview(ticket,text); });
            };
            queue.push_back(std::move(request)); job.submitted=true;
        }
    }
    m_clipboardReceiver->ready.notify_one();
}

void TaskbarNotificationService::finishClipboardPreview(quint64 ticket, const QString& text)
{
    m_clipboardJobs.removeIf([ticket](const ClipboardJob& job) { return job.ticket == ticket; });
    auto iterator = std::find_if(m_queue.begin(), m_queue.end(),
        [ticket](const QueuedNotification& entry) { return entry.clipboardTicket == ticket; });
    if (iterator != m_queue.end())
    {
        if (text.isEmpty()) m_queue.erase(iterator);
        else
        {
            iterator->notification.body = text;
            iterator->clipboardTicket = 0;
        }
    }
    if (!m_earthquakeActive && m_currentNotification.title.isEmpty()) updateCurrentNormalNotification();
}

void TaskbarNotificationService::handleDeviceChange(quintptr wParam, qintptr lParam)
{
    // 拓扑变化通常没有设备详情；仅接入广播生成一条通知。
    if (!m_deviceNotificationsEnabled || wParam != DBT_DEVICEARRIVAL)
    {
        return;
    }

    const QString title = deviceArrivalType(static_cast<LPARAM>(lParam)) + QStringLiteral("接入");
    const QString body = describeDeviceArrival(static_cast<LPARAM>(lParam));
    const QString key = title + QStringLiteral("|") + body;
    const qint64 now = monotonicMilliseconds();
    if (key == m_lastDeviceNotificationKey && now >= m_lastDeviceNotificationMs &&
        now - m_lastDeviceNotificationMs < kDeviceDeduplicationMs)
    {
        return;
    }

    m_lastDeviceNotificationKey = key;
    m_lastDeviceNotificationMs = now;
    TaskbarNotificationView notification;
    notification.kind = TaskbarNotificationKind::Device;
    notification.source = QStringLiteral("设备");
    notification.title = title;
    notification.body = body;
    enqueueNotification(notification);
}

void TaskbarNotificationService::enqueueNotification(const TaskbarNotificationView& notification, quint64 pendingClipboard)
{
    // 地震预警出现时普通队列仍可继续累积，但由高优先级状态统一暂停展示。
    if (m_queue.size() >= kMaximumQueuedNotifications)
    {
        const quint64 retiredTicket = m_queue.takeFirst().clipboardTicket;
        if (retiredTicket != 0) m_clipboardJobs.removeIf(
            [retiredTicket](const ClipboardJob& job) {
                if (job.ticket != retiredTicket) return false;
                job.canceled->store(true,std::memory_order_release); return true;
            });
    }
    m_queue.push_back({notification, pendingClipboard});
    if (!m_earthquakeActive && !m_currentNotification.title.isEmpty())
    {
        // 新消息进入队列时，当前消息继续使用用户设置的完整滞留时间。
        m_currentNormalDurationMs = normalNotificationDurationMilliseconds();
    }
    if (!m_earthquakeActive && m_currentNotification.title.isEmpty())
    {
        updateCurrentNormalNotification();
    }
}

void TaskbarNotificationService::updateCurrentNormalNotification()
{
    // 取 FIFO 首项并设置含动画预算的时长，正文完整滞留秒数由设置页统一控制。
    if (m_queue.isEmpty() || m_queue.first().clipboardTicket != 0)
    {
        const bool changed = !m_currentNotification.title.isEmpty();
        m_currentNotification = TaskbarNotificationView();
        m_currentNormalStartedMs = 0;
        if (changed)
        {
            emit presentationChanged();
        }
        return;
    }

    m_currentNotification = m_queue.takeFirst().notification;
    m_currentNormalStartedMs = monotonicMilliseconds();
    m_currentNormalDurationMs = normalNotificationDurationMilliseconds();
    emit presentationChanged();
}

qint64 TaskbarNotificationService::normalNotificationDurationMilliseconds() const
{
    // 返回值把正文滞留秒数和两段中央区域动画预算相加，保持设置含义稳定。
    return static_cast<qint64>(m_notificationDurationSeconds) * 1000 + kNotificationTransitionBudgetMs;
}

void TaskbarNotificationService::advancePresentation()
{
    // 每个 tick 先检查地震活动是否变化；其为真时刻意不推进普通队列计时。
    refreshEarthquakePresentation();
    if (m_earthquakeActive || m_currentNotification.title.isEmpty() || m_currentNormalStartedMs == 0)
    {
        return;
    }

    const qint64 now = monotonicMilliseconds();
    if (now >= m_currentNormalStartedMs && now - m_currentNormalStartedMs >= m_currentNormalDurationMs)
    {
        updateCurrentNormalNotification();
    }
}

void TaskbarNotificationService::refreshEarthquakePresentation()
{
    // 真实或测试预警只要存在就立即接管，取消/最终报/超时结束后再恢复普通队列。
    const QList<TaskbarEarthquakeEvent> warnings = m_earthquakeClient != nullptr && m_earthquakeNotificationsEnabled
        ? m_earthquakeClient->activeWarnings() : QList<TaskbarEarthquakeEvent>();
    const bool nextEarthquakeActive = !warnings.isEmpty();
    if (nextEarthquakeActive)
    {
        const TaskbarEarthquakeEvent& event = warnings.first();
        TaskbarNotificationView notification;
        notification.kind = TaskbarNotificationKind::Earthquake;
        notification.source = QStringLiteral("地震预警");
        notification.title = QStringLiteral("地震预警 %1").arg(event.hypoCenter.isEmpty() ? QStringLiteral("震源未知") : event.hypoCenter);
        QStringList details;
        if (!event.magnitudeText.isEmpty())
        {
            details.push_back(QStringLiteral("震级 %1").arg(event.magnitudeText));
        }
        if (!event.depthText.isEmpty())
        {
            details.push_back(QStringLiteral("深度 %1 公里").arg(event.depthText));
        }
        if (event.reportNum > 0)
        {
            details.push_back(QStringLiteral("第 %1 报").arg(event.reportNum));
        }
        notification.body = details.isEmpty() ? QStringLiteral("正在接收预警数据") : details.join(QStringLiteral("  |  "));
        notification.earthquake = true;

        const bool contentChanged = !m_earthquakeActive || m_currentNotification.title != notification.title ||
            m_currentNotification.body != notification.body;
        m_earthquakeActive = true;
        m_currentNotification = notification;
        if (contentChanged)
        {
            emit presentationChanged();
        }
        return;
    }

    if (m_earthquakeActive)
    {
        // 离开地震状态时立即发布下一状态；每个 Taskbar UI 负责 0.5 秒淡出警报并再淡入下一页面。
        m_earthquakeActive = false;
        m_currentNotification = TaskbarNotificationView();
        m_currentNormalStartedMs = 0;
        if (!m_queue.isEmpty() && m_queue.first().clipboardTicket == 0)
        {
            m_currentNotification = m_queue.takeFirst().notification;
            m_currentNormalStartedMs = monotonicMilliseconds();
            m_currentNormalDurationMs = normalNotificationDurationMilliseconds();
        }
        emit presentationChanged();
    }
}

void TaskbarNotificationService::loadSettings()
{
    // Taskbar 独立使用 IniFormat，避免主程序全局配置或注册表路径影响这个轻量常驻模块。
    QSettings settings(QCoreApplication::applicationDirPath() + QStringLiteral("/TaskbarNotifications.ini"),
        QSettings::IniFormat);
    m_clipboardNotificationsEnabled = settings.value(QStringLiteral("notifications/clipboardEnabled"), true).toBool();
    m_deviceNotificationsEnabled = settings.value(QStringLiteral("notifications/deviceEnabled"), true).toBool();
    m_earthquakeNotificationsEnabled = settings.value(QStringLiteral("notifications/earthquakeEnabled"), true).toBool();
    m_notificationDurationSeconds = qBound(kMinimumNotificationDurationSeconds,
        settings.value(QStringLiteral("notifications/durationSeconds"), kDefaultNotificationDurationSeconds).toInt(),
        kMaximumNotificationDurationSeconds);
    m_currentNormalDurationMs = normalNotificationDurationMilliseconds();
}

void TaskbarNotificationService::saveSettings() const
{
    // 仅写 TaskbarNotifications.ini 的三个开关和一个时长设置，不碰 WindowsMarker 或主程序配置文件。
    QSettings settings(QCoreApplication::applicationDirPath() + QStringLiteral("/TaskbarNotifications.ini"),
        QSettings::IniFormat);
    settings.setValue(QStringLiteral("notifications/clipboardEnabled"), m_clipboardNotificationsEnabled);
    settings.setValue(QStringLiteral("notifications/deviceEnabled"), m_deviceNotificationsEnabled);
    settings.setValue(QStringLiteral("notifications/earthquakeEnabled"), m_earthquakeNotificationsEnabled);
    settings.setValue(QStringLiteral("notifications/durationSeconds"), m_notificationDurationSeconds);
    settings.sync();
}

void TaskbarNotificationService::registerDeviceNotifications()
{
    // 对唯一隐藏窗口注册三类接口及卷广播；Taskbar 每屏窗口本身不直接订阅设备消息。
    const HWND messageWindowHandle = m_messageWindow != nullptr ? reinterpret_cast<HWND>(m_messageWindow->winId()) : nullptr;
    if (messageWindowHandle == nullptr)
    {
        return;
    }

    const GUID* interfaceClasses[] = {
        &kUsbDeviceInterfaceGuid,
        &kDiskDeviceInterfaceGuid,
        &kVolumeDeviceInterfaceGuid
    };
    for (const GUID* interfaceClass : interfaceClasses)
    {
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = *interfaceClass;
        HDEVNOTIFY handle = ::RegisterDeviceNotificationW(messageWindowHandle, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (handle != nullptr)
        {
            m_deviceNotificationHandles.push_back(handle);
        }
    }

    DEV_BROADCAST_VOLUME volumeFilter = {};
    volumeFilter.dbcv_size = sizeof(volumeFilter);
    volumeFilter.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    HDEVNOTIFY volumeHandle = ::RegisterDeviceNotificationW(messageWindowHandle, &volumeFilter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (volumeHandle != nullptr)
    {
        m_deviceNotificationHandles.push_back(volumeHandle);
    }
}

void TaskbarNotificationService::unregisterDeviceNotifications()
{
    // 退出前按注册顺序逐个注销，失败仅代表 Windows 已先行释放了关联设备状态。
    for (void* rawHandle : m_deviceNotificationHandles)
    {
        if (rawHandle != nullptr)
        {
            ::UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(rawHandle));
        }
    }
    m_deviceNotificationHandles.clear();
}
