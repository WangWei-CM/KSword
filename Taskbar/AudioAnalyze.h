#ifndef AUDIOSPECTRUMANALYZER_H
#define AUDIOSPECTRUMANALYZER_H

#include <QObject>
#include <QVector>
#include <atomic>
#include <memory>
#include <mutex>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

class AudioSpectrumAnalyzer : public QObject
{
    Q_OBJECT

public:
    // 构造频谱分析器并初始化不依赖设备的 FFT 数据。
    explicit AudioSpectrumAnalyzer(QObject* parent = nullptr);
    // 销毁分析器；会先等待采集线程退出，再释放 COM 接口。
    ~AudioSpectrumAnalyzer();

    // 停止采集线程并清理非 COM 的频谱缓存。
    void releaseResources();
    // 为下一次采集重置本地缓存；Core Audio 会话只在采集线程中创建。
    bool initialize();
    // 启动单一后台采集线程。
    void startCapture();
    // 请求后台采集线程退出并等待其自行释放音频接口。
    void stopCapture();
    // 返回最近一次计算出的频谱快照。
    QVector<float> getSpectrumData() const;
    // 仅在采样版本改变时复制最近16段快照，供GUI显示节拍读取。
    bool readLatestSpectrum(quint64& revision, QVector<float>& data) const;

signals:
    void spectrumDataReady(const QVector<float>& spectrumData);

private:
    static constexpr int FFT_SIZE = 1024;
    static constexpr int NUM_BANDS = 16;

    // 以下会话函数只从 captureAudioData 所在线程调用，避免跨 COM apartment 传递接口。
    bool initializeAudioSessionOnWorker();
    void releaseAudioSessionOnWorker();
    bool enumerateAudioDevices();

    // sample_rate 仅由采集线程读写，用于频段换算。
    int sample_rate = 48000;
    // defaultAudioDeviceChangedOnWorker：比较当前会话与系统默认输出设备，返回是否需要重建会话。
    bool defaultAudioDeviceChangedOnWorker() const;
    bool initializeAudioDevice();
    bool setupAudioClient();
    void captureAudioData();
    void processAudioData(const BYTE* data, UINT32 framesAvailable);
    void applyFFT(const float* audioData, int size);
    void calculateFrequencyBands();
    void applySmoothing();

    // Windows Core Audio 成员完全归采集线程 MTA 所有、使用和释放。
    IMMDeviceEnumerator* m_deviceEnumerator = nullptr;
    IMMDevice* m_audioDevice = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_waveFormat = nullptr;
    // 该标记仅由采集线程访问，用来平衡该线程自己的 CoInitializeEx。
    bool m_workerComInitialized = false;

    // 音频处理相关成员。频谱快照可由 UI 线程读取，故单独受互斥锁保护。
    QVector<float> m_audioBuffer;
    QVector<float> m_spectrumData;
    QVector<float> m_previousSpectrum;
    QVector<float> m_magnitudes;
    mutable std::mutex m_spectrumMutex;
    quint64 m_spectrumRevision = 0;

    // UI 线程只通过该原子标记请求退出；采集线程轮询并自行结束 Core Audio 会话。
    std::atomic<bool> m_isCapturing{ false };
    // 线程句柄只由启动/停止侧回收，始终保留到采集线程实际退出。
    HANDLE m_captureThread = nullptr;

    // FFT 窗口函数
    QVector<float> m_hanningWindow;
    void createWindowFunction();
};

#endif // AUDIOSPECTRUMANALYZER_H
