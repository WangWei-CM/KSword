#include "Taskbar.h"
#include "DisplayRestartMonitor.h"
#include "TaskbarSharedState.h"

#include <QtWidgets/QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QList>
#include <QScreen>
#include <QVector>

int main(int argc, char* argv[])
{
    // 启用 OpenGLES，沿用原任务栏绘制路径，避免改动 Qt 渲染策略。
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);

    // QApplication 管理所有 Taskbar 窗口、屏幕事件和共享状态对象生命周期。
    QApplication app(argc, argv);

    // 显示器变化监控器在屏幕数量或分辨率变化时自动拉起新进程并退出旧进程。
    DisplayRestartMonitor displayRestartMonitor(&app);

    // 共享状态只启动一次采样，多个显示器窗口只共享数据，不重复打开音频/CPU/网络采样器。
    TaskbarSharedState sharedState(&app);
    sharedState.start();

    // 每个显示器创建一个完全相同的 Taskbar 窗口，并分别向系统申请顶部 AppBar 边缘。
    QVector<Taskbar*> windows;
    const QList<QScreen*> screens = QGuiApplication::screens();
    windows.reserve(screens.size());

    for (QScreen* screen : screens) {
        Taskbar* window = new Taskbar(screen, &sharedState);
        windows.append(window);
        window->show();
    }

    return app.exec();
}
