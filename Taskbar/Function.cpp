#include "Function.h"
#include <windows.h>
#include <qthread.h>
#pragma comment(lib, "Pdh.lib")

std::vector<int> cpuUsage;

void lockWorkstation() {
    // 调用系统API锁定工作站
    ::LockWorkStation();
}

// 2. 打开CMD函数
void openCmd() {
    STARTUPINFOA si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    // 创建新控制台窗口的CMD进程
    char cmd[] = "cmd.exe";
    if (::CreateProcessA(
        nullptr, cmd, nullptr, nullptr,
        FALSE, CREATE_NEW_CONSOLE,
        nullptr, nullptr, &si, &pi
    )) {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }
}

// 3. 用户自定义功能（此处预留接口，用户可自行实现）
void userCustomFunction() {
    // 用户在这里编写自己的逻辑

}

// 封装函数：返回各CPU核心占用率（整数百分比，0-100）
std::vector<int> getCPUCoreUsage() {
    std::vector<int> coreUsage;
    PDH_HQUERY hQuery = NULL;
    std::vector<PDH_HCOUNTER> coreCounters;

    // 1. 获取逻辑核心数量
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD numCores = sysInfo.dwNumberOfProcessors;
    if (numCores == 0) {
        return coreUsage;  // 无核心信息，返回空
    }

    // 2. 打开性能计数器查询
    if (PdhOpenQueryA(NULL, 0, &hQuery) != ERROR_SUCCESS) {
        return coreUsage;
    }

    // 3. 为每个核心创建计数器
    coreCounters.resize(numCores);
    bool countersCreated = true;
    for (DWORD i = 0; i < numCores; ++i) {
        char counterPath[256];
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

    // 4. 初始数据采集（基准值）
    if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) {
        PdhCloseQuery(hQuery);
        return coreUsage;
    }

    QThread::msleep(500);

    // 6. 第二次数据采集
    if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) {
        PdhCloseQuery(hQuery);
        return coreUsage;
    }

    // 7. 计算各核心占用率并格式化
    coreUsage.reserve(numCores);
    for (DWORD i = 0; i < numCores; ++i) {
        PDH_FMT_COUNTERVALUE counterValue;
        if (PdhGetFormattedCounterValue(
            coreCounters[i],
            PDH_FMT_DOUBLE,
            NULL,
            &counterValue
        ) != ERROR_SUCCESS) {
            coreUsage.push_back(0);  // 获取失败时返回0
            continue;
        }

        // 转换为整数并限制范围（0-100）
        int usage = static_cast<int>(std::round(counterValue.doubleValue));
        if (usage < 0) usage = 0;
        if (usage > 100) usage = 100;
        coreUsage.push_back(usage);
    }

    // 8. 清理资源
    for (auto counter : coreCounters) {
        PdhRemoveCounter(counter);
    }
    PdhCloseQuery(hQuery);
	cpuUsage = coreUsage;
    return coreUsage;
}