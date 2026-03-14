// hGet.h
#pragma once
#include <Windows.h>
#include <pdh.h>
#include <mutex>
#include <array>
#pragma comment(lib, "Pdh.lib")

// Lightweight snapshot of metrics to store in history (POD - safe to default construct)
struct HardwareSnapshot {
    double cpuUsage = 0.0;
    double memoryAvailable = 0.0;  // MB
    double memoryCommitted = 0.0;  // MB
    double memoryUsagePercent = 0.0;
    double diskUsagePercent = 0.0;
    double diskReadBytesPerSec = 0.0;
    double diskWriteBytesPerSec = 0.0;
};

class HardwareUsage {
private:
    HQUERY hQuery;
    PDH_HCOUNTER hCounterCPU;
    PDH_HCOUNTER hCounterMemAvailable;
    PDH_HCOUNTER hCounterMemCommitted;
    PDH_HCOUNTER hCounterMemUsage;
    PDH_HCOUNTER hCounterDiskTime;
    PDH_HCOUNTER hCounterDiskRead;
    PDH_HCOUNTER hCounterDiskWrite;

    // 初始化PDH查询和计数器
    bool InitializeCounters();

public:
    // 性能数据成员
    double cpuUsage;
    double memoryAvailable;  // MB
    double memoryCommitted;  // MB
    double memoryUsagePercent;
    double diskUsagePercent;
    double diskReadBytesPerSec;
    double diskWriteBytesPerSec;

    // 构造函数：初始化并立即获取数据
    HardwareUsage();
    // 析构函数：清理资源
    ~HardwareUsage();

    // 重新获取最新性能数据
    bool UpdateData();

    // 返回一个轻量级快照用于历史记录
    HardwareSnapshot GetSnapshot() const;
};

class HardwareMonitor {
public:
    static const int HISTORY_SIZE = 10;  // 10 samples history
private:

    std::array<HardwareSnapshot, HISTORY_SIZE> history;  // 循环队列，存储快照
    int currentIndex;  // 当前索引
    HANDLE hThread;    // 监控线程句柄
    bool isRunning;    // 线程运行标志
    std::mutex mtx;    // 互斥锁

    static DWORD WINAPI MonitorThread(LPVOID lpParam);  // 线程函数
    void UpdateHistory(const HardwareSnapshot& snap);  // 更新历史记录
    HardwareUsage* m_singleUsage;
public:

    HardwareMonitor();
    ~HardwareMonitor();
    bool StartMonitoring();  // 开始监控
    void StopMonitoring();   // 停止监控
    std::array<HardwareSnapshot, HISTORY_SIZE> GetHistory();  // 获取历史数据
};