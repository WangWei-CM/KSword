#include "Function.h"
#include <windows.h>
#include <qthread.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <mutex>
#include <cstdio>

#pragma comment(lib, "Pdh.lib")
#pragma comment(lib, "Iphlpapi.lib")

// 全局 CPU 核心占用率缓存：供 UI 定时读取。
std::vector<int> cpuUsage;

namespace {
// 网络速率计算的状态锁，避免异步线程并发访问导致数据竞争。
std::mutex g_networkRateMutex;

// 上一次采样的累积字节数，用于计算每秒增量。
bool g_networkRateInitialized = false;
std::uint64_t g_prevUploadBytes = 0;
std::uint64_t g_prevDownloadBytes = 0;
ULONGLONG g_prevNetworkTickMs = 0;

// 读取当前所有可用网卡的总收发字节数（累积值，不是瞬时速率）。
bool queryNetworkTotalBytes(std::uint64_t& totalUploadBytes,
                            std::uint64_t& totalDownloadBytes) {
    totalUploadBytes = 0;
    totalDownloadBytes = 0;

    PMIB_IF_TABLE2 interfaceTable = nullptr;
    if (GetIfTable2(&interfaceTable) != NO_ERROR || interfaceTable == nullptr) {
        return false;
    }

    for (ULONG i = 0; i < interfaceTable->NumEntries; ++i) {
        const MIB_IF_ROW2& row = interfaceTable->Table[i];

        // 只统计在线接口，并过滤回环口，避免将本机环回流量计入网速。
        if (row.OperStatus != IfOperStatusUp) {
            continue;
        }
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        totalDownloadBytes += row.InOctets;
        totalUploadBytes += row.OutOctets;
    }

    FreeMibTable(interfaceTable);
    return true;
}
}

void lockWorkstation() {
    // 直接调用系统 API 锁定当前会话。
    ::LockWorkStation();
}

// 打开 CMD 控制台。
void openCmd() {
    STARTUPINFOA si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);

    char cmd[] = "cmd.exe";
    if (::CreateProcessA(
        nullptr,
        cmd,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }
}

// 用户自定义扩展点。
void userCustomFunction() {
    // 当前预留为空实现。
}

// 获取各逻辑核心占用率（返回 0-100 整数），并同步更新全局缓存 cpuUsage。
std::vector<int> getCPUCoreUsage() {
    std::vector<int> coreUsage;
    PDH_HQUERY hQuery = NULL;
    std::vector<PDH_HCOUNTER> coreCounters;

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD numCores = sysInfo.dwNumberOfProcessors;
    if (numCores == 0) {
        return coreUsage;
    }

    if (PdhOpenQueryA(NULL, 0, &hQuery) != ERROR_SUCCESS) {
        return coreUsage;
    }

    coreCounters.resize(numCores);
    bool countersCreated = true;
    for (DWORD i = 0; i < numCores; ++i) {
        char counterPath[256] = { 0 };
        if (sprintf_s(counterPath, "\\Processor(%d)\\%% Processor Time", i) < 0) {
            countersCreated = false;
            break;
        }
        if (PdhAddCounterA(hQuery, counterPath, 0, &coreCounters[i]) != ERROR_SUCCESS) {
            countersCreated = false;
            break;
        }
    }

    if (!countersCreated) {
        PdhCloseQuery(hQuery);
        return coreUsage;
    }

    if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) {
        PdhCloseQuery(hQuery);
        return coreUsage;
    }

    // PDH 需要两次采样间隔才能得到有效利用率。
    QThread::msleep(500);
    if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) {
        PdhCloseQuery(hQuery);
        return coreUsage;
    }

    coreUsage.reserve(numCores);
    for (DWORD i = 0; i < numCores; ++i) {
        PDH_FMT_COUNTERVALUE counterValue;
        if (PdhGetFormattedCounterValue(
            coreCounters[i],
            PDH_FMT_DOUBLE,
            NULL,
            &counterValue
        ) != ERROR_SUCCESS) {
            coreUsage.push_back(0);
            continue;
        }

        int usage = static_cast<int>(std::round(counterValue.doubleValue));
        if (usage < 0) {
            usage = 0;
        }
        if (usage > 100) {
            usage = 100;
        }
        coreUsage.push_back(usage);
    }

    for (auto counter : coreCounters) {
        PdhRemoveCounter(counter);
    }
    PdhCloseQuery(hQuery);

    cpuUsage = coreUsage;
    return coreUsage;
}

// 获取当前网速（字节/秒），函数本身不涉及 UI，仅提供纯数据。
NetworkSpeedRate getNetworkSpeedRate() {
    NetworkSpeedRate rate = { 0, 0 };

    std::uint64_t currentUploadBytes = 0;
    std::uint64_t currentDownloadBytes = 0;
    if (!queryNetworkTotalBytes(currentUploadBytes, currentDownloadBytes)) {
        return rate;
    }

    const ULONGLONG nowTickMs = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_networkRateMutex);

    if (!g_networkRateInitialized) {
        g_prevUploadBytes = currentUploadBytes;
        g_prevDownloadBytes = currentDownloadBytes;
        g_prevNetworkTickMs = nowTickMs;
        g_networkRateInitialized = true;
        return rate;
    }

    const ULONGLONG elapsedMs = nowTickMs - g_prevNetworkTickMs;
    if (elapsedMs > 0) {
        if (currentUploadBytes >= g_prevUploadBytes) {
            rate.uploadBytesPerSecond =
                ((currentUploadBytes - g_prevUploadBytes) * 1000ULL) / elapsedMs;
        }
        if (currentDownloadBytes >= g_prevDownloadBytes) {
            rate.downloadBytesPerSecond =
                ((currentDownloadBytes - g_prevDownloadBytes) * 1000ULL) / elapsedMs;
        }
    }

    g_prevUploadBytes = currentUploadBytes;
    g_prevDownloadBytes = currentDownloadBytes;
    g_prevNetworkTickMs = nowTickMs;

    return rate;
}
