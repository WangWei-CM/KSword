#include "TaskbarSharedState.h"

#include <QDebug>
#include <QMetaType>
#include <algorithm>
#include <chrono>

TaskbarSharedState::TaskbarSharedState(QObject* parent)
    : QObject(parent),
      m_analyzer(std::make_unique<AudioSpectrumAnalyzer>()),
      m_started(false),
      m_audioCaptureStarted(false),
      m_cpuWorkerRunning(false),
      m_networkWorkerRunning(false),
      m_uploadBytesPerSecond(0),
      m_downloadBytesPerSecond(0)
{
    // 频谱数据跨线程排队投递时需要注册元类型，避免音频采样线程直连 UI。
    qRegisterMetaType<QVector<float>>("QVector<float>");

    // 音频分析器只创建一次，多个 Taskbar 窗口共享同一份频谱数据广播。
    connect(
        m_analyzer.get(),
        &AudioSpectrumAnalyzer::spectrumDataReady,
        this,
        &TaskbarSharedState::spectrumDataReady,
        Qt::QueuedConnection
    );
}

TaskbarSharedState::~TaskbarSharedState()
{
    // 析构时统一停采样，保证多窗口关闭或自动重启时不遗留后台线程。
    stop();
}

void TaskbarSharedState::start()
{
    // start 允许被 main 或后续恢复逻辑重复调用，但真实采样只启动一次。
    if (m_started) {
        return;
    }
    m_started = true;

    // 音频失败不影响 CPU、网络与多显示器工具栏窗口继续工作。
    if (m_analyzer && m_analyzer->initialize()) {
        m_analyzer->startCapture();
        m_audioCaptureStarted = true;
    }
    else {
        qWarning() << "Taskbar shared audio analyzer failed to initialize.";
    }

    startCpuWorker();
    startNetworkWorker();
}

void TaskbarSharedState::stop()
{
    // stop 需要幂等，因为窗口关闭、进程重启和析构都可能触发。
    if (!m_started) {
        return;
    }
    m_started = false;

    stopCpuWorker();
    stopNetworkWorker();

    if (m_audioCaptureStarted && m_analyzer) {
        m_analyzer->stopCapture();
        m_audioCaptureStarted = false;
    }
}

QVector<int> TaskbarSharedState::cpuUsageSnapshot() const
{
    // 返回副本，调用者可以在 UI 线程安全读取并绘制。
    std::lock_guard<std::mutex> lock(m_cpuUsageMutex);
    return m_cpuUsage;
}

std::uint64_t TaskbarSharedState::uploadSpeedBytesPerSecond() const
{
    // 原子读取保证网络采样线程和多个 UI 窗口之间无数据竞争。
    return m_uploadBytesPerSecond.load(std::memory_order_acquire);
}

std::uint64_t TaskbarSharedState::downloadSpeedBytesPerSecond() const
{
    // 原子读取保证网络采样线程和多个 UI 窗口之间无数据竞争。
    return m_downloadBytesPerSecond.load(std::memory_order_acquire);
}

void TaskbarSharedState::startCpuWorker()
{
    // 防止重复启动 CPU 采样线程。
    if (m_cpuWorkerRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    m_cpuWorkerThread = std::thread([this]() {
        while (m_cpuWorkerRunning.load(std::memory_order_acquire)) {
            const std::vector<int> sampledUsage = getCPUCoreUsage();
            QVector<int> nextUsage;
            nextUsage.reserve(static_cast<int>(sampledUsage.size()));
            for (int usage : sampledUsage) {
                nextUsage.append(std::clamp(usage, 0, 100));
            }

            {
                std::lock_guard<std::mutex> lock(m_cpuUsageMutex);
                m_cpuUsage = nextUsage;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
}

void TaskbarSharedState::stopCpuWorker()
{
    // 先通知线程退出，再 join，避免进程重启时线程仍访问已释放对象。
    m_cpuWorkerRunning.store(false, std::memory_order_release);
    if (m_cpuWorkerThread.joinable()) {
        m_cpuWorkerThread.join();
    }
}

void TaskbarSharedState::startNetworkWorker()
{
    // 防止重复启动网络采样线程。
    if (m_networkWorkerRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    m_networkWorkerThread = std::thread([this]() {
        while (m_networkWorkerRunning.load(std::memory_order_acquire)) {
            const NetworkSpeedRate rate = getNetworkSpeedRate();

            m_uploadBytesPerSecond.store(
                rate.uploadBytesPerSecond,
                std::memory_order_release
            );
            m_downloadBytesPerSecond.store(
                rate.downloadBytesPerSecond,
                std::memory_order_release
            );

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
}

void TaskbarSharedState::stopNetworkWorker()
{
    // 先通知线程退出，再 join，避免 UI 关闭后后台线程继续运行。
    m_networkWorkerRunning.store(false, std::memory_order_release);
    if (m_networkWorkerThread.joinable()) {
        m_networkWorkerThread.join();
    }
}
