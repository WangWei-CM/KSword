#pragma once

#include <pdh.h>
#include <vector>
#include <cmath>  // 用于round取整
void lockWorkstation();
void openCmd();
void userCustomFunction();
std::vector<int> getCPUCoreUsage();