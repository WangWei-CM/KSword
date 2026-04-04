#pragma once

// ============================================================
// FileHandleUsageScanner.h
// 作用：
// - 提供“文件/文件夹占用句柄扫描”能力；
// - 输入目标路径列表，输出命中句柄列表；
// - 供 FileDock 右键“扫描占用句柄”窗口复用。
// ============================================================

#include "../Framework.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace filedock::handleusage
{
    // HandleUsageEntry 作用：
    // - 表示一条命中的占用句柄记录；
    // - 同时承载句柄信息、所属进程信息、命中路径信息。
    struct HandleUsageEntry
    {
        std::uint32_t processId = 0;        // processId：所属进程 PID。
        QString processName;                // processName：所属进程名。
        QString processImagePath;           // processImagePath：所属进程镜像路径。
        std::uint64_t handleValue = 0;      // handleValue：句柄值。
        std::uint16_t typeIndex = 0;        // typeIndex：对象类型索引。
        QString typeName;                   // typeName：对象类型名。
        QString objectName;                 // objectName：对象名（通常为 NT 路径）。
        std::uint32_t grantedAccess = 0;    // grantedAccess：访问掩码。
        std::uint32_t attributes = 0;       // attributes：句柄属性位。
        QString matchedTargetPath;          // matchedTargetPath：命中的目标路径（用户视角）。
        bool matchedByDirectoryRule = false; // matchedByDirectoryRule：true=目录前缀命中；false=精确命中。
        QString matchRuleText;              // matchRuleText：命中来源说明（文件句柄/进程映像/模块映像等）。
    };

    // HandleUsageScanResult 作用：
    // - 聚合一次扫描的完整结果；
    // - 附带统计计数与诊断信息，供 UI 状态栏直接展示。
    struct HandleUsageScanResult
    {
        std::vector<HandleUsageEntry> entries; // entries：命中句柄列表。
        std::size_t totalHandleCount = 0;      // totalHandleCount：系统总句柄数。
        std::size_t fileLikeHandleCount = 0;   // fileLikeHandleCount：File 类型句柄数量。
        std::size_t matchedHandleCount = 0;    // matchedHandleCount：命中目标路径的句柄数量。
        std::size_t processImageMatchCount = 0; // processImageMatchCount：命中“进程映像占用”的数量。
        std::size_t loadedModuleMatchCount = 0; // loadedModuleMatchCount：命中“模块加载占用”的数量。
        std::uint64_t elapsedMs = 0;           // elapsedMs：扫描耗时毫秒。
        QString diagnosticText;                // diagnosticText：诊断文本（失败计数/降级信息）。
    };

    // scanHandleUsageByPaths 作用：
    // - 扫描系统句柄并筛选“占用目标路径”的句柄；
    // - 支持文件和目录两种匹配规则（文件精确匹配、目录前缀匹配）。
    // 调用方式：FileHandleUsageWindow 后台线程调用。
    // 传入 absolutePaths：目标绝对路径集合（可多选）。
    // 传出：HandleUsageScanResult（按值返回）。
    HandleUsageScanResult scanHandleUsageByPaths(const std::vector<QString>& absolutePaths, int progressPid = 0);
}
