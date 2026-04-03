#pragma once

#include <QtWidgets/QMainWindow>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTableWidget>
#include <QSpacerItem>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QPixmap>
#include <QPainter>
#include <QKeyEvent>
#include <QRect>
#include <QAbstractAnimation>
#include <QDebug>
#include <QElapsedTimer>
#include <QGraphicsOpacityEffect>

// Qt Charts头文件（条形图相关）
#include <QtCharts/QChart>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QValueAxis>
#include <QtCharts/QChartView>
#include "hGet.h"

class HudPerformancePanel;
class QTabWidget;
class QTimer;

// 自定义OpenGL部件，用于硬件加速渲染
class OpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit OpenGLWidget(QWidget* parent = nullptr);
    ~OpenGLWidget();
    qreal opacity() const { return m_opacity; } // 新增获取当前透明度的接口
    void setBackgroundPixmap(const QPixmap& pixmap);
    void setOpacity(qreal opacity);
    void setChildWidgets(QWidget* left, QWidget* right);

    QPixmap captureContent(); // 捕获当前内容为图像

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    QPixmap m_backgroundPixmap;
    qreal m_opacity;
    QWidget* m_leftWidget = nullptr;
    QWidget* m_rightWidget = nullptr;
};

class KswordHUD : public QMainWindow
{
    Q_OBJECT
        
        Q_PROPERTY(qreal windowOpacity READ windowOpacity WRITE setWindowOpacity)

public:
    KswordHUD(QWidget* parent = nullptr);
    ~KswordHUD();

    qreal windowOpacity() const { return m_windowOpacity; }
    void setWindowOpacity(qreal opacity);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct HudConfig
    {
        QString backgroundImagePath;
        int leftWidgetOpacityPercent = 100;
        int rightWidgetOpacityPercent = 100;
        QString leftWidgetBackgroundColor = "#000000";
        int leftWidgetBackgroundOpacityPercent = 0;
        QString rightWidgetBackgroundColor = "#000000";
        int rightWidgetBackgroundOpacityPercent = 0;
    };

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static bool IsRightCtrlEvent(WPARAM wParam, const KBDLLHOOKSTRUCT* keyboardInfo);

    qreal m_windowOpacity;
    bool m_isVisible;
    bool m_isAnimating;
    QRect m_originalRect;
    QWidget* m_leftWidget;
    QWidget* m_rightWidget;
    QTableWidget* m_tableWidget;
    QPropertyAnimation* m_hideAnimation;
    QPropertyAnimation* m_showAnimation;
    OpenGLWidget* m_glWidget;

    QPixmap m_cachedContent;
    QPixmap m_lastStableContent;
    qreal m_contentScale;

    // 调试相关
    QElapsedTimer m_debugTimer;
    int m_paintFrameCount;
    int m_animationFrameCount;

    void toggleVisibility();
    void initializeAnimations();
    HudConfig loadOrCreateConfig() const;
    void applyHudConfig(const HudConfig& config);
    void cacheWindowContent(bool refreshFromLiveScene = true);
    void debugOutput(const QString& message);
    void clearCache();
    void forceUpdateCache();

    HardwareMonitor* m_hwMonitor = nullptr;    // 旧硬件监控对象（已停用）
    QTimer* m_updateTimer = nullptr;           // 旧定时器（已停用）

    // 2. 右侧TabWidget及图表相关
    QTabWidget* m_rightTabWidget = nullptr;    // 右侧Tab容器
    HudPerformancePanel* m_performancePanel = nullptr;
    QGraphicsOpacityEffect* m_leftOpacityEffect = nullptr;
    QGraphicsOpacityEffect* m_rightOpacityEffect = nullptr;
    // CPU图表（系列+数据集+坐标轴+视图）
    QBarSeries* m_cpuSeries = nullptr;
    QList<QBarSet*> m_cpuBarSets;
    QValueAxis* m_cpuAxisX = nullptr;
    QValueAxis* m_cpuAxisY = nullptr;
    QChartView* m_cpuChartView = nullptr;
    // RAM图表（同上）
    QBarSeries* m_ramSeries = nullptr;
    QList<QBarSet*> m_ramBarSets;
    QValueAxis* m_ramAxisX = nullptr;
    QValueAxis* m_ramAxisY = nullptr;
    QChartView* m_ramChartView = nullptr;
    // Disk图表（同上）
    QBarSeries* m_diskSeries = nullptr;
    QList<QBarSet*> m_diskBarSets;
    QValueAxis* m_diskAxisX = nullptr;
    QValueAxis* m_diskAxisY = nullptr;
    QChartView* m_diskChartView = nullptr;

    // 3. 新增函数声明
    void initCPUChart();     // 初始化CPU条形图
    void initRAMChart();     // 初始化RAM条形图
    void initDiskChart();    // 初始化Disk条形图
    void onUpdateTimerTimeout(); // 定时器槽函数（每秒更新数据）

    static HHOOK s_keyboardHook;
    static KswordHUD* s_instance;
    static bool s_rightCtrlDown;
};
