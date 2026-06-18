#pragma once

// ============================================================
// KernelSymbolicLinkWorker.h
// 作用说明：
// 1) 提供 Object Manager 常见目录中的 SymbolicLink 专项枚举；
// 2) 使用 NtOpenSymbolicLinkObject + NtQuerySymbolicLinkObject 解析目标；
// 3) 提供 NT 设备路径到 DOS 路径候选映射，不依赖 R0/驱动 IOCTL。
// ============================================================

#include <QString>

#include <vector>

struct KernelSymbolicLinkEntry
{
    QString sourceDirectory; // sourceDirectory：本条记录来自的对象目录。
    QString linkName;        // linkName：目录内对象名；目录打不开时用于显示失败占位。
    QString fullPath;        // fullPath：符号链接完整对象路径。
    QString targetPath;      // targetPath：NtQuerySymbolicLinkObject 返回的目标路径。
    QString dosCandidate;    // dosCandidate：按 QueryDosDeviceW 映射出的 DOS 路径候选。
    QString statusText;      // statusText：枚举/打开/解析状态，失败行保留原因。
};

// runKernelSymbolicLinkSnapshotTask：
// - 输入 rowsOut：输出容器，函数开始时会清空。
// - 输入 errorTextOut：致命错误文本，成功或部分成功时清空。
// - 处理逻辑：枚举常见对象目录，筛出 SymbolicLink，对每个链接解析目标和 DOS 候选。
// - 返回结果：true 表示任务完成；false 表示 Nt API 无法加载等致命失败。
bool runKernelSymbolicLinkSnapshotTask(
    std::vector<KernelSymbolicLinkEntry>& rowsOut,
    QString& errorTextOut);

// queryKernelSymbolicLinkTarget：
// - 输入 symbolicLinkPathText：完整符号链接对象路径，例如 \GLOBAL??\C:。
// - 输出 targetTextOut：解析成功时返回目标路径。
// - 输出 statusTextOut：返回 NtOpen/NtQuery 状态说明。
// - 处理逻辑：仅打开 SymbolicLink 对象并解析目标，不递归枚举目标。
// - 返回结果：true 表示解析成功；false 表示打开或查询失败。
bool queryKernelSymbolicLinkTarget(
    const QString& symbolicLinkPathText,
    QString& targetTextOut,
    QString& statusTextOut);

// queryKernelSymbolicLinkDosPathCandidates：
// - 输入 ntPathText：NT 设备路径，例如 \Device\HarddiskVolume3\Windows。
// - 处理逻辑：遍历 A: 到 Z: 的 QueryDosDeviceW 映射，生成可能的 DOS 路径。
// - 返回结果：候选 DOS 路径列表，找不到映射时返回空列表。
std::vector<QString> queryKernelSymbolicLinkDosPathCandidates(const QString& ntPathText);
