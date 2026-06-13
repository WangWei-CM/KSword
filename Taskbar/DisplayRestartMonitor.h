#ifndef DISPLAYRESTARTMONITOR_H
#define DISPLAYRESTARTMONITOR_H

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QRect>
#include <QTimer>

class QScreen;

class DisplayRestartMonitor : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    // 构造函数：输入父 QObject；处理初始显示器快照与信号挂接；无业务返回值。
    explicit DisplayRestartMonitor(QObject* parent = nullptr);

    // 析构函数：输入无；处理原生事件过滤器注销；无返回值。
    ~DisplayRestartMonitor() override;

    // 原生事件过滤：输入 Qt 事件类型、原生消息和返回值指针；处理 WM_DISPLAYCHANGE；返回是否截断事件。
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    // 显示器新增：输入新增屏幕；处理信号挂接与重启判断；无返回值。
    void onScreenAdded(QScreen* screen);

    // 显示器移除：输入被移除屏幕；处理重启判断；无返回值。
    void onScreenRemoved(QScreen* screen);

    // 显示器几何变化：输入新几何；处理重启判断；无返回值。
    void onScreenGeometryChanged(const QRect& geometry);

private:
    // 构建显示器签名：输入无；处理屏幕数量和几何排序；返回稳定签名字符串。
    QString buildDisplaySignature() const;

    // 挂接屏幕信号：输入屏幕对象；处理 geometryChanged 连接；无返回值。
    void attachScreen(QScreen* screen);

    // 判断并安排重启：输入无；处理签名比较和防抖；无返回值。
    void scheduleRestartIfChanged();

    // 安排重启：输入无；处理单次延迟重启；无返回值。
    void scheduleRestart();

    // 执行重启：输入无；处理启动新进程和退出当前事件循环；无返回值。
    void restartProcess();

    QString m_initialDisplaySignature;   // 启动时显示器数量与分辨率签名。
    bool m_restartScheduled;             // 防止多路显示器事件重复启动进程。
    QTimer m_restartTimer;               // 延迟重启定时器，用于等待系统显示器事件稳定。
};

#endif
