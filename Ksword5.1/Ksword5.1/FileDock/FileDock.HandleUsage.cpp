#include "FileDock.h"

// ============================================================
// FileDock.HandleUsage.cpp
// 作用：
// - 承载 FileDock “占用句柄扫描”入口逻辑；
// - 负责弹出结果窗口并桥接“转到进程详情”动作；
// - 避免继续膨胀 FileDock.cpp 主文件。
// ============================================================

#include "FileHandleUsageWindow.h"
#include "FileMappedProcessWindow.h"

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

void FileDock::openMappedProcessScanWindow(const std::vector<QString>& scanPaths)
{
    // 第一步：只保留文件路径。中文说明：ControlArea 反查第一版针对文件
    // DataSection/ImageSection，目录没有稳定可展示的映射语义。
    std::vector<QString> validPaths;
    validPaths.reserve(scanPaths.size());
    for (const QString& pathText : scanPaths)
    {
        QFileInfo fileInfo(pathText);
        if (!fileInfo.exists() || !fileInfo.isFile())
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
            << "[FileDock] 扫描映射进程取消：未选中有效文件。"
            << eol;
        return;
    }

    // 第二步：创建独立窗口，并复用与占用句柄窗口相同的进程详情跳转桥接。
    auto* scanWindow = new FileMappedProcessWindow(validPaths, this);
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

            (void)QMetaObject::invokeMethod(
                topWindowObject,
                "focusHandleDockByPid",
                Qt::QueuedConnection,
                Q_ARG(quint32, pidValue));
        });

    // 第三步：展示窗口并记录日志。
    scanWindow->show();
    scanWindow->raise();
    scanWindow->activateWindow();

    kLogEvent openWindowEvent;
    info << openWindowEvent
        << "[FileDock] openMappedProcessScanWindow: targetCount="
        << validPaths.size()
        << eol;
}
