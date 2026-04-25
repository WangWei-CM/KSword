#pragma once

#include "KernelDock.h"

#include <vector>

// runSsdtSnapshotTask：
// - 作用：通过驱动 IOCTL 获取 SSDT 遍历快照。
// - 调用：在后台线程调用，禁止在 UI 线程直接长时间执行。
// - 入参 rowsOut：输出 SSDT 行列表（函数内会先清空后写入）。
// - 入参 errorTextOut：失败原因文本（成功时会清空）。
// - 返回：true 表示成功，false 表示失败。
bool runSsdtSnapshotTask(std::vector<KernelSsdtEntry>& rowsOut, QString& errorTextOut);

