#pragma once

// ============================================================
// framework.h
// 作用：
// 1) 统一 APIMonitor_x64 的底层 Windows 头入口；
// 2) 先引入 Winsock 相关头，避免与 Windows.h 顺序冲突；
// 3) 为后续 Hook/命名管道/注册表/网络代码提供基础平台定义。
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Winsock2.h>
#include <Ws2tcpip.h>
#include <Windows.h>
