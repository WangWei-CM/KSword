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
#include <unordered_map>
// 若 evntrace.h 未定义，补充定义，放在 #include 之后
#ifndef EVENT_TRACE_ENABLE_INFO_DEFINED
#define EVENT_TRACE_ENABLE_INFO_DEFINED
#define CURRENT_MODULE "事件监控"
typedef struct _EVENT_TRACE_ENABLE_INFO {
    ULONG Version;               // 必须为 EVENT_TRACE_ENABLE_INFO_VERSION (1)
    ULONG EnableProperty;        // 启用的属性，如 EVENT_ENABLE_PROPERTY_STACK_TRACE等
} EVENT_TRACE_ENABLE_INFO, * PEVENT_TRACE_ENABLE_INFO;

// 版本号根据微软的规定填写
#define EVENT_TRACE_ENABLE_INFO_VERSION 1

#endif  // EVENT_TRACE_ENABLE_INFO_DEFINED

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

using BYTE = unsigned char;

// 为TCP/IP事件提供器GUID未找到的情况做兼容
// 使用Microsoft-Windows-TCPIP提供器的GUID
const GUID GUID_TCPIP_EVENTS = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };

// 事件记录结构
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

// 全局事件监控器类（单例模式）
class EventMonitor {
private:
    bool isRunning = false;
    std::vector<EventRecord> events;
    std::mutex eventsMutex;
    std::thread monitoringThread;
    std::vector<std::string> selectedEventTypes;

    // WMI 相关接口
    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IUnsecuredApartment* pUnsecApp = nullptr;
    IUnknown* pStubUnk = nullptr;
    IWbemObjectSink* pStubSink = nullptr;

    // ETW 相关接口
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
                return S_OK;
            }
            // FIX-ME： 这玩意咋也会导致 QI 递归啊？
            // 添加对IMarshal接口的支持
            //else if (riid == IID_IMarshal) {
            //    // 委托给标准Marshaller
            //    return CoGetStandardMarshal(riid, static_cast<IUnknown*>(this),
            //        MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL,
            //        reinterpret_cast<IMarshal**>(ppv));
            //}

            // 应该避免在 QI 中调用任何可能引起再次访问 QI 的方法，这会导致递归调用，引发栈溢出
            // 添加日志记录请求的接口
            // 记录不支持的接口IID（避免使用StringFromIID，直接记录IID的数值部分）
            // 将IID拆分成多个部分记录
            char buffer[256] = { 0 };
            sprintf_s(buffer, sizeof(buffer), "EventSink不支持接口: {%08lX-%04hX-%04hX-%02hX%02hX-%02hX%02hX%02hX%02hX%02hX%02hX}",
                riid.Data1, riid.Data2, riid.Data3,
                riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
            kLog.Add(Warn, C(buffer), C("事件监控"));
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        // IWbemObjectSink 接口实现
        HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
            if (lObjectCount <= 0 || apObjArray == nullptr) {
                kLog.Add(Warn, C("Indicate 接收事件数量无效或对象数组为空"), C("事件监控"));
                return WBEM_S_NO_ERROR;
            }

            // 处理每个事件之前的通用逻辑
            for (int i = 0; i < lObjectCount; ++i) {
                if (apObjArray[i] == nullptr) {
                    kLog.Add(Warn, C("事件对象数组中存在空指针"), C("事件监控"));
                    continue;
                }

                VARIANT vtProp;
                std::string type, source, description;

                // 1. 获取事件类型（__Class 属性）
                HRESULT hres = apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtProp, 0, 0);
                if (FAILED(hres)) {
                    kLog.Add(Warn, C("获取事件__Class属性失败，错误代码: " + std::to_string(hres)), C("事件监控"));
                }
                else {
                    if (vtProp.vt == VT_BSTR) {
                        type = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                    }
                    else {
                        kLog.Add(Warn, C("事件__Class属性类型不是BSTR"), C("事件监控"));
                    }
                    VariantClear(&vtProp);
                }

                // 2. 处理实例事件（__InstanceXXXEvent）的 TargetInstance 属性
                if (type == "__InstanceCreationEvent" || type == "__InstanceDeletionEvent" || type == "__InstanceModificationEvent") {
                    // 获取 TargetInstance 实际的对象引用
                    HRESULT targetRes = apObjArray[i]->Get(_bstr_t(L"TargetInstance"), 0, &vtProp, 0, 0);
                    if (FAILED(targetRes)) {
                        kLog.Add(Warn, C("获取TargetInstance属性失败，错误代码: " + std::to_string(targetRes)), C("事件监控"));
                        VariantClear(&vtProp);
                        continue;
                    }

                    if (vtProp.vt == VT_DISPATCH) {
                        IWbemClassObject* pTargetInstance = nullptr;
                        HRESULT qiRes = vtProp.pdispVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance);
                        if (FAILED(qiRes)) {
                            kLog.Add(Warn, C("TargetInstance查询IWbemClassObject接口失败，错误代码: " + std::to_string(qiRes)), C("事件监控"));
                            VariantClear(&vtProp);
                            continue;
                        }

                        // 从 TargetInstance 中获取名称
                        VARIANT vtName;
                        HRESULT nameRes = pTargetInstance->Get(_bstr_t(L"Name"), 0, &vtName, 0, 0);
                        if (FAILED(nameRes)) {
                            kLog.Add(Warn, C("获取TargetInstance名称失败，错误代码: " + std::to_string(nameRes)), C("事件监控"));
                        }
                        else {
                            if (vtName.vt == VT_BSTR) {
                                source = _com_util::ConvertBSTRToString(vtName.bstrVal);
                            }
                            else {
                                kLog.Add(Warn, C("TargetInstance名称属性类型不是BSTR"), C("事件监控"));
                            }
                            VariantClear(&vtName);
                        }
                        pTargetInstance->Release();
                    }
                    else {
                        kLog.Add(Warn, C("TargetInstance属性类型不是VT_DISPATCH"), C("事件监控"));
                    }
                    VariantClear(&vtProp);

                    // 设置描述信息
                    if (type == "__InstanceCreationEvent") description = "Process started";
                    else if (type == "__InstanceDeletionEvent") description = "Process stopped";
                    else if (type == "MSFT_NetConnectionCreate") {
                        // 处理网络连接事件
                        HRESULT pidRes = apObjArray[i]->Get(L"ProcessId", 0, &vtProp, 0, 0);
                        if (FAILED(pidRes)) {
                            kLog.Add(Warn, C("获取MSFT_NetConnectionCreate事件的ProcessId失败，错误代码: " + std::to_string(pidRes)),C(CURRENT_MODULE));
                        }
                        else {
                            if (vtProp.vt == VT_I4) {
                                DWORD pid = vtProp.lVal;
                                source = "PID: " + std::to_string(pid);
                            }
                            else {
                                kLog.Add(Warn, C("ProcessId属性类型不是整数"),C(CURRENT_MODULE));
                            }
                            VariantClear(&vtProp);
                        }
                        description = "Network connection created";
                    }
                    else description = "Process modified";
                }
                // 3. 处理 Win32_ProcessStartTrace/StopTrace 事件
                else if (type == "Win32_ProcessStartTrace" || type == "Win32_ProcessStopTrace") {
                    HRESULT nameRes = apObjArray[i]->Get(_bstr_t(L"ProcessName"), 0, &vtProp, 0, 0);
                    if (FAILED(nameRes)) {
                        kLog.Add(Warn, C("获取ProcessName属性失败，错误代码: " + std::to_string(nameRes)), C(CURRENT_MODULE));
                    }
                    else {
                        if (vtProp.vt == VT_BSTR) {
                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                        }
                        else {
                            kLog.Add(Warn, C("ProcessName属性类型不是BSTR"), C(CURRENT_MODULE));
                        }
                        VariantClear(&vtProp);
                    }
                    description = type == "Win32_ProcessStartTrace" ? "Process start traced" : "Process stop traced";
                }
                else if (type == "MSFT_NetConnectionCreate") {
                    // 先判断类是否存在（通过尝试获取一个关键属性验证）
                    HRESULT propRes = apObjArray[i]->Get(_bstr_t(L"RemoteAddress"), 0, &vtProp, 0, 0);
                    if (FAILED(propRes)) {
                        kLog.Add(Warn, C("MSFT_NetConnectionCreate 类或属性不存在，跳过解析（错误码: " + std::to_string(propRes) + "）"), C(CURRENT_MODULE));
                        VariantClear(&vtProp);
                        // 仅记录事件类型，不添加详细信息（避免空数据）
                        source = "Unsupported";
                        description = "Network event class not supported on this system";
                    }
                    else {
                        // 正常解析属性
                        if (vtProp.vt == VT_BSTR) {
                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                        }
                        else {
                            source = "Unknown";
                        }
                        VariantClear(&vtProp);
                        description = "Network connection created";
                    }
                }
                // 5. 其他未处理事件类型
                else {
                    kLog.Add(Info, C("未处理的事件类型: " + type), C(CURRENT_MODULE));
                    source = "Unprocessed";
                    description = "Event type not handled";
                }
                // 4. 生成时间戳
                auto now = std::chrono::system_clock::now();
                std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
                if (nowTime == -1) {
                    kLog.Add(Warn, C("转换系统时间失败"), C(CURRENT_MODULE));
                    continue;
                }
                std::tm* localTime = std::localtime(&nowTime);
                if (localTime == nullptr) {
                    kLog.Add(Warn, C("获取本地时间失败"), C(CURRENT_MODULE));
                    continue;
                }
                std::stringstream ss;
                ss << std::put_time(localTime, "%Y-%m-%d %H:%M:%S");

                // 5. 添加事件到列表（确保有有效信息）
                if (!source.empty()) {
                    parent->addEvent(EventRecord(ss.str(), type, source, description));
                }
                else {
                    kLog.Add(Warn, C("事件源信息为空，跳过该事件"), C(CURRENT_MODULE));
                }
            }
            return WBEM_S_NO_ERROR;
        }

        HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject* pObjParam) override {
            if (FAILED(hResult)) {
                kLog.Add(Warn, C("SetStatus 返回错误，代码: " + std::to_string(hResult)), C(CURRENT_MODULE));
            }
            return WBEM_S_NO_ERROR;
        }
    };

    // 初始化 WMI 环境
    bool initializeWMI() {
        HRESULT hres = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
        if (FAILED(hres)) {
            kLog.Add(Err, C("无法成功初始化WMI环境，错误1: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("CoInitializeEx 初始化成功"), C(CURRENT_MODULE));

        hres = CoInitializeSecurity(
            NULL, -1, NULL, NULL,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL, EOAC_NONE, NULL
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("无法成功初始化WMI环境，错误2: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("CoInitializeSecurity 初始化成功"), C(CURRENT_MODULE));

        hres = CoCreateInstance(
            CLSID_WbemLocator, NULL,
            CLSCTX_INPROC_SERVER, IID_IWbemLocator,
            reinterpret_cast<LPVOID*>(&pLoc)
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("无法成功初始化WMI环境，错误3: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("CoCreateInstance 创建IWbemLocator成功"), C(CURRENT_MODULE));

        hres = pLoc->ConnectServer(
            _bstr_t(L"ROOT\\CIMV2"),
            NULL, NULL, 0, NULL, 0, 0, &pSvc
        );
        if (FAILED(hres)) {
            pLoc->Release();
            CoUninitialize();
            kLog.Add(Err, C("无法成功初始化WMI环境，错误4: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("ConnectServer 连接WMI服务成功"), C(CURRENT_MODULE));

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
            kLog.Add(Err, C("无法成功初始化WMI环境，错误5: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("成功初始化WMI环境"), C(CURRENT_MODULE));
        return true;
    }

    // 订阅 WMI 事件（动态拼接查询语句）
    bool subscribeToEvents() {
        HRESULT hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL,
            CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
            reinterpret_cast<void**>(&pUnsecApp));
        if (FAILED(hres)) {
            kLog.Add(Err, C("创建IUnsecuredApartment失败，错误代码: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("创建IUnsecuredApartment成功"), C(CURRENT_MODULE));

        // 创建事件接收实例（用于处理接收到的事件类型）
        EventSink* pSink = new EventSink(this);
        pSink->AddRef();

        // 创建封送代理
        IStream* pStream = nullptr;
        HRESULT hr = CoMarshalInterThreadInterfaceInStream(
            IID_IWbemObjectSink,
            static_cast<IWbemObjectSink*>(pSink),
            &pStream
        );

        if (FAILED(hr)) {
            kLog.Add(Err, C("创建接口流失败: " + std::to_string(hr)), C(CURRENT_MODULE));
            pSink->Release();
            return false;
        }

        // 在目标线程解封送
        hr = CoGetInterfaceAndReleaseStream(
            pStream,
            IID_IWbemObjectSink,
            reinterpret_cast<void**>(&pStubSink)
        );

        if (FAILED(hr)) {
            kLog.Add(Err, C("解封送接口失败: " + std::to_string(hr)), C(CURRENT_MODULE));
            pSink->Release();
            return false;
        }

        hres = pUnsecApp->CreateObjectStub(static_cast<IUnknown*>(pSink), &pStubUnk);
        if (FAILED(hres)) {
            kLog.Add(Err, C("创建ObjectStub失败，错误代码: " + std::to_string(hres)), C(CURRENT_MODULE));
            pSink->Release();
            return false;
        }
        kLog.Add(Info, C("CreateObjectStub 成功"), C(CURRENT_MODULE));

        hres = pStubUnk->QueryInterface(IID_IWbemObjectSink,
            reinterpret_cast<void**>(&pStubSink));
        if (FAILED(hres)) {
            kLog.Add(Err, C("获取IWbemObjectSink接口失败，错误代码: " + std::to_string(hres)), C(CURRENT_MODULE));
            pStubUnk->Release();
            pSink->Release();
            return false;
        }
        kLog.Add(Info, C("获取IWbemObjectSink接口成功"), C(CURRENT_MODULE));

        // 为实例事件和目标类建立映射
        std::unordered_map<std::string, std::string> instanceEventTargetMap = {
            {"__InstanceCreationEvent", "Win32_Process"},       // 进程创建
            {"__InstanceDeletionEvent", "Win32_Process"},       // 进程删除
            {"__InstanceModificationEvent", "Win32_Process"},   // 进程修改
            {"RegistryKeyChangeEvent", "Win32_RegistryKey"}     // 注册表变更
        };

        // 处理每个事件类型（可修改，去掉UNION等复杂结构）
        for (const auto& eventType : selectedEventTypes) {
            std::string query;

            // 构建事件查询语句
            if (instanceEventTargetMap.count(eventType)) {
                std::string targetClass = instanceEventTargetMap[eventType];
                query = "SELECT * FROM " + eventType +
                    " WITHIN 1 WHERE TargetInstance ISA '" + targetClass + "'";
            }
            else if (eventType == "Win32_ProcessStartTrace" || eventType == "Win32_ProcessStopTrace") {
                query = "SELECT * FROM " + eventType + " WITHIN 1"; // 添加WITHIN 1
            }
            else {
                query = "SELECT * FROM " + eventType + " WITHIN 1";
            }

            // 为每个事件类型执行订阅
            hres = pSvc->ExecNotificationQueryAsync(
                _bstr_t(L"WQL"),  // 查询语言：WQL
                _bstr_t(query.c_str()),  // 事件查询语句
                WBEM_FLAG_SEND_STATUS,
                NULL,
                pStubSink  // 事件接收回调实例
            );

            // 若任一事件订阅失败，整体返回失败
            if (FAILED(hres)) {
                kLog.Add(Err, C("订阅事件 " + eventType + " 失败，错误代码: " + std::to_string(hres)), C(CURRENT_MODULE));
                pStubSink->Release();
                pStubUnk->Release();
                pSink->Release();
                return false;
            }
            kLog.Add(Info, C("成功订阅事件: " + eventType), C(CURRENT_MODULE));
        }

        return true;
    }

    // 停止 WMI 监控
    void stopWMI() {
        kLog.Add(Info, C("开始停止WMI监控"), C(CURRENT_MODULE));
        if (pSvc && pStubSink) {
            HRESULT hres = pSvc->CancelAsyncCall(pStubSink);
            if (FAILED(hres)) {
                kLog.Add(Warn, C("取消WMI异步调用失败，错误代码: " + std::to_string(hres)), C(CURRENT_MODULE));
            }
            else {
                kLog.Add(Info, C("成功取消WMI异步调用"), C(CURRENT_MODULE));
            }
        }

        // 释放资源（逆序释放）
        if (pStubSink) {
            pStubSink->Release();
            pStubSink = nullptr;
            kLog.Add(Info, C("释放pStubSink成功"), C(CURRENT_MODULE));
        }
        if (pStubUnk) {
            pStubUnk->Release();
            pStubUnk = nullptr;
            kLog.Add(Info, C("释放pStubUnk成功"), C(CURRENT_MODULE));
        }
        if (pUnsecApp) {
            pUnsecApp->Release();
            pUnsecApp = nullptr;
            kLog.Add(Info, C("释放pUnsecApp成功"), C(CURRENT_MODULE));
        }
        if (pSvc) {
            pSvc->Release();
            pSvc = nullptr;
            kLog.Add(Info, C("释放pSvc成功"), C(CURRENT_MODULE));
        }
        if (pLoc) {
            pLoc->Release();
            pLoc = nullptr;
            kLog.Add(Info, C("释放pLoc成功"), C(CURRENT_MODULE));
        }

        // 最后释放COM初始化（确保只调用一次）
        static int comCount = 0;
        comCount--;
        if (comCount <= 0) {
            CoUninitialize();
            comCount = 0;
            kLog.Add(Info, C("CoUninitialize 成功"), C(CURRENT_MODULE));
        }
    }

    // ETW 事件回调函数
    static VOID WINAPI EtwEventCallback(EVENT_RECORD* pEventRecord) {
        if (pEventRecord == nullptr) {
            kLog.Add(Warn, C("EtwEventCallback 接收空事件记录"), C(CURRENT_MODULE));
            return;
        }

        // 获取当前实例指针
        EventMonitor* monitor = static_cast<EventMonitor*>(pEventRecord->UserContext);
        if (monitor == nullptr) {
            kLog.Add(Warn, C("EtwEventCallback 无法获取EventMonitor实例"), C(CURRENT_MODULE));
            return;
        }

        // 格式化时间戳
        SYSTEMTIME st;
        FILETIME ft;
        ULARGE_INTEGER uli;
        uli.QuadPart = pEventRecord->EventHeader.TimeStamp.QuadPart;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        if (!FileTimeToSystemTime(&ft, &st)) {
            kLog.Add(Warn, C("FileTimeToSystemTime 转换失败"), C(CURRENT_MODULE));
            return;
        }

        std::stringstream ss;
        ss << std::setw(4) << std::setfill('0') << st.wYear << "-"
            << std::setw(2) << std::setfill('0') << st.wMonth << "-"
            << std::setw(2) << std::setfill('0') << st.wDay << " "
            << std::setw(2) << std::setfill('0') << st.wHour << ":"
            << std::setw(2) << std::setfill('0') << st.wMinute << ":"
            << std::setw(2) << std::setfill('0') << st.wSecond;

        // 记录事件信息
        std::string type = "ETW Event";
        std::string source = "TCP/IP";
        std::string description = "Network event captured";

        monitor->addEvent(EventRecord(ss.str(), type, source, description));
    }
    void generateUniqueSessionName() {
        // 基础名称
        const WCHAR baseName[] = L"KswordETWMonitor_";

        // 获取当前系统时间
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm* localTime = std::localtime(&nowTime);

        // 提取日期（年-月-日）和秒数
        WCHAR timeSuffix[64] = { 0 };
        swprintf_s(
            timeSuffix,
            L"%04d%02d%02d_%02d",  // 格式：年月日_秒（如20240730_15）
            localTime->tm_year + 1900,
            localTime->tm_mon + 1,
            localTime->tm_mday,
            localTime->tm_sec  // 秒数确保同一分钟内的唯一性
        );

        // 拼接完整会话名（基础名称+时间后缀）
        swprintf_s(
            etwSessionName,
            L"%s%s",
            baseName,
            timeSuffix
        );

        //kLog.Add(Info, C(std::wstring(L"生成唯一ETW会话名: " )+ std::wstring(etwSessionName)));
    }

    // 初始化 ETW 监控会话
    bool initializeETW() {
        kLog.Add(Info, C("开始初始化ETW监控"), C(CURRENT_MODULE));
        // 1. 计算内存大小，用于分配会话属性
        DWORD bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(etwSessionName);
        etwProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, bufferSize);
        if (etwProps == nullptr) {
            kLog.Add(Err, C("分配ETW属性内存失败"), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("ETW属性内存分配成功"), C(CURRENT_MODULE));

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
            kLog.Add(Err, C("启动ETW会话失败，错误代码: " + std::to_string(status)), C(CURRENT_MODULE));
            LocalFree(etwProps);
            etwProps = nullptr;
            return false;
        }
        kLog.Add(Info, C("ETW会话启动成功"), C(CURRENT_MODULE));

        // 4. 指定TCP/IP事件提供器
        const GUID providerGuid = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };


        // 5. 初始化ENABLE_TRACE_PARAMETERS结构（适配较新的系统）
        ENABLE_TRACE_PARAMETERS enableParams = { 0 };
        enableParams.Version = 1;  // 对应版本结构的版本号，包含EnableInfo成员
        enableParams.EnableProperty = EVENT_ENABLE_PROPERTY_STACK_TRACE;  // 启用堆栈跟踪

        // 6. 启用事件提供器
        status = EnableTraceEx2(
            etwSession,                          // 跟踪会话句柄
            &providerGuid,                       // 提供器GUID
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,  // 启用提供器
            TRACE_LEVEL_INFORMATION,             // 事件级别（这里为信息级）
            0x1,                                 // MatchAnyKeyword（筛选关键字）
            0x0,                                 // MatchAllKeyword
            0,                                   // 超时时间
            &enableParams                        // 结构参数（包含扩展信息）
        );

        if (status != ERROR_SUCCESS) {
            kLog.Add(Err, C("启用TCP/IP事件提供器失败，错误代码: " + std::to_string(status)), C(CURRENT_MODULE));
            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            LocalFree(etwProps);
            etwProps = nullptr;
            etwSession = 0;
            return false;
        }
        kLog.Add(Info, C("TCP/IP事件提供器启用成功"), C(CURRENT_MODULE));

        // 7. 启动事件处理线程
        std::thread etwThread([this]() {
            EVENT_TRACE_LOGFILE traceLogFile = { 0 };
            traceLogFile.LoggerName = etwSessionName;
            traceLogFile.EventRecordCallback = EtwEventCallback;
            traceLogFile.Context = this;
            traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;

            TRACEHANDLE traceHandle = OpenTrace(&traceLogFile);
            if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
                kLog.Add(Err, C("打开ETW跟踪失败，错误代码: " + std::to_string(GetLastError())), C(CURRENT_MODULE));
                return;
            }
            kLog.Add(Info, C("ETW跟踪打开成功"), C(CURRENT_MODULE));

            // 处理事件直到停止
            while (isRunning) {
                ULONG processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
                if (processStatus != ERROR_SUCCESS && processStatus != ERROR_MORE_DATA) {
                    kLog.Add(Err, C("处理ETW事件失败，错误代码: " + std::to_string(processStatus)), C(CURRENT_MODULE));
                    break;
                }
            }

            CloseTrace(traceHandle);
            kLog.Add(Info, C("ETW跟踪已关闭"), C(CURRENT_MODULE));
            });

        etwThread.detach();
        kLog.Add(Info, C("ETW事件处理线程启动成功"), C(CURRENT_MODULE));
        return true;
    }

    // 停止 ETW 监控
    void stopETW() {
        kLog.Add(Info, C("开始停止ETW监控"), C(CURRENT_MODULE));
        if (etwSession != 0 && etwProps != nullptr) {
            // 停止跟踪会话
            ULONG status = ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            if (status != ERROR_SUCCESS) {
                kLog.Add(Warn, C("停止ETW会话失败，错误代码: " + std::to_string(status)), C(CURRENT_MODULE));
            }
            else {
                kLog.Add(Info, C("ETW会话停止成功"), C(CURRENT_MODULE));
            }
            etwSession = 0;
        }

        if (etwProps != nullptr) {
            LocalFree(etwProps);
            etwProps = nullptr;
            kLog.Add(Info, C("ETW属性内存释放成功"), C(CURRENT_MODULE));
        }
    }

    // 安全添加事件到列表
    void addEvent(const EventRecord& event) {
        try {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.insert(events.begin(), event);
            if (events.size() > 1000) {  // 限制事件数量
                events.pop_back();
            }
        }
        catch (const std::exception& e) {
            kLog.Add(Err, C("添加事件到列表失败: " + std::string(e.what())), C(CURRENT_MODULE));
        }
        catch (...) {
            kLog.Add(Err, C("添加事件到列表时发生未知错误"), C(CURRENT_MODULE));
        }
    }

public:
    EventMonitor() = default;
    ~EventMonitor() {
        stopMonitoring();
    }

    // 启动监控（WMI + ETW）
    void startMonitoring(const std::vector<std::string>& eventTypes) {
        if (isRunning) {
            kLog.Add(Warn, C("监控已在运行，无需重复启动"), C(CURRENT_MODULE));
            return;
        }

        if (eventTypes.empty()) {
            kLog.Add(Warn, C("未选择任何事件类型，启动监控失败"), C(CURRENT_MODULE));
            return;
        }

        selectedEventTypes = eventTypes;
        isRunning = true;
        kLog.Add(Info, C("开始启动监控线程"), C(CURRENT_MODULE));

        monitoringThread = std::thread([this]() {
            bool wmiOk = initializeWMI() && subscribeToEvents();
            bool etwOk = initializeETW();

            if (!wmiOk && !etwOk) {
                kLog.Add(Err, C("WMI和ETW监控均初始化失败，监控无法启动"), C(CURRENT_MODULE));
                isRunning = false;
                return;
            }

            while (isRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (wmiOk) {
                stopWMI();
            }
            if (etwOk) {
                stopETW();
            }
            kLog.Add(Info, C("监控线程已退出"), C(CURRENT_MODULE));
            });
    }

    // 停止监控
    void stopMonitoring() {
        if (!isRunning) {
            kLog.Add(Warn, C("监控未在运行，无需停止"), C(CURRENT_MODULE));
            return;
        }

        kLog.Add(Info, C("正在停止监控"), C(CURRENT_MODULE));
        isRunning = false;
        if (monitoringThread.joinable()) {
            monitoringThread.join();
            kLog.Add(Info, C("监控线程已成功join"), C(CURRENT_MODULE));
        }
        else {
            kLog.Add(Warn, C("监控线程不可join"), C(CURRENT_MODULE));
        }
    }

    // 清空事件列表
    void clearEvents() {
        try {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.clear();
            kLog.Add(Info, C("已清空事件列表"), C(CURRENT_MODULE));
        }
        catch (const std::exception& e) {
            kLog.Add(Err, C("清空事件列表失败: " + std::string(e.what())), C(CURRENT_MODULE));
        }
        catch (...) {
            kLog.Add(Err, C("清空事件列表时发生未知错误"), C(CURRENT_MODULE));
        }
    }

    // 获取当前事件列表
    std::vector<EventRecord> getEvents() {
        try {
            std::lock_guard<std::mutex> lock(eventsMutex);
            return events;
        }
        catch (const std::exception& e) {
            kLog.Add(Err, C("获取事件列表失败: " + std::string(e.what())), C(CURRENT_MODULE));
            return {};
        }
        catch (...) {
            kLog.Add(Err, C("获取事件列表时发生未知错误"), C(CURRENT_MODULE));
            return {};
        }
    }

    // 查询是否正在监控
    bool isMonitoring() const {
        return isRunning;
    }
};

// 全局事件监控器实例
EventMonitor g_eventMonitor;

// 扩展事件类型列表（可根据需要增删）
std::vector<std::pair<std::string, bool>> eventTypes = {
    { "__InstanceCreationEvent", false },   // 实例创建事件（进程等）
    { "__InstanceDeletionEvent", false },   // 实例删除事件
    { "__InstanceModificationEvent", false },// 实例修改事件
    { "Win32_ProcessStartTrace", false },   // 进程启动跟踪
    { "Win32_ProcessStopTrace", false },    // 进程停止跟踪
    { "MSFT_NetConnectionCreate", false },  // 网络连接创建（Win10+支持）
    { "CIM_DataFile", false },              // 文件系统事件（创建/删除等）
    { "RegistryKeyChangeEvent", false },    // 注册表键变更
    { "SecurityEvent", false },             // 安全日志事件
};


void KswordMonitorMain() {
    if (ImGui::GetCurrentContext() == nullptr) {
        kLog.Add(Err, C("ImGui上下文未初始化，无法显示监控界面"), C(CURRENT_MODULE));
        return;
    }

    ImGui::Text(C("选择要监控的事件类型:"), C(CURRENT_MODULE));
    ImGui::Separator();

    // 每行显示2个选项 - 确保Columns正确使用
    {
        int columns = 2;
        ImGui::Columns(columns, NULL, false);  // Push columns

        for (size_t i = 0; i < eventTypes.size(); ++i) {
            if (!ImGui::Checkbox(eventTypes[i].first.c_str(), &eventTypes[i].second)) {
                // 不需要错误日志，Checkbox失败通常不影响流程
            }
            if ((i + 1) % columns == 0) {
                ImGui::NextColumn();
            }
        }

        ImGui::Columns(1);  // Pop columns回到默认
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

    // 事件列表 - 确保Begin/End正确配对
    ImGui::Spacing();
    ImGui::Text(C("事件列表:"));
    ImGui::Separator();

    auto events = g_eventMonitor.getEvents();

    // 表格显示事件列表
    if (ImGui::BeginTable("事件列表", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
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

        ImGui::EndTable();  // 确保表格正确关闭
    }
    else {
        kLog.Add(Warn, C("创建事件列表表格失败"), C(CURRENT_MODULE));
    }
    ImGui::EndTabItem();
}

#undef CURRENT_MODULE