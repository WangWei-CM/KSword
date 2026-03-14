#ifndef AUDIOSPECTRUMANALYZER_H
#define AUDIOSPECTRUMANALYZER_H

#include <QObject>
#include <QVector>
#include <memory>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

class AudioSpectrumAnalyzer : public QObject
{
    Q_OBJECT

public:
    explicit AudioSpectrumAnalyzer(QObject* parent = nullptr);
    ~AudioSpectrumAnalyzer();

    void releaseResources();
    bool initialize();
    bool enumerateAudioDevices();
    void startCapture();
    void stopCapture();
    QVector<float> getSpectrumData() const;

signals:
    void spectrumDataReady(const QVector<float>& spectrumData);

private:
    static constexpr int FFT_SIZE = 1024;
    static constexpr int NUM_BANDS = 16;
    //static constexpr int SAMPLE_RATE = 44100;

    int sample_rate = 48000;
    bool initializeAudioDevice();
    bool setupAudioClient();
    void captureAudioData();
    void processAudioData(const BYTE* data, UINT32 framesAvailable);
    void applyFFT(const float* audioData, int size);
    void calculateFrequencyBands();
    void applySmoothing();

    // Windows Core Audio 相关成员
    IMMDeviceEnumerator* m_deviceEnumerator = nullptr;
    IMMDevice* m_audioDevice = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_waveFormat = nullptr;

    // 音频处理相关成员
    QVector<float> m_audioBuffer;
    QVector<float> m_spectrumData;
    QVector<float> m_previousSpectrum;
    QVector<float> m_magnitudes;  // 添加缺失的声明

    // 线程控制
    std::atomic<bool> m_isCapturing{ false };
    HANDLE m_captureThread = nullptr;

    // FFT 窗口函数
    QVector<float> m_hanningWindow;
    void createWindowFunction();
};

#endif // AUDIOSPECTRUMANALYZER_H