#ifndef TASKBARSHAREDSTATE_H
#define TASKBARSHAREDSTATE_H

#include "AudioAnalyze.h"
#include "Function.h"

#include <QObject>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

class TaskbarSharedState : public QObject
{
    Q_OBJECT

public:
    // 构造函数：输入父 QObject；处理共享采样器与缓存初始化；无业务返回值。
    explicit TaskbarSharedState(QObject* parent = nullptr);

    // 析构函数：输入无；处理音频、CPU、网络采样线程停止；无返回值。
    ~TaskbarSharedState() override;

    // 启动共享采样：输入无；处理音频捕获、CPU 采样、网络采样的一次性启动；无返回值。
    void start();

    // 停止共享采样：输入无；处理所有后台采样线程安全退出；无返回值。
    void stop();

    // 获取 CPU 快照：输入无；处理互斥读取；返回每个逻辑核心 0-100 的占用率。
    QVector<int> cpuUsageSnapshot() const;

    // 获取上行速率：输入无；处理原子读取；返回上行字节数每秒。
    std::uint64_t uploadSpeedBytesPerSecond() const;

    // 获取下行速率：输入无；处理原子读取；返回下行字节数每秒。
    std::uint64_t downloadSpeedBytesPerSecond() const;

signals:
    // 频谱数据广播：输入 16 段频谱数据；处理由接收窗口完成；信号本身无返回值。
    void spectrumDataReady(const QVector<float>& spectrumData);

private:
    // 启动 CPU 线程：输入无；处理防重入与线程创建；无返回值。
    void startCpuWorker();

    // 停止 CPU 线程：输入无；处理停止标记与 join；无返回值。
    void stopCpuWorker();

    // 启动网络线程：输入无；处理防重入与线程创建；无返回值。
    void startNetworkWorker();

    // 停止网络线程：输入无；处理停止标记与 join；无返回值。
    void stopNetworkWorker();

    std::unique_ptr<AudioSpectrumAnalyzer> m_analyzer;// 全进程唯一音频频谱采样器。
    bool m_started;                                   // start/stop 幂等保护标记。
    bool m_audioCaptureStarted;                       // 音频捕获是否已成功启动。

    mutable std::mutex m_cpuUsageMutex;               // CPU 利用率缓存互斥锁。
    QVector<int> m_cpuUsage;                          // 每核心 CPU 利用率共享快照。
    std::atomic<bool> m_cpuWorkerRunning;             // CPU 后台线程运行标记。
    std::thread m_cpuWorkerThread;                    // CPU 后台采样线程对象。

    std::atomic<bool> m_networkWorkerRunning;         // 网络后台线程运行标记。
    std::thread m_networkWorkerThread;                // 网络后台采样线程对象。
    std::atomic<std::uint64_t> m_uploadBytesPerSecond;// 上行速度缓存，单位 B/s。
    std::atomic<std::uint64_t> m_downloadBytesPerSecond;// 下行速度缓存，单位 B/s。
};

#endif
