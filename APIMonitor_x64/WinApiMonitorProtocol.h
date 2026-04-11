#pragma once

// ============================================================
// APIMonitor_x64/WinApiMonitorProtocol.h
// 作用：
// 1) 为 DLL 工程提供稳定的本地包含入口；
// 2) 把实际协议定义统一转发到仓库根目录 shared；
// 3) 避免 core/hook 层直接写过长的跨目录相对路径。
// ============================================================

#include "../shared/WinApiMonitorProtocol.h"
