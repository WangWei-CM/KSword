#pragma once

// ============================================================
// KernelDockQueryWorker.h
// 作用说明：
// 1) 提供 KernelDock 后台线程可复用的数据采集函数；
// 2) 将耗时的 NtQuery* 调用与原始数据解析从 UI 文件剥离；
// 3) 保持 KernelDock.cpp 小于 1000 行，便于维护与审计。
// ============================================================

#include "KernelDock.h"

#include <vector> // std::vector：承载结果列表。

// runKernelTypeSnapshotTask：
// - 作用：采集“内核对象类型”页需要的对象类型编号/名称/统计数据。
// - 调用：在后台线程调用，禁止在 UI 线程直接长时间执行。
// - 入参 rowsOut：输出对象类型行列表（函数内会先清空后写入）。
// - 入参 errorTextOut：失败原因文本（成功时会清空）。
// - 返回：true 表示成功，false 表示失败。
bool runKernelTypeSnapshotTask(std::vector<KernelObjectTypeEntry>& rowsOut, QString& errorTextOut);

// runNtQuerySnapshotTask：
// - 作用：采集“NtQuery 信息”页需要的常见 NtQuery*Information 调用结果。
// - 调用：在后台线程调用，禁止在 UI 线程直接长时间执行。
// - 入参 rowsOut：输出 NtQuery 结果行列表（函数内会先清空后写入）。
// - 入参 errorTextOut：失败原因文本（成功时会清空）。
// - 返回：true 表示成功，false 表示失败。
bool runNtQuerySnapshotTask(std::vector<KernelNtQueryResultEntry>& rowsOut, QString& errorTextOut);

