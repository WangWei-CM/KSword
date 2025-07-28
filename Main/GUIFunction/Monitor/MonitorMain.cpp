#include <Wbemidl.h>
#define NTDDI_VERSION 0x06030000
#define _WIN32_WINNT 0x0603  // 表示 Windows 8.1 及以上
#include "../../KswordTotalHead.h"
#include <evntrace.h>
#include <tdh.h>
#include <comdef.h>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
// 若 evntrace.h 未定义，手动添加（放在 #include 之后）
#ifndef EVENT_TRACE_ENABLE_INFO_DEFINED
#define EVENT_TRACE_ENABLE_INFO_DEFINED

typedef struct _EVENT_TRACE_ENABLE_INFO {
    ULONG Version;               // 必须设为 EVENT_TRACE_ENABLE_INFO_VERSION (1)
    ULONG EnableProperty;        // 启用的属性（如 EVENT_ENABLE_PROPERTY_STACK_TRACE）
} EVENT_TRACE_ENABLE_INFO, * PEVENT_TRACE_ENABLE_INFO;

// 版本常量（官方文档规定）
#define EVENT_TRACE_ENABLE_INFO_VERSION 1

#endif  // EVENT_TRACE_ENABLE_INFO_DEFINED

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

using BYTE = unsigned char;

// 定义TCP/IP事件提供商GUID（缺失的定义）
// 这是Microsoft-Windows-TCPIP提供商的GUID
const GUID GUID_TCPIP_EVENTS = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };

// 事件记录类
class EventRecord {
public:
    std::string timestamp;
    std::string type;
    std::string source;
    std::string description;

    EventRecord(const std::string& ts, const std::string& t, const std::string& s, const std::string& d)
        : timestamp(ts), type(t), source(s), description(d) {
    }
};

// 全局事件监控器（核心类）
class EventMonitor {
private:
    bool isRunning = false;
    std::vector<EventRecord> events;
    std::mutex eventsMutex;
    std::thread monitoringThread;
    std::vector<std::string> selectedEventTypes;

    // WMI 相关变量
    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IUnsecuredApartment* pUnsecApp = nullptr;
    IUnknown* pStubUnk = nullptr;
    IWbemObjectSink* pStubSink = nullptr;

    // ETW 相关变量
    TRACEHANDLE etwSession = 0;
    EVENT_TRACE_PROPERTIES* etwProps = nullptr;
    WCHAR etwSessionName[256] = L"KswordETWMonitor";

    // WMI 事件接收回调类
    class EventSink : public IWbemObjectSink {
    private:
        LONG m_lRef = 0;
        EventMonitor* parent;

    public:
        explicit EventSink(EventMonitor* monitor) : parent(monitor) {}
        ~EventSink() = default;

        ULONG STDMETHODCALLTYPE AddRef() override {
            return InterlockedIncrement(&m_lRef);
        }

        ULONG STDMETHODCALLTYPE Release() override {
            LONG ref = InterlockedDecrement(&m_lRef);
            if (ref == 0) {
                delete this;
            }
            return ref;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
                *ppv = static_cast<IWbemObjectSink*>(this);
                AddRef();
                return WBEM_S_NO_ERROR;
            }
            return E_NOINTERFACE;
        }

        // IWbemObjectSink 接口方法
        HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
            // 这里放入之前修正的事件解析逻辑
            for (int i = 0; i < lObjectCount; ++i) {
                VARIANT vtProp;
                std::string type, source, description;

                // 1. 获取事件类型（__Class 属性）
                if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtProp, 0, 0))) {
                    if (vtProp.vt == VT_BSTR) {
                        type = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                    }
                    VariantClear(&vtProp);
                }

                // 2. 解析实例事件（__InstanceXXXEvent）的 TargetInstance 属性
                if (type == "__InstanceCreationEvent" || type == "__InstanceDeletionEvent" || type == "__InstanceModificationEvent") {
                    // 获取 TargetInstance（实际的进程对象）
                    if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"TargetInstance"), 0, &vtProp, 0, 0))) {
                        if (vtProp.vt == VT_DISPATCH) {
                            IWbemClassObject* pTargetInstance = nullptr;
                            if (SUCCEEDED(vtProp.pdispVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance))) {
                                // 从 TargetInstance 中获取进程名
                                VARIANT vtName;
                                if (SUCCEEDED(pTargetInstance->Get(_bstr_t(L"Name"), 0, &vtName, 0, 0))) {
                                    if (vtName.vt == VT_BSTR) {
                                        source = _com_util::ConvertBSTRToString(vtName.bstrVal);
                                    }
                                    VariantClear(&vtName);
                                }
                                pTargetInstance->Release();
                            }
                        }
                        VariantClear(&vtProp);
                    }
                    // 补充描述信息
                    if (type == "__InstanceCreationEvent") description = "Process started";
                    else if (type == "__InstanceDeletionEvent") description = "Process stopped";
                    // 在EventSink::Indicate中添加
                    else if (type == "MSFT_NetConnectionCreate") {
                        // 解析网络连接事件
                        if (SUCCEEDED(apObjArray[i]->Get(L"ProcessId", 0, &vtProp, 0, 0))) {
                            DWORD pid = vtProp.lVal;
                            source = "PID: " + std::to_string(pid);
                            VariantClear(&vtProp);
                        }
                        description = "Network connection created";
                    }
                    else description = "Process modified";
                }
                // 3. 解析 Win32_ProcessStartTrace/StopTrace 事件
                else if (type == "Win32_ProcessStartTrace" || type == "Win32_ProcessStopTrace") {
                    if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"ProcessName"), 0, &vtProp, 0, 0))) {
                        if (vtProp.vt == VT_BSTR) {
                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                        }
                        VariantClear(&vtProp);
                    }
                    description = type == "Win32_ProcessStartTrace" ? "Process start traced" : "Process stop traced";
                }

                // 4. 生成时间戳
                auto now = std::chrono::system_clock::now();
                std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
                std::stringstream ss;
                ss << std::put_time(std::localtime(&nowTime), "%Y-%m-%d %H:%M:%S");

                // 5. 添加事件（确保有有效信息）
                if (!source.empty()) {
                    parent->addEvent(EventRecord(ss.str(), type, source, description));
                }
            }
            return WBEM_S_NO_ERROR;
        }

        HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject* pObjParam) override {
            // 必须实现此方法（即使为空），否则类仍是抽象类
            return WBEM_S_NO_ERROR;
        }
    };

    // 初始化 WMI 连接
    bool initializeWMI() {
        HRESULT hres = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
        if (FAILED(hres)) {
            kLog.Add(Err, C("未能成功初始化WMI连接，第1步出现问题"));
            return false;
        }
        
        hres = CoInitializeSecurity(
            NULL, -1, NULL, NULL,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL, EOAC_NONE, NULL
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("未能成功初始化WMI连接，第2步出现问题"));
            return false;
        }

        hres = CoCreateInstance(
            CLSID_WbemLocator, NULL,
            CLSCTX_INPROC_SERVER, IID_IWbemLocator,
            reinterpret_cast<LPVOID*>(&pLoc)
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("未能成功初始化WMI连接，第3步出现问题"));
            return false;
        }

        hres = pLoc->ConnectServer(
            _bstr_t(L"ROOT\\CIMV2"),
            NULL, NULL, 0, NULL, 0, 0, &pSvc
        );
        if (FAILED(hres)) {
            pLoc->Release();
            CoUninitialize();
            kLog.Add(Err, C("未能成功初始化WMI连接，第4步出现问题"));
            return false;
        }

        hres = CoSetProxyBlanket(
            pSvc,
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            NULL,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL,
            EOAC_NONE
        );
        if (FAILED(hres)) {
            pSvc->Release();
            pLoc->Release();
            CoUninitialize();
            kLog.Add(Err, C("未能成功初始化WMI连接，第5步出现问题"));
            return false;
        }
        kLog.Add(Info, C("成功初始化WMI连接"));
        return true;
    }

    // 订阅 WMI 事件（动态拼接查询）
    bool subscribeToEvents() {
        HRESULT hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL,
            CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
            reinterpret_cast<void**>(&pUnsecApp));
        if (FAILED(hres)) {
            return false;
        }

        EventSink* pSink = new EventSink(this);
        pSink->AddRef();

        hres = pUnsecApp->CreateObjectStub(static_cast<IUnknown*>(pSink), &pStubUnk);
        if (FAILED(hres)) {
            pSink->Release();
            return false;
        }

        hres = pStubUnk->QueryInterface(IID_IWbemObjectSink,
            reinterpret_cast<void**>(&pStubSink));
        if (FAILED(hres)) {
            pStubUnk->Release();
            pSink->Release();
            return false;
        }

        // 动态构建 WMI 查询语句
        std::vector<std::string> queries;
        for (const auto& eventType : selectedEventTypes) {
            if (eventType == "Win32_ProcessStartTrace" || eventType == "Win32_ProcessStopTrace") {
                // 指定命名空间
                queries.push_back("SELECT * FROM \\\\.\\ROOT\\CIMV2:" + eventType);
            }
            else if (eventType.find("__Instance") == 0) {
                // 添加WITHIN子句
                queries.push_back("SELECT * FROM " + eventType + " WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'");
            }
            else {
                queries.push_back("SELECT * FROM " + eventType);
            }
        }
        // 拼接多事件查询
        std::string combinedQuery;
        for (size_t i = 0; i < queries.size(); ++i) {
            if (i > 0) {
                combinedQuery += " UNION ALL ";
            }
            combinedQuery += queries[i];
        }

        hres = pSvc->ExecNotificationQueryAsync(
            _bstr_t(combinedQuery.c_str()),
            _bstr_t(L"WQL"),
            WBEM_FLAG_SEND_STATUS,
            NULL,
            pStubSink
        );
        if (FAILED(hres)) {
            pStubSink->Release();
            pStubUnk->Release();
            pSink->Release();
            return false;
        }

        return true;
    }

    // 停止 WMI 订阅
    void stopWMI() {
        if (pSvc && pStubSink) {
            pSvc->CancelAsyncCall(pStubSink);
        }

        // 释放资源顺序优化
        if (pStubSink) pStubSink->Release();
        if (pStubUnk) pStubUnk->Release();
        if (pUnsecApp) pUnsecApp->Release();
        if (pSvc) pSvc->Release();
        if (pLoc) pLoc->Release();

        // 仅在最后一次释放时取消初始化COM
        static int comCount = 0;
        comCount--;
        if (comCount <= 0) {
            CoUninitialize();
            comCount = 0;
        }
    }

    // ETW 事件回调函数
    static VOID WINAPI EtwEventCallback(EVENT_RECORD* pEventRecord) {
        // 这里可以处理接收到的ETW事件
        // 简单示例：将事件信息添加到事件列表
        if (pEventRecord == nullptr) return;

        // 获取当前实例指针
        EventMonitor* monitor = static_cast<EventMonitor*>(pEventRecord->UserContext);
        if (monitor == nullptr) return;

        // 格式化时间戳
        SYSTEMTIME st;
        FILETIME ft;
        ULARGE_INTEGER uli;
        uli.QuadPart = pEventRecord->EventHeader.TimeStamp.QuadPart;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        FileTimeToSystemTime(&ft, &st);

        std::stringstream ss;
        ss << std::setw(4) << std::setfill('0') << st.wYear << "-"
            << std::setw(2) << std::setfill('0') << st.wMonth << "-"
            << std::setw(2) << std::setfill('0') << st.wDay << " "
            << std::setw(2) << std::setfill('0') << st.wHour << ":"
            << std::setw(2) << std::setfill('0') << st.wMinute << ":"
            << std::setw(2) << std::setfill('0') << st.wSecond;

        // 创建事件记录
        std::string type = "ETW Event";
        std::string source = "TCP/IP";
        std::string description = "Network event captured";

        monitor->addEvent(EventRecord(ss.str(), type, source, description));
    }

    // 初始化 ETW 用于网络监控
    bool initializeETW() {
        // 1. 计算内存大小（包含会话名称）
        DWORD bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(etwSessionName);
        etwProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, bufferSize);
        if (etwProps == nullptr) {
            return false;
        }

        // 2. 初始化事件跟踪会话属性
        ZeroMemory(etwProps, bufferSize);
        etwProps->Wnode.BufferSize = bufferSize;
        etwProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        etwProps->Wnode.ClientContext = 1;  // QPC计时
        etwProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        etwProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        // 3. 启动跟踪会话
        ULONG status = StartTrace(&etwSession, etwSessionName, etwProps);
        if (status != ERROR_SUCCESS) {
            LocalFree(etwProps);
            etwProps = nullptr;
            return false;
        }

        // 4. 配置TCP/IP事件提供商
        const GUID providerGuid = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };

        
        // 5. 初始化ENABLE_TRACE_PARAMETERS（严格匹配你的结构体定义）
        ENABLE_TRACE_PARAMETERS enableParams = { 0 };
        enableParams.Version = 1;  // 低版本结构体版本号（无EnableInfo成员）
        enableParams.EnableProperty = EVENT_ENABLE_PROPERTY_STACK_TRACE;  // 启用堆栈跟踪
        // 其他成员按需设置（ControlFlags/SourceId等，此处保持默认）

        // 6. 调用EnableTraceEx2（参数完全匹配结构体和函数定义）
        status = EnableTraceEx2(
            etwSession,                          // 跟踪会话句柄
            &providerGuid,                       // 提供商GUID
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,  // 启用提供商
            TRACE_LEVEL_INFORMATION,             // 事件级别（直接作为参数）
            0x1,                                 // MatchAnyKeyword（过滤关键字）
            0x0,                                 // MatchAllKeyword（过滤关键字）
            0,                                   // 超时时间
            &enableParams                        // 结构体参数（无EnableInfo，直接传自身）
        );

        if (status != ERROR_SUCCESS) {
            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            LocalFree(etwProps);
            etwProps = nullptr;
            etwSession = 0;
            return false;
        }

        // 7. 启动事件处理线程
        std::thread etwThread([this]() {
            EVENT_TRACE_LOGFILE traceLogFile = { 0 };
            traceLogFile.LoggerName = etwSessionName;
            traceLogFile.EventRecordCallback = EtwEventCallback;
            traceLogFile.Context = this;
            traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;

            TRACEHANDLE traceHandle = OpenTrace(&traceLogFile);
            if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
                return;
            }

            // 处理事件直到停止
            while (isRunning) {
                ULONG processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
                if (processStatus != ERROR_SUCCESS && processStatus != ERROR_MORE_DATA) {
                    break;
                }
            }

            CloseTrace(traceHandle);
            });

        etwThread.detach();
        return true;
    }
    // 停止 ETW 监控
    void stopETW() {
        if (etwSession != 0 && etwProps != nullptr) {
            // 停止跟踪会话
            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            etwSession = 0;
        }

        if (etwProps != nullptr) {
            LocalFree(etwProps);
            etwProps = nullptr;
        }
    }

    // 安全添加事件到列表
    void addEvent(const EventRecord& event) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.insert(events.begin(), event);
        if (events.size() > 1000) {  // 限制最大事件数量
            events.pop_back();
        }
    }

public:
    EventMonitor() = default;
    ~EventMonitor() {
        stopMonitoring();
    }

    // 启动监控（WMI + ETW）
    void startMonitoring(const std::vector<std::string>& eventTypes) {
        if (isRunning) return;

        selectedEventTypes = eventTypes;
        isRunning = true;

        monitoringThread = std::thread([this]() {
            bool wmiOk = initializeWMI() && subscribeToEvents();
            bool etwOk = initializeETW();

            while (isRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (wmiOk) {
                stopWMI();
            }
            if (etwOk) {
                stopETW();
            }
            });
    }

    // 停止监控
    void stopMonitoring() {
        if (!isRunning) return;

        isRunning = false;
        if (monitoringThread.joinable()) {
            monitoringThread.join();
        }
    }

    // 清空事件列表
    void clearEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.clear();
    }

    // 获取当前事件列表
    std::vector<EventRecord> getEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return events;
    }

    // 查询是否正在监控
    bool isMonitoring() const {
        return isRunning;
    }
};

// 全局事件监控器实例
EventMonitor g_eventMonitor;

// 扩展事件类型列表（覆盖更多监控项）
std::vector<std::pair<std::string, bool>> eventTypes = {
    { "__InstanceCreationEvent", false },   // 实例创建（进程/线程等）
    { "__InstanceDeletionEvent", false },   // 实例删除
    { "__InstanceModificationEvent", false },// 实例修改
    { "Win32_ProcessStartTrace", false },   // 进程启动追踪
    { "Win32_ProcessStopTrace", false },    // 进程退出追踪
    { "MSFT_NetConnectionCreate", false },  // 网络连接建立（Win10+）
    { "CIM_DataFile", false },              // 文件操作（创建/删除等）
    { "RegistryKeyChangeEvent", false },    // 注册表项修改
    { "SecurityEvent", false },             // 安全审计事件
};


void KswordMonitorMain() {
    // 事件类型选择复选框
    ImGui::Text(C("选择要监控的事件类型:"));
    ImGui::Separator();

    // 每行显示2个复选框 - 使用作用域确保Columns正确配对
    {
        int columns = 2;
        ImGui::Columns(columns, NULL, false);  // Push columns

        for (size_t i = 0; i < eventTypes.size(); ++i) {
            ImGui::Checkbox(eventTypes[i].first.c_str(), &eventTypes[i].second);
            if ((i + 1) % columns == 0) {
                ImGui::NextColumn();
            }
        }

        ImGui::Columns(1);  // Pop columns，恢复默认
    }

    ImGui::Separator();

    // 控制按钮
    ImGui::Spacing();
    if (ImGui::Button(C("开始监控"), ImVec2(100, 0)) && !g_eventMonitor.isMonitoring()) {
        std::vector<std::string> selectedTypes;
        for (const auto& type : eventTypes) {
            if (type.second) {
                selectedTypes.push_back(type.first);
            }
        }
        g_eventMonitor.startMonitoring(selectedTypes);
    }

    ImGui::SameLine();
    if (ImGui::Button(C("停止监控"), ImVec2(100, 0)) && g_eventMonitor.isMonitoring()) {
        g_eventMonitor.stopMonitoring();
    }

    ImGui::SameLine();
    if (ImGui::Button(C("清空列表"), ImVec2(100, 0))) {
        g_eventMonitor.clearEvents();
    }

    ImGui::SameLine();
    if (g_eventMonitor.isMonitoring()) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), C("正在监控..."));
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), C("未监控"));
    }

    // 事件表格 - 确保表格的Begin/End正确配对
    ImGui::Spacing();
    ImGui::Text(C("事件列表:"));
    ImGui::Separator();

    auto events = g_eventMonitor.getEvents();

    // 表格操作使用独立作用域
    // 在KswordMonitorMain中
    ImGui::BeginTable("事件表格", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit); {
        ImGui::TableSetupColumn(C("时间"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(C("类型"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(C("源"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(C("描述"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& event : events) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", event.timestamp.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", event.type.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", event.source.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", event.description.c_str());
        }

        ImGui::EndTable();  // 确保表格被正确关闭
    }
    ImGui::EndTabItem();
}

