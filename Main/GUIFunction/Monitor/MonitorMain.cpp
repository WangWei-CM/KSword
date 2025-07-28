#include <Wbemidl.h>
#define NTDDI_VERSION 0x06030000
#define _WIN32_WINNT 0x0603  // ��ʾ Windows 8.1 ������
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
// �� evntrace.h δ���壬�ֶ���ӣ����� #include ֮��
#ifndef EVENT_TRACE_ENABLE_INFO_DEFINED
#define EVENT_TRACE_ENABLE_INFO_DEFINED

typedef struct _EVENT_TRACE_ENABLE_INFO {
    ULONG Version;               // ������Ϊ EVENT_TRACE_ENABLE_INFO_VERSION (1)
    ULONG EnableProperty;        // ���õ����ԣ��� EVENT_ENABLE_PROPERTY_STACK_TRACE��
} EVENT_TRACE_ENABLE_INFO, * PEVENT_TRACE_ENABLE_INFO;

// �汾�������ٷ��ĵ��涨��
#define EVENT_TRACE_ENABLE_INFO_VERSION 1

#endif  // EVENT_TRACE_ENABLE_INFO_DEFINED

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

using BYTE = unsigned char;

// ����TCP/IP�¼��ṩ��GUID��ȱʧ�Ķ��壩
// ����Microsoft-Windows-TCPIP�ṩ�̵�GUID
const GUID GUID_TCPIP_EVENTS = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };

// �¼���¼��
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

// ȫ���¼�������������ࣩ
class EventMonitor {
private:
    bool isRunning = false;
    std::vector<EventRecord> events;
    std::mutex eventsMutex;
    std::thread monitoringThread;
    std::vector<std::string> selectedEventTypes;

    // WMI ��ر���
    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IUnsecuredApartment* pUnsecApp = nullptr;
    IUnknown* pStubUnk = nullptr;
    IWbemObjectSink* pStubSink = nullptr;

    // ETW ��ر���
    TRACEHANDLE etwSession = 0;
    EVENT_TRACE_PROPERTIES* etwProps = nullptr;
    WCHAR etwSessionName[256] = L"KswordETWMonitor";

    // WMI �¼����ջص���
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

        // IWbemObjectSink �ӿڷ���
        HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
            // �������֮ǰ�������¼������߼�
            for (int i = 0; i < lObjectCount; ++i) {
                VARIANT vtProp;
                std::string type, source, description;

                // 1. ��ȡ�¼����ͣ�__Class ���ԣ�
                if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtProp, 0, 0))) {
                    if (vtProp.vt == VT_BSTR) {
                        type = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                    }
                    VariantClear(&vtProp);
                }

                // 2. ����ʵ���¼���__InstanceXXXEvent���� TargetInstance ����
                if (type == "__InstanceCreationEvent" || type == "__InstanceDeletionEvent" || type == "__InstanceModificationEvent") {
                    // ��ȡ TargetInstance��ʵ�ʵĽ��̶���
                    if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"TargetInstance"), 0, &vtProp, 0, 0))) {
                        if (vtProp.vt == VT_DISPATCH) {
                            IWbemClassObject* pTargetInstance = nullptr;
                            if (SUCCEEDED(vtProp.pdispVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance))) {
                                // �� TargetInstance �л�ȡ������
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
                    // ����������Ϣ
                    if (type == "__InstanceCreationEvent") description = "Process started";
                    else if (type == "__InstanceDeletionEvent") description = "Process stopped";
                    // ��EventSink::Indicate�����
                    else if (type == "MSFT_NetConnectionCreate") {
                        // �������������¼�
                        if (SUCCEEDED(apObjArray[i]->Get(L"ProcessId", 0, &vtProp, 0, 0))) {
                            DWORD pid = vtProp.lVal;
                            source = "PID: " + std::to_string(pid);
                            VariantClear(&vtProp);
                        }
                        description = "Network connection created";
                    }
                    else description = "Process modified";
                }
                // 3. ���� Win32_ProcessStartTrace/StopTrace �¼�
                else if (type == "Win32_ProcessStartTrace" || type == "Win32_ProcessStopTrace") {
                    if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"ProcessName"), 0, &vtProp, 0, 0))) {
                        if (vtProp.vt == VT_BSTR) {
                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                        }
                        VariantClear(&vtProp);
                    }
                    description = type == "Win32_ProcessStartTrace" ? "Process start traced" : "Process stop traced";
                }

                // 4. ����ʱ���
                auto now = std::chrono::system_clock::now();
                std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
                std::stringstream ss;
                ss << std::put_time(std::localtime(&nowTime), "%Y-%m-%d %H:%M:%S");

                // 5. ����¼���ȷ������Ч��Ϣ��
                if (!source.empty()) {
                    parent->addEvent(EventRecord(ss.str(), type, source, description));
                }
            }
            return WBEM_S_NO_ERROR;
        }

        HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject* pObjParam) override {
            // ����ʵ�ִ˷�������ʹΪ�գ������������ǳ�����
            return WBEM_S_NO_ERROR;
        }
    };

    // ��ʼ�� WMI ����
    bool initializeWMI() {
        HRESULT hres = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
        if (FAILED(hres)) {
            kLog.Add(Err, C("δ�ܳɹ���ʼ��WMI���ӣ���1����������"));
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
            kLog.Add(Err, C("δ�ܳɹ���ʼ��WMI���ӣ���2����������"));
            return false;
        }

        hres = CoCreateInstance(
            CLSID_WbemLocator, NULL,
            CLSCTX_INPROC_SERVER, IID_IWbemLocator,
            reinterpret_cast<LPVOID*>(&pLoc)
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("δ�ܳɹ���ʼ��WMI���ӣ���3����������"));
            return false;
        }

        hres = pLoc->ConnectServer(
            _bstr_t(L"ROOT\\CIMV2"),
            NULL, NULL, 0, NULL, 0, 0, &pSvc
        );
        if (FAILED(hres)) {
            pLoc->Release();
            CoUninitialize();
            kLog.Add(Err, C("δ�ܳɹ���ʼ��WMI���ӣ���4����������"));
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
            kLog.Add(Err, C("δ�ܳɹ���ʼ��WMI���ӣ���5����������"));
            return false;
        }
        kLog.Add(Info, C("�ɹ���ʼ��WMI����"));
        return true;
    }

    // ���� WMI �¼�����̬ƴ�Ӳ�ѯ��
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

        // ��̬���� WMI ��ѯ���
        std::vector<std::string> queries;
        for (const auto& eventType : selectedEventTypes) {
            if (eventType == "Win32_ProcessStartTrace" || eventType == "Win32_ProcessStopTrace") {
                // ָ�������ռ�
                queries.push_back("SELECT * FROM \\\\.\\ROOT\\CIMV2:" + eventType);
            }
            else if (eventType.find("__Instance") == 0) {
                // ���WITHIN�Ӿ�
                queries.push_back("SELECT * FROM " + eventType + " WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'");
            }
            else {
                queries.push_back("SELECT * FROM " + eventType);
            }
        }
        // ƴ�Ӷ��¼���ѯ
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

    // ֹͣ WMI ����
    void stopWMI() {
        if (pSvc && pStubSink) {
            pSvc->CancelAsyncCall(pStubSink);
        }

        // �ͷ���Դ˳���Ż�
        if (pStubSink) pStubSink->Release();
        if (pStubUnk) pStubUnk->Release();
        if (pUnsecApp) pUnsecApp->Release();
        if (pSvc) pSvc->Release();
        if (pLoc) pLoc->Release();

        // �������һ���ͷ�ʱȡ����ʼ��COM
        static int comCount = 0;
        comCount--;
        if (comCount <= 0) {
            CoUninitialize();
            comCount = 0;
        }
    }

    // ETW �¼��ص�����
    static VOID WINAPI EtwEventCallback(EVENT_RECORD* pEventRecord) {
        // ������Դ�����յ���ETW�¼�
        // ��ʾ�������¼���Ϣ��ӵ��¼��б�
        if (pEventRecord == nullptr) return;

        // ��ȡ��ǰʵ��ָ��
        EventMonitor* monitor = static_cast<EventMonitor*>(pEventRecord->UserContext);
        if (monitor == nullptr) return;

        // ��ʽ��ʱ���
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

        // �����¼���¼
        std::string type = "ETW Event";
        std::string source = "TCP/IP";
        std::string description = "Network event captured";

        monitor->addEvent(EventRecord(ss.str(), type, source, description));
    }

    // ��ʼ�� ETW ����������
    bool initializeETW() {
        // 1. �����ڴ��С�������Ự���ƣ�
        DWORD bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(etwSessionName);
        etwProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, bufferSize);
        if (etwProps == nullptr) {
            return false;
        }

        // 2. ��ʼ���¼����ٻỰ����
        ZeroMemory(etwProps, bufferSize);
        etwProps->Wnode.BufferSize = bufferSize;
        etwProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        etwProps->Wnode.ClientContext = 1;  // QPC��ʱ
        etwProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        etwProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        // 3. �������ٻỰ
        ULONG status = StartTrace(&etwSession, etwSessionName, etwProps);
        if (status != ERROR_SUCCESS) {
            LocalFree(etwProps);
            etwProps = nullptr;
            return false;
        }

        // 4. ����TCP/IP�¼��ṩ��
        const GUID providerGuid = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };

        
        // 5. ��ʼ��ENABLE_TRACE_PARAMETERS���ϸ�ƥ����Ľṹ�嶨�壩
        ENABLE_TRACE_PARAMETERS enableParams = { 0 };
        enableParams.Version = 1;  // �Ͱ汾�ṹ��汾�ţ���EnableInfo��Ա��
        enableParams.EnableProperty = EVENT_ENABLE_PROPERTY_STACK_TRACE;  // ���ö�ջ����
        // ������Ա�������ã�ControlFlags/SourceId�ȣ��˴�����Ĭ�ϣ�

        // 6. ����EnableTraceEx2��������ȫƥ��ṹ��ͺ������壩
        status = EnableTraceEx2(
            etwSession,                          // ���ٻỰ���
            &providerGuid,                       // �ṩ��GUID
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,  // �����ṩ��
            TRACE_LEVEL_INFORMATION,             // �¼�����ֱ����Ϊ������
            0x1,                                 // MatchAnyKeyword�����˹ؼ��֣�
            0x0,                                 // MatchAllKeyword�����˹ؼ��֣�
            0,                                   // ��ʱʱ��
            &enableParams                        // �ṹ���������EnableInfo��ֱ�Ӵ�����
        );

        if (status != ERROR_SUCCESS) {
            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            LocalFree(etwProps);
            etwProps = nullptr;
            etwSession = 0;
            return false;
        }

        // 7. �����¼������߳�
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

            // �����¼�ֱ��ֹͣ
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
    // ֹͣ ETW ���
    void stopETW() {
        if (etwSession != 0 && etwProps != nullptr) {
            // ֹͣ���ٻỰ
            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            etwSession = 0;
        }

        if (etwProps != nullptr) {
            LocalFree(etwProps);
            etwProps = nullptr;
        }
    }

    // ��ȫ����¼����б�
    void addEvent(const EventRecord& event) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.insert(events.begin(), event);
        if (events.size() > 1000) {  // ��������¼�����
            events.pop_back();
        }
    }

public:
    EventMonitor() = default;
    ~EventMonitor() {
        stopMonitoring();
    }

    // ������أ�WMI + ETW��
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

    // ֹͣ���
    void stopMonitoring() {
        if (!isRunning) return;

        isRunning = false;
        if (monitoringThread.joinable()) {
            monitoringThread.join();
        }
    }

    // ����¼��б�
    void clearEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.clear();
    }

    // ��ȡ��ǰ�¼��б�
    std::vector<EventRecord> getEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return events;
    }

    // ��ѯ�Ƿ����ڼ��
    bool isMonitoring() const {
        return isRunning;
    }
};

// ȫ���¼������ʵ��
EventMonitor g_eventMonitor;

// ��չ�¼������б����Ǹ������
std::vector<std::pair<std::string, bool>> eventTypes = {
    { "__InstanceCreationEvent", false },   // ʵ������������/�̵߳ȣ�
    { "__InstanceDeletionEvent", false },   // ʵ��ɾ��
    { "__InstanceModificationEvent", false },// ʵ���޸�
    { "Win32_ProcessStartTrace", false },   // ��������׷��
    { "Win32_ProcessStopTrace", false },    // �����˳�׷��
    { "MSFT_NetConnectionCreate", false },  // �������ӽ�����Win10+��
    { "CIM_DataFile", false },              // �ļ�����������/ɾ���ȣ�
    { "RegistryKeyChangeEvent", false },    // ע������޸�
    { "SecurityEvent", false },             // ��ȫ����¼�
};


void KswordMonitorMain() {
    // �¼�����ѡ��ѡ��
    ImGui::Text(C("ѡ��Ҫ��ص��¼�����:"));
    ImGui::Separator();

    // ÿ����ʾ2����ѡ�� - ʹ��������ȷ��Columns��ȷ���
    {
        int columns = 2;
        ImGui::Columns(columns, NULL, false);  // Push columns

        for (size_t i = 0; i < eventTypes.size(); ++i) {
            ImGui::Checkbox(eventTypes[i].first.c_str(), &eventTypes[i].second);
            if ((i + 1) % columns == 0) {
                ImGui::NextColumn();
            }
        }

        ImGui::Columns(1);  // Pop columns���ָ�Ĭ��
    }

    ImGui::Separator();

    // ���ư�ť
    ImGui::Spacing();
    if (ImGui::Button(C("��ʼ���"), ImVec2(100, 0)) && !g_eventMonitor.isMonitoring()) {
        std::vector<std::string> selectedTypes;
        for (const auto& type : eventTypes) {
            if (type.second) {
                selectedTypes.push_back(type.first);
            }
        }
        g_eventMonitor.startMonitoring(selectedTypes);
    }

    ImGui::SameLine();
    if (ImGui::Button(C("ֹͣ���"), ImVec2(100, 0)) && g_eventMonitor.isMonitoring()) {
        g_eventMonitor.stopMonitoring();
    }

    ImGui::SameLine();
    if (ImGui::Button(C("����б�"), ImVec2(100, 0))) {
        g_eventMonitor.clearEvents();
    }

    ImGui::SameLine();
    if (g_eventMonitor.isMonitoring()) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), C("���ڼ��..."));
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), C("δ���"));
    }

    // �¼���� - ȷ������Begin/End��ȷ���
    ImGui::Spacing();
    ImGui::Text(C("�¼��б�:"));
    ImGui::Separator();

    auto events = g_eventMonitor.getEvents();

    // ������ʹ�ö���������
    // ��KswordMonitorMain��
    ImGui::BeginTable("�¼����", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit); {
        ImGui::TableSetupColumn(C("ʱ��"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(C("Դ"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch);
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

        ImGui::EndTable();  // ȷ�������ȷ�ر�
    }
    ImGui::EndTabItem();
}

