#define CURRENT_MODULE C("ETW监控")
#include "../../KswordTotalHead.h"

#include <windows.h>
#include <evntrace.h>
#include <tdh.h> // 修正：确保包含tdh.h
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")
static inline std::string HrToStr(HRESULT hr) { // [NEW]
    char buf[32];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}
inline std::string GuidToString(const GUID& guid) {
    char buf[64];
    sprintf_s(buf, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}


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

// UI集成示例（可在主线程调用）
void ETWMonitorMain() {
    if (ImGui::CollapsingHeader(C("ETW监控"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::GetCurrentContext() == nullptr) return;

        ImGui::Text(C("ETW事件监控"));
        ImGui::Separator();

        // 控制按钮
        if (ImGui::Button(C("启动监控")) && !g_etwMonitor.IsRunning()) {
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
        if (ImGui::Button(C("停止监控")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(C("清空事件"))) {
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
            ImGui::EndTable();
        }
    }
}

#undef CURRENT_MODULE