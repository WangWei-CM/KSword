#ifndef TASKBAR_H
#define TASKBAR_H

#include "AudioAnalyze.h"
#include "SpectrumWidget.h"

#include <QMainWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <windows.h>
#include <shellapi.h>
#include <QCloseEvent>
#include <qpushbutton.h>
#pragma comment(lib, "shell32.lib")

class Taskbar : public QMainWindow
{
    Q_OBJECT

public:
    explicit Taskbar(QWidget* parent = nullptr);
    ~Taskbar() override;

private:
    SpectrumWidget* m_leftSpectrum;
    SpectrumWidget* m_rightSpectrum;
    AudioSpectrumAnalyzer* m_analyzer;
    SpectrumWidget* m_spectrumWidget;
    QWidget* cpuBarContainer;
    QVector<QLabel*> cpuBars;
    QTimer* timer; // 计时器对象
    QLabel* timeLabel; // 显示时间的标签
    bool useAppBar = false; // 若 false 则不注册为 AppBar，使用 SetWindowPos 强制置顶
    
    // 强制将窗口放到屏幕顶端(0,0)，处理 DPI 差异并使用 Win32 SetWindowPos
    void ForceSetTopMostAtZero();

    bool isAppBarRegistered = false; // track whether we've registered ABM_NEW

    QWidget* centralWidget;
    
    QWidget* rightBtnContainer; // 右上角按钮组的容器（父载体）
    QHBoxLayout* rightBtnLayout; // 容器内部的局部布局（管理多个按钮）
    
    
    QPushButton* exitBtn; // 现有退出按钮
    // 注册为应用桌面工具栏
    void RegisterAsAppBar();
    // 处理Windows消息的回调函数
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    // 应用栏回调消息ID
    int appBarMessageId;
    // 窗口句柄
    HWND hWnd;
    QTimer* cpuUpdateTimer;

protected:
    // 在窗口关闭时确保注销AppBar
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onSpectrumDataReady(const QVector<float>& spectrumData);
    void onExitClicked();
    void updateTime();
	void updateCPUUsage();
};
#endif