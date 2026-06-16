#ifndef TASKBAR_H
#define TASKBAR_H

#include "SpectrumWidget.h"
#include "TaskbarSharedState.h"

#include <QMainWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QString>
#include <QRect>
#include <windows.h>
#include <shellapi.h>
#include <QCloseEvent>
#include <qpushbutton.h>
#include <cstdint>

class QScreen;

#pragma comment(lib, "shell32.lib")

class Taskbar : public QMainWindow
{
    Q_OBJECT

public:
    explicit Taskbar(QScreen* targetScreen, TaskbarSharedState* sharedState, QWidget* parent = nullptr);
    ~Taskbar() override;

private:
    SpectrumWidget* m_leftSpectrum;      // 左侧频谱组件
    SpectrumWidget* m_rightSpectrum;     // 右侧频谱组件
    TaskbarSharedState* m_sharedState;   // 多窗口共享的音频、CPU、网络采样状态
    QRect m_targetScreenGeometry;        // 当前窗口启动时绑定的目标显示器矩形
    QString m_targetScreenName;          // Qt screen name used to match the corresponding Win32 monitor.
    qreal m_targetDevicePixelRatio;      // Scale factor used when converting Qt DIP geometry to native pixels.

    QWidget* cpuBarContainer;            // CPU 柱状图容器
    QVector<QLabel*> cpuBars;            // CPU 每核心柱子集合
    QTimer* timer;                       // 时间刷新定时器
    QLabel* timeLabel;                   // 时间文本标签

    QWidget* networkSpeedContainer;      // 网络速率显示容器
    QLabel* uploadSpeedLabel;            // 上行速率文本标签
    QLabel* downloadSpeedLabel;          // 下行速率文本标签
    QTimer* networkUiTimer;              // UI 标签刷新定时器

    bool isAppBarRegistered;             // 是否已经注册 ABM_NEW

    QWidget* centralWidget;

    QWidget* rightBtnContainer;          // 右侧按钮组容器
    QHBoxLayout* rightBtnLayout;         // 右侧按钮组布局

    QPushButton* exitBtn;                // 退出按钮

    // AppBar 注册与系统消息处理。
    // AppBar thickness helper: no input; converts logical window height to native pixels; returns pixel height.
    int appBarThicknessInNativePixels() const;

    // Target monitor helper: no input; resolves the Win32 monitor rectangle; returns native pixel geometry.
    QRect targetScreenNativeGeometry() const;

    // Target logical geometry helper: no input; resolves the Qt screen rectangle used for QWidget placement; returns logical coordinates.
    QRect targetScreenLogicalGeometry() const;

    // Spectrum minimum width helper: no input; adapts to logical screen width; returns a Qt DIP width.
    int spectrumMinimumWidthForScreen() const;

    // Spectrum maximum width helper: no input; caps elastic spectrum width; returns a Qt DIP width.
    int spectrumMaximumWidthForScreen() const;

    void RegisterAsAppBar();
    void RemoveAppBar();
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    UINT appBarMessageId;
    QTimer* cpuUpdateTimer;

    // 采样与显示相关辅助逻辑。
    QString formatNetworkSpeed(std::uint64_t bytesPerSecond) const;
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
