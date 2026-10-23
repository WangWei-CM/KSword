#pragma once

// ============================================================
// DriverFileSystemParser.h
// 作用：
// 1) 为 FileDock 提供显式 R0 目录解析入口；
// 2) 只消费 ArkDriverClient 的结构化分页结果；
// 3) 不让 Dock UI 直接调用 KswordARK DeviceIoControl。
// ============================================================

#include "ManualFileSystemParser.h"

namespace ks::file
{
    // DriverFileSystemParser 作用：
    // - 通过 KswordARK 在内核调用文件系统目录查询；
    // - 把固定协议行转换为 FileDock 已有 ManualDirectoryEntry 模型。
    class DriverFileSystemParser final
    {
    public:
        // enumerateDirectory 作用：
        // - 使用驱动分页枚举已挂载目录，不回退到 QFileSystemModel/WinAPI 枚举。
        // 调用方法：
        // - FileDock 选择“R0 驱动解析”后在后台线程调用。
        // 入参 pathText：
        // - Windows 本地、卷 GUID 或 UNC 目录路径。
        // 出参 entriesOut：
        // - 返回驱动枚举到的目录行。
        // 出参 fsTypeOut：
        // - 根据 R0 返回的文件系统名映射 NTFS/FAT32/exFAT，未知类型保留 Unknown。
        // 出参 errorTextOut：
        // - 失败时返回通信、协议或 NTSTATUS 诊断。
        // 出参 partialOut：
        // - true 表示驱动返回部分页、名称截断或达到 R3 总行预算。
        // 出参 sourceDetailOut：
        // - 返回用于状态栏的 R0 来源摘要。
        // 返回值：
        // - 至少取得可信完整/部分目录结果时返回 true；完全失败返回 false。
        static bool enumerateDirectory(
            const QString& pathText,
            std::vector<ManualDirectoryEntry>& entriesOut,
            ManualFsType& fsTypeOut,
            QString& errorTextOut,
            bool* partialOut = nullptr,
            QString* sourceDetailOut = nullptr,
            const DirectoryCancellationCallback& canceled = {});
    };
}
