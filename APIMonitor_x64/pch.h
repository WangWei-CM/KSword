#ifndef PCH_H
#define PCH_H

// ============================================================
// pch.h
// 作用：
// 1) 放置 APIMonitor_x64 高频稳定头，降低重复编译开销；
// 2) 统一常用 STL 与 Windows 基础头，避免各 cpp 分散重复包含；
// 3) 保持预编译头本身尽量稳定，不把频繁改动的实现细节塞进来。
// ============================================================

#include "framework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#endif
