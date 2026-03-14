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

// Qt Charts头文件（条形图相关）
#include <QtCharts/QChart>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QValueAxis>
#include <QtCharts/QChartView>
#include "hGet.h"
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
    static const int HOTKEY_ID_CTRL = 1;
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
    qreal m_contentScale;

    // 调试相关
    QElapsedTimer m_debugTimer;
    int m_paintFrameCount;
    int m_animationFrameCount;

    void toggleVisibility();
    void initializeAnimations();
    void cacheWindowContent();
    void debugOutput(const QString& message);
    void clearCache();
    void forceUpdateCache();

    HardwareMonitor* m_hwMonitor;    // 硬件监控对象（来自hGet.h）
    QTimer* m_updateTimer;           // 1秒更新定时器

    // 2. 右侧TabWidget及图表相关
    QTabWidget* m_rightTabWidget;    // 右侧Tab容器
    // CPU图表（系列+数据集+坐标轴+视图）
    QBarSeries* m_cpuSeries;
    QList<QBarSet*> m_cpuBarSets;
    QValueAxis* m_cpuAxisX;
    QValueAxis* m_cpuAxisY;
    QChartView* m_cpuChartView;
    // RAM图表（同上）
    QBarSeries* m_ramSeries;
    QList<QBarSet*> m_ramBarSets;
    QValueAxis* m_ramAxisX;
    QValueAxis* m_ramAxisY;
    QChartView* m_ramChartView;
    // Disk图表（同上）
    QBarSeries* m_diskSeries;
    QList<QBarSet*> m_diskBarSets;
    QValueAxis* m_diskAxisX;
    QValueAxis* m_diskAxisY;
    QChartView* m_diskChartView;

    // 3. 新增函数声明
    void initCPUChart();     // 初始化CPU条形图
    void initRAMChart();     // 初始化RAM条形图
    void initDiskChart();    // 初始化Disk条形图
    void onUpdateTimerTimeout(); // 定时器槽函数（每秒更新数据）
};