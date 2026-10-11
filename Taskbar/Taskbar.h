#ifndef TASKBAR_H
#define TASKBAR_H

#include "SpectrumWidget.h"
#include "TaskbarSharedState.h"
#include "TaskbarNotificationService.h"

#include <QMainWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QString>
#include <QRect>
#include <QColor>
#include <QPixmap>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QList>
#include <functional>
#include <windows.h>
#include <shellapi.h>
#include <QCloseEvent>
#include <qpushbutton.h>
#include <cstdint>

class QScreen;
class QStackedLayout;
class QVariantAnimation;
class QResizeEvent;
class TaskbarSettingsDialog;
class GlowIconButton;

#pragma comment(lib, "shell32.lib")

class Taskbar : public QMainWindow
{
    Q_OBJECT

public:
    explicit Taskbar(QScreen* targetScreen, TaskbarSharedState* sharedState,
        TaskbarNotificationService* notificationService, QWidget* parent = nullptr);
    ~Taskbar() override;

private:
    SpectrumWidget* m_leftSpectrum;      // 左侧频谱组件
    SpectrumWidget* m_rightSpectrum;     // 右侧频谱组件
    TaskbarSharedState* m_sharedState;   // 多窗口共享的音频、CPU、网络采样状态
    TaskbarNotificationService* m_notificationService; // 多窗口共享通知服务和地震警报状态。
    QRect m_targetScreenGeometry;        // 当前窗口启动时绑定的目标显示器矩形
    QString m_targetScreenName;          // Qt screen name used to match the corresponding Win32 monitor.
    qreal m_targetDevicePixelRatio;      // Scale factor used when converting Qt DIP geometry to native pixels.

    QWidget* cpuBarContainer;            // CPU 柱状图容器
    QVector<QLabel*> cpuBars;            // CPU 每核心柱子集合
    QTimer* timer;                       // 时间刷新定时器
    QLabel* timeLabel;                   // 时间文本标签
    QLabel* contentLabel;                // 左侧当前用户名文本，警报态与其它 Taskbar 文本同步变为白色。
    QLabel* logoLabel;                   // 左侧 Logo 标签。
    QPixmap logoPixmap;                  // 左侧 Logo 原图，保留透明通道用于警报态着色。

    QWidget* networkSpeedContainer;      // 网络速率显示容器
    QLabel* uploadSpeedLabel;            // 上行速率文本标签
    QLabel* downloadSpeedLabel;          // 下行速率文本标签
    QTimer* networkUiTimer;              // UI 标签刷新定时器

    bool isAppBarRegistered;             // 是否已经注册 ABM_NEW

    QWidget* centralWidget;
    QWidget* normalCenterWidget;         // 保存频谱和时钟的常态中央页面。
    QWidget* notificationCenterWidget;   // 显示来源、标题和正文的通知中央页面。
    QStackedLayout* centerStackLayout;   // 用 StackAll 保持两个页面可交叉淡入淡出。
    QGraphicsOpacityEffect* normalCenterOpacity; // 常态中央页面透明度效果。
    QGraphicsOpacityEffect* notificationCenterOpacity; // 通知中央页面透明度效果。
    QLabel* notificationSourceLabel;     // 通知来源短文本。
    QLabel* notificationTitleLabel;      // 通知标题文本。
    QLabel* notificationBodyLabel;       // 通知正文文本。
    QList<QPropertyAnimation*> centralAnimations; // 当前中央区域的全部透明度动画。
    TaskbarNotificationView displayedNotification; // 已渲染通知，用于忽略重复刷新。
    bool notificationVisible;             // 当前中央区域是否已切换到通知页。
    bool earthquakePresentation;          // 当前是否由地震预警无淡入地接管中央区域。
    QVariantAnimation* alertFlashAnimation; // 地震期间从亮红到暗红的 500ms 渐变动画。
    bool alertFlashBright;                   // 当前闪烁相位，true 代表刚瞬时切换到亮红背景。
    QWidget* notificationFlashWidget;       // 新消息产生时覆盖整条 Taskbar 的轻微提亮层。
    QGraphicsOpacityEffect* notificationFlashOpacity; // 提亮层的透明度效果。
    QPropertyAnimation* notificationFlashAnimation;  // 提亮层从亮到暗的 500ms 动画。

    QWidget* rightBtnContainer;          // 右侧按钮组容器
    QHBoxLayout* rightBtnLayout;         // 右侧按钮组布局

    QPushButton* exitBtn;                // 退出按钮
    GlowIconButton* lockBtn;              // 锁定工作站图标按钮。
    GlowIconButton* toolBtn;              // 打开命令提示符图标按钮。
    GlowIconButton* settingsBtn;          // 打开通知设置图标按钮。
    GlowIconButton* overviewBtn = nullptr; // 打开透明置顶的 KSword 页面平铺窗口。
    GlowIconButton* userBtn;              // 预留用户扩展图标按钮。
    TaskbarSettingsDialog* settingsDialog; // 当前显示器可打开的非模态设置窗口。

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
    void updateNotificationPresentation();
    void updateNotificationText(const TaskbarNotificationView& notification);
    void transitionToNotification(const TaskbarNotificationView& notification);
    void transitionToNormalCenter();
    void showEarthquakePresentation(const TaskbarNotificationView& notification);
    void animateOpacity(QGraphicsOpacityEffect* effect, qreal startOpacity, qreal endOpacity,
        const std::function<void()>& completed);
    void stopCentralAnimations();
    void applyTaskbarTheme(bool earthquakeAlert, const QColor& backgroundColor);
    void startAlertFlashCycle();
    void flashNotificationBackground();
    void showSettingsDialog();

protected:
    // 在窗口关闭时确保注销 AppBar 并安全停止后台线程。
    void closeEvent(QCloseEvent* event) override;
    // 窗口尺寸变化时同步整条 Taskbar 提亮层的覆盖范围。
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onSpectrumDataReady(const QVector<float>& spectrumData);
    void onExitClicked();
    void updateTime();
    void updateCPUUsage();
    void onNotificationPresentationChanged();
};

#endif
