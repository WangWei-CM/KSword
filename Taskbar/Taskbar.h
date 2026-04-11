#ifndef TASKBAR_H
#define TASKBAR_H

#include "AudioAnalyze.h"
#include "SpectrumWidget.h"

#include <QMainWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QString>
#include <windows.h>
#include <shellapi.h>
#include <QCloseEvent>
#include <qpushbutton.h>
#include <atomic>
#include <thread>
#include <cstdint>

#pragma comment(lib, "shell32.lib")

class Taskbar : public QMainWindow
{
    Q_OBJECT

public:
    explicit Taskbar(QWidget* parent = nullptr);
    ~Taskbar() override;

private:
    SpectrumWidget* m_leftSpectrum;      // 左侧频谱组件
    SpectrumWidget* m_rightSpectrum;     // 右侧频谱组件
    AudioSpectrumAnalyzer* m_analyzer;   // 音频分析器
    SpectrumWidget* m_spectrumWidget;    // 兼容旧接口的频谱指针

    QWidget* cpuBarContainer;            // CPU 柱状图容器
    QVector<QLabel*> cpuBars;            // CPU 每核心柱子集合
    QTimer* timer;                       // 时间刷新定时器
    QLabel* timeLabel;                   // 时间文本标签
    bool useAppBar = false;              // 是否按 AppBar 方式定位窗口

    QWidget* networkSpeedContainer;      // 网络速率显示容器
    QLabel* uploadSpeedLabel;            // 上行速率文本标签
    QLabel* downloadSpeedLabel;          // 下行速率文本标签
    QTimer* networkUiTimer;              // UI 标签刷新定时器

    std::atomic<bool> networkWorkerRunning;              // 网速采样线程运行状态
    std::atomic<std::uint64_t> uploadBytesPerSecond;     // 上行速率缓存(B/s)
    std::atomic<std::uint64_t> downloadBytesPerSecond;   // 下行速率缓存(B/s)
    std::thread networkWorkerThread;                     // 网速采样线程对象

    // 兼容旧逻辑：保留强制顶层定位函数声明。
    void ForceSetTopMostAtZero();

    bool isAppBarRegistered = false;     // 是否已经注册 ABM_NEW

    QWidget* centralWidget;

    QWidget* rightBtnContainer;          // 右侧按钮组容器
    QHBoxLayout* rightBtnLayout;         // 右侧按钮组布局

    QPushButton* exitBtn;                // 退出按钮

    // AppBar 注册与系统消息处理。
    void RegisterAsAppBar();
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    int appBarMessageId;
    HWND hWnd;
    QTimer* cpuUpdateTimer;

    // 网络速率相关辅助逻辑。
    QString formatNetworkSpeed(std::uint64_t bytesPerSecond) const;
    void startNetworkWorker();
    void stopNetworkWorker();
    void updateNetworkSpeedLabels();

protected:
    // 在窗口关闭时确保注销 AppBar 并安全停止后台线程。
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onSpectrumDataReady(const QVector<float>& spectrumData);
    void onExitClicked();
    void updateTime();
    void updateCPUUsage();
};

#endif
