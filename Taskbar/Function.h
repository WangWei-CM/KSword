#pragma once

#include <pdh.h>
#include <vector>
#include <cstdint>
#include <cmath>  // 用于 round 取整

// 网络速度数据：单位统一为字节/秒，供 UI 层异步读取。
struct NetworkSpeedRate {
    std::uint64_t uploadBytesPerSecond;   // 上行速度，单位 B/s
    std::uint64_t downloadBytesPerSecond; // 下行速度，单位 B/s
};

void lockWorkstation();
void openCmd();
void userCustomFunction();
std::vector<int> getCPUCoreUsage();
NetworkSpeedRate getNetworkSpeedRate();
