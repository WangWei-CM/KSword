#include "Taskbar.h"
#include "DisplayRestartMonitor.h"
#include "TaskbarSharedState.h"

#include <QtWidgets/QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QList>
#include <QScreen>
#include <QVector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // initializeProcessDpiAwareness:
    // - Input: none.
    // - Processing: sets process DPI awareness before QApplication is created.
    // - Return: none; the process falls back to system DPI awareness on older Windows builds.
    void initializeProcessDpiAwareness()
    {
        HMODULE user32ModuleHandle = ::GetModuleHandleW(L"user32.dll");
        if (user32ModuleHandle != nullptr)
        {
            using SetDpiAwarenessContextFunction = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            const SetDpiAwarenessContextFunction setContextFunction =
                reinterpret_cast<SetDpiAwarenessContextFunction>(
                    ::GetProcAddress(user32ModuleHandle, "SetProcessDpiAwarenessContext"));
            if (setContextFunction != nullptr)
            {
                if (setContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                {
                    return;
                }
                if (setContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
                {
                    return;
                }
            }
        }

        // Older Windows fallback: keep at least system-DPI-aware coordinates.
        ::SetProcessDPIAware();
    }
}

int main(int argc, char* argv[])
{
    // 启用 OpenGLES，沿用原任务栏绘制路径，避免改动 Qt 渲染策略。
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);

    // Set DPI awareness before QApplication so QScreen geometry and AppBar math agree.
    initializeProcessDpiAwareness();

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
