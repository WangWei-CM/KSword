#include "AudioAnalyze.h"
#include <cmath>
#include <complex>
#include <thread>
#include <QDebug>

// 简单的复数类型定义
typedef std::complex<double> Complex;

AudioSpectrumAnalyzer::AudioSpectrumAnalyzer(QObject* parent)
    : QObject(parent)
    , m_spectrumData(NUM_BANDS, 0.0f)
    , m_previousSpectrum(NUM_BANDS, 0.0f)
    , m_magnitudes(FFT_SIZE / 2, 0.0f)  // 添加缺失的定义
{
    m_audioBuffer.reserve(FFT_SIZE * 2);
    createWindowFunction();
}

AudioSpectrumAnalyzer::~AudioSpectrumAnalyzer()
{
    stopCapture();

    if (m_captureClient) m_captureClient->Release();
    if (m_audioClient) m_audioClient->Release();
    if (m_audioDevice) m_audioDevice->Release();
    if (m_deviceEnumerator) m_deviceEnumerator->Release();

    if (m_captureThread) {
        CloseHandle(m_captureThread);
    }
    CoUninitialize();  // 与 CoInitializeEx 对应，释放 COM 资源
}

bool AudioSpectrumAnalyzer::initialize() {
    // 1. 初始化COM（已修复为STA模式）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        qWarning() << "COM初始化失败. 错误码:" << hr;
        return false;
    }

    // 2. 创建设备枚举器
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&m_deviceEnumerator
    );
    if (FAILED(hr)) {
        qWarning() << "创建设备枚举器失败. 错误码:" << hr;
        CoUninitialize();
        return false;
    }

    // 3. 枚举所有活跃的音频设备（根据需求选择输入/输出）
    IMMDeviceCollection* pDevices = nullptr;
    EDataFlow flow = eRender;  // 若分析扬声器输出，用eRender；若分析麦克风，用eCapture
    hr = m_deviceEnumerator->EnumAudioEndpoints(
        flow,
        DEVICE_STATE_ACTIVE,  // 只枚举活跃设备
        &pDevices
    );
    if (FAILED(hr)) {
        qWarning() << "枚举设备失败. 错误码:" << hr;
        releaseResources();  // 自定义释放资源的函数
        return false;
    }

    // 4. 遍历设备，尝试初始化第一个可用设备
    UINT deviceCount = 0;
    pDevices->GetCount(&deviceCount);
    qDebug() << "找到" << deviceCount << "个活跃音频设备";

    bool foundValidDevice = false;
    for (UINT i = 0; i < deviceCount; ++i) {
        IMMDevice* pDevice = nullptr;
        if (SUCCEEDED(pDevices->Item(i, &pDevice))) {
            qDebug() << "尝试初始化设备" << i;
            m_audioDevice = pDevice;  // 临时使用当前设备

            // 尝试初始化该设备的音频客户端
            if (setupAudioClient()) {
                qDebug() << "设备" << i << "初始化成功！";
                foundValidDevice = true;
                break;  // 找到可用设备，退出循环
            }
            else {
                pDevice->Release();  // 初始化失败，释放设备
                m_audioDevice = nullptr;
            }
        }
    }

    pDevices->Release();  // 释放设备集合

    if (!foundValidDevice) {
        qWarning() << "所有设备均无法初始化（可能被占用或不支持格式）";
        releaseResources();
        return false;
    }

    // 5. 初始化成功，继续后续步骤（如启动捕获线程）
    return initializeAudioDevice();
}
void AudioSpectrumAnalyzer::releaseResources() {
    // 释放COM接口
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_audioDevice) {
        m_audioDevice->Release();
        m_audioDevice = nullptr;
    }
    if (m_deviceEnumerator) {
        m_deviceEnumerator->Release();
        m_deviceEnumerator = nullptr;
    }

    // 关闭线程（若有）
    if (m_captureThread) {
        CloseHandle(m_captureThread);
        m_captureThread = nullptr;
    }

    // 反初始化COM
    CoUninitialize();
}
bool AudioSpectrumAnalyzer::enumerateAudioDevices()
{
    IMMDeviceCollection* deviceCollection = nullptr;
    HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &deviceCollection);

    if (SUCCEEDED(hr) && deviceCollection) {
        UINT count = 0;
        deviceCollection->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            IMMDevice* device = nullptr;
            hr = deviceCollection->Item(i, &device);
            if (SUCCEEDED(hr)) {
                m_audioDevice = device;
                if (setupAudioClient()) {
                    deviceCollection->Release();
                    return true;
                }
                device->Release();
            }
        }
        deviceCollection->Release();
    }
    return false;
}
bool AudioSpectrumAnalyzer::initializeAudioDevice()
{
    HRESULT hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, &m_audioDevice);

    if (FAILED(hr)) {
        qWarning() << "Failed to get default audio endpoint, trying loopback...";
        // 尝试枚举设备查找合适的环回设备
        return enumerateAudioDevices();
    }

    return setupAudioClient();
}

bool AudioSpectrumAnalyzer::setupAudioClient()
{
    HRESULT hr = m_audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
        nullptr, reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr)) {
        qWarning() << "Failed to activate audio device";
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
        qWarning() << "Failed to get mix format";
        return false;
    }

    // 设置我们需要的格式（32位浮点数）
    WAVEFORMATEX waveFormat = {};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;       // PCM格式（最通用）
    waveFormat.nChannels = 1;                      // 单声道（比双声道兼容性好）
    waveFormat.nSamplesPerSec = 48000;             // 44.1kHz（大多数设备支持）
    waveFormat.wBitsPerSample = 16;                // 16位深度（主流设备支持）
    waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
    waveFormat.cbSize = 0;

    WAVEFORMATEX* pDefaultFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&pDefaultFormat);
    if (FAILED(hr)) {
        qWarning() << "获取设备默认格式失败. 错误码:" << hr;
        return false;
    }

    // 打印默认格式（调试用）
    qDebug() << "设备默认格式: " << pDefaultFormat->nSamplesPerSec << "Hz, "
        << pDefaultFormat->nChannels << "声道, "
        << pDefaultFormat->wBitsPerSample << "位";

    // 初始化音频客户端：使用默认格式 + 共享模式
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,  // 捕获扬声器输出（若捕获麦克风则移除）
        50000000,  // 缓冲区时长调整为50ms（增大缓冲区提高兼容性）
        0,
        pDefaultFormat,
        nullptr
    );

    // 释放默认格式内存（必须调用，否则内存泄漏）
    sample_rate = pDefaultFormat->nSamplesPerSec;
    CoTaskMemFree(pDefaultFormat);
    m_waveFormat = mixFormat;

    if (FAILED(hr)) {
        QString errorMsg;
        if (hr == AUDCLNT_E_DEVICE_IN_USE) {
            errorMsg = "设备被其他程序独占占用（请关闭占用程序）";
        }
        else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            errorMsg = "设备不支持默认格式（罕见）";
        }
        else {
            errorMsg = "初始化失败，错误码: " + QString::number(hr, 16);
        }
        qWarning() << "设备初始化失败:" << errorMsg;
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&m_captureClient));
    if (FAILED(hr)) {
        qWarning() << "Failed to get audio capture client";
        return false;
    }

    return true;
}

void AudioSpectrumAnalyzer::startCapture()
{
    if (m_isCapturing) return;

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        qWarning() << "Failed to start audio capture";
        return;
    }

    m_isCapturing = true;

    // 创建捕获线程
    m_captureThread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        auto analyzer = static_cast<AudioSpectrumAnalyzer*>(param);
        analyzer->captureAudioData();
        return 0;
        }, this, 0, nullptr);
}

void AudioSpectrumAnalyzer::stopCapture()
{
    m_isCapturing = false;

    if (m_audioClient) {
        m_audioClient->Stop();
    }

    if (m_captureThread) {
        WaitForSingleObject(m_captureThread, 1000);
        CloseHandle(m_captureThread);
        m_captureThread = nullptr;
    }
}

void AudioSpectrumAnalyzer::captureAudioData()
{
    while (m_isCapturing) {
        UINT32 packetSize = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetSize);

        if (FAILED(hr)) {
            qWarning() << "GetNextPacketSize失败，错误码:" << hr; // 新增日志
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr) && framesAvailable > 0) {
                processAudioData(data, framesAvailable);
                m_captureClient->ReleaseBuffer(framesAvailable);
            }
            else {
                qWarning() << "GetBuffer失败，错误码:" << hr; // 新增日志
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    qDebug() << "捕获线程退出"; // 新增日志，确认退出时机
}

void AudioSpectrumAnalyzer::processAudioData(const BYTE* data, UINT32 framesAvailable) {


    if (!m_waveFormat) {
        qDebug() << "m_waveFormat is null!";
        return;
    }

    qDebug() << "Processing audio data - Frames:" << framesAvailable
        << "Format:" << m_waveFormat->wFormatTag
        << "Channels:" << m_waveFormat->nChannels
        << "Bits:" << m_waveFormat->wBitsPerSample;

    // 检查数据范围
    float maxSample = 0.0f;
    float minSample = 0.0f;

    float mono;
    // 根据实际格式解析样本
    qDebug() << "Audio format - Tag:" << m_waveFormat->wFormatTag
        << "Bits:" << m_waveFormat->wBitsPerSample;

    // 根据实际格式解析样本
    if (m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // 转换为扩展格式结构体
        WAVEFORMATEXTENSIBLE* waveFormatExt = (WAVEFORMATEXTENSIBLE*)m_waveFormat;
        GUID subFormat = waveFormatExt->SubFormat;

        qDebug() << "Extended format - SubFormat:" << subFormat.Data1;

        // 检查是否为IEEE浮点数格式
        if (subFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT) {
            qDebug() << "Detected IEEE FLOAT format";
            const float* floatData = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < framesAvailable * m_waveFormat->nChannels; i += m_waveFormat->nChannels) {
                float mono = 0.0f;
                if (m_waveFormat->nChannels == 2) {
                    float left = floatData[i];
                    float right = floatData[i + 1];
                    mono = (left + right) * 0.5f;
                }
                else if (m_waveFormat->nChannels == 1) {
                    mono = floatData[i];
                }
                m_audioBuffer.append(mono);
            }
        }
        // 检查是否为PCM格式
        else if (subFormat.Data1 == WAVE_FORMAT_PCM) {
            qDebug() << "Detected PCM format in extensible wrapper";
            if (m_waveFormat->wBitsPerSample == 32) {
                const int32_t* intData = reinterpret_cast<const int32_t*>(data);
                for (UINT32 i = 0; i < framesAvailable * m_waveFormat->nChannels; i += m_waveFormat->nChannels) {
                    float mono = 0.0f;
                    if (m_waveFormat->nChannels == 2) {
                        float left = static_cast<float>(intData[i]) / 2147483648.0f;
                        float right = static_cast<float>(intData[i + 1]) / 2147483648.0f;
                        mono = (left + right) * 0.5f;
                    }
                    else if (m_waveFormat->nChannels == 1) {
                        mono = static_cast<float>(intData[i]) / 2147483648.0f;
                    }
                    m_audioBuffer.append(mono);
                }
            }
        }
        else {
            qDebug() << "Unsupported subformat:" << subFormat.Data1;
            return;
        }
    }
    else
        if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && m_waveFormat->wBitsPerSample == 32) {
        // 32位浮点格式（设备返回的格式）
        const float* floatData = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < framesAvailable * m_waveFormat->nChannels; i += m_waveFormat->nChannels) {
            float mono = 0.0f;
            if (m_waveFormat->nChannels == 2) {
                float left = floatData[i];
                float right = floatData[i + 1];
                mono = (left + right) * 0.5f;
            }
            else if (m_waveFormat->nChannels == 1) {
                mono = floatData[i];
            }
            m_audioBuffer.append(mono);
        }
    }
    else if (m_waveFormat->wFormatTag == WAVE_FORMAT_PCM) {
        // PCM格式处理
        if (m_waveFormat->wBitsPerSample == 32) {
            const int32_t* intData = reinterpret_cast<const int32_t*>(data);
            for (UINT32 i = 0; i < framesAvailable * m_waveFormat->nChannels; i += m_waveFormat->nChannels) {
                float mono = 0.0f;
                if (m_waveFormat->nChannels == 2) {
                    float left = static_cast<float>(intData[i]) / 2147483648.0f;
                    float right = static_cast<float>(intData[i + 1]) / 2147483648.0f;
                    mono = (left + right) * 0.5f;
                }
                else if (m_waveFormat->nChannels == 1) {
                    mono = static_cast<float>(intData[i]) / 2147483648.0f;
                }
                m_audioBuffer.append(mono);
            }
        }
        else if (m_waveFormat->wBitsPerSample == 16) {
            const int16_t* intData = reinterpret_cast<const int16_t*>(data);
            for (UINT32 i = 0; i < framesAvailable * m_waveFormat->nChannels; i += m_waveFormat->nChannels) {
                float mono = 0.0f;
                if (m_waveFormat->nChannels == 2) {
                    float left = static_cast<float>(intData[i]) / 32768.0f;
                    float right = static_cast<float>(intData[i + 1]) / 32768.0f;
                    mono = (left + right) * 0.5f;
                }
                else if (m_waveFormat->nChannels == 1) {
                    mono = static_cast<float>(intData[i]) / 32768.0f;
                }
                m_audioBuffer.append(mono);
            }
        }
    }
    else {
        qDebug() << "Unsupported format tag:" << m_waveFormat->wFormatTag;
        return;
    }
    if (!m_audioBuffer.isEmpty()) {

        float minSample = *std::min_element(m_audioBuffer.end() - framesAvailable * m_waveFormat->nChannels, m_audioBuffer.end());
        float maxSample = *std::max_element(m_audioBuffer.end() - framesAvailable * m_waveFormat->nChannels, m_audioBuffer.end());
        qDebug() << "Collected" << framesAvailable << "frames, buffer size:" << m_audioBuffer.size();
        qDebug() << "Sample range: min=" << minSample << "max=" << maxSample;
    }
    else {
        qDebug() << "No data collected! Buffer remains empty.";
    }


    // FFT 处理
    if (m_audioBuffer.size() >= FFT_SIZE) {
        qDebug() << "Performing FFT...";
        applyFFT(m_audioBuffer.constData(), FFT_SIZE);
        m_audioBuffer.remove(0, FFT_SIZE / 2);
        qDebug() << "Buffer after FFT:" << m_audioBuffer.size();
    }
    qDebug() << "Sample range: min=" << minSample << "max=" << maxSample;
    qDebug() << "Audio buffer size:" << m_audioBuffer.size();
}
void AudioSpectrumAnalyzer::applyFFT(const float* audioData, int size)
{
    std::vector<Complex> complexData(size);

    // 应用汉宁窗并转换为复数
    for (int i = 0; i < size; ++i) {
        float windowedSample = audioData[i] * m_hanningWindow[i];
        complexData[i] = Complex(windowedSample, 0.0);
    }

    // 执行FFT
    for (int i = 1, j = 0; i < size; ++i) {
        int bit = size >> 1;
        for (; j >= bit; bit >>= 1) {
            j -= bit;
        }
        j += bit;
        if (i < j) {
            std::swap(complexData[i], complexData[j]);
        }
    }

    for (int length = 2; length <= size; length <<= 1) {
        double angle = -2.0 * M_PI / length;
        Complex wlen(std::cos(angle), std::sin(angle));

        for (int i = 0; i < size; i += length) {
            Complex w(1.0, 0.0);
            for (int j = 0; j < length / 2; ++j) {
                Complex u = complexData[i + j];
                Complex v = complexData[i + j + length / 2] * w;
                complexData[i + j] = u + v;
                complexData[i + j + length / 2] = u - v;
                w *= wlen;
            }
        }
    }

    // 计算幅度并存储到m_magnitudes
    for (int i = 0; i < size / 2; ++i) {
        m_magnitudes[i] = static_cast<float>(std::abs(complexData[i]));
    }

    calculateFrequencyBands();
}

void AudioSpectrumAnalyzer::calculateFrequencyBands() {
    qDebug() << "Calculating frequency bands...";

    // 重置频带数据
    m_spectrumData.fill(0.0f, NUM_BANDS);

    if (m_magnitudes.isEmpty()) {
        qDebug() << "ERROR: Magnitudes array is empty!";
        return;
    }

    // 计算每个频带的平均幅度
    int fftSize = m_magnitudes.size();
    float sampleRate = 48000.0f; // 根据实际采样率调整

    for (int band = 0; band < NUM_BANDS; ++band) {
        // 计算频带对应的频率范围（对数刻度）
        float lowFreq = band == 0 ? 20.0f : (sampleRate / 2) * pow(2.0f, (band - 1) / (NUM_BANDS - 1.0f));
        float highFreq = (sampleRate / 2) * pow(2.0f, band / (NUM_BANDS - 1.0f));

        int lowBin = qMax(0, static_cast<int>(lowFreq * fftSize / sampleRate));
        int highBin = qMin(fftSize - 1, static_cast<int>(highFreq * fftSize / sampleRate));

        if (lowBin >= highBin) {
            lowBin = highBin - 1;
            if (lowBin < 0) lowBin = 0;
        }

        // 计算该频带的平均值
        float sum = 0.0f;
        int count = 0;
        for (int bin = lowBin; bin <= highBin; ++bin) {
            sum += m_magnitudes[bin];
            count++;
        }
        //自定义频谱放缩系数
        if(band < 4){
            sum *= 0.8f; // 低频放大
        } else if(band < 8){
            sum *= 1.5f; // 中低频稍微放大
        } else if(band < 12){
            sum *= 1.5f; // 中高频稍微减小
        } else {
            sum *= 5.0f; // 高频飞起来
		}
        m_spectrumData[band] = count > 0 ? sum / count : 0.0f
            ;

        
    }

    // 调试输出频带数据
    float maxBand = *std::max_element(m_spectrumData.begin(), m_spectrumData.end());
    qDebug() << "Frequency bands calculated - max band:" << maxBand;

    if (maxBand > 0) {
        qDebug() << "First 5 bands:";
        for (int i = 0; i < 5 && i < NUM_BANDS; ++i) {
            qDebug() << "  Band" << i << ":" << m_spectrumData[i];
        }
    }

    emit spectrumDataReady(m_spectrumData);
    qDebug() << "Spectrum data signal emitted";
}
void AudioSpectrumAnalyzer::createWindowFunction()
{
    m_hanningWindow.resize(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i) {
        m_hanningWindow[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
    }
}

QVector<float> AudioSpectrumAnalyzer::getSpectrumData() const
{
    return m_spectrumData;
}