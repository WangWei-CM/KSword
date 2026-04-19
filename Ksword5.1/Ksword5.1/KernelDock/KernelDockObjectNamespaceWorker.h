#pragma once

// ============================================================
// KernelDockObjectNamespaceWorker.h
// 作用说明：
// 1) 提供对象管理器命名空间枚举后台任务；
// 2) 提供符号链接目标解析能力；
// 3) 提供 NT 设备路径到 DOS 路径映射能力。
// ============================================================

#include "KernelDock.h"

#include <vector> // std::vector：承载输出列表。

// runObjectNamespaceSnapshotTask：
// - 作用：枚举对象管理器关键目录并生成表格行。
// - 调用：建议在后台线程调用。
// - 入参 rowsOut：传出枚举结果（函数内会先清空）。
// - 入参 errorTextOut：传出致命错误文本（成功时清空）。
// - 返回：true=任务可用；false=致命错误（如 Nt API 装载失败）。
bool runObjectNamespaceSnapshotTask(std::vector<KernelObjectNamespaceEntry>& rowsOut, QString& errorTextOut);

// queryObjectNamespaceSymbolicLinkTarget：
// - 作用：按对象路径解析符号链接目标。
// - 调用：右键菜单“解析符号链接目标”动作复用。
// - 入参 symbolicLinkPathText：目标符号链接完整路径（\??\C: 这类路径需以 \ 开头）。
// - 传出 targetTextOut：解析到的目标路径。
// - 传出 statusTextOut：状态文本（含失败原因）。
// - 返回：true=解析成功；false=解析失败。
bool queryObjectNamespaceSymbolicLinkTarget(
    const QString& symbolicLinkPathText,
    QString& targetTextOut,
    QString& statusTextOut);

// queryDosPathCandidatesByNtPath：
// - 作用：把 NT 设备路径（如 \Device\HarddiskVolume3\Windows）映射为 DOS 路径候选。
// - 调用：对象表右键菜单“尝试映射 DOS 路径”动作复用。
// - 入参 ntPathText：NT 路径。
// - 返回：DOS 路径候选列表（可能为空）。
std::vector<QString> queryDosPathCandidatesByNtPath(const QString& ntPathText);
