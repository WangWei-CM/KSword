//#include <Wbemidl.h>
//#define NTDDI_VERSION 0x06030000
//#define _WIN32_WINNT 0x0603  // 表示 Windows 8.1 及以上
//#include "../../KswordTotalHead.h"
//#include <evntrace.h>
//#include <tdh.h>
//#include <comdef.h>
//#include <thread>
//#include <mutex>
//#include <vector>
//#include <string>
//#include <sstream>
//#include <chrono>
//#include <iomanip>
//#include <algorithm>
//#include <unordered_map>
//// 若 evntrace.h 未定义，补充定义，放在 #include 之后
//#ifndef EVENT_TRACE_ENABLE_INFO_DEFINED
//#define EVENT_TRACE_ENABLE_INFO_DEFINED
//
//typedef struct _EVENT_TRACE_ENABLE_INFO {
//    ULONG Version;               // 必须为 EVENT_TRACE_ENABLE_INFO_VERSION (1)
//    ULONG EnableProperty;        // 启用的属性，如 EVENT_ENABLE_PROPERTY_STACK_TRACE等
//} EVENT_TRACE_ENABLE_INFO, * PEVENT_TRACE_ENABLE_INFO;
//
//// 版本号根据微软的规定填写
//#define EVENT_TRACE_ENABLE_INFO_VERSION 1
//
//#endif  // EVENT_TRACE_ENABLE_INFO_DEFINED
//
//#pragma comment(lib, "wbemuuid.lib")
//#pragma comment(lib, "tdh.lib")
//#pragma comment(lib, "advapi32.lib")
//
//using BYTE = unsigned char;
//
//// 为TCP/IP事件提供器GUID未找到的情况做兼容
//// 使用Microsoft-Windows-TCPIP提供器的GUID
//const GUID GUID_TCPIP_EVENTS = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };
//
//// 事件记录结构
//class EventRecord {
//public:
//    std::string timestamp;
//    std::string type;
//    std::string source;
//    std::string description;
//
//    EventRecord(const std::string& ts, const std::string& t, const std::string& s, const std::string& d)
//        : timestamp(ts), type(t), source(s), description(d) {
//    }
//};
//
//// 全局事件监控器类（单例模式）
//class EventMonitor {
//private:
//    bool isRunning = false;
//    std::vector<EventRecord> events;
//    std::mutex eventsMutex;
//    std::thread monitoringThread;
//    std::vector<std::string> selectedEventTypes;
//
//    // WMI 相关接口
//    IWbemLocator* pLoc = nullptr;
//    IWbemServices* pSvc = nullptr;
//    IUnsecuredApartment* pUnsecApp = nullptr;
//    IUnknown* pStubUnk = nullptr;
//    IWbemObjectSink* pStubSink = nullptr;
//
//    // ETW 相关接口
//    TRACEHANDLE etwSession = 0;
//    EVENT_TRACE_PROPERTIES* etwProps = nullptr;
//    WCHAR etwSessionName[256] = L"KswordETWMonitor";
//
//    // WMI 事件接收回调类
//    class EventSink : public IWbemObjectSink {
//    private:
//        LONG m_lRef = 0;
//        EventMonitor* parent;
//
//    public:
//        explicit EventSink(EventMonitor* monitor) : parent(monitor) {}
//        ~EventSink() = default;
//
//        ULONG STDMETHODCALLTYPE AddRef() override {
//            return InterlockedIncrement(&m_lRef);
//        }
//
//        ULONG STDMETHODCALLTYPE Release() override {
//            LONG ref = InterlockedDecrement(&m_lRef);
//            if (ref == 0) {
//                delete this;
//            }
//            return ref;
//        }
//
//        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
//            if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
//                *ppv = static_cast<IWbemObjectSink*>(this);
//                AddRef();
//                return S_OK;
//            }
//            // FIX-ME： 这玩意咋也会导致 QI 递归啊？
//            // 添加对IMarshal接口的支持
//            //else if (riid == IID_IMarshal) {
//            //    // 委托给标准Marshaller
//            //    return CoGetStandardMarshal(riid, static_cast<IUnknown*>(this),
//            //        MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL,
//            //        reinterpret_cast<IMarshal**>(ppv));
//            //}
//
//            // 应该避免在 QI 中调用任何可能引起再次访问 QI 的方法，这会导致递归调用，引发栈溢出
//            // 添加日志记录请求的接口
//            // 记录不支持的接口IID（避免使用StringFromIID，直接记录IID的数值部分）
//            // 将IID拆分成多个部分记录
//            char buffer[256] = { 0 };
//            sprintf_s(buffer, sizeof(buffer), "EventSink不支持接口: {%08lX-%04hX-%04hX-%02hX%02hX-%02hX%02hX%02hX%02hX%02hX%02hX}",
//                riid.Data1, riid.Data2, riid.Data3,
//                riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
//                riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
//            kLog.Add(Warn, C(buffer));
//            *ppv = nullptr;
//            return E_NOINTERFACE;
//        }
//
//        // IWbemObjectSink 接口实现
//        HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
//            if (lObjectCount <= 0 || apObjArray == nullptr) {
//                kLog.Add(Warn, C("Indicate 接收事件数量无效或对象数组为空"));
//                return WBEM_S_NO_ERROR;
//            }
//
//            // 处理每个事件之前的通用逻辑
//            for (int i = 0; i < lObjectCount; ++i) {
//                if (apObjArray[i] == nullptr) {
//                    kLog.Add(Warn, C("事件对象数组中存在空指针"));
//                    continue;
//                }
//
//                VARIANT vtProp;
//                std::string type, source, description;
//
//                // 1. 获取事件类型（__Class 属性）
//                HRESULT hres = apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtProp, 0, 0);
//                if (FAILED(hres)) {
//                    kLog.Add(Warn, C("获取事件__Class属性失败，错误代码: " + std::to_string(hres)));
//                }
//                else {
//                    if (vtProp.vt == VT_BSTR) {
//                        type = _com_util::ConvertBSTRToString(vtProp.bstrVal);
//                    }
//                    else {
//                        kLog.Add(Warn, C("事件__Class属性类型不是BSTR"));
//                    }
//                    VariantClear(&vtProp);
//                }
//
//                // 2. 处理实例事件（__InstanceXXXEvent）的 TargetInstance 属性
//                if (type == "__InstanceCreationEvent" || type == "__InstanceDeletionEvent" || type == "__InstanceModificationEvent") {
//                    // 获取 TargetInstance 实际的对象引用
//                    HRESULT targetRes = apObjArray[i]->Get(_bstr_t(L"TargetInstance"), 0, &vtProp, 0, 0);
//                    if (FAILED(targetRes)) {
//                        kLog.Add(Warn, C("获取TargetInstance属性失败，错误代码: " + std::to_string(targetRes)));
//                        VariantClear(&vtProp);
//                        continue;
//                    }
//
//                    if (vtProp.vt == VT_DISPATCH) {
//                        IWbemClassObject* pTargetInstance = nullptr;
//                        HRESULT qiRes = vtProp.pdispVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance);
//                        if (FAILED(qiRes)) {
//                            kLog.Add(Warn, C("TargetInstance查询IWbemClassObject接口失败，错误代码: " + std::to_string(qiRes)));
//                            VariantClear(&vtProp);
//                            continue;
//                        }
//
//                        // 从 TargetInstance 中获取名称
//                        VARIANT vtName;
//                        HRESULT nameRes = pTargetInstance->Get(_bstr_t(L"Name"), 0, &vtName, 0, 0);
//                        if (FAILED(nameRes)) {
//                            kLog.Add(Warn, C("获取TargetInstance名称失败，错误代码: " + std::to_string(nameRes)));
//                        }
//                        else {
//                            if (vtName.vt == VT_BSTR) {
//                                source = _com_util::ConvertBSTRToString(vtName.bstrVal);
//                            }
//                            else {
//                                kLog.Add(Warn, C("TargetInstance名称属性类型不是BSTR"));
//                            }
//                            VariantClear(&vtName);
//                        }
//                        pTargetInstance->Release();
//                    }
//                    else {
//                        kLog.Add(Warn, C("TargetInstance属性类型不是VT_DISPATCH"));
//                    }
//                    VariantClear(&vtProp);
//
//                    // 设置描述信息
//                    if (type == "__InstanceCreationEvent") description = "Process started";
//                    else if (type == "__InstanceDeletionEvent") description = "Process stopped";
//                    else if (type == "MSFT_NetConnectionCreate") {
//                        // 处理网络连接事件
//                        HRESULT pidRes = apObjArray[i]->Get(L"ProcessId", 0, &vtProp, 0, 0);
//                        if (FAILED(pidRes)) {
//                            kLog.Add(Warn, C("获取MSFT_NetConnectionCreate事件的ProcessId失败，错误代码: " + std::to_string(pidRes)));
//                        }
//                        else {
//                            if (vtProp.vt == VT_I4) {
//                                DWORD pid = vtProp.lVal;
//                                source = "PID: " + std::to_string(pid);
//                            }
//                            else {
//                                kLog.Add(Warn, C("ProcessId属性类型不是整数"));
//                            }
//                            VariantClear(&vtProp);
//                        }
//                        description = "Network connection created";
//                    }
//                    else description = "Process modified";
//                }
//                // 3. 处理 Win32_ProcessStartTrace/StopTrace 事件
//                else if (type == "Win32_ProcessStartTrace" || type == "Win32_ProcessStopTrace") {
//                    HRESULT nameRes = apObjArray[i]->Get(_bstr_t(L"ProcessName"), 0, &vtProp, 0, 0);
//                    if (FAILED(nameRes)) {
//                        kLog.Add(Warn, C("获取ProcessName属性失败，错误代码: " + std::to_string(nameRes)));
//                    }
//                    else {
//                        if (vtProp.vt == VT_BSTR) {
//                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
//                        }
//                        else {
//                            kLog.Add(Warn, C("ProcessName属性类型不是BSTR"));
//                        }
//                        VariantClear(&vtProp);
//                    }
//                    description = type == "Win32_ProcessStartTrace" ? "Process start traced" : "Process stop traced";
//                }
//
//                // 4. 生成时间戳
//                auto now = std::chrono::system_clock::now();
//                std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
//                if (nowTime == -1) {
//                    kLog.Add(Warn, C("转换系统时间失败"));
//                    continue;
//                }
//                std::tm* localTime = std::localtime(&nowTime);
//                if (localTime == nullptr) {
//                    kLog.Add(Warn, C("获取本地时间失败"));
//                    continue;
//                }
//                std::stringstream ss;
//                ss << std::put_time(localTime, "%Y-%m-%d %H:%M:%S");
//
//                // 5. 添加事件到列表（确保有有效信息）
//                if (!source.empty()) {
//                    parent->addEvent(EventRecord(ss.str(), type, source, description));
//                }
//                else {
//                    kLog.Add(Warn, C("事件源信息为空，跳过该事件"));
//                }
//            }
//            return WBEM_S_NO_ERROR;
//        }
//
//        HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject* pObjParam) override {
//            if (FAILED(hResult)) {
//                kLog.Add(Warn, C("SetStatus 返回错误，代码: " + std::to_string(hResult)));
//            }
//            return WBEM_S_NO_ERROR;
//        }
//    };
//
//    // 初始化 WMI 环境
//    bool initializeWMI() {
//        HRESULT hres = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("无法成功初始化WMI环境，错误1: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("CoInitializeEx 初始化成功"));
//
//        hres = CoInitializeSecurity(
//            NULL, -1, NULL, NULL,
//            RPC_C_AUTHN_LEVEL_DEFAULT,
//            RPC_C_IMP_LEVEL_IMPERSONATE,
//            NULL, EOAC_NONE, NULL
//        );
//        if (FAILED(hres)) {
//            CoUninitialize();
//            kLog.Add(Err, C("无法成功初始化WMI环境，错误2: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("CoInitializeSecurity 初始化成功"));
//
//        hres = CoCreateInstance(
//            CLSID_WbemLocator, NULL,
//            CLSCTX_INPROC_SERVER, IID_IWbemLocator,
//            reinterpret_cast<LPVOID*>(&pLoc)
//        );
//        if (FAILED(hres)) {
//            CoUninitialize();
//            kLog.Add(Err, C("无法成功初始化WMI环境，错误3: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("CoCreateInstance 创建IWbemLocator成功"));
//
//        hres = pLoc->ConnectServer(
//            _bstr_t(L"ROOT\\CIMV2"),
//            NULL, NULL, 0, NULL, 0, 0, &pSvc
//        );
//        if (FAILED(hres)) {
//            pLoc->Release();
//            CoUninitialize();
//            kLog.Add(Err, C("无法成功初始化WMI环境，错误4: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("ConnectServer 连接WMI服务成功"));
//
//        hres = CoSetProxyBlanket(
//            pSvc,
//            RPC_C_AUTHN_WINNT,
//            RPC_C_AUTHZ_NONE,
//            NULL,
//            RPC_C_AUTHN_LEVEL_CALL,
//            RPC_C_IMP_LEVEL_IMPERSONATE,
//            NULL,
//            EOAC_NONE
//        );
//        if (FAILED(hres)) {
//            pSvc->Release();
//            pLoc->Release();
//            CoUninitialize();
//            kLog.Add(Err, C("无法成功初始化WMI环境，错误5: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("成功初始化WMI环境"));
//        return true;
//    }
//
//    // 订阅 WMI 事件（动态拼接查询语句）
//    bool subscribeToEvents() {
//        HRESULT hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL,
//            CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
//            reinterpret_cast<void**>(&pUnsecApp));
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("创建IUnsecuredApartment失败，错误代码: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("创建IUnsecuredApartment成功"));
//
//        // 创建事件接收实例（用于处理接收到的事件类型）
//        EventSink* pSink = new EventSink(this);
//        pSink->AddRef();
//
//        hres = pUnsecApp->CreateObjectStub(static_cast<IUnknown*>(pSink), &pStubUnk);
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("创建ObjectStub失败，错误代码: " + std::to_string(hres)));
//            pSink->Release();
//            return false;
//        }
//        kLog.Add(Info, C("CreateObjectStub 成功"));
//
//        hres = pStubUnk->QueryInterface(IID_IWbemObjectSink,
//            reinterpret_cast<void**>(&pStubSink));
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("获取IWbemObjectSink接口失败，错误代码: " + std::to_string(hres)));
//            pStubUnk->Release();
//            pSink->Release();
//            return false;
//        }
//        kLog.Add(Info, C("获取IWbemObjectSink接口成功"));
//
//        // 为实例事件和目标类建立映射
//        std::unordered_map<std::string, std::string> instanceEventTargetMap = {
//            {"__InstanceCreationEvent", "Win32_Process"},       // 进程创建
//            {"__InstanceDeletionEvent", "Win32_Process"},       // 进程删除
//            {"__InstanceModificationEvent", "Win32_Process"},   // 进程修改
//            {"RegistryKeyChangeEvent", "Win32_RegistryKey"}     // 注册表变更
//        };
//
//        // 处理每个事件类型（可修改，去掉UNION等复杂结构）
//        for (const auto& eventType : selectedEventTypes) {
//            std::string query;
//
//            // 构建事件查询语句
//            if (instanceEventTargetMap.count(eventType)) {
//                std::string targetClass = instanceEventTargetMap[eventType];
//                query = "SELECT * FROM " + eventType +
//                    " WITHIN 1 WHERE TargetInstance ISA '" + targetClass + "'";
//            }
//            else if (eventType == "Win32_ProcessStartTrace") {
//                query = "SELECT * FROM Win32_ProcessStartTrace";
//            }
//            else {
//                query = "SELECT * FROM " + eventType + " WITHIN 1";
//            }
//
//            // 为每个事件类型执行订阅
//            hres = pSvc->ExecNotificationQueryAsync(
//                _bstr_t(L"WQL"),  // 查询语言：WQL
//                _bstr_t(query.c_str()),  // 事件查询语句
//                WBEM_FLAG_SEND_STATUS,
//                NULL,
//                pStubSink  // 事件接收回调实例
//            );
//
//            // 若任一事件订阅失败，整体返回失败
//            if (FAILED(hres)) {
//                kLog.Add(Err, C("订阅事件 " + eventType + " 失败，错误代码: " + std::to_string(hres)));
//                pStubSink->Release();
//                pStubUnk->Release();
//                pSink->Release();
//                return false;
//            }
//            kLog.Add(Info, C("成功订阅事件: " + eventType));
//        }
//
//        return true;
//    }
//
//    // 停止 WMI 监控
//    void stopWMI() {
//        kLog.Add(Info, C("开始停止WMI监控"));
//        if (pSvc && pStubSink) {
//            HRESULT hres = pSvc->CancelAsyncCall(pStubSink);
//            if (FAILED(hres)) {
//                kLog.Add(Warn, C("取消WMI异步调用失败，错误代码: " + std::to_string(hres)));
//            }
//            else {
//                kLog.Add(Info, C("成功取消WMI异步调用"));
//            }
//        }
//
//        // 释放资源（逆序释放）
//        if (pStubSink) {
//            pStubSink->Release();
//            pStubSink = nullptr;
//            kLog.Add(Info, C("释放pStubSink成功"));
//        }
//        if (pStubUnk) {
//            pStubUnk->Release();
//            pStubUnk = nullptr;
//            kLog.Add(Info, C("释放pStubUnk成功"));
//        }
//        if (pUnsecApp) {
//            pUnsecApp->Release();
//            pUnsecApp = nullptr;
//            kLog.Add(Info, C("释放pUnsecApp成功"));
//        }
//        if (pSvc) {
//            pSvc->Release();
//            pSvc = nullptr;
//            kLog.Add(Info, C("释放pSvc成功"));
//        }
//        if (pLoc) {
//            pLoc->Release();
//            pLoc = nullptr;
//            kLog.Add(Info, C("释放pLoc成功"));
//        }
//
//        // 最后释放COM初始化（确保只调用一次）
//        static int comCount = 0;
//        comCount--;
//        if (comCount <= 0) {
//            CoUninitialize();
//            comCount = 0;
//            kLog.Add(Info, C("CoUninitialize 成功"));
//        }
//    }
//
//    // ETW 事件回调函数
//    static VOID WINAPI EtwEventCallback(EVENT_RECORD* pEventRecord) {
//        if (pEventRecord == nullptr) {
//            kLog.Add(Warn, C("EtwEventCallback 接收空事件记录"));
//            return;
//        }
//
//        // 获取当前实例指针
//        EventMonitor* monitor = static_cast<EventMonitor*>(pEventRecord->UserContext);
//        if (monitor == nullptr) {
//            kLog.Add(Warn, C("EtwEventCallback 无法获取EventMonitor实例"));
//            return;
//        }
//
//        // 格式化时间戳
//        SYSTEMTIME st;
//        FILETIME ft;
//        ULARGE_INTEGER uli;
//        uli.QuadPart = pEventRecord->EventHeader.TimeStamp.QuadPart;
//        ft.dwLowDateTime = uli.LowPart;
//        ft.dwHighDateTime = uli.HighPart;
//        if (!FileTimeToSystemTime(&ft, &st)) {
//            kLog.Add(Warn, C("FileTimeToSystemTime 转换失败"));
//            return;
//        }
//
//        std::stringstream ss;
//        ss << std::setw(4) << std::setfill('0') << st.wYear << "-"
//            << std::setw(2) << std::setfill('0') << st.wMonth << "-"
//            << std::setw(2) << std::setfill('0') << st.wDay << " "
//            << std::setw(2) << std::setfill('0') << st.wHour << ":"
//            << std::setw(2) << std::setfill('0') << st.wMinute << ":"
//            << std::setw(2) << std::setfill('0') << st.wSecond;
//
//        // 记录事件信息
//        std::string type = "ETW Event";
//        std::string source = "TCP/IP";
//        std::string description = "Network event captured";
//
//        monitor->addEvent(EventRecord(ss.str(), type, source, description));
//    }
//
//    // 初始化 ETW 监控会话
//    bool initializeETW() {
//        kLog.Add(Info, C("开始初始化ETW监控"));
//        // 1. 计算内存大小，用于分配会话属性
//        DWORD bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(etwSessionName);
//        etwProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, bufferSize);
//        if (etwProps == nullptr) {
//            kLog.Add(Err, C("分配ETW属性内存失败"));
//            return false;
//        }
//        kLog.Add(Info, C("ETW属性内存分配成功"));
//
//        // 2. 初始化事件跟踪会话属性
//        ZeroMemory(etwProps, bufferSize);
//        etwProps->Wnode.BufferSize = bufferSize;
//        etwProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
//        etwProps->Wnode.ClientContext = 1;  // QPC计时
//        etwProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
//        etwProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
//
//        // 3. 启动跟踪会话
//        ULONG status = StartTrace(&etwSession, etwSessionName, etwProps);
//        if (status != ERROR_SUCCESS) {
//            kLog.Add(Err, C("启动ETW会话失败，错误代码: " + std::to_string(status)));
//            LocalFree(etwProps);
//            etwProps = nullptr;
//            return false;
//        }
//        kLog.Add(Info, C("ETW会话启动成功"));
//
//        // 4. 指定TCP/IP事件提供器
//        const GUID providerGuid = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };
//
//
//        // 5. 初始化ENABLE_TRACE_PARAMETERS结构（适配较新的系统）
//        ENABLE_TRACE_PARAMETERS enableParams = { 0 };
//        enableParams.Version = 1;  // 对应版本结构的版本号，包含EnableInfo成员
//        enableParams.EnableProperty = EVENT_ENABLE_PROPERTY_STACK_TRACE;  // 启用堆栈跟踪
//
//        // 6. 启用事件提供器
//        status = EnableTraceEx2(
//            etwSession,                          // 跟踪会话句柄
//            &providerGuid,                       // 提供器GUID
//            EVENT_CONTROL_CODE_ENABLE_PROVIDER,  // 启用提供器
//            TRACE_LEVEL_INFORMATION,             // 事件级别（这里为信息级）
//            0x1,                                 // MatchAnyKeyword（筛选关键字）
//            0x0,                                 // MatchAllKeyword
//            0,                                   // 超时时间
//            &enableParams                        // 结构参数（包含扩展信息）
//        );
//
//        if (status != ERROR_SUCCESS) {
//            kLog.Add(Err, C("启用TCP/IP事件提供器失败，错误代码: " + std::to_string(status)));
//            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
//            LocalFree(etwProps);
//            etwProps = nullptr;
//            etwSession = 0;
//            return false;
//        }
//        kLog.Add(Info, C("TCP/IP事件提供器启用成功"));
//
//        // 7. 启动事件处理线程
//        std::thread etwThread([this]() {
//            EVENT_TRACE_LOGFILE traceLogFile = { 0 };
//            traceLogFile.LoggerName = etwSessionName;
//            traceLogFile.EventRecordCallback = EtwEventCallback;
//            traceLogFile.Context = this;
//            traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
//
//            TRACEHANDLE traceHandle = OpenTrace(&traceLogFile);
//            if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
//                kLog.Add(Err, C("打开ETW跟踪失败，错误代码: " + std::to_string(GetLastError())));
//                return;
//            }
//            kLog.Add(Info, C("ETW跟踪打开成功"));
//
//            // 处理事件直到停止
//            while (isRunning) {
//                ULONG processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
//                if (processStatus != ERROR_SUCCESS && processStatus != ERROR_MORE_DATA) {
//                    kLog.Add(Err, C("处理ETW事件失败，错误代码: " + std::to_string(processStatus)));
//                    break;
//                }
//            }
//
//            CloseTrace(traceHandle);
//            kLog.Add(Info, C("ETW跟踪已关闭"));
//            });
//
//        etwThread.detach();
//        kLog.Add(Info, C("ETW事件处理线程启动成功"));
//        return true;
//    }
//
//    // 停止 ETW 监控
//    void stopETW() {
//        kLog.Add(Info, C("开始停止ETW监控"));
//        if (etwSession != 0 && etwProps != nullptr) {
//            // 停止跟踪会话
//            ULONG status = ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
//            if (status != ERROR_SUCCESS) {
//                kLog.Add(Warn, C("停止ETW会话失败，错误代码: " + std::to_string(status)));
//            }
//            else {
//                kLog.Add(Info, C("ETW会话停止成功"));
//            }
//            etwSession = 0;
//        }
//
//        if (etwProps != nullptr) {
//            LocalFree(etwProps);
//            etwProps = nullptr;
//            kLog.Add(Info, C("ETW属性内存释放成功"));
//        }
//    }
//
//    // 安全添加事件到列表
//    void addEvent(const EventRecord& event) {
//        try {
//            std::lock_guard<std::mutex> lock(eventsMutex);
//            events.insert(events.begin(), event);
//            if (events.size() > 1000) {  // 限制事件数量
//                events.pop_back();
//            }
//        }
//        catch (const std::exception& e) {
//            kLog.Add(Err, C("添加事件到列表失败: " + std::string(e.what())));
//        }
//        catch (...) {
//            kLog.Add(Err, C("添加事件到列表时发生未知错误"));
//        }
//    }
//
//public:
//    EventMonitor() = default;
//    ~EventMonitor() {
//        stopMonitoring();
//    }
//
//    // 启动监控（WMI + ETW）
//    void startMonitoring(const std::vector<std::string>& eventTypes) {
//        if (isRunning) {
//            kLog.Add(Warn, C("监控已在运行，无需重复启动"));
//            return;
//        }
//
//        if (eventTypes.empty()) {
//            kLog.Add(Warn, C("未选择任何事件类型，启动监控失败"));
//            return;
//        }
//
//        selectedEventTypes = eventTypes;
//        isRunning = true;
//        kLog.Add(Info, C("开始启动监控线程"));
//
//        monitoringThread = std::thread([this]() {
//            bool wmiOk = initializeWMI() && subscribeToEvents();
//            bool etwOk = initializeETW();
//
//            if (!wmiOk && !etwOk) {
//                kLog.Add(Err, C("WMI和ETW监控均初始化失败，监控无法启动"));
//                isRunning = false;
//                return;
//            }
//
//            while (isRunning) {
//                std::this_thread::sleep_for(std::chrono::milliseconds(100));
//            }
//
//            if (wmiOk) {
//                stopWMI();
//            }
//            if (etwOk) {
//                stopETW();
//            }
//            kLog.Add(Info, C("监控线程已退出"));
//            });
//    }
//
//    // 停止监控
//    void stopMonitoring() {
//        if (!isRunning) {
//            kLog.Add(Warn, C("监控未在运行，无需停止"));
//            return;
//        }
//
//        kLog.Add(Info, C("正在停止监控"));
//        isRunning = false;
//        if (monitoringThread.joinable()) {
//            monitoringThread.join();
//            kLog.Add(Info, C("监控线程已成功join"));
//        }
//        else {
//            kLog.Add(Warn, C("监控线程不可join"));
//        }
//    }
//
//    // 清空事件列表
//    void clearEvents() {
//        try {
//            std::lock_guard<std::mutex> lock(eventsMutex);
//            events.clear();
//            kLog.Add(Info, C("已清空事件列表"));
//        }
//        catch (const std::exception& e) {
//            kLog.Add(Err, C("清空事件列表失败: " + std::string(e.what())));
//        }
//        catch (...) {
//            kLog.Add(Err, C("清空事件列表时发生未知错误"));
//        }
//    }
//
//    // 获取当前事件列表
//    std::vector<EventRecord> getEvents() {
//        try {
//            std::lock_guard<std::mutex> lock(eventsMutex);
//            return events;
//        }
//        catch (const std::exception& e) {
//            kLog.Add(Err, C("获取事件列表失败: " + std::string(e.what())));
//            return {};
//        }
//        catch (...) {
//            kLog.Add(Err, C("获取事件列表时发生未知错误"));
//            return {};
//        }
//    }
//
//    // 查询是否正在监控
//    bool isMonitoring() const {
//        return isRunning;
//    }
//};
//
//// 全局事件监控器实例
//EventMonitor g_eventMonitor;
//
//// 扩展事件类型列表（可根据需要增删）
//std::vector<std::pair<std::string, bool>> eventTypes = {
//    { "__InstanceCreationEvent", false },   // 实例创建事件（进程等）
//    { "__InstanceDeletionEvent", false },   // 实例删除事件
//    { "__InstanceModificationEvent", false },// 实例修改事件
//    { "Win32_ProcessStartTrace", false },   // 进程启动跟踪
//    { "Win32_ProcessStopTrace", false },    // 进程停止跟踪
//    { "MSFT_NetConnectionCreate", false },  // 网络连接创建（Win10+支持）
//    { "CIM_DataFile", false },              // 文件系统事件（创建/删除等）
//    { "RegistryKeyChangeEvent", false },    // 注册表键变更
//    { "SecurityEvent", false },             // 安全日志事件
//};
//
//
//void KswordMonitorMain() {
//    if (ImGui::GetCurrentContext() == nullptr) {
//        kLog.Add(Err, C("ImGui上下文未初始化，无法显示监控界面"));
//        return;
//    }
//
//    ImGui::Text(C("选择要监控的事件类型:"));
//    ImGui::Separator();
//
//    // 每行显示2个选项 - 确保Columns正确使用
//    {
//        int columns = 2;
//        ImGui::Columns(columns, NULL, false);  // Push columns
//
//        for (size_t i = 0; i < eventTypes.size(); ++i) {
//            if (!ImGui::Checkbox(eventTypes[i].first.c_str(), &eventTypes[i].second)) {
//                // 不需要错误日志，Checkbox失败通常不影响流程
//            }
//            if ((i + 1) % columns == 0) {
//                ImGui::NextColumn();
//            }
//        }
//
//        ImGui::Columns(1);  // Pop columns回到默认
//    }
//
//    ImGui::Separator();
//
//    // 控制按钮
//    ImGui::Spacing();
//    if (ImGui::Button(C("开始监控"), ImVec2(100, 0)) && !g_eventMonitor.isMonitoring()) {
//        std::vector<std::string> selectedTypes;
//        for (const auto& type : eventTypes) {
//            if (type.second) {
//                selectedTypes.push_back(type.first);
//            }
//        }
//        g_eventMonitor.startMonitoring(selectedTypes);
//    }
//
//    ImGui::SameLine();
//    if (ImGui::Button(C("停止监控"), ImVec2(100, 0)) && g_eventMonitor.isMonitoring()) {
//        g_eventMonitor.stopMonitoring();
//    }
//
//    ImGui::SameLine();
//    if (ImGui::Button(C("清空列表"), ImVec2(100, 0))) {
//        g_eventMonitor.clearEvents();
//    }
//
//    ImGui::SameLine();
//    if (g_eventMonitor.isMonitoring()) {
//        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), C("正在监控..."));
//    }
//    else {
//        ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), C("未监控"));
//    }
//
//    // 事件列表 - 确保Begin/End正确配对
//    ImGui::Spacing();
//    ImGui::Text(C("事件列表:"));
//    ImGui::Separator();
//
//    auto events = g_eventMonitor.getEvents();
//
//    // 表格显示事件列表
//    if (ImGui::BeginTable("事件列表", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
//        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
//        ImGui::TableSetupColumn(C("时间"), ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn(C("类型"), ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn(C("源"), ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn(C("描述"), ImGuiTableColumnFlags_WidthStretch);
//        ImGui::TableHeadersRow();
//
//        for (const auto& event : events) {
//            ImGui::TableNextRow();
//
//            ImGui::TableSetColumnIndex(0);
//            ImGui::Text("%s", event.timestamp.c_str());
//
//            ImGui::TableSetColumnIndex(1);
//            ImGui::Text("%s", event.type.c_str());
//
//            ImGui::TableSetColumnIndex(2);
//            ImGui::Text("%s", event.source.c_str());
//
//            ImGui::TableSetColumnIndex(3);
//            ImGui::TextWrapped("%s", event.description.c_str());
//        }
//
//        ImGui::EndTable();  // 确保表格正确关闭
//    }
//    else {
//        kLog.Add(Warn, C("创建事件列表表格失败"));
//    }
//    ImGui::EndTabItem();
//}

// [CHANGED] —— 已经大幅重构为“任务聚合式”WMI 监控框架，方便随时扩展事件处理


#define CURRENT_MODULE C("事件监控")

#include <Wbemidl.h>
#include "../../KswordTotalHead.h"
#include <comdef.h>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <unordered_map>
#include <atomic>
#include <memory>   // [NEW]
#include <utility>  // [NEW]

#pragma comment(lib, "wbemuuid.lib")

using BYTE = unsigned char;

// =================== 公共数据结构 ===================

class EventRecord {
public:
    uint64_t    id = 0;       // [NEW] 自增事件ID
    std::string timestamp;
    std::string type;
    std::string source;
    std::string description;

    EventRecord(const std::string& ts, const std::string& t,
        const std::string& s, const std::string& d)
        : timestamp(ts), type(t), source(s), description(d) {
    }
};

// =================== 工具函数 ===================

static inline std::string HrToStr(HRESULT hr) { // [NEW]
    char buf[32];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

template<typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

static std::string NowTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime;
    localtime_s(&localTime, &nowTime);
    std::stringstream ss;
    ss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// [NEW] 把 DMTF datetime（YYYYMMDDHHMMSS.mmmmmmsUUU）转成可读
static std::string DmtfToReadable(const std::string& d) {
    try {
        // 将字符串转换为uint64_t
        uint64_t filetime = std::stoull(d);

        // 定义FILETIME起始点到Unix epoch的偏移量 (单位：100纳秒)
        const uint64_t FILETIME_OFFSET = 116444736000000000ULL;

        // 计算自1970-01-01 00:00:00 UTC的总秒数和剩余100纳秒数
        uint64_t total_100ns = filetime - FILETIME_OFFSET;
        uint64_t seconds = total_100ns / 10000000;
        uint64_t remainder_100ns = total_100ns % 10000000;
        uint64_t nanoseconds = remainder_100ns * 100; // 转换为纳秒

        // 使用time_t处理秒级时间
        time_t t = static_cast<time_t>(seconds);

        // 转换为UTC时间
        struct tm tm;
#if defined(_WIN32)
        gmtime_s(&tm, &t); // Windows安全函数
#else
        gmtime_r(&t, &tm); // Linux/macOS安全函数
#endif
        // 增加8小时（中国时区UTC+8）
        tm.tm_hour += 8;
        // 标准化时间（处理可能的溢出，如超过23小时）
        mktime(&tm);
        // 格式化日期和时间部分
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        // 添加纳秒部分（9位小数）
        oss << '.' << std::setfill('0') << std::setw(9) << nanoseconds;

        return oss.str();
    }
    catch (const std::exception& e) {
        return "转换失败: " + std::string(e.what());
    }
}
// 正确解析DMTF时间格式（YYYYMMDDHHMMSS.mmmmmmsUUU）
//static std::string DmtfToReadable(const std::string& dmtfTime) {
//    // DMTF时间格式至少需要14个字符（YYYYMMDDHHMMSS）
//    if (dmtfTime.size() < 14) {
//        return "Invalid DMTF time"; // 明确错误提示，避免空值
//    }
//
//    try {
//        // 提取年月日时分秒（前14个字符）
//        int year = std::stoi(dmtfTime.substr(0, 4));
//        int month = std::stoi(dmtfTime.substr(4, 2));
//        int day = std::stoi(dmtfTime.substr(6, 2));
//        int hour = std::stoi(dmtfTime.substr(8, 2));
//        int minute = std::stoi(dmtfTime.substr(10, 2));
//        int second = std::stoi(dmtfTime.substr(12, 2));
//
//        // 验证时间合法性（简单校验）
//        if (month < 1 || month > 12 || day < 1 || day > 31 ||
//            hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
//            return "Invalid time value";
//        }
//
//        // 格式化输出
//        char buf[32];
//        sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d",
//            year, month, day, hour, minute, second);
//        return buf;
//    }
//    catch (...) {
//        // 捕获所有解析异常（如非数字字符）
//        return "Parse failed";
//    }
//}
// [NEW] LogonType 数值转文字（复制了一些常用的枚举）
static const char* LogonTypeToStr(unsigned t) {
    switch (t) {
    case 2:  return "Interactive";
    case 3:  return "Network";
    case 4:  return "Batch";
    case 5:  return "Service";
    case 7:  return "Unlock";
    case 8:  return "NetworkCleartext";
    case 9:  return "NewCredentials";
    case 10: return "RemoteInteractive";
    case 11: return "CachedInteractive";
    case 12: return "CachedRemoteInteractive";
    case 13: return "CachedUnlock";
    default: return "Unknown";
    }
}

// [NEW] 过滤系统/服务/机器账号（显示时跳过优先级低的）
static bool IsSystemOrServiceAccount(const std::string& domain, const std::string& name) {
    auto ieq = [](const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; };
    if (ieq(domain, "NT AUTHORITY") || ieq(domain, "NT SERVICE") || ieq(domain, "Window Manager"))
        return true;
    if (_strnicmp(name.c_str(), "DWM-", 4) == 0 || _strnicmp(name.c_str(), "UMFD-", 5) == 0)
        return true;
    if (ieq(name, "LOCAL SERVICE") || ieq(name, "NETWORK SERVICE") || ieq(name, "SYSTEM"))
        return true;
    if (!name.empty() && name.back() == '$') // 机器账号
        return true;
    return false;
}

// =================== 前置声明 ===================

class EventMonitor; // [NEW]

// =================== 通用 Sink（任务对象） ===================
// [NEW] 每个任务拥有自己的 Sink，这样取消订阅与命名空间分离更清晰

class TaskSink : public IWbemObjectSink {
    std::atomic<LONG> m_ref{ 0 };
    EventMonitor* m_monitor;           // 宿主（用于 AddEvent）
    std::function<void(IWbemClassObject*, EventMonitor*)> m_onEvent; // 任务的回调

public:
    TaskSink(EventMonitor* mon,
        std::function<void(IWbemClassObject*, EventMonitor*)> cb)
        : m_monitor(mon), m_onEvent(std::move(cb)) {
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
            *ppv = static_cast<IWbemObjectSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Indicate(LONG n, IWbemClassObject** arr) override {
        if (n <= 0 || !arr) return WBEM_S_NO_ERROR;
        for (LONG i = 0; i < n; ++i) {
            if (!arr[i]) continue;
            try {
                if (m_onEvent) m_onEvent(arr[i], m_monitor);
            }
            catch (...) {
                kLog.Add(Warn, C("处理WMI任务事件时发生异常"),CURRENT_MODULE);
            }
        }
        return WBEM_S_NO_ERROR;
    }
    HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR, IWbemClassObject*) override {
        return WBEM_S_NO_ERROR;
    }
};

// =================== 任务基类 ===================
// [NEW] 每种订阅定义为一个任务；含命名空间、查询字符串、解析逻辑

class WmiEventTask {
protected:
    std::wstring m_namespace;      // 例如 L"ROOT\\CIMV2" 或 L"ROOT\\DEFAULT"
    std::wstring m_query;          // 完整 WQL
    std::string  m_name;           // 用于UI显示/日志
    IWbemServices* m_svc = nullptr;
    IUnsecuredApartment* m_unsec = nullptr;
    IWbemObjectSink* m_stubSink = nullptr; // Apartment stub
    TaskSink* m_sink = nullptr;     // 实际的事件接收器（拥有回调）

public:
    virtual ~WmiEventTask() { Unsubscribe(); }

    const std::string& Name() const { return m_name; }
    const std::wstring& NameSpace() const { return m_namespace; }
    const std::wstring& Query() const { return m_query; }

    // [NEW] 每个任务必须实现事件解析逻辑
    virtual void OnEvent(IWbemClassObject* pEvent, EventMonitor* host) = 0;

    // [NEW] 连接命名空间
    bool Connect(IWbemLocator* loc) {
        HRESULT hr = loc->ConnectServer(_bstr_t(m_namespace.c_str()),
            nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_svc);
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("连接命名空间失败: ") + std::string(m_name) + " " + HrToStr(hr)), CURRENT_MODULE);
            return false;
        }
        hr = CoSetProxyBlanket(
            m_svc,
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE
        );
        if (FAILED(hr)) {
            kLog.Add(Warn, C(std::string("设置代理安全失败: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
        }
        return true;
    }

    // [NEW] 订阅（每个任务自建 UnsecuredApartment + Stub + Sink）
    bool Subscribe(EventMonitor* host) {
        HRESULT hr = CoCreateInstance(
            CLSID_UnsecuredApartment, nullptr, CLSCTX_LOCAL_SERVER,
            IID_IUnsecuredApartment, reinterpret_cast<void**>(&m_unsec));
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("创建UnsecuredApartment失败: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            return false;
        }

        m_sink = new TaskSink(host, [this](IWbemClassObject* obj, EventMonitor* h) {
            this->OnEvent(obj, h);
            });
        m_sink->AddRef();

        IUnknown* stubUnk = nullptr;
        hr = m_unsec->CreateObjectStub(m_sink, &stubUnk);
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("创建对象存根失败: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            m_sink->Release(); m_sink = nullptr;
            return false;
        }

        hr = stubUnk->QueryInterface(IID_IWbemObjectSink, reinterpret_cast<void**>(&m_stubSink));
        stubUnk->Release();
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("QI IWbemObjectSink 失败: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            m_sink->Release(); m_sink = nullptr;
            return false;
        }

        hr = m_svc->ExecNotificationQueryAsync(
            _bstr_t(L"WQL"),
            _bstr_t(m_query.c_str()),
            WBEM_FLAG_SEND_STATUS,
            nullptr,
            m_stubSink
        );
        if (FAILED(hr)) {
            kLog.Add(Warn, C(std::string("订阅失败: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            SafeRelease(m_stubSink);
            SafeRelease(m_unsec);
            m_sink->Release(); m_sink = nullptr;
            return false;
        }

        kLog.Add(Info, C(std::string("已订阅: ") + m_name), CURRENT_MODULE);
        return true;
    }

    // [NEW] 取消订阅
    void Unsubscribe() {
        if (m_svc && m_stubSink) {
            m_svc->CancelAsyncCall(m_stubSink);
        }
        SafeRelease(m_stubSink);
        if (m_sink) { m_sink->Release(); m_sink = nullptr; }
        SafeRelease(m_unsec);
        SafeRelease(m_svc);
    }
};

// =================== 通用属性读取 ===================
// [NEW] 抽离出来，使得任务可复用

static std::string ReadStrProp(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v; VariantInit(&v);
    if (FAILED(obj->Get(name, 0, &v, nullptr, nullptr))) return "";
    std::string out;
    switch (v.vt) {
    case VT_BSTR: out = _com_util::ConvertBSTRToString(v.bstrVal); break;
    case VT_I4:   out = std::to_string(v.lVal); break;
    case VT_UI4:  out = std::to_string(v.ulVal); break;
    case VT_UI8: {
        ULARGE_INTEGER uli; uli.QuadPart = v.ullVal;
        FILETIME ft = { uli.LowPart, uli.HighPart };
        SYSTEMTIME st;
        if (FileTimeToSystemTime(&ft, &st)) {
            char buffer[32];
            sprintf_s(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            out = buffer;
        }
        break;
    }
    default: break;
    }
    VariantClear(&v);
    return out;
}

static IWbemClassObject* ReadEmbeddedObject(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v; VariantInit(&v);
    IWbemClassObject* target = nullptr;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr))) {
        if ((v.vt == VT_UNKNOWN || v.vt == VT_DISPATCH) && v.punkVal) {
            (void)v.punkVal->QueryInterface(IID_IWbemClassObject, (void**)&target);
        }
    }
    VariantClear(&v);
    return target; // 需要调用方 Release
}

// =================== 具体任务实现 ===================

// [NEW] 1) __Instance* 进程类事件（必须 WITHIN）
class InstanceProcessTask : public WmiEventTask {
    std::string m_eventClass; // "__InstanceCreationEvent" / "__InstanceDeletionEvent" / "__InstanceModificationEvent"
    std::string m_descCreate, m_descDelete, m_descModify;

public:
    InstanceProcessTask(const std::string& eventClass, unsigned withinSecs = 2)
        : m_eventClass(eventClass)
    {
        m_namespace = L"ROOT\\CIMV2";
        std::wstring wEvent(m_eventClass.begin(), m_eventClass.end());
        std::wstringstream ss;
        ss << L"SELECT * FROM " << wEvent << L" WITHIN " << withinSecs
            << L" WHERE TargetInstance ISA 'Win32_Process'";
        m_query = ss.str();

        m_name = eventClass + " (Win32_Process)";
        m_descCreate = "Process object created";
        m_descDelete = "Process object deleted";
        m_descModify = "Process object modified";
    }

    void OnEvent(IWbemClassObject* evt, EventMonitor* host) override;
};

// [NEW] 2) Win32_ProcessStartTrace / StopTrace（外在事件，不需要 WITHIN）
class ProcessTraceTask : public WmiEventTask {
    std::string m_className; // "Win32_ProcessStartTrace" or "Win32_ProcessStopTrace"
public:
    explicit ProcessTraceTask(const std::string& cls) : m_className(cls) {
        m_namespace = L"ROOT\\CIMV2";
        std::wstring wCls(m_className.begin(), m_className.end());
        std::wstringstream ss;
        ss << L"SELECT * FROM " << wCls;
        m_query = ss.str();
        m_name = m_className;
    }

    void OnEvent(IWbemClassObject* evt, EventMonitor* host) override;
};

// [NEW] 3) RegistryKeyChangeEvent（外在事件，ROOT\\DEFAULT）
class RegistryKeyChangeTask : public WmiEventTask {
    std::string m_hive;     // 例如 "HKEY_LOCAL_MACHINE"
    std::string m_keyPath;  // 例如 "SOFTWARE\\MyApp"
public:
    RegistryKeyChangeTask(std::string hive, std::string keyPath)
        : m_hive(std::move(hive)), m_keyPath(std::move(keyPath))
    {
        m_namespace = L"ROOT\\DEFAULT";
        std::wstringstream ss;
        ss << L"SELECT * FROM RegistryKeyChangeEvent WHERE Hive='";
        ss << std::wstring(m_hive.begin(), m_hive.end()) << L"' AND KeyPath='";
        // 反斜杠在WQL里需要双反斜杠
        std::string escaped = m_keyPath;
        for (size_t pos = 0; (pos = escaped.find("\\", pos)) != std::string::npos; pos += 2)
            escaped.replace(pos, 1, "\\\\");
        ss << std::wstring(escaped.begin(), escaped.end()) << L"'";
        m_query = ss.str();
        m_name = "RegistryKeyChangeEvent(" + m_hive + "\\" + m_keyPath + ")";
    }

    void OnEvent(IWbemClassObject* evt, EventMonitor* host) override;
};

// [NEW] 卷事件任务
class VolumeChangeTask : public WmiEventTask {
public:
    VolumeChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_VolumeChangeEvent";
        m_name = "Win32_VolumeChangeEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 交互式登录任务
class InteractiveLogonTask : public WmiEventTask {
    // [NEW] 为注销事件做用户名回填
    std::mutex m_cacheMutex;
    std::unordered_map<std::string, std::string> m_logonId2User;

    // [NEW] 根据 LogonSession 的 __RELPATH 取最佳账号（过滤内置/服务/机器账号）
    std::string QueryBestAccountByRelPath(const std::string& relPath) {
        if (relPath.empty() || !m_svc) return {};
        std::wstring wrel(relPath.begin(), relPath.end());
        std::wstringstream wql;
        wql << L"ASSOCIATORS OF {" << wrel
            << L"} WHERE AssocClass=Win32_LoggedOnUser Role=Dependent ResultClass=Win32_Account";

        IEnumWbemClassObject* penum = nullptr;
        HRESULT hr = m_svc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(wql.str().c_str()),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &penum);
        if (FAILED(hr) || !penum) return {};

        std::string first; // 兜底
        IWbemClassObject* obj = nullptr; ULONG ret = 0;
        while (penum->Next(5000, 1, &obj, &ret) == S_OK && ret == 1) {
            std::string name = ReadStrProp(obj, L"Name");
            std::string domain = ReadStrProp(obj, L"Domain");
            std::string acct = domain.empty() ? name : (domain + "\\" + name);
            if (first.empty()) first = acct;
            if (!IsSystemOrServiceAccount(domain, name)) { obj->Release(); penum->Release(); return acct; }
            obj->Release();
        }
        penum->Release();
        return first;
    }

public:
    InteractiveLogonTask() {
        m_namespace = L"ROOT\\CIMV2";
        // 同时捕获“交互式(2)”与“远程交互(10)”的创建/删除
        m_query = L"SELECT * FROM __InstanceOperationEvent WITHIN 5 "
            L"WHERE TargetInstance ISA 'Win32_LogonSession' "
            L"AND (TargetInstance.LogonType = 2 OR TargetInstance.LogonType = 10)";
        m_name = "InteractiveLogon";
    }

    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};


////////////////////// 以下任务未经过审查，由 AI 生成 ///////////////////////
/////////////////////////////////////////////////////////////////////////
// [NEW] 设备变更粗粒度：Win32_DeviceChangeEvent
class DeviceChangeTask : public WmiEventTask {
public:
    DeviceChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_DeviceChangeEvent";
        m_name = "Win32_DeviceChangeEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 电源/睡眠唤醒：Win32_PowerManagementEvent
class PowerManagementEventTask : public WmiEventTask {
public:
    PowerManagementEventTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_PowerManagementEvent";
        m_name = "Win32_PowerManagementEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 关机/重启/注销：Win32_ComputerShutdownEvent
class ComputerShutdownEventTask : public WmiEventTask {
public:
    ComputerShutdownEventTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_ComputerShutdownEvent";
        m_name = "Win32_ComputerShutdownEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 服务状态变更：__InstanceModificationEvent + Win32_Service
class ServiceStateChangeTask : public WmiEventTask {
public:
    ServiceStateChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM __InstanceModificationEvent WITHIN 5 "
            L"WHERE TargetInstance ISA 'Win32_Service' AND TargetInstance.State <> PreviousInstance.State";
        m_name = "ServiceStateChange";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 打印作业创建：__InstanceCreationEvent + Win32_PrintJob
class PrintJobCreateTask : public WmiEventTask {
public:
    PrintJobCreateTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM __InstanceCreationEvent WITHIN 5 "
            L"WHERE TargetInstance ISA 'Win32_PrintJob'";
        m_name = "PrintJobCreate";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 网线插拔：MSNdis_StatusMediaConnect / Disconnect（ROOT\\WMI）
class NdisMediaConnectTask : public WmiEventTask {
public:
    NdisMediaConnectTask() {
        m_namespace = L"ROOT\\WMI";
        m_query = L"SELECT * FROM MSNdis_StatusMediaConnect";
        m_name = "NdisMediaConnect";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

class NdisMediaDisconnectTask : public WmiEventTask {
public:
    NdisMediaDisconnectTask() {
        m_namespace = L"ROOT\\WMI";
        m_query = L"SELECT * FROM MSNdis_StatusMediaDisconnect";
        m_name = "NdisMediaDisconnect";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] IP 地址/网络配置变化：__InstanceOperationEvent + MSFT_NetIPAddress（Win8+）
class NetIPAddressChangeTask : public WmiEventTask {
public:
    NetIPAddressChangeTask() {
        m_namespace = L"ROOT\\StandardCimv2";
        m_query = L"SELECT * FROM __InstanceOperationEvent WITHIN 5 "
            L"WHERE TargetInstance ISA 'MSFT_NetIPAddress'";
        m_name = "NetIPAddressChange";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 防火墙规则变更：__InstanceOperationEvent + MSFT_NetFirewallRule（Win8+，常需管理员）
class FirewallRuleChangeTask : public WmiEventTask {
public:
    FirewallRuleChangeTask() {
        m_namespace = L"ROOT\\StandardCimv2";
        m_query = L"SELECT * FROM __InstanceOperationEvent WITHIN 5 "
            L"WHERE TargetInstance ISA 'MSFT_NetFirewallRule'";
        m_name = "FirewallRuleChange";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 系统配置变化提示：Win32_SystemConfigurationChangeEvent
class SystemConfigurationChangeTask : public WmiEventTask {
public:
    SystemConfigurationChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_SystemConfigurationChangeEvent";
        m_name = "Win32_SystemConfigurationChangeEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] 进程模块加载（高级/依赖系统、常需管理员）：Win32_ModuleLoadTrace
class ModuleLoadTraceTask : public WmiEventTask {
public:
    ModuleLoadTraceTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_ModuleLoadTrace";
        m_name = "Win32_ModuleLoadTrace";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// =================== 事件监控器（宿主） ===================

class EventMonitor {
private:
    std::atomic<bool> isRunning{ false };
    std::atomic<uint64_t> m_nextEventId{ 1 }; // [NEW] 事件编号累加器
    std::vector<EventRecord> events;
    std::mutex eventsMutex;
    std::thread monitoringThread;

    // [NEW] 任务集合（每个订阅一个任务实例）
    std::vector<std::unique_ptr<WmiEventTask>> m_tasks;

    // [CHANGED] 不再持有全局 pSvc/pStubSink；改为每个任务各自管理
    IWbemLocator* m_locator = nullptr;

public:
    EventMonitor() = default;
    ~EventMonitor() { StopMonitoring(); }

    // [CHANGED] 添加事件的方法（供任务回调使用）
    // [CHANGED] 改为“按值传入”，在这里赋 id
    void AddEvent(EventRecord ev) {
        ev.id = m_nextEventId.fetch_add(1, std::memory_order_relaxed); // [NEW]
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.insert(events.begin(), std::move(ev)); // 仍旧把新事件插到最前
        if (events.size() > 1000) events.pop_back();
    }

    // [CHANGED] 清空时重置编号
    void ClearEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.clear();
        m_nextEventId.store(1, std::memory_order_relaxed); // [NEW]
    }

    std::vector<EventRecord> GetEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return events;
    }

    // [NEW] 工厂：根据“类型字符串”创建任务
    static std::unique_ptr<WmiEventTask> MakeTaskFromType(const std::string& type) {
        if (type == "__InstanceCreationEvent")
            return std::make_unique<InstanceProcessTask>("__InstanceCreationEvent", 2);
        if (type == "__InstanceDeletionEvent")
            return std::make_unique<InstanceProcessTask>("__InstanceDeletionEvent", 2);
        if (type == "__InstanceModificationEvent")
            return std::make_unique<InstanceProcessTask>("__InstanceModificationEvent", 2);
        if (type == "Win32_ProcessStartTrace")
            return std::make_unique<ProcessTraceTask>("Win32_ProcessStartTrace");
        if (type == "Win32_ProcessStopTrace")
            return std::make_unique<ProcessTraceTask>("Win32_ProcessStopTrace");
        if (type == "Win32_VolumeChangeEvent")
            return std::make_unique<VolumeChangeTask>();
        if (type == "Win32_LogonSession")
            return std::make_unique<InteractiveLogonTask>();
        if (type == "Win32_DeviceChangeEvent")
            return std::make_unique<DeviceChangeTask>();
        if (type == "Win32_PowerManagementEvent")
            return std::make_unique<PowerManagementEventTask>();
        if (type == "Win32_ComputerShutdownEvent")
            return std::make_unique<ComputerShutdownEventTask>();
        if (type == "Win32_ServiceStateChangeEvent")
            return std::make_unique<ServiceStateChangeTask>();
        if (type == "Win32_JobCreationEvent")
            return std::make_unique<PrintJobCreateTask>();
        if (type == "MSNdis_StatusMediaConnectEvent")
            return std::make_unique<NdisMediaConnectTask>();
        if (type == "MSNdis_StatusMediaDisconnectEvent")
            return std::make_unique<NdisMediaDisconnectTask>();
        if (type == "MSFT_NetIPAddressChangeEvent")
            return std::make_unique<NetIPAddressChangeTask>();
        if (type == "MSFT_NetFirewallRuleChangeEvent")
            return std::make_unique<FirewallRuleChangeTask>();
        if (type == "Win32_SystemConfigurationChangeEvent")
            return std::make_unique<SystemConfigurationChangeTask>();
        if (type == "Win32_ModuleLoadTraceEvent")
            return std::make_unique<ModuleLoadTraceTask>();

        // 你也可以传入 "RegistryKeyChangeEvent:HKLM:SOFTWARE\\MyApp"
        const std::string prefix = "RegistryKeyChangeEvent:";
        if (type.rfind(prefix, 0) == 0) {
            // 格式：RegistryKeyChangeEvent:HIVE:KEYPATH
            auto rest = type.substr(prefix.size());
            auto sep = rest.find(':');
            if (sep != std::string::npos) {
                std::string hive = rest.substr(0, sep);
                std::string key = rest.substr(sep + 1);
                if (_stricmp(hive.c_str(), "HKLM") == 0) hive = "HKEY_LOCAL_MACHINE";
                else if (_stricmp(hive.c_str(), "HKCU") == 0) hive = "HKEY_CURRENT_USER";
                return std::make_unique<RegistryKeyChangeTask>(hive, key);
            }
        }
        return {};
    }

    // [NEW] 初始化 COM + 安全 + 定位器
    bool InitializeCOM() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            kLog.Add(Err, C("CoInitializeEx 失败: " + HrToStr(hr)), CURRENT_MODULE);
            return false;
        }
        // 考虑到多次初始化问题，将 RPC_E_TOO_LATE 视为可继续
        hr = CoInitializeSecurity(
            nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr
        );
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            kLog.Add(Warn, C("CoInitializeSecurity 警告: " + HrToStr(hr)), CURRENT_MODULE);
        }

        hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, reinterpret_cast<void**>(&m_locator));
        if (FAILED(hr)) {
            kLog.Add(Err, C("创建 WbemLocator 失败: " + HrToStr(hr)), CURRENT_MODULE);
            CoUninitialize();
            return false;
        }
        return true;
    }

    // [NEW] 反初始化
    void UninitializeCOM() {
        SafeRelease(m_locator);
        CoUninitialize();
    }

    // [NEW] 启动：根据类型创建任务 -> 连接 -> 订阅
    void StartMonitoring(const std::vector<std::string>& typeList) {
        if (isRunning || typeList.empty()) return;
        isRunning = true;

        monitoringThread = std::thread([this, typeList]() {
            if (!InitializeCOM()) {
                isRunning = false;
                return;
            }

            // 构造任务
            for (const auto& t : typeList) {
                if (auto task = MakeTaskFromType(t)) {
                    if (!task->Connect(m_locator)) continue;
                    if (!task->Subscribe(this)) continue;
                    m_tasks.emplace_back(std::move(task));
                }
                else {
                    kLog.Add(Warn, C(std::string("未知/不支持的任务类型: ") + t), CURRENT_MODULE);
                }
            }

            if (m_tasks.empty()) {
                kLog.Add(Warn, C("没有任何任务成功订阅，终止监控"), CURRENT_MODULE);
                UninitializeCOM();
                isRunning = false;
                return;
            }

            // 在 STA 模型中，客户端线程必须持续获取消息，否则事件会丢失
            while (isRunning) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                SleepEx(50, TRUE);
            }

            // 清理
            for (auto& task : m_tasks) task->Unsubscribe();
            m_tasks.clear();
            UninitializeCOM();
            });
    }

    void StopMonitoring() {
        if (!isRunning) return;
        isRunning = false;
        if (monitoringThread.joinable()) monitoringThread.join();
    }

    bool IsMonitoring() const { return isRunning; }
};

// =================== 任务事件实现 ===================

void InstanceProcessTask::OnEvent(IWbemClassObject* evt, EventMonitor* host) { // [NEW]
    // 取类型与时间
    std::string cls = ReadStrProp(evt, L"__CLASS");
    std::string ts = ReadStrProp(evt, L"TIME_CREATED");
    if (ts.empty()) ts = NowTimestamp();

    std::string source = "Process";
    // 取 TargetInstance.Name
    if (IWbemClassObject* target = ReadEmbeddedObject(evt, L"TargetInstance")) {
        std::string name = ReadStrProp(target, L"Name");
        if (!name.empty()) source = name;
        target->Release();
    }

    std::string desc;
    if (m_eventClass == "__InstanceCreationEvent") desc = "Object created";
    else if (m_eventClass == "__InstanceDeletionEvent") desc = "Object deleted";
    else desc = "Object modified";

    host->AddEvent(EventRecord(ts, cls.empty() ? m_eventClass : cls, source, desc));
}

void ProcessTraceTask::OnEvent(IWbemClassObject* evt, EventMonitor* host) { // [NEW]
    std::string ts = ReadStrProp(evt, L"TIME_CREATED");
    if (ts.empty()) ts = NowTimestamp();

    std::string name = ReadStrProp(evt, L"ProcessName");
    if (name.empty()) name = "Process";

    std::string desc = (m_className == "Win32_ProcessStartTrace") ? "Process started" : "Process stopped";
    host->AddEvent(EventRecord(ts, m_className, name, desc));
}

void RegistryKeyChangeTask::OnEvent(IWbemClassObject* evt, EventMonitor* host) { // [NEW]
    std::string ts = ReadStrProp(evt, L"TIME_CREATED");
    if (ts.empty()) ts = NowTimestamp();

    // 可以取 Hive/KeyPath 确认
    std::string hive = ReadStrProp(evt, L"Hive");
    std::string key = ReadStrProp(evt, L"KeyPath");
    std::string src = hive.empty() ? m_hive : hive;
    if (!key.empty()) src += ("\\" + key);

    host->AddEvent(EventRecord(ts, "RegistryKeyChangeEvent", src, "Registry key changed"));
}

void VolumeChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host) {
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string evt = ReadStrProp(e, L"EventType");   // 自行映射为 Arrived/Removed
    std::string drive = ReadStrProp(e, L"DriveName"); // 可能为空
    host->AddEvent(EventRecord(ts, m_name, drive.empty() ? "Volume" : drive, "Volume event type: " + evt));
}

void InteractiveLogonTask::OnEvent(IWbemClassObject* e, EventMonitor* host) { // [CHANGED]
    // 事件类别：__InstanceCreationEvent / __InstanceDeletionEvent / __InstanceModificationEvent
    std::string evtClass = ReadStrProp(e, L"__CLASS");
    bool isLogon = (evtClass == "__InstanceCreationEvent");
    bool isLogoff = (evtClass == "__InstanceDeletionEvent");

    std::string ts = ReadStrProp(e, L"TIME_CREATED");
    if (ts.empty()) ts = NowTimestamp();

    IWbemClassObject* ti = ReadEmbeddedObject(e, L"TargetInstance"); // Win32_LogonSession
    if (!ti) {
        host->AddEvent(EventRecord(ts, m_name, "LogonSession", isLogon ? "Interactive logon" :
            isLogoff ? "Interactive logoff" : "Interactive session change"));
        return;
    }

    // 从会话取关键字段
    std::string logonId = ReadStrProp(ti, L"LogonId");
    std::string authPkg = ReadStrProp(ti, L"AuthenticationPackage");
    std::string startTime = ReadStrProp(ti, L"StartTime");       // DMTF
    std::string relPath = ReadStrProp(ti, L"__RELPATH");
    unsigned logonType = 0;
    {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(ti->Get(L"LogonType", 0, &v, nullptr, nullptr)) && (v.vt == VT_I4 || v.vt == VT_UI4))
            logonType = (v.vt == VT_I4) ? (unsigned)v.lVal : v.ulVal;
        VariantClear(&v);
    }
    ti->Release();

    // 取用户名：优先实时关联；注销场景取不到则回退缓存
    std::string user = QueryBestAccountByRelPath(relPath);
    if (isLogon) {
        if (!logonId.empty() && !user.empty()) {
            std::lock_guard<std::mutex> _g(m_cacheMutex);
            m_logonId2User[logonId] = user;
        }
    }
    else if (isLogoff) {
        if (user.empty() && !logonId.empty()) {
            std::lock_guard<std::mutex> _g(m_cacheMutex);
            auto it = m_logonId2User.find(logonId);
            if (it != m_logonId2User.end()) { user = it->second; m_logonId2User.erase(it); }
        }
        else if (!logonId.empty()) {
            // 有实时用户名也顺便清理缓存
            std::lock_guard<std::mutex> _g(m_cacheMutex);
            m_logonId2User.erase(logonId);
        }
    }

    // 组织展示
    std::string source = user.empty() ? "LogonSession" : user;
    std::string desc = isLogon ? "Interactive logon" : (isLogoff ? "Interactive logoff" : "Interactive session change");

    if (!logonId.empty())         desc += " | LogonId=" + logonId;
    if (logonType)                desc += " | Type=" + std::string(LogonTypeToStr(logonType));
    if (!authPkg.empty())         desc += " | Auth=" + authPkg;
    if (!startTime.empty()) {
        std::string pretty = DmtfToReadable(startTime);
        if (!pretty.empty())     desc += " | Start=" + pretty;
    }

    host->AddEvent(EventRecord(ts, m_name, source, desc));
}

void DeviceChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string et = ReadStrProp(e, L"EventType");    // 参照 DBT_* 枚举映射
    host->AddEvent(EventRecord(ts, m_name, "Device", "Device change (EventType=" + et + ")"));
}

void PowerManagementEventTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string et = ReadStrProp(e, L"EventType"); // 常见：4=进入待机，7=从待机恢复等（不同版本略有差异）
    host->AddEvent(EventRecord(ts, m_name, "Power", "Power event (EventType=" + et + ")"));
}

void ComputerShutdownEventTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    host->AddEvent(EventRecord(ts, m_name, "System", "System is shutting down / restarting / logging off"));
}

void ServiceStateChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string name = "Service";
    std::string oldS = "", newS = "";
    if (IWbemClassObject* cur = ReadEmbeddedObject(e, L"TargetInstance")) {
        name = ReadStrProp(cur, L"Name");
        newS = ReadStrProp(cur, L"State");
        cur->Release();
    }
    if (IWbemClassObject* prev = ReadEmbeddedObject(e, L"PreviousInstance")) {
        oldS = ReadStrProp(prev, L"State");
        prev->Release();
    }
    if (name.empty()) name = "Service";
    std::string desc = "State changed";
    if (!oldS.empty() || !newS.empty()) desc = oldS + " -> " + newS;
    host->AddEvent(EventRecord(ts, m_name, name, desc));
}

void PrintJobCreateTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string doc = "PrintJob";
    if (IWbemClassObject* ti = ReadEmbeddedObject(e, L"TargetInstance")) {
        std::string d = ReadStrProp(ti, L"Document");
        if (!d.empty()) doc = d;
        ti->Release();
    }
    host->AddEvent(EventRecord(ts, m_name, doc, "New print job"));
}

void NdisMediaConnectTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string nic = ReadStrProp(e, L"InstanceName");
    host->AddEvent(EventRecord(ts, m_name, nic.empty() ? "NIC" : nic, "Media connected"));
}

void NdisMediaDisconnectTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string nic = ReadStrProp(e, L"InstanceName");
    host->AddEvent(EventRecord(ts, m_name, nic.empty() ? "NIC" : nic, "Media disconnected"));
}

void NetIPAddressChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string op = ReadStrProp(e, L"__CLASS"); // __InstanceCreationEvent/Deletion/Modification
    std::string alias, ip;
    if (IWbemClassObject* ti = ReadEmbeddedObject(e, L"TargetInstance")) {
        alias = ReadStrProp(ti, L"InterfaceAlias");
        ip = ReadStrProp(ti, L"IPAddress");      // 若为数组，可能会是空；不影响显示
        ti->Release();
    }
    std::string src = alias.empty() ? "NetIP" : alias;
    std::string desc = (op.empty() ? "IP change" : op) + (ip.empty() ? "" : (": " + ip));
    host->AddEvent(EventRecord(ts, m_name, src, desc));
}

void FirewallRuleChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string op = ReadStrProp(e, L"__CLASS");
    std::string display, action, enabled;
    if (IWbemClassObject* ti = ReadEmbeddedObject(e, L"TargetInstance")) {
        display = ReadStrProp(ti, L"DisplayName");
        action = ReadStrProp(ti, L"Action");
        enabled = ReadStrProp(ti, L"Enabled");
        ti->Release();
    }
    if (display.empty()) display = "FirewallRule";
    std::string desc = (op.empty() ? "Rule changed" : op);
    if (!action.empty() || !enabled.empty()) desc += " (" + action + (enabled.empty() ? "" : (", enabled=" + enabled)) + ")";
    host->AddEvent(EventRecord(ts, m_name, display, desc));
}

void SystemConfigurationChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    host->AddEvent(EventRecord(ts, m_name, "System", "System configuration changed"));
}

void ModuleLoadTraceTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string proc = ReadStrProp(e, L"ProcessName");  // 不同系统属性可能不同
    std::string mod = ReadStrProp(e, L"FileName");
    if (proc.empty()) proc = "Process";
    std::string desc = "Module loaded";
    if (!mod.empty()) desc += (": " + mod);
    host->AddEvent(EventRecord(ts, m_name, proc, desc));
}

// =================== UI/集成（与原来接口兼容） ===================

// [CHANGED] 全局实例
static EventMonitor g_eventMonitor;

// [CHANGED] 事件类型列表
static std::vector<std::pair<std::string, bool>> eventTypes = {
    {"__InstanceCreationEvent", true},
    {"__InstanceDeletionEvent", true},
    {"Win32_ProcessStartTrace", true},
    {"Win32_ProcessStopTrace", true},
    {"Win32_VolumeChangeEvent", true},
    {"Win32_LogonSession", true},
    {"Win32_DeviceChangeEvent", true},
    {"Win32_PowerManagementEvent", true},
    {"Win32_ComputerShutdownEvent", true},
    {"Win32_ServiceStateChangeEvent", true},
    {"Win32_JobCreationEvent", true},
    {"MSNdis_StatusMediaConnectEvent", true},
    {"MSNdis_StatusMediaDisconnectEvent", true},
    {"MSFT_NetIPAddressChangeEvent", true},
    {"MSFT_NetFirewallRuleChangeEvent", true},
    {"Win32_SystemConfigurationChangeEvent", true},
    {"Win32_ModuleLoadTraceEvent", true},


    // 需要时也可在UI里加：
    // {"RegistryKeyChangeEvent:HKLM:SOFTWARE\\MyApp", false},
};

// [CHANGED] ImGui 面板：为“事件类型选择”加入可滚动区域 + 过滤 + 多列布局 + 批量操作
void KswordMonitorMain() {
    if (ImGui::GetCurrentContext() == nullptr) return;

    ImGui::Text(C("选择监控事件类型:"));

    // ---------- 过滤与批量操作工具条 ----------
    // [NEW] 过滤输入（在当前会话内保存）
    static char s_filter[128] = { 0 };
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##evt_filter", C("过滤(支持子串匹配)"), s_filter, IM_ARRAYSIZE(s_filter));

    ImGui::SameLine();
    if (ImGui::SmallButton(C("全选"))) {
        const bool hasFilter = (s_filter[0] != '\0');
        for (auto& [name, on] : eventTypes) {
            if (!hasFilter || (strstr(name.c_str(), s_filter) != nullptr)) on = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(C("全不选"))) {
        const bool hasFilter = (s_filter[0] != '\0');
        for (auto& [name, on] : eventTypes) {
            if (!hasFilter || (strstr(name.c_str(), s_filter) != nullptr)) on = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(C("反选"))) {
        const bool hasFilter = (s_filter[0] != '\0');
        for (auto& [name, on] : eventTypes) {
            if (!hasFilter || (strstr(name.c_str(), s_filter) != nullptr)) on = !on;
        }
    }

    ImGui::Separator();

    // ---------- 可滚动的事件类型区域 ----------
    // [NEW] 限高的子窗口，避免挤压下面的事件表
    // 高度按 ~8 行控件估算，你也可以做成配置项
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float childH = rowH * 8.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    if (ImGui::BeginChild("EventTypeList", ImVec2(0, childH), true,
        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {

        // [NEW] 先生成过滤后的索引列表
        std::vector<int> filtered;
        filtered.reserve(eventTypes.size());
        const bool useFilter = (s_filter[0] != '\0');
        for (int i = 0; i < (int)eventTypes.size(); ++i) {
            if (!useFilter || (strstr(eventTypes[i].first.c_str(), s_filter) != nullptr))
                filtered.push_back(i);
        }

        // [NEW] 自适应列数：每列大概 220px，最多 4 列，至少 1 列
        int cols = (int)std::floor(ImGui::GetContentRegionAvail().x / 220.0f);
        cols = (cols < 1) ? 1 : (cols > 4 ? 4 : cols);

        if (ImGui::BeginTable("EvtTypeTable", cols,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {

            // [NEW] 使用行裁剪器，长列表也不卡
            const int rows = (int)((filtered.size() + cols - 1) / cols);
            ImGuiListClipper clipper;
            clipper.Begin(rows, rowH);
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < cols; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        int idxInFiltered = r * cols + c;
                        if (idxInFiltered >= (int)filtered.size()) continue;
                        int realIdx = filtered[idxInFiltered];

                        auto& nameEnabled = eventTypes[realIdx];
                        bool  enabled = nameEnabled.second;
                        // 显示复选框；为避免标签过长占位，支持悬浮显示完整名
                        ImGui::Checkbox(nameEnabled.first.c_str(), &enabled);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", nameEnabled.first.c_str());
                        }
                        nameEnabled.second = enabled;
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    // ---------- 控制按钮区 ----------
    ImGui::Separator();

    if (ImGui::Button(C("开始监控")) && !g_eventMonitor.IsMonitoring()) {
        std::vector<std::string> selected;
        selected.reserve(eventTypes.size());
        for (const auto& [type, enabled] : eventTypes)
            if (enabled) selected.push_back(type);
        g_eventMonitor.StartMonitoring(selected);
    }

    ImGui::SameLine();
    if (ImGui::Button(C("停止监控")) && g_eventMonitor.IsMonitoring()) {
        g_eventMonitor.StopMonitoring();
    }

    ImGui::SameLine();
    if (ImGui::Button(C("清空列表"))) {
        g_eventMonitor.ClearEvents();
    }

    ImGui::SameLine();
    // [NEW] 统计已选数量
    int selectedCount = 0;
    for (auto& kv : eventTypes) if (kv.second) ++selectedCount;
    ImGui::Text("%s | %s: %d",
        g_eventMonitor.IsMonitoring() ? C("监控中...") : C("未监控"),
        C("已选"), selectedCount);

    // ---------- 事件列表显示 ----------
    ImGui::Separator();
    ImGui::Text(C("事件列表:"));

    // 可选自动滚动（默认开启）
    static bool s_autoScroll = true;
    ImGui::SameLine();
    ImGui::Checkbox(C("自动滚动"), &s_autoScroll);

    // 表格外部尺寸要有一个明确的高度（比如 280px），而不是“无限高”
    /*static float s_tableHeight = 280.0f;
    ImVec2 outerSize(0.0f, s_tableHeight);*/
    const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y); // 占满剩余高度

    // 使用表格自带滚动
    const ImGuiTableFlags tblFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable;

    if (ImGui::BeginTable("EventsTable", 5, tblFlags, outerSize)) {
        ImGui::TableSetupColumn(C("ID"), ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn(C("时间"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn(C("类型"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn(C("源"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn(C("描述"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto events = g_eventMonitor.GetEvents();
        const int n = (int)events.size();

        for (int i = n - 1; i >= 0; --i) {
            const auto& ev = events[i];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", (unsigned long long)ev.id);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(DmtfToReadable(ev.timestamp.c_str()).c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(C(ev.type.c_str()));
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(C(ev.source.c_str()));
            ImGui::TableSetColumnIndex(4); ImGui::TextWrapped("%s", C(ev.description.c_str()));

            // 自动滚动
            if (s_autoScroll && i == 0) {
                ImGui::SetScrollHereY(1.0f);
            }
        }

        ImGui::EndTable();
    }
    ImGui::EndTabItem();
}

