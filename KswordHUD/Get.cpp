// Get.cpp
//#include "stdafx.h"

#include <iostream>
#include <mutex>
#include "hGet.h"

bool HardwareUsage::InitializeCounters() {
    PDH_STATUS status;

    // 初始化句柄
    hQuery = nullptr;
    hCounterCPU = nullptr;
    hCounterMemAvailable = nullptr;
    hCounterMemCommitted = nullptr;
    hCounterMemUsage = nullptr;
    hCounterDiskTime = nullptr;
    hCounterDiskRead = nullptr;
    hCounterDiskWrite = nullptr;

    // 打开本地机器的查询
    status = PdhOpenQuery(NULL, 0, &hQuery);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // 添加CPU计数器
    status = PdhAddCounterA(hQuery, "\\Processor(_Total)\\% Processor Time", 0, &hCounterCPU);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // 添加内存计数器
    status = PdhAddCounterA(hQuery, "\\Memory\\Available Bytes", 0, &hCounterMemAvailable);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = PdhAddCounterA(hQuery, "\\Memory\\Committed Bytes", 0, &hCounterMemCommitted);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = PdhAddCounterA(hQuery, "\\Memory\\% Committed Bytes In Use", 0, &hCounterMemUsage);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // 添加磁盘计数器
    status = PdhAddCounterA(hQuery, "\\PhysicalDisk(_Total)\\% Disk Time", 0, &hCounterDiskTime);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = PdhAddCounterA(hQuery, "\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &hCounterDiskRead);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = PdhAddCounterA(hQuery, "\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &hCounterDiskWrite);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // 收集初始数据
    status = PdhCollectQueryData(hQuery);
    return status == ERROR_SUCCESS;
}

HardwareUsage::HardwareUsage()
    : hQuery(nullptr), hCounterCPU(nullptr), hCounterMemAvailable(nullptr),
    hCounterMemCommitted(nullptr), hCounterMemUsage(nullptr),
    hCounterDiskTime(nullptr), hCounterDiskRead(nullptr), hCounterDiskWrite(nullptr),
    cpuUsage(0.0), memoryAvailable(0.0), memoryCommitted(0.0),
    memoryUsagePercent(0.0), diskUsagePercent(0.0),
    diskReadBytesPerSec(0.0), diskWriteBytesPerSec(0.0) {

    // 初始化计数器
    if (InitializeCounters()) {
        // 等待PDH收集有意义的样本（必须有两次采样才能得到速率类计数器）
        Sleep(1000);
        // 初始化时立即获取数据
        UpdateData();
    }
}

HardwareUsage::~HardwareUsage() {
    // 清理计数器
    if (hCounterCPU) PdhRemoveCounter(hCounterCPU);
    if (hCounterMemAvailable) PdhRemoveCounter(hCounterMemAvailable);
    if (hCounterMemCommitted) PdhRemoveCounter(hCounterMemCommitted);
    if (hCounterMemUsage) PdhRemoveCounter(hCounterMemUsage);
    if (hCounterDiskTime) PdhRemoveCounter(hCounterDiskTime);
    if (hCounterDiskRead) PdhRemoveCounter(hCounterDiskRead);
    if (hCounterDiskWrite) PdhRemoveCounter(hCounterDiskWrite);

    // 关闭查询
    if (hQuery) PdhCloseQuery(hQuery);
}

bool HardwareUsage::UpdateData() {
    PDH_STATUS status;
    PDH_FMT_COUNTERVALUE counterValue;

    // 收集最新数据
    status = PdhCollectQueryData(hQuery);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // 获取CPU使用率
    if (PdhGetFormattedCounterValue(hCounterCPU, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        cpuUsage = counterValue.doubleValue;
    }

    // 获取可用内存(转换为MB)
    if (PdhGetFormattedCounterValue(hCounterMemAvailable, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        memoryAvailable = counterValue.doubleValue / (1024.0 * 1024.0);
    }

    // 获取已提交内存(转换为MB)
    if (PdhGetFormattedCounterValue(hCounterMemCommitted, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        memoryCommitted = counterValue.doubleValue / (1024.0 * 1024.0);
    }

    // 获取内存使用率
    if (PdhGetFormattedCounterValue(hCounterMemUsage, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        memoryUsagePercent = counterValue.doubleValue;
    }

    // 获取磁盘使用率
    if (PdhGetFormattedCounterValue(hCounterDiskTime, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        diskUsagePercent = counterValue.doubleValue;
    }

    // 获取磁盘读取速度
    if (PdhGetFormattedCounterValue(hCounterDiskRead, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        diskReadBytesPerSec = counterValue.doubleValue;
    }

    // 获取磁盘写入速度
    if (PdhGetFormattedCounterValue(hCounterDiskWrite, PDH_FMT_DOUBLE, NULL, &counterValue) == ERROR_SUCCESS) {
        diskWriteBytesPerSec = counterValue.doubleValue;
    }

    return true;
}

// 返回轻量级快照
HardwareSnapshot HardwareUsage::GetSnapshot() const {
    HardwareSnapshot snap;
    snap.cpuUsage = cpuUsage;
    snap.memoryAvailable = memoryAvailable;
    snap.memoryCommitted = memoryCommitted;
    snap.memoryUsagePercent = memoryUsagePercent;
    snap.diskUsagePercent = diskUsagePercent;
    snap.diskReadBytesPerSec = diskReadBytesPerSec;
    snap.diskWriteBytesPerSec = diskWriteBytesPerSec;
    return snap;
}

// 构造函数：仅初始化1次HardwareUsage
HardwareMonitor::HardwareMonitor()
    : currentIndex(0), hThread(nullptr), isRunning(false), m_singleUsage(nullptr) {
    // 初始化历史数组为零快照
    history.fill(HardwareSnapshot());
    // 唯一一次初始化PDH计数器（耗时仅这一次）
    m_singleUsage = new HardwareUsage();
}

// 析构函数：释放唯一实例
HardwareMonitor::~HardwareMonitor() {
    StopMonitoring();
    if (m_singleUsage) {
        delete m_singleUsage;
        m_singleUsage = nullptr;
    }
}

// 启动监控：线程启动后立即返回，不阻塞
bool HardwareMonitor::StartMonitoring() {
    if (isRunning) return false;
    isRunning = true;
    // 启动线程（线程内仅复用m_singleUsage，无重复初始化）
    hThread = CreateThread(nullptr, 0, MonitorThread, this, 0, nullptr);
    return hThread != nullptr;
}

// 停止监控
void HardwareMonitor::StopMonitoring() {
    if (!isRunning) return;
    isRunning = false;
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
        hThread = nullptr;
    }
}

// 更新历史记录（线程安全）
void HardwareMonitor::UpdateHistory(const HardwareSnapshot& snap) {
    std::lock_guard<std::mutex> lock(mtx);
    history[currentIndex] = snap;
    currentIndex = (currentIndex + 1) % HISTORY_SIZE;
}

// 线程函数：核心优化——仅复用m_singleUsage更新数据
DWORD WINAPI HardwareMonitor::MonitorThread(LPVOID lpParam) {
    HardwareMonitor* monitor = static_cast<HardwareMonitor*>(lpParam);
    if (!monitor->m_singleUsage) return 1;  // 防空指针

    while (monitor->isRunning) {
        // 仅更新数据（轻量操作，无初始化）
        if (monitor->m_singleUsage->UpdateData()) {
            // 复制数据到历史数组（使用轻量快照）
            HardwareSnapshot snap = monitor->m_singleUsage->GetSnapshot();
            monitor->UpdateHistory(snap);
        }
        // 每秒更新一次历史数据，确保PDH有时间采样，两次采样间隔用于速率计数器
        Sleep(1000);
    }
    return 0;
}

// 获取历史数据
std::array<HardwareSnapshot, HardwareMonitor::HISTORY_SIZE> HardwareMonitor::GetHistory() {
    std::lock_guard<std::mutex> lock(mtx);
    return history;
}