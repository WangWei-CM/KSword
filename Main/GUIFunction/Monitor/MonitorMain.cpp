//#include <Wbemidl.h>
//#define NTDDI_VERSION 0x06030000
//#define _WIN32_WINNT 0x0603  // ��ʾ Windows 8.1 ������
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
//// �� evntrace.h δ���壬���䶨�壬���� #include ֮��
//#ifndef EVENT_TRACE_ENABLE_INFO_DEFINED
//#define EVENT_TRACE_ENABLE_INFO_DEFINED
//
//typedef struct _EVENT_TRACE_ENABLE_INFO {
//    ULONG Version;               // ����Ϊ EVENT_TRACE_ENABLE_INFO_VERSION (1)
//    ULONG EnableProperty;        // ���õ����ԣ��� EVENT_ENABLE_PROPERTY_STACK_TRACE��
//} EVENT_TRACE_ENABLE_INFO, * PEVENT_TRACE_ENABLE_INFO;
//
//// �汾�Ÿ���΢��Ĺ涨��д
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
//// ΪTCP/IP�¼��ṩ��GUIDδ�ҵ������������
//// ʹ��Microsoft-Windows-TCPIP�ṩ����GUID
//const GUID GUID_TCPIP_EVENTS = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };
//
//// �¼���¼�ṹ
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
//// ȫ���¼�������ࣨ����ģʽ��
//class EventMonitor {
//private:
//    bool isRunning = false;
//    std::vector<EventRecord> events;
//    std::mutex eventsMutex;
//    std::thread monitoringThread;
//    std::vector<std::string> selectedEventTypes;
//
//    // WMI ��ؽӿ�
//    IWbemLocator* pLoc = nullptr;
//    IWbemServices* pSvc = nullptr;
//    IUnsecuredApartment* pUnsecApp = nullptr;
//    IUnknown* pStubUnk = nullptr;
//    IWbemObjectSink* pStubSink = nullptr;
//
//    // ETW ��ؽӿ�
//    TRACEHANDLE etwSession = 0;
//    EVENT_TRACE_PROPERTIES* etwProps = nullptr;
//    WCHAR etwSessionName[256] = L"KswordETWMonitor";
//
//    // WMI �¼����ջص���
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
//            // FIX-ME�� ������զҲ�ᵼ�� QI �ݹ鰡��
//            // ��Ӷ�IMarshal�ӿڵ�֧��
//            //else if (riid == IID_IMarshal) {
//            //    // ί�и���׼Marshaller
//            //    return CoGetStandardMarshal(riid, static_cast<IUnknown*>(this),
//            //        MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL,
//            //        reinterpret_cast<IMarshal**>(ppv));
//            //}
//
//            // Ӧ�ñ����� QI �е����κο��������ٴη��� QI �ķ�������ᵼ�µݹ���ã�����ջ���
//            // �����־��¼����Ľӿ�
//            // ��¼��֧�ֵĽӿ�IID������ʹ��StringFromIID��ֱ�Ӽ�¼IID����ֵ���֣�
//            // ��IID��ֳɶ�����ּ�¼
//            char buffer[256] = { 0 };
//            sprintf_s(buffer, sizeof(buffer), "EventSink��֧�ֽӿ�: {%08lX-%04hX-%04hX-%02hX%02hX-%02hX%02hX%02hX%02hX%02hX%02hX}",
//                riid.Data1, riid.Data2, riid.Data3,
//                riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
//                riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
//            kLog.Add(Warn, C(buffer));
//            *ppv = nullptr;
//            return E_NOINTERFACE;
//        }
//
//        // IWbemObjectSink �ӿ�ʵ��
//        HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
//            if (lObjectCount <= 0 || apObjArray == nullptr) {
//                kLog.Add(Warn, C("Indicate �����¼�������Ч���������Ϊ��"));
//                return WBEM_S_NO_ERROR;
//            }
//
//            // ����ÿ���¼�֮ǰ��ͨ���߼�
//            for (int i = 0; i < lObjectCount; ++i) {
//                if (apObjArray[i] == nullptr) {
//                    kLog.Add(Warn, C("�¼����������д��ڿ�ָ��"));
//                    continue;
//                }
//
//                VARIANT vtProp;
//                std::string type, source, description;
//
//                // 1. ��ȡ�¼����ͣ�__Class ���ԣ�
//                HRESULT hres = apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtProp, 0, 0);
//                if (FAILED(hres)) {
//                    kLog.Add(Warn, C("��ȡ�¼�__Class����ʧ�ܣ��������: " + std::to_string(hres)));
//                }
//                else {
//                    if (vtProp.vt == VT_BSTR) {
//                        type = _com_util::ConvertBSTRToString(vtProp.bstrVal);
//                    }
//                    else {
//                        kLog.Add(Warn, C("�¼�__Class�������Ͳ���BSTR"));
//                    }
//                    VariantClear(&vtProp);
//                }
//
//                // 2. ����ʵ���¼���__InstanceXXXEvent���� TargetInstance ����
//                if (type == "__InstanceCreationEvent" || type == "__InstanceDeletionEvent" || type == "__InstanceModificationEvent") {
//                    // ��ȡ TargetInstance ʵ�ʵĶ�������
//                    HRESULT targetRes = apObjArray[i]->Get(_bstr_t(L"TargetInstance"), 0, &vtProp, 0, 0);
//                    if (FAILED(targetRes)) {
//                        kLog.Add(Warn, C("��ȡTargetInstance����ʧ�ܣ��������: " + std::to_string(targetRes)));
//                        VariantClear(&vtProp);
//                        continue;
//                    }
//
//                    if (vtProp.vt == VT_DISPATCH) {
//                        IWbemClassObject* pTargetInstance = nullptr;
//                        HRESULT qiRes = vtProp.pdispVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance);
//                        if (FAILED(qiRes)) {
//                            kLog.Add(Warn, C("TargetInstance��ѯIWbemClassObject�ӿ�ʧ�ܣ��������: " + std::to_string(qiRes)));
//                            VariantClear(&vtProp);
//                            continue;
//                        }
//
//                        // �� TargetInstance �л�ȡ����
//                        VARIANT vtName;
//                        HRESULT nameRes = pTargetInstance->Get(_bstr_t(L"Name"), 0, &vtName, 0, 0);
//                        if (FAILED(nameRes)) {
//                            kLog.Add(Warn, C("��ȡTargetInstance����ʧ�ܣ��������: " + std::to_string(nameRes)));
//                        }
//                        else {
//                            if (vtName.vt == VT_BSTR) {
//                                source = _com_util::ConvertBSTRToString(vtName.bstrVal);
//                            }
//                            else {
//                                kLog.Add(Warn, C("TargetInstance�����������Ͳ���BSTR"));
//                            }
//                            VariantClear(&vtName);
//                        }
//                        pTargetInstance->Release();
//                    }
//                    else {
//                        kLog.Add(Warn, C("TargetInstance�������Ͳ���VT_DISPATCH"));
//                    }
//                    VariantClear(&vtProp);
//
//                    // ����������Ϣ
//                    if (type == "__InstanceCreationEvent") description = "Process started";
//                    else if (type == "__InstanceDeletionEvent") description = "Process stopped";
//                    else if (type == "MSFT_NetConnectionCreate") {
//                        // �������������¼�
//                        HRESULT pidRes = apObjArray[i]->Get(L"ProcessId", 0, &vtProp, 0, 0);
//                        if (FAILED(pidRes)) {
//                            kLog.Add(Warn, C("��ȡMSFT_NetConnectionCreate�¼���ProcessIdʧ�ܣ��������: " + std::to_string(pidRes)));
//                        }
//                        else {
//                            if (vtProp.vt == VT_I4) {
//                                DWORD pid = vtProp.lVal;
//                                source = "PID: " + std::to_string(pid);
//                            }
//                            else {
//                                kLog.Add(Warn, C("ProcessId�������Ͳ�������"));
//                            }
//                            VariantClear(&vtProp);
//                        }
//                        description = "Network connection created";
//                    }
//                    else description = "Process modified";
//                }
//                // 3. ���� Win32_ProcessStartTrace/StopTrace �¼�
//                else if (type == "Win32_ProcessStartTrace" || type == "Win32_ProcessStopTrace") {
//                    HRESULT nameRes = apObjArray[i]->Get(_bstr_t(L"ProcessName"), 0, &vtProp, 0, 0);
//                    if (FAILED(nameRes)) {
//                        kLog.Add(Warn, C("��ȡProcessName����ʧ�ܣ��������: " + std::to_string(nameRes)));
//                    }
//                    else {
//                        if (vtProp.vt == VT_BSTR) {
//                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
//                        }
//                        else {
//                            kLog.Add(Warn, C("ProcessName�������Ͳ���BSTR"));
//                        }
//                        VariantClear(&vtProp);
//                    }
//                    description = type == "Win32_ProcessStartTrace" ? "Process start traced" : "Process stop traced";
//                }
//
//                // 4. ����ʱ���
//                auto now = std::chrono::system_clock::now();
//                std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
//                if (nowTime == -1) {
//                    kLog.Add(Warn, C("ת��ϵͳʱ��ʧ��"));
//                    continue;
//                }
//                std::tm* localTime = std::localtime(&nowTime);
//                if (localTime == nullptr) {
//                    kLog.Add(Warn, C("��ȡ����ʱ��ʧ��"));
//                    continue;
//                }
//                std::stringstream ss;
//                ss << std::put_time(localTime, "%Y-%m-%d %H:%M:%S");
//
//                // 5. ����¼����б�ȷ������Ч��Ϣ��
//                if (!source.empty()) {
//                    parent->addEvent(EventRecord(ss.str(), type, source, description));
//                }
//                else {
//                    kLog.Add(Warn, C("�¼�Դ��ϢΪ�գ��������¼�"));
//                }
//            }
//            return WBEM_S_NO_ERROR;
//        }
//
//        HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject* pObjParam) override {
//            if (FAILED(hResult)) {
//                kLog.Add(Warn, C("SetStatus ���ش��󣬴���: " + std::to_string(hResult)));
//            }
//            return WBEM_S_NO_ERROR;
//        }
//    };
//
//    // ��ʼ�� WMI ����
//    bool initializeWMI() {
//        HRESULT hres = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������1: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("CoInitializeEx ��ʼ���ɹ�"));
//
//        hres = CoInitializeSecurity(
//            NULL, -1, NULL, NULL,
//            RPC_C_AUTHN_LEVEL_DEFAULT,
//            RPC_C_IMP_LEVEL_IMPERSONATE,
//            NULL, EOAC_NONE, NULL
//        );
//        if (FAILED(hres)) {
//            CoUninitialize();
//            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������2: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("CoInitializeSecurity ��ʼ���ɹ�"));
//
//        hres = CoCreateInstance(
//            CLSID_WbemLocator, NULL,
//            CLSCTX_INPROC_SERVER, IID_IWbemLocator,
//            reinterpret_cast<LPVOID*>(&pLoc)
//        );
//        if (FAILED(hres)) {
//            CoUninitialize();
//            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������3: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("CoCreateInstance ����IWbemLocator�ɹ�"));
//
//        hres = pLoc->ConnectServer(
//            _bstr_t(L"ROOT\\CIMV2"),
//            NULL, NULL, 0, NULL, 0, 0, &pSvc
//        );
//        if (FAILED(hres)) {
//            pLoc->Release();
//            CoUninitialize();
//            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������4: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("ConnectServer ����WMI����ɹ�"));
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
//            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������5: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("�ɹ���ʼ��WMI����"));
//        return true;
//    }
//
//    // ���� WMI �¼�����̬ƴ�Ӳ�ѯ��䣩
//    bool subscribeToEvents() {
//        HRESULT hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL,
//            CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
//            reinterpret_cast<void**>(&pUnsecApp));
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("����IUnsecuredApartmentʧ�ܣ��������: " + std::to_string(hres)));
//            return false;
//        }
//        kLog.Add(Info, C("����IUnsecuredApartment�ɹ�"));
//
//        // �����¼�����ʵ�������ڴ�����յ����¼����ͣ�
//        EventSink* pSink = new EventSink(this);
//        pSink->AddRef();
//
//        hres = pUnsecApp->CreateObjectStub(static_cast<IUnknown*>(pSink), &pStubUnk);
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("����ObjectStubʧ�ܣ��������: " + std::to_string(hres)));
//            pSink->Release();
//            return false;
//        }
//        kLog.Add(Info, C("CreateObjectStub �ɹ�"));
//
//        hres = pStubUnk->QueryInterface(IID_IWbemObjectSink,
//            reinterpret_cast<void**>(&pStubSink));
//        if (FAILED(hres)) {
//            kLog.Add(Err, C("��ȡIWbemObjectSink�ӿ�ʧ�ܣ��������: " + std::to_string(hres)));
//            pStubUnk->Release();
//            pSink->Release();
//            return false;
//        }
//        kLog.Add(Info, C("��ȡIWbemObjectSink�ӿڳɹ�"));
//
//        // Ϊʵ���¼���Ŀ���ཨ��ӳ��
//        std::unordered_map<std::string, std::string> instanceEventTargetMap = {
//            {"__InstanceCreationEvent", "Win32_Process"},       // ���̴���
//            {"__InstanceDeletionEvent", "Win32_Process"},       // ����ɾ��
//            {"__InstanceModificationEvent", "Win32_Process"},   // �����޸�
//            {"RegistryKeyChangeEvent", "Win32_RegistryKey"}     // ע�����
//        };
//
//        // ����ÿ���¼����ͣ����޸ģ�ȥ��UNION�ȸ��ӽṹ��
//        for (const auto& eventType : selectedEventTypes) {
//            std::string query;
//
//            // �����¼���ѯ���
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
//            // Ϊÿ���¼�����ִ�ж���
//            hres = pSvc->ExecNotificationQueryAsync(
//                _bstr_t(L"WQL"),  // ��ѯ���ԣ�WQL
//                _bstr_t(query.c_str()),  // �¼���ѯ���
//                WBEM_FLAG_SEND_STATUS,
//                NULL,
//                pStubSink  // �¼����ջص�ʵ��
//            );
//
//            // ����һ�¼�����ʧ�ܣ����巵��ʧ��
//            if (FAILED(hres)) {
//                kLog.Add(Err, C("�����¼� " + eventType + " ʧ�ܣ��������: " + std::to_string(hres)));
//                pStubSink->Release();
//                pStubUnk->Release();
//                pSink->Release();
//                return false;
//            }
//            kLog.Add(Info, C("�ɹ������¼�: " + eventType));
//        }
//
//        return true;
//    }
//
//    // ֹͣ WMI ���
//    void stopWMI() {
//        kLog.Add(Info, C("��ʼֹͣWMI���"));
//        if (pSvc && pStubSink) {
//            HRESULT hres = pSvc->CancelAsyncCall(pStubSink);
//            if (FAILED(hres)) {
//                kLog.Add(Warn, C("ȡ��WMI�첽����ʧ�ܣ��������: " + std::to_string(hres)));
//            }
//            else {
//                kLog.Add(Info, C("�ɹ�ȡ��WMI�첽����"));
//            }
//        }
//
//        // �ͷ���Դ�������ͷţ�
//        if (pStubSink) {
//            pStubSink->Release();
//            pStubSink = nullptr;
//            kLog.Add(Info, C("�ͷ�pStubSink�ɹ�"));
//        }
//        if (pStubUnk) {
//            pStubUnk->Release();
//            pStubUnk = nullptr;
//            kLog.Add(Info, C("�ͷ�pStubUnk�ɹ�"));
//        }
//        if (pUnsecApp) {
//            pUnsecApp->Release();
//            pUnsecApp = nullptr;
//            kLog.Add(Info, C("�ͷ�pUnsecApp�ɹ�"));
//        }
//        if (pSvc) {
//            pSvc->Release();
//            pSvc = nullptr;
//            kLog.Add(Info, C("�ͷ�pSvc�ɹ�"));
//        }
//        if (pLoc) {
//            pLoc->Release();
//            pLoc = nullptr;
//            kLog.Add(Info, C("�ͷ�pLoc�ɹ�"));
//        }
//
//        // ����ͷ�COM��ʼ����ȷ��ֻ����һ�Σ�
//        static int comCount = 0;
//        comCount--;
//        if (comCount <= 0) {
//            CoUninitialize();
//            comCount = 0;
//            kLog.Add(Info, C("CoUninitialize �ɹ�"));
//        }
//    }
//
//    // ETW �¼��ص�����
//    static VOID WINAPI EtwEventCallback(EVENT_RECORD* pEventRecord) {
//        if (pEventRecord == nullptr) {
//            kLog.Add(Warn, C("EtwEventCallback ���տ��¼���¼"));
//            return;
//        }
//
//        // ��ȡ��ǰʵ��ָ��
//        EventMonitor* monitor = static_cast<EventMonitor*>(pEventRecord->UserContext);
//        if (monitor == nullptr) {
//            kLog.Add(Warn, C("EtwEventCallback �޷���ȡEventMonitorʵ��"));
//            return;
//        }
//
//        // ��ʽ��ʱ���
//        SYSTEMTIME st;
//        FILETIME ft;
//        ULARGE_INTEGER uli;
//        uli.QuadPart = pEventRecord->EventHeader.TimeStamp.QuadPart;
//        ft.dwLowDateTime = uli.LowPart;
//        ft.dwHighDateTime = uli.HighPart;
//        if (!FileTimeToSystemTime(&ft, &st)) {
//            kLog.Add(Warn, C("FileTimeToSystemTime ת��ʧ��"));
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
//        // ��¼�¼���Ϣ
//        std::string type = "ETW Event";
//        std::string source = "TCP/IP";
//        std::string description = "Network event captured";
//
//        monitor->addEvent(EventRecord(ss.str(), type, source, description));
//    }
//
//    // ��ʼ�� ETW ��ػỰ
//    bool initializeETW() {
//        kLog.Add(Info, C("��ʼ��ʼ��ETW���"));
//        // 1. �����ڴ��С�����ڷ���Ự����
//        DWORD bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(etwSessionName);
//        etwProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, bufferSize);
//        if (etwProps == nullptr) {
//            kLog.Add(Err, C("����ETW�����ڴ�ʧ��"));
//            return false;
//        }
//        kLog.Add(Info, C("ETW�����ڴ����ɹ�"));
//
//        // 2. ��ʼ���¼����ٻỰ����
//        ZeroMemory(etwProps, bufferSize);
//        etwProps->Wnode.BufferSize = bufferSize;
//        etwProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
//        etwProps->Wnode.ClientContext = 1;  // QPC��ʱ
//        etwProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
//        etwProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
//
//        // 3. �������ٻỰ
//        ULONG status = StartTrace(&etwSession, etwSessionName, etwProps);
//        if (status != ERROR_SUCCESS) {
//            kLog.Add(Err, C("����ETW�Ựʧ�ܣ��������: " + std::to_string(status)));
//            LocalFree(etwProps);
//            etwProps = nullptr;
//            return false;
//        }
//        kLog.Add(Info, C("ETW�Ự�����ɹ�"));
//
//        // 4. ָ��TCP/IP�¼��ṩ��
//        const GUID providerGuid = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };
//
//
//        // 5. ��ʼ��ENABLE_TRACE_PARAMETERS�ṹ��������µ�ϵͳ��
//        ENABLE_TRACE_PARAMETERS enableParams = { 0 };
//        enableParams.Version = 1;  // ��Ӧ�汾�ṹ�İ汾�ţ�����EnableInfo��Ա
//        enableParams.EnableProperty = EVENT_ENABLE_PROPERTY_STACK_TRACE;  // ���ö�ջ����
//
//        // 6. �����¼��ṩ��
//        status = EnableTraceEx2(
//            etwSession,                          // ���ٻỰ���
//            &providerGuid,                       // �ṩ��GUID
//            EVENT_CONTROL_CODE_ENABLE_PROVIDER,  // �����ṩ��
//            TRACE_LEVEL_INFORMATION,             // �¼���������Ϊ��Ϣ����
//            0x1,                                 // MatchAnyKeyword��ɸѡ�ؼ��֣�
//            0x0,                                 // MatchAllKeyword
//            0,                                   // ��ʱʱ��
//            &enableParams                        // �ṹ������������չ��Ϣ��
//        );
//
//        if (status != ERROR_SUCCESS) {
//            kLog.Add(Err, C("����TCP/IP�¼��ṩ��ʧ�ܣ��������: " + std::to_string(status)));
//            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
//            LocalFree(etwProps);
//            etwProps = nullptr;
//            etwSession = 0;
//            return false;
//        }
//        kLog.Add(Info, C("TCP/IP�¼��ṩ�����óɹ�"));
//
//        // 7. �����¼������߳�
//        std::thread etwThread([this]() {
//            EVENT_TRACE_LOGFILE traceLogFile = { 0 };
//            traceLogFile.LoggerName = etwSessionName;
//            traceLogFile.EventRecordCallback = EtwEventCallback;
//            traceLogFile.Context = this;
//            traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
//
//            TRACEHANDLE traceHandle = OpenTrace(&traceLogFile);
//            if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
//                kLog.Add(Err, C("��ETW����ʧ�ܣ��������: " + std::to_string(GetLastError())));
//                return;
//            }
//            kLog.Add(Info, C("ETW���ٴ򿪳ɹ�"));
//
//            // �����¼�ֱ��ֹͣ
//            while (isRunning) {
//                ULONG processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
//                if (processStatus != ERROR_SUCCESS && processStatus != ERROR_MORE_DATA) {
//                    kLog.Add(Err, C("����ETW�¼�ʧ�ܣ��������: " + std::to_string(processStatus)));
//                    break;
//                }
//            }
//
//            CloseTrace(traceHandle);
//            kLog.Add(Info, C("ETW�����ѹر�"));
//            });
//
//        etwThread.detach();
//        kLog.Add(Info, C("ETW�¼������߳������ɹ�"));
//        return true;
//    }
//
//    // ֹͣ ETW ���
//    void stopETW() {
//        kLog.Add(Info, C("��ʼֹͣETW���"));
//        if (etwSession != 0 && etwProps != nullptr) {
//            // ֹͣ���ٻỰ
//            ULONG status = ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
//            if (status != ERROR_SUCCESS) {
//                kLog.Add(Warn, C("ֹͣETW�Ựʧ�ܣ��������: " + std::to_string(status)));
//            }
//            else {
//                kLog.Add(Info, C("ETW�Ựֹͣ�ɹ�"));
//            }
//            etwSession = 0;
//        }
//
//        if (etwProps != nullptr) {
//            LocalFree(etwProps);
//            etwProps = nullptr;
//            kLog.Add(Info, C("ETW�����ڴ��ͷųɹ�"));
//        }
//    }
//
//    // ��ȫ����¼����б�
//    void addEvent(const EventRecord& event) {
//        try {
//            std::lock_guard<std::mutex> lock(eventsMutex);
//            events.insert(events.begin(), event);
//            if (events.size() > 1000) {  // �����¼�����
//                events.pop_back();
//            }
//        }
//        catch (const std::exception& e) {
//            kLog.Add(Err, C("����¼����б�ʧ��: " + std::string(e.what())));
//        }
//        catch (...) {
//            kLog.Add(Err, C("����¼����б�ʱ����δ֪����"));
//        }
//    }
//
//public:
//    EventMonitor() = default;
//    ~EventMonitor() {
//        stopMonitoring();
//    }
//
//    // ������أ�WMI + ETW��
//    void startMonitoring(const std::vector<std::string>& eventTypes) {
//        if (isRunning) {
//            kLog.Add(Warn, C("����������У������ظ�����"));
//            return;
//        }
//
//        if (eventTypes.empty()) {
//            kLog.Add(Warn, C("δѡ���κ��¼����ͣ��������ʧ��"));
//            return;
//        }
//
//        selectedEventTypes = eventTypes;
//        isRunning = true;
//        kLog.Add(Info, C("��ʼ��������߳�"));
//
//        monitoringThread = std::thread([this]() {
//            bool wmiOk = initializeWMI() && subscribeToEvents();
//            bool etwOk = initializeETW();
//
//            if (!wmiOk && !etwOk) {
//                kLog.Add(Err, C("WMI��ETW��ؾ���ʼ��ʧ�ܣ�����޷�����"));
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
//            kLog.Add(Info, C("����߳����˳�"));
//            });
//    }
//
//    // ֹͣ���
//    void stopMonitoring() {
//        if (!isRunning) {
//            kLog.Add(Warn, C("���δ�����У�����ֹͣ"));
//            return;
//        }
//
//        kLog.Add(Info, C("����ֹͣ���"));
//        isRunning = false;
//        if (monitoringThread.joinable()) {
//            monitoringThread.join();
//            kLog.Add(Info, C("����߳��ѳɹ�join"));
//        }
//        else {
//            kLog.Add(Warn, C("����̲߳���join"));
//        }
//    }
//
//    // ����¼��б�
//    void clearEvents() {
//        try {
//            std::lock_guard<std::mutex> lock(eventsMutex);
//            events.clear();
//            kLog.Add(Info, C("������¼��б�"));
//        }
//        catch (const std::exception& e) {
//            kLog.Add(Err, C("����¼��б�ʧ��: " + std::string(e.what())));
//        }
//        catch (...) {
//            kLog.Add(Err, C("����¼��б�ʱ����δ֪����"));
//        }
//    }
//
//    // ��ȡ��ǰ�¼��б�
//    std::vector<EventRecord> getEvents() {
//        try {
//            std::lock_guard<std::mutex> lock(eventsMutex);
//            return events;
//        }
//        catch (const std::exception& e) {
//            kLog.Add(Err, C("��ȡ�¼��б�ʧ��: " + std::string(e.what())));
//            return {};
//        }
//        catch (...) {
//            kLog.Add(Err, C("��ȡ�¼��б�ʱ����δ֪����"));
//            return {};
//        }
//    }
//
//    // ��ѯ�Ƿ����ڼ��
//    bool isMonitoring() const {
//        return isRunning;
//    }
//};
//
//// ȫ���¼������ʵ��
//EventMonitor g_eventMonitor;
//
//// ��չ�¼������б��ɸ�����Ҫ��ɾ��
//std::vector<std::pair<std::string, bool>> eventTypes = {
//    { "__InstanceCreationEvent", false },   // ʵ�������¼������̵ȣ�
//    { "__InstanceDeletionEvent", false },   // ʵ��ɾ���¼�
//    { "__InstanceModificationEvent", false },// ʵ���޸��¼�
//    { "Win32_ProcessStartTrace", false },   // ������������
//    { "Win32_ProcessStopTrace", false },    // ����ֹͣ����
//    { "MSFT_NetConnectionCreate", false },  // �������Ӵ�����Win10+֧�֣�
//    { "CIM_DataFile", false },              // �ļ�ϵͳ�¼�������/ɾ���ȣ�
//    { "RegistryKeyChangeEvent", false },    // ע�������
//    { "SecurityEvent", false },             // ��ȫ��־�¼�
//};
//
//
//void KswordMonitorMain() {
//    if (ImGui::GetCurrentContext() == nullptr) {
//        kLog.Add(Err, C("ImGui������δ��ʼ�����޷���ʾ��ؽ���"));
//        return;
//    }
//
//    ImGui::Text(C("ѡ��Ҫ��ص��¼�����:"));
//    ImGui::Separator();
//
//    // ÿ����ʾ2��ѡ�� - ȷ��Columns��ȷʹ��
//    {
//        int columns = 2;
//        ImGui::Columns(columns, NULL, false);  // Push columns
//
//        for (size_t i = 0; i < eventTypes.size(); ++i) {
//            if (!ImGui::Checkbox(eventTypes[i].first.c_str(), &eventTypes[i].second)) {
//                // ����Ҫ������־��Checkboxʧ��ͨ����Ӱ������
//            }
//            if ((i + 1) % columns == 0) {
//                ImGui::NextColumn();
//            }
//        }
//
//        ImGui::Columns(1);  // Pop columns�ص�Ĭ��
//    }
//
//    ImGui::Separator();
//
//    // ���ư�ť
//    ImGui::Spacing();
//    if (ImGui::Button(C("��ʼ���"), ImVec2(100, 0)) && !g_eventMonitor.isMonitoring()) {
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
//    if (ImGui::Button(C("ֹͣ���"), ImVec2(100, 0)) && g_eventMonitor.isMonitoring()) {
//        g_eventMonitor.stopMonitoring();
//    }
//
//    ImGui::SameLine();
//    if (ImGui::Button(C("����б�"), ImVec2(100, 0))) {
//        g_eventMonitor.clearEvents();
//    }
//
//    ImGui::SameLine();
//    if (g_eventMonitor.isMonitoring()) {
//        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), C("���ڼ��..."));
//    }
//    else {
//        ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), C("δ���"));
//    }
//
//    // �¼��б� - ȷ��Begin/End��ȷ���
//    ImGui::Spacing();
//    ImGui::Text(C("�¼��б�:"));
//    ImGui::Separator();
//
//    auto events = g_eventMonitor.getEvents();
//
//    // �����ʾ�¼��б�
//    if (ImGui::BeginTable("�¼��б�", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
//        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
//        ImGui::TableSetupColumn(C("ʱ��"), ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn(C("Դ"), ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch);
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
//        ImGui::EndTable();  // ȷ�������ȷ�ر�
//    }
//    else {
//        kLog.Add(Warn, C("�����¼��б���ʧ��"));
//    }
//    ImGui::EndTabItem();
//}

// [CHANGED] ���� �Ѿ�����ع�Ϊ������ۺ�ʽ��WMI ��ؿ�ܣ�������ʱ��չ�¼�����


#define CURRENT_MODULE C("�¼����")

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

// =================== �������ݽṹ ===================

class EventRecord {
public:
    uint64_t    id = 0;       // [NEW] �����¼�ID
    std::string timestamp;
    std::string type;
    std::string source;
    std::string description;

    EventRecord(const std::string& ts, const std::string& t,
        const std::string& s, const std::string& d)
        : timestamp(ts), type(t), source(s), description(d) {
    }
};

// =================== ���ߺ��� ===================

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

// [NEW] �� DMTF datetime��YYYYMMDDHHMMSS.mmmmmmsUUU��ת�ɿɶ�
static std::string DmtfToReadable(const std::string& d) {
    try {
        // ���ַ���ת��Ϊuint64_t
        uint64_t filetime = std::stoull(d);

        // ����FILETIME��ʼ�㵽Unix epoch��ƫ���� (��λ��100����)
        const uint64_t FILETIME_OFFSET = 116444736000000000ULL;

        // ������1970-01-01 00:00:00 UTC����������ʣ��100������
        uint64_t total_100ns = filetime - FILETIME_OFFSET;
        uint64_t seconds = total_100ns / 10000000;
        uint64_t remainder_100ns = total_100ns % 10000000;
        uint64_t nanoseconds = remainder_100ns * 100; // ת��Ϊ����

        // ʹ��time_t�����뼶ʱ��
        time_t t = static_cast<time_t>(seconds);

        // ת��ΪUTCʱ��
        struct tm tm;
#if defined(_WIN32)
        gmtime_s(&tm, &t); // Windows��ȫ����
#else
        gmtime_r(&t, &tm); // Linux/macOS��ȫ����
#endif
        // ����8Сʱ���й�ʱ��UTC+8��
        tm.tm_hour += 8;
        // ��׼��ʱ�䣨������ܵ�������糬��23Сʱ��
        mktime(&tm);
        // ��ʽ�����ں�ʱ�䲿��
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        // ������벿�֣�9λС����
        oss << '.' << std::setfill('0') << std::setw(9) << nanoseconds;

        return oss.str();
    }
    catch (const std::exception& e) {
        return "ת��ʧ��: " + std::string(e.what());
    }
}
// ��ȷ����DMTFʱ���ʽ��YYYYMMDDHHMMSS.mmmmmmsUUU��
//static std::string DmtfToReadable(const std::string& dmtfTime) {
//    // DMTFʱ���ʽ������Ҫ14���ַ���YYYYMMDDHHMMSS��
//    if (dmtfTime.size() < 14) {
//        return "Invalid DMTF time"; // ��ȷ������ʾ�������ֵ
//    }
//
//    try {
//        // ��ȡ������ʱ���루ǰ14���ַ���
//        int year = std::stoi(dmtfTime.substr(0, 4));
//        int month = std::stoi(dmtfTime.substr(4, 2));
//        int day = std::stoi(dmtfTime.substr(6, 2));
//        int hour = std::stoi(dmtfTime.substr(8, 2));
//        int minute = std::stoi(dmtfTime.substr(10, 2));
//        int second = std::stoi(dmtfTime.substr(12, 2));
//
//        // ��֤ʱ��Ϸ��ԣ���У�飩
//        if (month < 1 || month > 12 || day < 1 || day > 31 ||
//            hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
//            return "Invalid time value";
//        }
//
//        // ��ʽ�����
//        char buf[32];
//        sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d",
//            year, month, day, hour, minute, second);
//        return buf;
//    }
//    catch (...) {
//        // �������н����쳣����������ַ���
//        return "Parse failed";
//    }
//}
// [NEW] LogonType ��ֵת���֣�������һЩ���õ�ö�٣�
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

// [NEW] ����ϵͳ/����/�����˺ţ���ʾʱ�������ȼ��͵ģ�
static bool IsSystemOrServiceAccount(const std::string& domain, const std::string& name) {
    auto ieq = [](const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; };
    if (ieq(domain, "NT AUTHORITY") || ieq(domain, "NT SERVICE") || ieq(domain, "Window Manager"))
        return true;
    if (_strnicmp(name.c_str(), "DWM-", 4) == 0 || _strnicmp(name.c_str(), "UMFD-", 5) == 0)
        return true;
    if (ieq(name, "LOCAL SERVICE") || ieq(name, "NETWORK SERVICE") || ieq(name, "SYSTEM"))
        return true;
    if (!name.empty() && name.back() == '$') // �����˺�
        return true;
    return false;
}

// =================== ǰ������ ===================

class EventMonitor; // [NEW]

// =================== ͨ�� Sink��������� ===================
// [NEW] ÿ������ӵ���Լ��� Sink������ȡ�������������ռ���������

class TaskSink : public IWbemObjectSink {
    std::atomic<LONG> m_ref{ 0 };
    EventMonitor* m_monitor;           // ���������� AddEvent��
    std::function<void(IWbemClassObject*, EventMonitor*)> m_onEvent; // ����Ļص�

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
                kLog.Add(Warn, C("����WMI�����¼�ʱ�����쳣"),CURRENT_MODULE);
            }
        }
        return WBEM_S_NO_ERROR;
    }
    HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR, IWbemClassObject*) override {
        return WBEM_S_NO_ERROR;
    }
};

// =================== ������� ===================
// [NEW] ÿ�ֶ��Ķ���Ϊһ�����񣻺������ռ䡢��ѯ�ַ����������߼�

class WmiEventTask {
protected:
    std::wstring m_namespace;      // ���� L"ROOT\\CIMV2" �� L"ROOT\\DEFAULT"
    std::wstring m_query;          // ���� WQL
    std::string  m_name;           // ����UI��ʾ/��־
    IWbemServices* m_svc = nullptr;
    IUnsecuredApartment* m_unsec = nullptr;
    IWbemObjectSink* m_stubSink = nullptr; // Apartment stub
    TaskSink* m_sink = nullptr;     // ʵ�ʵ��¼���������ӵ�лص���

public:
    virtual ~WmiEventTask() { Unsubscribe(); }

    const std::string& Name() const { return m_name; }
    const std::wstring& NameSpace() const { return m_namespace; }
    const std::wstring& Query() const { return m_query; }

    // [NEW] ÿ���������ʵ���¼������߼�
    virtual void OnEvent(IWbemClassObject* pEvent, EventMonitor* host) = 0;

    // [NEW] ���������ռ�
    bool Connect(IWbemLocator* loc) {
        HRESULT hr = loc->ConnectServer(_bstr_t(m_namespace.c_str()),
            nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_svc);
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("���������ռ�ʧ��: ") + std::string(m_name) + " " + HrToStr(hr)), CURRENT_MODULE);
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
            kLog.Add(Warn, C(std::string("���ô���ȫʧ��: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
        }
        return true;
    }

    // [NEW] ���ģ�ÿ�������Խ� UnsecuredApartment + Stub + Sink��
    bool Subscribe(EventMonitor* host) {
        HRESULT hr = CoCreateInstance(
            CLSID_UnsecuredApartment, nullptr, CLSCTX_LOCAL_SERVER,
            IID_IUnsecuredApartment, reinterpret_cast<void**>(&m_unsec));
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("����UnsecuredApartmentʧ��: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            return false;
        }

        m_sink = new TaskSink(host, [this](IWbemClassObject* obj, EventMonitor* h) {
            this->OnEvent(obj, h);
            });
        m_sink->AddRef();

        IUnknown* stubUnk = nullptr;
        hr = m_unsec->CreateObjectStub(m_sink, &stubUnk);
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("����������ʧ��: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            m_sink->Release(); m_sink = nullptr;
            return false;
        }

        hr = stubUnk->QueryInterface(IID_IWbemObjectSink, reinterpret_cast<void**>(&m_stubSink));
        stubUnk->Release();
        if (FAILED(hr)) {
            kLog.Add(Err, C(std::string("QI IWbemObjectSink ʧ��: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
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
            kLog.Add(Warn, C(std::string("����ʧ��: ") + m_name + " " + HrToStr(hr)), CURRENT_MODULE);
            SafeRelease(m_stubSink);
            SafeRelease(m_unsec);
            m_sink->Release(); m_sink = nullptr;
            return false;
        }

        kLog.Add(Info, C(std::string("�Ѷ���: ") + m_name), CURRENT_MODULE);
        return true;
    }

    // [NEW] ȡ������
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

// =================== ͨ�����Զ�ȡ ===================
// [NEW] ���������ʹ������ɸ���

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
    return target; // ��Ҫ���÷� Release
}

// =================== ��������ʵ�� ===================

// [NEW] 1) __Instance* �������¼������� WITHIN��
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

// [NEW] 2) Win32_ProcessStartTrace / StopTrace�������¼�������Ҫ WITHIN��
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

// [NEW] 3) RegistryKeyChangeEvent�������¼���ROOT\\DEFAULT��
class RegistryKeyChangeTask : public WmiEventTask {
    std::string m_hive;     // ���� "HKEY_LOCAL_MACHINE"
    std::string m_keyPath;  // ���� "SOFTWARE\\MyApp"
public:
    RegistryKeyChangeTask(std::string hive, std::string keyPath)
        : m_hive(std::move(hive)), m_keyPath(std::move(keyPath))
    {
        m_namespace = L"ROOT\\DEFAULT";
        std::wstringstream ss;
        ss << L"SELECT * FROM RegistryKeyChangeEvent WHERE Hive='";
        ss << std::wstring(m_hive.begin(), m_hive.end()) << L"' AND KeyPath='";
        // ��б����WQL����Ҫ˫��б��
        std::string escaped = m_keyPath;
        for (size_t pos = 0; (pos = escaped.find("\\", pos)) != std::string::npos; pos += 2)
            escaped.replace(pos, 1, "\\\\");
        ss << std::wstring(escaped.begin(), escaped.end()) << L"'";
        m_query = ss.str();
        m_name = "RegistryKeyChangeEvent(" + m_hive + "\\" + m_keyPath + ")";
    }

    void OnEvent(IWbemClassObject* evt, EventMonitor* host) override;
};

// [NEW] ���¼�����
class VolumeChangeTask : public WmiEventTask {
public:
    VolumeChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_VolumeChangeEvent";
        m_name = "Win32_VolumeChangeEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] ����ʽ��¼����
class InteractiveLogonTask : public WmiEventTask {
    // [NEW] Ϊע���¼����û�������
    std::mutex m_cacheMutex;
    std::unordered_map<std::string, std::string> m_logonId2User;

    // [NEW] ���� LogonSession �� __RELPATH ȡ����˺ţ���������/����/�����˺ţ�
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

        std::string first; // ����
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
        // ͬʱ���񡰽���ʽ(2)���롰Զ�̽���(10)���Ĵ���/ɾ��
        m_query = L"SELECT * FROM __InstanceOperationEvent WITHIN 5 "
            L"WHERE TargetInstance ISA 'Win32_LogonSession' "
            L"AND (TargetInstance.LogonType = 2 OR TargetInstance.LogonType = 10)";
        m_name = "InteractiveLogon";
    }

    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};


////////////////////// ��������δ������飬�� AI ���� ///////////////////////
/////////////////////////////////////////////////////////////////////////
// [NEW] �豸��������ȣ�Win32_DeviceChangeEvent
class DeviceChangeTask : public WmiEventTask {
public:
    DeviceChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_DeviceChangeEvent";
        m_name = "Win32_DeviceChangeEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] ��Դ/˯�߻��ѣ�Win32_PowerManagementEvent
class PowerManagementEventTask : public WmiEventTask {
public:
    PowerManagementEventTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_PowerManagementEvent";
        m_name = "Win32_PowerManagementEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] �ػ�/����/ע����Win32_ComputerShutdownEvent
class ComputerShutdownEventTask : public WmiEventTask {
public:
    ComputerShutdownEventTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_ComputerShutdownEvent";
        m_name = "Win32_ComputerShutdownEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] ����״̬�����__InstanceModificationEvent + Win32_Service
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

// [NEW] ��ӡ��ҵ������__InstanceCreationEvent + Win32_PrintJob
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

// [NEW] ���߲�Σ�MSNdis_StatusMediaConnect / Disconnect��ROOT\\WMI��
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

// [NEW] IP ��ַ/�������ñ仯��__InstanceOperationEvent + MSFT_NetIPAddress��Win8+��
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

// [NEW] ����ǽ��������__InstanceOperationEvent + MSFT_NetFirewallRule��Win8+���������Ա��
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

// [NEW] ϵͳ���ñ仯��ʾ��Win32_SystemConfigurationChangeEvent
class SystemConfigurationChangeTask : public WmiEventTask {
public:
    SystemConfigurationChangeTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_SystemConfigurationChangeEvent";
        m_name = "Win32_SystemConfigurationChangeEvent";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// [NEW] ����ģ����أ��߼�/����ϵͳ���������Ա����Win32_ModuleLoadTrace
class ModuleLoadTraceTask : public WmiEventTask {
public:
    ModuleLoadTraceTask() {
        m_namespace = L"ROOT\\CIMV2";
        m_query = L"SELECT * FROM Win32_ModuleLoadTrace";
        m_name = "Win32_ModuleLoadTrace";
    }
    void OnEvent(IWbemClassObject* e, EventMonitor* host) override;
};

// =================== �¼�������������� ===================

class EventMonitor {
private:
    std::atomic<bool> isRunning{ false };
    std::atomic<uint64_t> m_nextEventId{ 1 }; // [NEW] �¼�����ۼ���
    std::vector<EventRecord> events;
    std::mutex eventsMutex;
    std::thread monitoringThread;

    // [NEW] ���񼯺ϣ�ÿ������һ������ʵ����
    std::vector<std::unique_ptr<WmiEventTask>> m_tasks;

    // [CHANGED] ���ٳ���ȫ�� pSvc/pStubSink����Ϊÿ��������Թ���
    IWbemLocator* m_locator = nullptr;

public:
    EventMonitor() = default;
    ~EventMonitor() { StopMonitoring(); }

    // [CHANGED] ����¼��ķ�����������ص�ʹ�ã�
    // [CHANGED] ��Ϊ����ֵ���롱�������︳ id
    void AddEvent(EventRecord ev) {
        ev.id = m_nextEventId.fetch_add(1, std::memory_order_relaxed); // [NEW]
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.insert(events.begin(), std::move(ev)); // �Ծɰ����¼��嵽��ǰ
        if (events.size() > 1000) events.pop_back();
    }

    // [CHANGED] ���ʱ���ñ��
    void ClearEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.clear();
        m_nextEventId.store(1, std::memory_order_relaxed); // [NEW]
    }

    std::vector<EventRecord> GetEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return events;
    }

    // [NEW] ���������ݡ������ַ�������������
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

        // ��Ҳ���Դ��� "RegistryKeyChangeEvent:HKLM:SOFTWARE\\MyApp"
        const std::string prefix = "RegistryKeyChangeEvent:";
        if (type.rfind(prefix, 0) == 0) {
            // ��ʽ��RegistryKeyChangeEvent:HIVE:KEYPATH
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

    // [NEW] ��ʼ�� COM + ��ȫ + ��λ��
    bool InitializeCOM() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            kLog.Add(Err, C("CoInitializeEx ʧ��: " + HrToStr(hr)), CURRENT_MODULE);
            return false;
        }
        // ���ǵ���γ�ʼ�����⣬�� RPC_E_TOO_LATE ��Ϊ�ɼ���
        hr = CoInitializeSecurity(
            nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr
        );
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            kLog.Add(Warn, C("CoInitializeSecurity ����: " + HrToStr(hr)), CURRENT_MODULE);
        }

        hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, reinterpret_cast<void**>(&m_locator));
        if (FAILED(hr)) {
            kLog.Add(Err, C("���� WbemLocator ʧ��: " + HrToStr(hr)), CURRENT_MODULE);
            CoUninitialize();
            return false;
        }
        return true;
    }

    // [NEW] ����ʼ��
    void UninitializeCOM() {
        SafeRelease(m_locator);
        CoUninitialize();
    }

    // [NEW] �������������ʹ������� -> ���� -> ����
    void StartMonitoring(const std::vector<std::string>& typeList) {
        if (isRunning || typeList.empty()) return;
        isRunning = true;

        monitoringThread = std::thread([this, typeList]() {
            if (!InitializeCOM()) {
                isRunning = false;
                return;
            }

            // ��������
            for (const auto& t : typeList) {
                if (auto task = MakeTaskFromType(t)) {
                    if (!task->Connect(m_locator)) continue;
                    if (!task->Subscribe(this)) continue;
                    m_tasks.emplace_back(std::move(task));
                }
                else {
                    kLog.Add(Warn, C(std::string("δ֪/��֧�ֵ���������: ") + t), CURRENT_MODULE);
                }
            }

            if (m_tasks.empty()) {
                kLog.Add(Warn, C("û���κ�����ɹ����ģ���ֹ���"), CURRENT_MODULE);
                UninitializeCOM();
                isRunning = false;
                return;
            }

            // �� STA ģ���У��ͻ����̱߳��������ȡ��Ϣ�������¼��ᶪʧ
            while (isRunning) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                SleepEx(50, TRUE);
            }

            // ����
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

// =================== �����¼�ʵ�� ===================

void InstanceProcessTask::OnEvent(IWbemClassObject* evt, EventMonitor* host) { // [NEW]
    // ȡ������ʱ��
    std::string cls = ReadStrProp(evt, L"__CLASS");
    std::string ts = ReadStrProp(evt, L"TIME_CREATED");
    if (ts.empty()) ts = NowTimestamp();

    std::string source = "Process";
    // ȡ TargetInstance.Name
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

    // ����ȡ Hive/KeyPath ȷ��
    std::string hive = ReadStrProp(evt, L"Hive");
    std::string key = ReadStrProp(evt, L"KeyPath");
    std::string src = hive.empty() ? m_hive : hive;
    if (!key.empty()) src += ("\\" + key);

    host->AddEvent(EventRecord(ts, "RegistryKeyChangeEvent", src, "Registry key changed"));
}

void VolumeChangeTask::OnEvent(IWbemClassObject* e, EventMonitor* host) {
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string evt = ReadStrProp(e, L"EventType");   // ����ӳ��Ϊ Arrived/Removed
    std::string drive = ReadStrProp(e, L"DriveName"); // ����Ϊ��
    host->AddEvent(EventRecord(ts, m_name, drive.empty() ? "Volume" : drive, "Volume event type: " + evt));
}

void InteractiveLogonTask::OnEvent(IWbemClassObject* e, EventMonitor* host) { // [CHANGED]
    // �¼����__InstanceCreationEvent / __InstanceDeletionEvent / __InstanceModificationEvent
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

    // �ӻỰȡ�ؼ��ֶ�
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

    // ȡ�û���������ʵʱ������ע������ȡ��������˻���
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
            // ��ʵʱ�û���Ҳ˳��������
            std::lock_guard<std::mutex> _g(m_cacheMutex);
            m_logonId2User.erase(logonId);
        }
    }

    // ��֯չʾ
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
    std::string et = ReadStrProp(e, L"EventType");    // ���� DBT_* ö��ӳ��
    host->AddEvent(EventRecord(ts, m_name, "Device", "Device change (EventType=" + et + ")"));
}

void PowerManagementEventTask::OnEvent(IWbemClassObject* e, EventMonitor* host)
{
    std::string ts = ReadStrProp(e, L"TIME_CREATED"); if (ts.empty()) ts = NowTimestamp();
    std::string et = ReadStrProp(e, L"EventType"); // ������4=���������7=�Ӵ����ָ��ȣ���ͬ�汾���в��죩
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
        ip = ReadStrProp(ti, L"IPAddress");      // ��Ϊ���飬���ܻ��ǿգ���Ӱ����ʾ
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
    std::string proc = ReadStrProp(e, L"ProcessName");  // ��ͬϵͳ���Կ��ܲ�ͬ
    std::string mod = ReadStrProp(e, L"FileName");
    if (proc.empty()) proc = "Process";
    std::string desc = "Module loaded";
    if (!mod.empty()) desc += (": " + mod);
    host->AddEvent(EventRecord(ts, m_name, proc, desc));
}

// =================== UI/���ɣ���ԭ���ӿڼ��ݣ� ===================

// [CHANGED] ȫ��ʵ��
static EventMonitor g_eventMonitor;

// [CHANGED] �¼������б�
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


    // ��ҪʱҲ����UI��ӣ�
    // {"RegistryKeyChangeEvent:HKLM:SOFTWARE\\MyApp", false},
};

// [CHANGED] ImGui ��壺Ϊ���¼�����ѡ�񡱼���ɹ������� + ���� + ���в��� + ��������
void KswordMonitorMain() {
    if (ImGui::GetCurrentContext() == nullptr) return;

    ImGui::Text(C("ѡ�����¼�����:"));

    // ---------- �������������������� ----------
    // [NEW] �������루�ڵ�ǰ�Ự�ڱ��棩
    static char s_filter[128] = { 0 };
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##evt_filter", C("����(֧���Ӵ�ƥ��)"), s_filter, IM_ARRAYSIZE(s_filter));

    ImGui::SameLine();
    if (ImGui::SmallButton(C("ȫѡ"))) {
        const bool hasFilter = (s_filter[0] != '\0');
        for (auto& [name, on] : eventTypes) {
            if (!hasFilter || (strstr(name.c_str(), s_filter) != nullptr)) on = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(C("ȫ��ѡ"))) {
        const bool hasFilter = (s_filter[0] != '\0');
        for (auto& [name, on] : eventTypes) {
            if (!hasFilter || (strstr(name.c_str(), s_filter) != nullptr)) on = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(C("��ѡ"))) {
        const bool hasFilter = (s_filter[0] != '\0');
        for (auto& [name, on] : eventTypes) {
            if (!hasFilter || (strstr(name.c_str(), s_filter) != nullptr)) on = !on;
        }
    }

    ImGui::Separator();

    // ---------- �ɹ������¼��������� ----------
    // [NEW] �޸ߵ��Ӵ��ڣ����⼷ѹ������¼���
    // �߶Ȱ� ~8 �пؼ����㣬��Ҳ��������������
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float childH = rowH * 8.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    if (ImGui::BeginChild("EventTypeList", ImVec2(0, childH), true,
        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {

        // [NEW] �����ɹ��˺�������б�
        std::vector<int> filtered;
        filtered.reserve(eventTypes.size());
        const bool useFilter = (s_filter[0] != '\0');
        for (int i = 0; i < (int)eventTypes.size(); ++i) {
            if (!useFilter || (strstr(eventTypes[i].first.c_str(), s_filter) != nullptr))
                filtered.push_back(i);
        }

        // [NEW] ����Ӧ������ÿ�д�� 220px����� 4 �У����� 1 ��
        int cols = (int)std::floor(ImGui::GetContentRegionAvail().x / 220.0f);
        cols = (cols < 1) ? 1 : (cols > 4 ? 4 : cols);

        if (ImGui::BeginTable("EvtTypeTable", cols,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {

            // [NEW] ʹ���вü��������б�Ҳ����
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
                        // ��ʾ��ѡ��Ϊ�����ǩ����ռλ��֧��������ʾ������
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

    // ---------- ���ư�ť�� ----------
    ImGui::Separator();

    if (ImGui::Button(C("��ʼ���")) && !g_eventMonitor.IsMonitoring()) {
        std::vector<std::string> selected;
        selected.reserve(eventTypes.size());
        for (const auto& [type, enabled] : eventTypes)
            if (enabled) selected.push_back(type);
        g_eventMonitor.StartMonitoring(selected);
    }

    ImGui::SameLine();
    if (ImGui::Button(C("ֹͣ���")) && g_eventMonitor.IsMonitoring()) {
        g_eventMonitor.StopMonitoring();
    }

    ImGui::SameLine();
    if (ImGui::Button(C("����б�"))) {
        g_eventMonitor.ClearEvents();
    }

    ImGui::SameLine();
    // [NEW] ͳ����ѡ����
    int selectedCount = 0;
    for (auto& kv : eventTypes) if (kv.second) ++selectedCount;
    ImGui::Text("%s | %s: %d",
        g_eventMonitor.IsMonitoring() ? C("�����...") : C("δ���"),
        C("��ѡ"), selectedCount);

    // ---------- �¼��б���ʾ ----------
    ImGui::Separator();
    ImGui::Text(C("�¼��б�:"));

    // ��ѡ�Զ�������Ĭ�Ͽ�����
    static bool s_autoScroll = true;
    ImGui::SameLine();
    ImGui::Checkbox(C("�Զ�����"), &s_autoScroll);

    // ����ⲿ�ߴ�Ҫ��һ����ȷ�ĸ߶ȣ����� 280px���������ǡ����޸ߡ�
    /*static float s_tableHeight = 280.0f;
    ImVec2 outerSize(0.0f, s_tableHeight);*/
    const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y); // ռ��ʣ��߶�

    // ʹ�ñ���Դ�����
    const ImGuiTableFlags tblFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable;

    if (ImGui::BeginTable("EventsTable", 5, tblFlags, outerSize)) {
        ImGui::TableSetupColumn(C("ID"), ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn(C("ʱ��"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn(C("Դ"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch);
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

            // �Զ�����
            if (s_autoScroll && i == 0) {
                ImGui::SetScrollHereY(1.0f);
            }
        }

        ImGui::EndTable();
    }
    ImGui::EndTabItem();
}

