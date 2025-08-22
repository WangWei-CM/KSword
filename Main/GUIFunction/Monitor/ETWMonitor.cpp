#define CURRENT_MODULE C("ETW监控")
#include <windows.h>
#include <tdh.h> // 修正：确保包含tdh.h
#include "../../KswordTotalHead.h"
#include <shlwapi.h>

#include <evntrace.h>

#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <future>
#include <atomic>
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")
static inline std::string HrToStr(HRESULT hr) { // [NEW]
    char buf[32];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}
#define MAX_GUID_SIZE 64 // GUID字符串最大长度（包括结尾的null字符）
static bool firstRun = true; // 用于第一次运行时的初始化
// Provider及其事件ID列表
inline std::wstring GuidToWstring(const GUID& guid) {
    WCHAR guidBuf[MAX_GUID_SIZE] = { 0 };
    StringFromGUID2(guid, guidBuf, ARRAYSIZE(guidBuf));
    return std::wstring(guidBuf);
}
inline std::string GuidToString(const GUID& guid) {
    char buf[64];
    sprintf_s(buf, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}
// 存储单个ETW Provider的完整信息（微软官方字段+事件ID列表）
struct EtwProviderEvents {
    GUID guid;                          // Provider的GUID（来自TRACE_PROVIDER_INFO）
    std::wstring providerName;          // Provider名称（宽字符，保留原始编码）
    bool isFromMOF;                     // 来源：true=WMI MOF类，false=XML manifest
    std::vector<uint32_t> eventIds;     // Provider关联的事件ID列表
};

// 线程任务结构体：传递并行处理所需的参数
struct ETWEnumThreadTask {
    size_t startIdx;                    // 线程处理的起始索引（对应TRACE_PROVIDER_INFO数组）
    size_t endIdx;                      // 线程处理的结束索引（对应TRACE_PROVIDER_INFO数组）
    const TRACE_PROVIDER_INFO* traceProvArr; // 官方TRACE_PROVIDER_INFO数组（存储Provider基础信息）
    const BYTE* enumBufferBase;         // 枚举缓冲区基地址（计算名称偏移用）
    std::vector<EtwProviderEvents>* threadResult; // 线程本地结果容器
    int progressId;                     // 线程对应的进度条ID
    std::atomic<size_t>& completedCount;// 全局已完成计数（原子变量，线程安全）
    size_t totalCount;                  // 总Provider数量（计算总进度用）
	static int _TOTLA_PROGRESS_ID; // 全局总进度条ID（静态成员，所有线程共享）
}; std::vector<EtwProviderEvents> etwProviders;
int ETWEnumThreadTask::_TOTLA_PROGRESS_ID = 0;
// 线程处理函数：单个线程处理一段TRACE_PROVIDER_INFO数组
void ETWEnumThreadProc(ETWEnumThreadTask task) {
    
    size_t threadTotal = task.endIdx - task.startIdx; // 线程需处理的Provider总数
    WCHAR stepTextW[512] = { 0 };                       // 宽字符进度文本（避免编码问题）

    for (size_t i = task.startIdx; i < task.endIdx; ++i) {
        // 1. 计算当前线程内的进度（0.0~1.0）
        float threadProgress = static_cast<float>(i - task.startIdx + 1) / threadTotal;

        // 2. 提取当前Provider的基础信息（来自官方TRACE_PROVIDER_INFO）
        const TRACE_PROVIDER_INFO& currProv = task.traceProvArr[i];
        EtwProviderEvents providerData;

        // 2.1 填充GUID
        providerData.guid = currProv.ProviderGuid;

        // 2.2 填充Provider名称（通过缓冲区基地址+偏移计算，微软官方逻辑）
        if (currProv.ProviderNameOffset != 0) {
            LPWSTR nameW = reinterpret_cast<LPWSTR>(
                const_cast<BYTE*>(task.enumBufferBase) + currProv.ProviderNameOffset
                );
            providerData.providerName = (nameW != nullptr) ? nameW : L"Unknown Provider";
        }
        else {
            providerData.providerName = L"Unknown Provider";
        }

        // 2.3 填充来源（MOF/manifest，微软官方SchemaSource字段）
        providerData.isFromMOF = (currProv.SchemaSource != 0);

        // 3. 查询当前Provider的事件ID列表（保留原TdhEnumerateManifestProviderEvents逻辑）
        ULONG eventBufSize = 0;
        ULONG status = TdhEnumerateManifestProviderEvents(
            &providerData.guid,
            nullptr,
            &eventBufSize
        );

        if (status == ERROR_INSUFFICIENT_BUFFER && eventBufSize > 0) {
            std::vector<BYTE> eventBuf(eventBufSize);
            PROVIDER_EVENT_INFO* eventInfos = reinterpret_cast<PROVIDER_EVENT_INFO*>(eventBuf.data());

            status = TdhEnumerateManifestProviderEvents(
                &providerData.guid,
                eventInfos,
                &eventBufSize
            );

            if (status == ERROR_SUCCESS && eventInfos->NumberOfEvents > 0) {
                for (ULONG j = 0; j < eventInfos->NumberOfEvents; ++j) {
                    providerData.eventIds.push_back(eventInfos->EventDescriptorsArray[j].Id);
                }
            }
        }

        // 4. 更新线程进度条（宽字符转多字节，兼容Ksword的C()宏）
        swprintf_s(stepTextW, L"线程%d：%s（%s）",
            task.progressId,
            providerData.providerName.c_str(),
            GuidToWstring(providerData.guid).c_str()
        );

        char stepTextA[1024] = { 0 };
        WideCharToMultiByte(
            CP_ACP,
            0,
            stepTextW,
            -1,
            stepTextA,
            sizeof(stepTextA),
            NULL,
            NULL
        );
        kItem.SetProcess(task.progressId, C(stepTextA), threadProgress);

        // 5. 保存结果并更新全局已完成计数
        task.threadResult->push_back(providerData);
        task.completedCount++;

        // 6. 更新总进度条（10%初始化+90%处理进度）
        float totalProgress = 0.1f + 0.9f * static_cast<float>(task.completedCount) / task.totalCount;
        kItem.SetProcess(ETWEnumThreadTask::_TOTLA_PROGRESS_ID, C("并行枚举ETW Provider..."), totalProgress);
    }
}
void EnumerateAllEtwProvidersWithEvents() {
    std::vector<EtwProviderEvents> finalResult;
    ULONG status = ERROR_SUCCESS;
    PROVIDER_ENUMERATION_INFO* pProvEnum = nullptr; // 微软官方枚举结果结构体
    DWORD bufferSize = 0;
    const int THREAD_COUNT = 4;                     // 固定4线程
    int TOTAL_PROGRESS_ID;                // 总进度条ID（ID=0）
    int threadProgressIds[THREAD_COUNT] = { 0 };      // 4个线程进度条ID

    // --------------------------
    // 步骤1：创建5个进度条（1总+4线程）
    // --------------------------
    // 总进度条（第1个，描述整体状态）
    TOTAL_PROGRESS_ID = kItem.AddProcess(
        C("ETW Provider枚举（总进度）"),
        C("查询Provider列表大小..."),
        NULL,
        0.0f
    );
	ETWEnumThreadTask::_TOTLA_PROGRESS_ID = TOTAL_PROGRESS_ID; // 设置静态成员变量
    // 4个线程进度条（第2~5个，分别对应4个线程）
    for (int i = 0; i < THREAD_COUNT; ++i) {
        threadProgressIds[i] = kItem.AddProcess(
            C(std::string("线程") + std::to_string(i + 1) + "处理进度"),
            C("等待任务分配..."),
            NULL,
            0.0f
        );
    }

    // --------------------------
    // 步骤2：按微软官方逻辑枚举Provider列表
    // --------------------------
    // 首次调用：获取所需缓冲区大小（微软推荐逻辑）
    status = TdhEnumerateProviders(pProvEnum, &bufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER && status != ERROR_SUCCESS) {
        kItem.SetProcess(TOTAL_PROGRESS_ID, C("获取缓冲区大小失败"), 1.00f);
        kLog.err(C(std::string("TdhEnumerateProviders首次调用失败：") + HrToStr(status)), CURRENT_MODULE);
        if (pProvEnum != nullptr) {
            free(pProvEnum);
            pProvEnum = nullptr;
        }
        return; // 直接返回，避免后续处理
    }

    // 循环分配缓冲区（避免Provider列表动态变化导致大小不足，微软官方推荐）
    while (true) {
        // 释放旧缓冲区（若有），分配新缓冲区
        if (pProvEnum != nullptr) {
            free(pProvEnum);
            pProvEnum = nullptr;
        }
        pProvEnum = reinterpret_cast<PROVIDER_ENUMERATION_INFO*>(malloc(bufferSize));
        if (pProvEnum == nullptr) {
            kItem.SetProcess(TOTAL_PROGRESS_ID, C("分配缓冲区失败"), 1.00f);
            kLog.err(C(std::string("malloc失败，大小：") + std::to_string(bufferSize)), CURRENT_MODULE);
            return;
        }

        // 再次调用：获取Provider枚举结果
        status = TdhEnumerateProviders(pProvEnum, &bufferSize);
        if (status == ERROR_SUCCESS) {
            // 成功获取结果，检查是否有Provider
            if (pProvEnum->NumberOfProviders == 0) {
                kItem.SetProcess(TOTAL_PROGRESS_ID, C("未发现任何ETW Provider"), 1.00f);
                free(pProvEnum);
                pProvEnum = nullptr;
                return;
            }
            break; // 跳出循环，进入后续处理
        }
        else if (status != ERROR_INSUFFICIENT_BUFFER) {
            // 非缓冲区不足错误，终止流程
            kItem.SetProcess(TOTAL_PROGRESS_ID, C("枚举Provider失败"), 1.00f);
            kLog.err(C(std::string("TdhEnumerateProviders枚举失败：") + HrToStr(status)), CURRENT_MODULE);
            free(pProvEnum);
            pProvEnum = nullptr;
            return;
        }
        // 缓冲区不足，继续循环（bufferSize已被API更新为新需求）
    }

    // 提取微软枚举结果的核心数据
    size_t totalProvCount = pProvEnum->NumberOfProviders;
    const TRACE_PROVIDER_INFO* pTraceProvArr = pProvEnum->TraceProviderInfoArray;
    const BYTE* enumBufferBase = reinterpret_cast<BYTE*>(pProvEnum); // 缓冲区基地址（计算名称偏移用）

    // --------------------------
    // 步骤3：初始化进度+分配线程任务
    // --------------------------
    // 更新总进度条：初始化完成（10%）
    kItem.SetProcess(TOTAL_PROGRESS_ID,
        C("开始并行处理（共" + std::to_string(totalProvCount) + "个Provider）"),
        0.1f
    );

    // 更新线程进度条：开始处理任务
    for (int i = 0; i < THREAD_COUNT; ++i) {
        kItem.SetProcess(threadProgressIds[i], C("启动处理任务"), 0.0f);
    }

    // 均分任务（最后一个线程处理剩余部分，避免遗漏）
    size_t perThreadCount = totalProvCount / THREAD_COUNT;
    std::atomic<size_t> completedCount(0); // 原子计数：确保线程安全
    std::vector<std::thread> threads;
    std::vector<std::vector<EtwProviderEvents>> threadLocalResults(THREAD_COUNT); // 线程本地结果

    // 启动4个线程，分配任务
    for (int i = 0; i < THREAD_COUNT; ++i) {
        size_t taskStart = i * perThreadCount;
        size_t taskEnd = (i == THREAD_COUNT - 1) ? totalProvCount : (i + 1) * perThreadCount;

        threads.emplace_back(ETWEnumThreadProc, ETWEnumThreadTask{
            taskStart,
            taskEnd,
            pTraceProvArr,
            enumBufferBase,
            &threadLocalResults[i],
            threadProgressIds[i],
            std::ref(completedCount),
            totalProvCount
            });
    }

    // --------------------------
    // 步骤4：等待线程完成+合并结果
    // --------------------------
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // 合并所有线程的本地结果到最终结果
    for (auto& localResult : threadLocalResults) {
        finalResult.insert(finalResult.end(),
            std::make_move_iterator(localResult.begin()),
            std::make_move_iterator(localResult.end())
        );
    }

    // --------------------------
    // 步骤5：更新进度为“枚举完成”
    // --------------------------
    kItem.SetProcess(TOTAL_PROGRESS_ID,
        C("枚举完成（共" + std::to_string(finalResult.size()) + "个Provider）"),
        1.00f
    );
    for (int i = 0; i < THREAD_COUNT; ++i) {
        kItem.SetProcess(threadProgressIds[i], C("处理完成"), 1.00f);
    }

    // 赋值给全局变量
    etwProviders = std::move(finalResult);

    // 释放微软枚举结果缓冲区（微软官方示例的内存释放逻辑）
    if (pProvEnum != nullptr) {
        free(pProvEnum);
        pProvEnum = nullptr;
    }
}


// 线程任务数据结构



// 扩展事件结构，包含更详细信息
struct ETWEventRecord {
    uint64_t id = 0;
    std::string timestamp;       // 精确到毫秒的时间戳
    std::string providerGuid;    // Provider GUID字符串
    std::string providerName;    // Provider名称（友好名称）
    uint32_t eventId = 0;
    uint8_t level = 0;           // 事件级别（1-5）
    uint8_t opcode = 0;          // 操作码
    std::string taskName;        // 任务名称
    std::string description;     // 事件详细描述（包含属性）
};

class ETWMonitor {
public:
    ETWMonitor(size_t maxEvents = 2000) : maxEventCount(maxEvents) {}
    ~ETWMonitor() { Stop(); }

    // 启动监控（支持初始Provider列表）
    bool Start(const std::vector<GUID>& providers = {});
    // 停止监控
    void Stop();
    // 获取事件列表（支持过滤）
    std::vector<ETWEventRecord> GetEvents(
        uint32_t eventId = 0,          // 0表示不过滤
        const std::string& provider = "",  // 空字符串表示不过滤
        uint8_t minLevel = 0           // 0表示不限制
    );
    // 动态添加Provider
    bool AddProvider(const GUID& provider, uint8_t level = TRACE_LEVEL_INFORMATION);
    // 移除Provider
    bool RemoveProvider(const GUID& provider);
    // 清空事件列表
    void ClearEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.clear();
    }
    bool IsRunning() const { return running.load(); }
    static std::string GetProviderName(const GUID& providerGuid);
private:
    std::atomic<bool> running{false};
    std::atomic<uint64_t> nextId{1};
    size_t maxEventCount;          // 最大事件缓存数
    std::vector<ETWEventRecord> events;
    std::mutex eventsMutex;
    std::thread traceThread;

    TRACEHANDLE sessionHandle = 0;
    EVENT_TRACE_PROPERTIES* sessionProps = nullptr;
    WCHAR sessionName[128] = L"KswordETWSession";
    std::vector<GUID> providerGuids;
    std::mutex providerMutex;      // 保护providerGuids的线程安全

    // 事件回调函数
    static VOID WINAPI EventCallback(EVENT_RECORD* pEventRecord);
    // 跟踪循环
    void TraceLoop();
    // 添加事件到列表
    void AddEvent(const ETWEventRecord& ev);
    // 解析Provider名称（GUID->名称）

    // 解析事件详情（包含任务名称、属性等）
    std::string ParseEventDetails(EVENT_RECORD* pEventRecord, std::string& taskName);
    // 格式化时间戳（精确到毫秒）
    std::string FormatTimestamp(ULONGLONG timeStamp);
};
std::string ETWMonitor::GetProviderName(const GUID& providerGuid) {
    // 只返回GUID字符串，兼容所有Windows版本
    return GuidToString(providerGuid);
}

// 添加事件到缓存
void ETWMonitor::AddEvent(const ETWEventRecord& ev) {
    ETWEventRecord rec = ev;
    rec.id = nextId.fetch_add(1, std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(eventsMutex);
    events.insert(events.begin(), rec);  // 新事件插入头部
    // 超过最大数量时移除最旧事件
    if (events.size() > maxEventCount) {
        events.pop_back();
    }
}

// 格式化时间戳（FileTime->yyyy-MM-dd HH:mm:ss.fff）
std::string ETWMonitor::FormatTimestamp(ULONGLONG timeStamp) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    uli.QuadPart = timeStamp;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    SYSTEMTIME st, lst;
    if (!FileTimeToSystemTime(&ft, &st) || !SystemTimeToTzSpecificLocalTime(nullptr, &st, &lst)) {
        return "InvalidTime";
    }

    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        lst.wYear, lst.wMonth, lst.wDay,
        lst.wHour, lst.wMinute, lst.wSecond, lst.wMilliseconds);
    return buf;
}

// 获取Provider名称（通过GUID）



// 解析事件详情（任务名称、属性等）
std::string ETWMonitor::ParseEventDetails(EVENT_RECORD* pEventRecord, std::string& taskName) {
    std::stringstream ss;
    const auto& desc = pEventRecord->EventHeader.EventDescriptor;
    ss << "Level=" << (int)desc.Level 
       << ", Opcode=" << (int)desc.Opcode 
       << ", Task=" << desc.Task;
    // 解析任务名称（使用TdhGetEventInformation获取TRACE_EVENT_INFO）
    ULONG infoSize = 0;
    ULONG status = TdhGetEventInformation(pEventRecord, 0, nullptr, nullptr, &infoSize);
    if (status != ERROR_INSUFFICIENT_BUFFER) {
        ss << " | 属性解析失败: " << HrToStr(status);
        taskName = "UnknownTask";
        return ss.str();
    }
    TRACE_EVENT_INFO* pInfo = (TRACE_EVENT_INFO*)LocalAlloc(LPTR, infoSize);
    if (!pInfo) {
        ss << " | 内存分配失败";
        taskName = "UnknownTask";
        return ss.str();
    }
    status = TdhGetEventInformation(pEventRecord, 0, nullptr, pInfo, &infoSize);
    if (status == ERROR_SUCCESS) {
        // 任务名
        if (pInfo->TaskNameOffset) {
            PWSTR taskW = (PWSTR)((BYTE*)pInfo + pInfo->TaskNameOffset);
            taskName = WstringToString(std::wstring(taskW));
            ss << " (" << taskName << ")";
        } else {
            taskName = "UnknownTask";
        }
        // 属性名列表
        ss << " | 属性:";
        for (ULONG i = 0; i < pInfo->PropertyCount; ++i) {
            PWSTR propName = (PWSTR)((BYTE*)pInfo + pInfo->EventPropertyInfoArray[i].NameOffset);
            ss << " " << WstringToString(std::wstring(propName));
        }
    } else {
        ss << " | 属性解析失败: " << HrToStr(status);
        taskName = "UnknownTask";
    }
    LocalFree(pInfo);
    return ss.str();
}

// 启动监控
bool ETWMonitor::Start(const std::vector<GUID>& providers) {
    if (running) {
        kLog.warn(C("ETW监控已在运行中"), CURRENT_MODULE);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        providerGuids = providers;
    }
    DWORD propsSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(sessionName);
    sessionProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, propsSize);
    if (!sessionProps) {
        kLog.err(C("分配EVENT_TRACE_PROPERTIES失败"), CURRENT_MODULE);
        return false;
    }
    ZeroMemory(sessionProps, propsSize);
    sessionProps->Wnode.BufferSize = propsSize;
    sessionProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    sessionProps->Wnode.ClientContext = 1;
    sessionProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    sessionProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    ULONG status = StartTrace(&sessionHandle, sessionName, sessionProps);
    if (status != ERROR_SUCCESS) {
        std::stringstream ss;
        ss << "启动跟踪会话失败: " << HrToStr(status);
        if (status == ERROR_ALREADY_EXISTS) {
            ss << "（会话已存在，尝试停止现有会话）";
            ControlTrace(0, sessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
            status = StartTrace(&sessionHandle, sessionName, sessionProps);
        }
        if (status != ERROR_SUCCESS) {
            kLog.err(C(ss.str()), CURRENT_MODULE);
            LocalFree(sessionProps);
            sessionProps = nullptr;
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        for (const auto& guid : providerGuids) {
            status = EnableTraceEx2(
                sessionHandle,
                &guid,
                EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                TRACE_LEVEL_INFORMATION,
                0xFFFFFFFFFFFFFFFF,
                0,
                0,
                nullptr
            );
            if (status != ERROR_SUCCESS) {
                std::stringstream ss;
                ss << "启用Provider失败，GUID: " << GuidToString(guid) << "，错误: " << HrToStr(status);
                kLog.warn(C(ss.str()), CURRENT_MODULE);
            } else {
                std::stringstream ss;
                ss << "已启用Provider: " << GetProviderName(guid);
                kLog.info(C(ss.str()), CURRENT_MODULE);
            }
        }
    }
    running = true;
    traceThread = std::thread(&ETWMonitor::TraceLoop, this);
    kLog.info(C("ETW监控已启动"), CURRENT_MODULE);
    return true;
}

// 停止监控
void ETWMonitor::Stop() {
    if (!running) return;

    running = false;
    // 停止跟踪会话
    if (sessionHandle != 0 && sessionProps != nullptr) {
        ControlTrace(sessionHandle, sessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
        sessionHandle = 0;
        kLog.info(C("跟踪会话已停止"), CURRENT_MODULE);
    }
    // 释放资源
    if (sessionProps != nullptr) {
        LocalFree(sessionProps);
        sessionProps = nullptr;
    }
    // 等待线程结束
    if (traceThread.joinable()) {
        traceThread.join();
    }
    // 清空Provider列表
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        providerGuids.clear();
    }
    kLog.info(C("ETW监控已停止"), CURRENT_MODULE);
}

// 动态添加Provider
bool ETWMonitor::AddProvider(const GUID& provider, uint8_t level) {
    if (!running) {
        kLog.warn(C("监控未运行，无法添加Provider"), CURRENT_MODULE);
        return false;
    }
    std::lock_guard<std::mutex> lock(providerMutex);
    // 检查是否已存在
    if (std::find(providerGuids.begin(), providerGuids.end(), provider) != providerGuids.end()) {
        std::stringstream ss;
        ss << "Provider已存在: " << GetProviderName(provider);
        kLog.info(C(ss.str()), CURRENT_MODULE);
        return true;
    }

    // 启用Provider
    ULONG status = EnableTraceEx2(
        sessionHandle,
        &provider,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        level,
        0xFFFFFFFFFFFFFFFF,
        0,
        0,
        nullptr
    );
    if (status != ERROR_SUCCESS) {
        std::stringstream ss;
        ss << "添加Provider失败: " << GuidToString(provider) << "，错误: " << HrToStr(status);
        kLog.err(C(ss.str()), CURRENT_MODULE);
        return false;
    }

    providerGuids.push_back(provider);
    std::stringstream ss;
    ss << "已添加Provider: " << GetProviderName(provider);
    kLog.info(C(ss.str()), CURRENT_MODULE);
    return true;
}

// 移除Provider
bool ETWMonitor::RemoveProvider(const GUID& provider) {
    if (!running) return false;

    std::lock_guard<std::mutex> lock(providerMutex);
    auto it = std::find(providerGuids.begin(), providerGuids.end(), provider);
    if (it == providerGuids.end()) {
        std::stringstream ss;
        ss << "Provider不存在: " << GuidToString(provider);
        kLog.warn(C(ss.str()), CURRENT_MODULE);
        return false;
    }

    // 禁用Provider
    ULONG status = EnableTraceEx2(
        sessionHandle,
        &provider,
        EVENT_CONTROL_CODE_DISABLE_PROVIDER,
        0, 0, 0, 0, nullptr
    );
    if (status != ERROR_SUCCESS) {
        std::stringstream ss;
        ss << "禁用Provider失败: " << GuidToString(provider) << "，错误: " << HrToStr(status);
        kLog.warn(C(ss.str()), CURRENT_MODULE);
    }

    providerGuids.erase(it);
    std::stringstream ss;
    ss << "已移除Provider: " << GetProviderName(provider);
    kLog.info(C(ss.str()), CURRENT_MODULE);
    return true;
}

// 获取过滤后的事件列表
std::vector<ETWEventRecord> ETWMonitor::GetEvents(uint32_t eventId, const std::string& provider, uint8_t minLevel) {
    std::lock_guard<std::mutex> lock(eventsMutex);
    std::vector<ETWEventRecord> filtered;

    for (const auto& ev : events) {
        // 事件ID过滤
        if (eventId != 0 && ev.eventId != eventId) continue;
        // Provider过滤（支持GUID或名称）
        if (!provider.empty() && ev.providerGuid != provider && ev.providerName != provider) continue;
        // 级别过滤（只保留>=minLevel的事件）
        if (minLevel != 0 && ev.level < minLevel) continue;

        filtered.push_back(ev);
    }

    return filtered;
}

// 跟踪循环（处理实时事件）
void ETWMonitor::TraceLoop() {
    EVENT_TRACE_LOGFILE logFile = {0};
    logFile.LoggerName = sessionName;
    logFile.EventRecordCallback = ETWMonitor::EventCallback;  // 设置回调
    logFile.Context = this;  // 传递当前实例作为上下文
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;

    // 打开跟踪会话
    TRACEHANDLE handle = OpenTrace(&logFile);
    if (handle == INVALID_PROCESSTRACE_HANDLE) {
        ULONG err = GetLastError();
        kLog.err(C(std::string("打开跟踪失败: ") + HrToStr(err)), CURRENT_MODULE);
        running = false;
        return;
    }

    kLog.info(C("开始处理ETW事件"), CURRENT_MODULE);
    // 处理事件（阻塞调用，直到Stop被调用）
    ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
        kLog.err(C(std::string("事件处理结束，错误: ") + HrToStr(status)), CURRENT_MODULE);
    }

    // 关闭跟踪
    CloseTrace(handle);
}

// 事件回调（解析并存储事件）
VOID WINAPI ETWMonitor::EventCallback(EVENT_RECORD* pEventRecord) {
    if (!pEventRecord || !pEventRecord->UserContext) return;

    ETWMonitor* monitor = reinterpret_cast<ETWMonitor*>(pEventRecord->UserContext);
    ETWEventRecord ev;

    // 填充基本信息
    ev.timestamp = monitor->FormatTimestamp(pEventRecord->EventHeader.TimeStamp.QuadPart);
    ev.providerGuid = GuidToString(pEventRecord->EventHeader.ProviderId);  // 假设存在GUID转字符串工具
    ev.providerName = monitor->GetProviderName(pEventRecord->EventHeader.ProviderId);
    ev.eventId = pEventRecord->EventHeader.EventDescriptor.Id;
    ev.level = pEventRecord->EventHeader.EventDescriptor.Level;
    ev.opcode = pEventRecord->EventHeader.EventDescriptor.Opcode;

    // 解析事件详情
    ev.description = monitor->ParseEventDetails(pEventRecord, ev.taskName);

    // 添加到事件列表
    monitor->AddEvent(ev);
}

// 工具函数：GUID转字符串（辅助实现）

#include <atlstr.h>


// 全局ETW监视器实例
static ETWMonitor g_etwMonitor(300000);  // 最大缓存300000条事件

// 示例：常用Provider GUID（可扩展）
namespace EtwProviders {
    // TCP/IP 事件
    const GUID TcpIp = {0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA}};
    // 进程创建/退出
    const GUID Process = {0x22FB2CD6, 0x0E7B, 0x422B, {0x90, 0xC2, 0x34, 0x4F, 0x40, 0x9F, 0xAA, 0x3A}};
    // 注册表操作
    const GUID Registry = {0x70EB4F03, 0x1435, 0x49E7, {0x9A, 0x20, 0x84, 0x71, 0x93, 0x38, 0xC7, 0x93}};
}
  static char filterText[256] = "";
  static EtwProviderEvents s_selectedGUID; // 当前选中的Provider GUID
  static char customGuidInput[64] = { 0 }; // 用于输入GUID字符串
  static uint8_t customLevel = TRACE_LEVEL_INFORMATION; // 事件级别默认值
// UI集成示例（可在主线程调用）
void ETWMonitorMain() {
    if (ImGui::CollapsingHeader(C("ETW监控"))) {
        // 用于存储筛选文本的缓冲区
        if (firstRun) {
            firstRun = false;
            std::thread(EnumerateAllEtwProvidersWithEvents).detach(); // 异步加载ETW Providers
        }
        if (ImGui::BeginChild("ETWProviderScrolling", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetStyle().ItemSpacing.y), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            // 获取ETW Providers数据
            // 创建筛选输入框
            ImGui::InputText(C("筛选 Provider 或事件ID..."), filterText, IM_ARRAYSIZE(filterText));

            // 创建表格
            if (ImGui::BeginTable(C("EtwProvidersTable"), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                // 设置表头
                ImGui::TableSetupColumn(C("Provider GUID"), ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn(C("事件ID列表"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // 遍历所有Providers
                for (const auto& provider : etwProviders)
                {

                    // 将GUID转换为字符串以便显示和筛选
                    WCHAR guidStr[64];
                    StringFromGUID2(provider.guid, guidStr, IM_ARRAYSIZE(guidStr));
                    char ansiGuidStr[64];
                    WideCharToMultiByte(CP_ACP, 0, guidStr, -1, ansiGuidStr, sizeof(ansiGuidStr), NULL, NULL);

                    // 检查是否符合筛选条件
                    bool matchesFilter = false;

                    if (strstr(ansiGuidStr, filterText) != nullptr) {
                        matchesFilter = true;
                    }
                    else {
                        // 2. 新增：检查Provider Name是否匹配（不分大小写包含）
                        // 宽字符Provider Name转多字节（与filterText编码一致）
                        char providerNameA[512] = { 0 };
                        WideCharToMultiByte(
                            CP_ACP,
                            0,
                            provider.providerName.c_str(),  // 使用Provider的名称（宽字符）
                            -1,
                            providerNameA,
                            sizeof(providerNameA),
                            NULL,
                            NULL
                        );
                        // 不分大小写模糊匹配（使用_stricmp逐字符比较子串）
                        if (_strnicmp(providerNameA, filterText, strlen(filterText)) != 0) {
                            // 若前缀不匹配，再检查是否包含（自定义不分大小写strstr）
                            auto w2a = [](const wchar_t* wstr) { char b[512] = { 0 }; WideCharToMultiByte(CP_ACP, 0, wstr, -1, b, 512, nullptr, nullptr); return b; };
                            char* findPos = StrStrIA(w2a(provider.providerName.c_str()), filterText);
                            if (findPos != nullptr) {
                                matchesFilter = true;
                            }
                        }
                        else {
                            matchesFilter = true;
                        }

                        // 3. 原有：检查事件ID是否匹配（保留原逻辑，优先级低于Provider Name）
                        if (!matchesFilter) {
                            for (uint32_t eventId : provider.eventIds) {
                                char eventIdStr[32];
                                sprintf_s(eventIdStr, "%u", eventId);
                                if (strstr(eventIdStr, filterText) != nullptr) {
                                    matchesFilter = true;
                                    break;
                                }
                            }
                        }
                    }
                    // 只显示符合筛选条件的项
                    if (matchesFilter || filterText[0] == '\0')
                    {
                        ImGui::PushID(ansiGuidStr);
                        ImGui::TableNextRow();

                        // 显示GUID
                        ImGui::TableSetColumnIndex(0);
                        bool isSelected = (s_selectedGUID.guid == provider.guid);
                        if (ImGui::Selectable(C(ansiGuidStr), isSelected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                        {
                            s_selectedGUID = provider;
                        }
                        if (ImGui::BeginPopupContextItem("DLLModuleRightClick")) {  // 使用当前ID的上下文菜单

                            if (ImGui::MenuItem(C("复制GUID##" + GuidToString(provider.guid)))) {
                                ImGui::SetClipboardText(ansiGuidStr);
                            }ImGui::EndPopup();  // 关键修复：添加此行关闭弹出菜单
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(C(WCharToString(provider.providerName.c_str())));

                        // 显示事件ID列表
                        ImGui::TableSetColumnIndex(2);
                        std::string eventIdsStr;
                        for (size_t i = 0; i < provider.eventIds.size(); ++i)
                        {
                            if (i > 0)
                                eventIdsStr += ", ";
                            eventIdsStr += std::to_string(provider.eventIds[i]);
                        }
                        ImGui::Text(C("%s"), eventIdsStr.c_str());
                        ImGui::PopID();
                    }
                    
                }

                
            }ImGui::EndTable();
            
        }ImGui::EndChild();
        if (ImGui::GetCurrentContext() == nullptr) return;
        ImGui::Text(C("ETW事件监控"));
        ImGui::Separator();
        ImGui::InputText(C("自定义GUID"), customGuidInput, sizeof(customGuidInput), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SameLine();
        ImGui::Text(C("格式: {xxxxxxxx - xxxx - xxxx - xxxx - xxxxxxxxxxxx}"));

        ImGui::InputInt(C("事件级别"), (int*)&customLevel, 1, 1);
        ImGui::Text(C("级别范围: 1(详细) - 5(致命)"));
        if (customLevel < 1) customLevel = 1;
        if (customLevel > 5) customLevel = 5;

        // 新增：添加自定义Provider按钮
        if (ImGui::Button(C("添加自定义Provider")) && g_etwMonitor.IsRunning()) {
            GUID customGuid;
            int wideSize = MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, nullptr, 0);
            std::vector<wchar_t> wideGuid(wideSize);
            MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, wideGuid.data(), wideSize);
            // 尝试从输入字符串解析GUID
            HRESULT hr = IIDFromString(wideGuid.data(), &customGuid);
            if (SUCCEEDED(hr)) {
                bool result = g_etwMonitor.AddProvider(customGuid, customLevel);
                if (result) {
                    kLog.info(C("成功添加自定义Provider: " + std::string(customGuidInput)), CURRENT_MODULE);
                    // 清空输入框
                    memset(customGuidInput, 0, sizeof(customGuidInput));
                }
            }
            else {
                kLog.err(C("GUID格式无效，请检查输入"), CURRENT_MODULE);
            }
        }


        // 控制按钮
        if (ImGui::Button(C("启动监控##ETWMonitor")) && !g_etwMonitor.IsRunning()) {
            // 初始启动TCP/IP和进程监控
            std::vector<GUID> initialProviders = {
                EtwProviders::TcpIp,
                EtwProviders::Process
            };
            if (g_etwMonitor.Start(initialProviders)) {
                kLog.info(C("ETW监控启动成功"), CURRENT_MODULE);
            }
            else {
                kLog.err(C("ETW监控启动失败"), CURRENT_MODULE);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("停止监控##ETWMonitor")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(C("清空事件##ETWMonitor"))) {
            g_etwMonitor.ClearEvents();
        }

        // 动态添加Provider
        ImGui::Separator();
        ImGui::Text(C("动态Provider管理"));
        if (ImGui::Button(C("添加注册表监控")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.AddProvider(EtwProviders::Registry, TRACE_LEVEL_VERBOSE);
        }

        // 事件过滤控件
        static uint32_t filterEventId = 0;
        static char filterProvider[256] = { 0 };
        static uint8_t filterMinLevel = 0;
        ImGui::Separator();
        ImGui::Text(C("事件过滤"));
        ImGui::InputInt(C("事件ID"), (int*)&filterEventId);
        ImGui::InputText(C("Provider(GUID/名称)"), filterProvider, sizeof(filterProvider));
        ImGui::InputInt(C("最小级别(1-5)"), (int*)&filterMinLevel, 1, 1);
        if (filterMinLevel > 5) filterMinLevel = 5;

        // 事件列表展示
        ImGui::Separator();
        ImGui::Text(C("事件列表 (总数: %d)"), g_etwMonitor.GetEvents().size());
        if (ImGui::BeginChild("ETWEventTableescrolling", ImVec2(0, 300), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y - 10);
            if (ImGui::BeginTable("ETWEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, outerSize)) {
                ImGui::TableSetupColumn(C("ID"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("时间戳"), ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn(C("Provider"), ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn(C("事件ID"), ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn(C("级别"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("详情"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                auto events = g_etwMonitor.GetEvents(filterEventId, filterProvider, filterMinLevel);
                for (const auto& ev : events) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", ev.id);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ev.timestamp.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(ev.providerName.c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%u", ev.eventId);
                    ImGui::TableSetColumnIndex(4); ImGui::Text("%hhu", ev.level);
                    ImGui::TableSetColumnIndex(5); ImGui::TextWrapped("%s", ev.description.c_str());
                }
                
            }ImGui::EndTable();

        }            ImGui::EndChild();
    }
}

#undef CURRENT_MODULE