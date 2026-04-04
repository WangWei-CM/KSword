#include "HandleDock.h"

// ============================================================
// HandleDock.Icon.cpp
// 作用：
// - 承载句柄模块中“进程图标解析与缓存”逻辑；
// - 同一个 PID 只解析一次图标，后续直接复用缓存；
// - 避免在表格重建时重复加载图标。
// ============================================================

#include <QFileIconProvider>
#include <QFileInfo>

QIcon HandleDock::resolveProcessIconByPid(const std::uint32_t processId)
{
    const quint32 cacheKey = static_cast<quint32>(processId);
    auto iconIt = m_processIconCacheByPid.find(cacheKey);
    if (iconIt != m_processIconCacheByPid.end())
    {
        return iconIt.value();
    }

    const QString processImagePath = queryProcessImagePathCached(processId);
    QIcon processIcon;
    if (!processImagePath.trimmed().isEmpty())
    {
        processIcon = QIcon(processImagePath);
        if (processIcon.isNull())
        {
            QFileIconProvider iconProvider;
            processIcon = iconProvider.icon(QFileInfo(processImagePath));
        }
    }

    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }

    m_processIconCacheByPid.insert(cacheKey, processIcon);
    return processIcon;
}

QString HandleDock::queryProcessImagePathCached(const std::uint32_t processId)
{
    const quint32 cacheKey = static_cast<quint32>(processId);
    auto pathIt = m_processImagePathCacheByPid.find(cacheKey);
    if (pathIt != m_processImagePathCacheByPid.end())
    {
        return pathIt.value();
    }

    const QString processImagePath =
        QString::fromStdString(ks::process::QueryProcessPathByPid(processId));
    m_processImagePathCacheByPid.insert(cacheKey, processImagePath);
    return processImagePath;
}

