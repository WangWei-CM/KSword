#include "FileDock.h"

// ============================================================
// FileDock.HandleUsage.cpp
// 作用：
// - 承载 FileDock “占用句柄扫描”入口逻辑；
// - 负责弹出结果窗口并桥接“转到进程详情”动作；
// - 避免继续膨胀 FileDock.cpp 主文件。
// ============================================================

#include "FileHandleUsageWindow.h"

#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>

void FileDock::openHandleUsageScanWindow(const std::vector<QString>& scanPaths)
{
    // 第一步：清理并去重目标路径，只保留文件/目录实体。
    std::vector<QString> validPaths;
    validPaths.reserve(scanPaths.size());
    for (const QString& pathText : scanPaths)
    {
        QFileInfo fileInfo(pathText);
        if (!fileInfo.exists())
        {
            continue;
        }
        if (!fileInfo.isFile() && !fileInfo.isDir())
        {
            continue;
        }

        const QString absolutePath = fileInfo.absoluteFilePath();
        if (std::find(validPaths.begin(), validPaths.end(), absolutePath) == validPaths.end())
        {
            validPaths.push_back(absolutePath);
        }
    }

    if (validPaths.empty())
    {
        kLogEvent emptyEvent;
        warn << emptyEvent
            << "[FileDock] 扫描占用句柄取消：未选中有效文件或目录。"
            << eol;
        return;
    }

    // 第二步：创建独立结果窗口，并配置“打开进程详情”的桥接回调。
    auto* scanWindow = new FileHandleUsageWindow(validPaths, this);
    scanWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    scanWindow->setOpenProcessDetailCallback([this](const std::uint32_t processId)
        {
            const quint32 pidValue = static_cast<quint32>(processId);
            QObject* topWindowObject = this->window();
            if (topWindowObject == nullptr)
            {
                return;
            }

            const bool invokeOk = QMetaObject::invokeMethod(
                topWindowObject,
                "openProcessDetailByPid",
                Qt::QueuedConnection,
                Q_ARG(quint32, pidValue));
            if (invokeOk)
            {
                return;
            }

            // 兜底：若主窗口未提供进程详情入口，则至少跳到“句柄Dock + PID过滤”。
            (void)QMetaObject::invokeMethod(
                topWindowObject,
                "focusHandleDockByPid",
                Qt::QueuedConnection,
                Q_ARG(quint32, pidValue));
        });

    // 第三步：展示窗口并记录日志，便于后续链路审计。
    scanWindow->show();
    scanWindow->raise();
    scanWindow->activateWindow();

    kLogEvent openWindowEvent;
    info << openWindowEvent
        << "[FileDock] openHandleUsageScanWindow: targetCount="
        << validPaths.size()
        << eol;
}
