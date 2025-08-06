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
#include <unordered_map>
// �� evntrace.h δ���壬���䶨�壬���� #include ֮��
#ifndef EVENT_TRACE_ENABLE_INFO_DEFINED
#define EVENT_TRACE_ENABLE_INFO_DEFINED
#define CURRENT_MODULE "�¼����"
typedef struct _EVENT_TRACE_ENABLE_INFO {
    ULONG Version;               // ����Ϊ EVENT_TRACE_ENABLE_INFO_VERSION (1)
    ULONG EnableProperty;        // ���õ����ԣ��� EVENT_ENABLE_PROPERTY_STACK_TRACE��
} EVENT_TRACE_ENABLE_INFO, * PEVENT_TRACE_ENABLE_INFO;

// �汾�Ÿ���΢��Ĺ涨��д
#define EVENT_TRACE_ENABLE_INFO_VERSION 1

#endif  // EVENT_TRACE_ENABLE_INFO_DEFINED

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

using BYTE = unsigned char;

// ΪTCP/IP�¼��ṩ��GUIDδ�ҵ������������
// ʹ��Microsoft-Windows-TCPIP�ṩ����GUID
const GUID GUID_TCPIP_EVENTS = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };

// �¼���¼�ṹ
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

// ȫ���¼�������ࣨ����ģʽ��
class EventMonitor {
private:
    bool isRunning = false;
    std::vector<EventRecord> events;
    std::mutex eventsMutex;
    std::thread monitoringThread;
    std::vector<std::string> selectedEventTypes;

    // WMI ��ؽӿ�
    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IUnsecuredApartment* pUnsecApp = nullptr;
    IUnknown* pStubUnk = nullptr;
    IWbemObjectSink* pStubSink = nullptr;

    // ETW ��ؽӿ�
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
                return S_OK;
            }
            // FIX-ME�� ������զҲ�ᵼ�� QI �ݹ鰡��
            // ��Ӷ�IMarshal�ӿڵ�֧��
            //else if (riid == IID_IMarshal) {
            //    // ί�и���׼Marshaller
            //    return CoGetStandardMarshal(riid, static_cast<IUnknown*>(this),
            //        MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL,
            //        reinterpret_cast<IMarshal**>(ppv));
            //}

            // Ӧ�ñ����� QI �е����κο��������ٴη��� QI �ķ�������ᵼ�µݹ���ã�����ջ���
            // �����־��¼����Ľӿ�
            // ��¼��֧�ֵĽӿ�IID������ʹ��StringFromIID��ֱ�Ӽ�¼IID����ֵ���֣�
            // ��IID��ֳɶ�����ּ�¼
            char buffer[256] = { 0 };
            sprintf_s(buffer, sizeof(buffer), "EventSink��֧�ֽӿ�: {%08lX-%04hX-%04hX-%02hX%02hX-%02hX%02hX%02hX%02hX%02hX%02hX}",
                riid.Data1, riid.Data2, riid.Data3,
                riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
            kLog.Add(Warn, C(buffer), C("�¼����"));
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        // IWbemObjectSink �ӿ�ʵ��
        HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
            if (lObjectCount <= 0 || apObjArray == nullptr) {
                kLog.Add(Warn, C("Indicate �����¼�������Ч���������Ϊ��"), C("�¼����"));
                return WBEM_S_NO_ERROR;
            }

            // ����ÿ���¼�֮ǰ��ͨ���߼�
            for (int i = 0; i < lObjectCount; ++i) {
                if (apObjArray[i] == nullptr) {
                    kLog.Add(Warn, C("�¼����������д��ڿ�ָ��"), C("�¼����"));
                    continue;
                }

                VARIANT vtProp;
                std::string type, source, description;

                // 1. ��ȡ�¼����ͣ�__Class ���ԣ�
                HRESULT hres = apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtProp, 0, 0);
                if (FAILED(hres)) {
                    kLog.Add(Warn, C("��ȡ�¼�__Class����ʧ�ܣ��������: " + std::to_string(hres)), C("�¼����"));
                }
                else {
                    if (vtProp.vt == VT_BSTR) {
                        type = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                    }
                    else {
                        kLog.Add(Warn, C("�¼�__Class�������Ͳ���BSTR"), C("�¼����"));
                    }
                    VariantClear(&vtProp);
                }

                // 2. ����ʵ���¼���__InstanceXXXEvent���� TargetInstance ����
                if (type == "__InstanceCreationEvent" || type == "__InstanceDeletionEvent" || type == "__InstanceModificationEvent") {
                    // ��ȡ TargetInstance ʵ�ʵĶ�������
                    HRESULT targetRes = apObjArray[i]->Get(_bstr_t(L"TargetInstance"), 0, &vtProp, 0, 0);
                    if (FAILED(targetRes)) {
                        kLog.Add(Warn, C("��ȡTargetInstance����ʧ�ܣ��������: " + std::to_string(targetRes)), C("�¼����"));
                        VariantClear(&vtProp);
                        continue;
                    }

                    if (vtProp.vt == VT_DISPATCH) {
                        IWbemClassObject* pTargetInstance = nullptr;
                        HRESULT qiRes = vtProp.pdispVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance);
                        if (FAILED(qiRes)) {
                            kLog.Add(Warn, C("TargetInstance��ѯIWbemClassObject�ӿ�ʧ�ܣ��������: " + std::to_string(qiRes)), C("�¼����"));
                            VariantClear(&vtProp);
                            continue;
                        }

                        // �� TargetInstance �л�ȡ����
                        VARIANT vtName;
                        HRESULT nameRes = pTargetInstance->Get(_bstr_t(L"Name"), 0, &vtName, 0, 0);
                        if (FAILED(nameRes)) {
                            kLog.Add(Warn, C("��ȡTargetInstance����ʧ�ܣ��������: " + std::to_string(nameRes)), C("�¼����"));
                        }
                        else {
                            if (vtName.vt == VT_BSTR) {
                                source = _com_util::ConvertBSTRToString(vtName.bstrVal);
                            }
                            else {
                                kLog.Add(Warn, C("TargetInstance�����������Ͳ���BSTR"), C("�¼����"));
                            }
                            VariantClear(&vtName);
                        }
                        pTargetInstance->Release();
                    }
                    else {
                        kLog.Add(Warn, C("TargetInstance�������Ͳ���VT_DISPATCH"), C("�¼����"));
                    }
                    VariantClear(&vtProp);

                    // ����������Ϣ
                    if (type == "__InstanceCreationEvent") description = "Process started";
                    else if (type == "__InstanceDeletionEvent") description = "Process stopped";
                    else if (type == "MSFT_NetConnectionCreate") {
                        // �������������¼�
                        HRESULT pidRes = apObjArray[i]->Get(L"ProcessId", 0, &vtProp, 0, 0);
                        if (FAILED(pidRes)) {
                            kLog.Add(Warn, C("��ȡMSFT_NetConnectionCreate�¼���ProcessIdʧ�ܣ��������: " + std::to_string(pidRes)),C(CURRENT_MODULE));
                        }
                        else {
                            if (vtProp.vt == VT_I4) {
                                DWORD pid = vtProp.lVal;
                                source = "PID: " + std::to_string(pid);
                            }
                            else {
                                kLog.Add(Warn, C("ProcessId�������Ͳ�������"),C(CURRENT_MODULE));
                            }
                            VariantClear(&vtProp);
                        }
                        description = "Network connection created";
                    }
                    else description = "Process modified";
                }
                // 3. ���� Win32_ProcessStartTrace/StopTrace �¼�
                else if (type == "Win32_ProcessStartTrace" || type == "Win32_ProcessStopTrace") {
                    HRESULT nameRes = apObjArray[i]->Get(_bstr_t(L"ProcessName"), 0, &vtProp, 0, 0);
                    if (FAILED(nameRes)) {
                        kLog.Add(Warn, C("��ȡProcessName����ʧ�ܣ��������: " + std::to_string(nameRes)), C(CURRENT_MODULE));
                    }
                    else {
                        if (vtProp.vt == VT_BSTR) {
                            source = _com_util::ConvertBSTRToString(vtProp.bstrVal);
                        }
                        else {
                            kLog.Add(Warn, C("ProcessName�������Ͳ���BSTR"), C(CURRENT_MODULE));
                        }
                        VariantClear(&vtProp);
                    }
                    description = type == "Win32_ProcessStartTrace" ? "Process start traced" : "Process stop traced";
                }
                else if (type == "MSFT_NetConnectionCreate") {
                    // ���ж����Ƿ���ڣ�ͨ�����Ի�ȡһ���ؼ�������֤��
                    HRESULT propRes = apObjArray[i]->Get(_bstr_t(L"RemoteAddress"), 0, &vtProp, 0, 0);
                    if (FAILED(propRes)) {
                        kLog.Add(Warn, C("MSFT_NetConnectionCreate ������Բ����ڣ�����������������: " + std::to_string(propRes) + "��"), C(CURRENT_MODULE));
                        VariantClear(&vtProp);
                        // ����¼�¼����ͣ��������ϸ��Ϣ����������ݣ�
                        source = "Unsupported";
                        description = "Network event class not supported on this system";
                    }
                    else {
                        // ������������
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
                // 5. ����δ�����¼�����
                else {
                    kLog.Add(Info, C("δ������¼�����: " + type), C(CURRENT_MODULE));
                    source = "Unprocessed";
                    description = "Event type not handled";
                }
                // 4. ����ʱ���
                auto now = std::chrono::system_clock::now();
                std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
                if (nowTime == -1) {
                    kLog.Add(Warn, C("ת��ϵͳʱ��ʧ��"), C(CURRENT_MODULE));
                    continue;
                }
                std::tm* localTime = std::localtime(&nowTime);
                if (localTime == nullptr) {
                    kLog.Add(Warn, C("��ȡ����ʱ��ʧ��"), C(CURRENT_MODULE));
                    continue;
                }
                std::stringstream ss;
                ss << std::put_time(localTime, "%Y-%m-%d %H:%M:%S");

                // 5. ����¼����б�ȷ������Ч��Ϣ��
                if (!source.empty()) {
                    parent->addEvent(EventRecord(ss.str(), type, source, description));
                }
                else {
                    kLog.Add(Warn, C("�¼�Դ��ϢΪ�գ��������¼�"), C(CURRENT_MODULE));
                }
            }
            return WBEM_S_NO_ERROR;
        }

        HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject* pObjParam) override {
            if (FAILED(hResult)) {
                kLog.Add(Warn, C("SetStatus ���ش��󣬴���: " + std::to_string(hResult)), C(CURRENT_MODULE));
            }
            return WBEM_S_NO_ERROR;
        }
    };

    // ��ʼ�� WMI ����
    bool initializeWMI() {
        HRESULT hres = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
        if (FAILED(hres)) {
            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������1: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("CoInitializeEx ��ʼ���ɹ�"), C(CURRENT_MODULE));

        hres = CoInitializeSecurity(
            NULL, -1, NULL, NULL,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL, EOAC_NONE, NULL
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������2: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("CoInitializeSecurity ��ʼ���ɹ�"), C(CURRENT_MODULE));

        hres = CoCreateInstance(
            CLSID_WbemLocator, NULL,
            CLSCTX_INPROC_SERVER, IID_IWbemLocator,
            reinterpret_cast<LPVOID*>(&pLoc)
        );
        if (FAILED(hres)) {
            CoUninitialize();
            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������3: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("CoCreateInstance ����IWbemLocator�ɹ�"), C(CURRENT_MODULE));

        hres = pLoc->ConnectServer(
            _bstr_t(L"ROOT\\CIMV2"),
            NULL, NULL, 0, NULL, 0, 0, &pSvc
        );
        if (FAILED(hres)) {
            pLoc->Release();
            CoUninitialize();
            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������4: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("ConnectServer ����WMI����ɹ�"), C(CURRENT_MODULE));

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
            kLog.Add(Err, C("�޷��ɹ���ʼ��WMI����������5: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("�ɹ���ʼ��WMI����"), C(CURRENT_MODULE));
        return true;
    }

    // ���� WMI �¼�����̬ƴ�Ӳ�ѯ��䣩
    bool subscribeToEvents() {
        HRESULT hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL,
            CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
            reinterpret_cast<void**>(&pUnsecApp));
        if (FAILED(hres)) {
            kLog.Add(Err, C("����IUnsecuredApartmentʧ�ܣ��������: " + std::to_string(hres)), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("����IUnsecuredApartment�ɹ�"), C(CURRENT_MODULE));

        // �����¼�����ʵ�������ڴ�����յ����¼����ͣ�
        EventSink* pSink = new EventSink(this);
        pSink->AddRef();

        // �������ʹ���
        IStream* pStream = nullptr;
        HRESULT hr = CoMarshalInterThreadInterfaceInStream(
            IID_IWbemObjectSink,
            static_cast<IWbemObjectSink*>(pSink),
            &pStream
        );

        if (FAILED(hr)) {
            kLog.Add(Err, C("�����ӿ���ʧ��: " + std::to_string(hr)), C(CURRENT_MODULE));
            pSink->Release();
            return false;
        }

        // ��Ŀ���߳̽����
        hr = CoGetInterfaceAndReleaseStream(
            pStream,
            IID_IWbemObjectSink,
            reinterpret_cast<void**>(&pStubSink)
        );

        if (FAILED(hr)) {
            kLog.Add(Err, C("����ͽӿ�ʧ��: " + std::to_string(hr)), C(CURRENT_MODULE));
            pSink->Release();
            return false;
        }

        hres = pUnsecApp->CreateObjectStub(static_cast<IUnknown*>(pSink), &pStubUnk);
        if (FAILED(hres)) {
            kLog.Add(Err, C("����ObjectStubʧ�ܣ��������: " + std::to_string(hres)), C(CURRENT_MODULE));
            pSink->Release();
            return false;
        }
        kLog.Add(Info, C("CreateObjectStub �ɹ�"), C(CURRENT_MODULE));

        hres = pStubUnk->QueryInterface(IID_IWbemObjectSink,
            reinterpret_cast<void**>(&pStubSink));
        if (FAILED(hres)) {
            kLog.Add(Err, C("��ȡIWbemObjectSink�ӿ�ʧ�ܣ��������: " + std::to_string(hres)), C(CURRENT_MODULE));
            pStubUnk->Release();
            pSink->Release();
            return false;
        }
        kLog.Add(Info, C("��ȡIWbemObjectSink�ӿڳɹ�"), C(CURRENT_MODULE));

        // Ϊʵ���¼���Ŀ���ཨ��ӳ��
        std::unordered_map<std::string, std::string> instanceEventTargetMap = {
            {"__InstanceCreationEvent", "Win32_Process"},       // ���̴���
            {"__InstanceDeletionEvent", "Win32_Process"},       // ����ɾ��
            {"__InstanceModificationEvent", "Win32_Process"},   // �����޸�
            {"RegistryKeyChangeEvent", "Win32_RegistryKey"}     // ע�����
        };

        // ����ÿ���¼����ͣ����޸ģ�ȥ��UNION�ȸ��ӽṹ��
        for (const auto& eventType : selectedEventTypes) {
            std::string query;

            // �����¼���ѯ���
            if (instanceEventTargetMap.count(eventType)) {
                std::string targetClass = instanceEventTargetMap[eventType];
                query = "SELECT * FROM " + eventType +
                    " WITHIN 1 WHERE TargetInstance ISA '" + targetClass + "'";
            }
            else if (eventType == "Win32_ProcessStartTrace" || eventType == "Win32_ProcessStopTrace") {
                query = "SELECT * FROM " + eventType + " WITHIN 1"; // ���WITHIN 1
            }
            else {
                query = "SELECT * FROM " + eventType + " WITHIN 1";
            }

            // Ϊÿ���¼�����ִ�ж���
            hres = pSvc->ExecNotificationQueryAsync(
                _bstr_t(L"WQL"),  // ��ѯ���ԣ�WQL
                _bstr_t(query.c_str()),  // �¼���ѯ���
                WBEM_FLAG_SEND_STATUS,
                NULL,
                pStubSink  // �¼����ջص�ʵ��
            );

            // ����һ�¼�����ʧ�ܣ����巵��ʧ��
            if (FAILED(hres)) {
                kLog.Add(Err, C("�����¼� " + eventType + " ʧ�ܣ��������: " + std::to_string(hres)), C(CURRENT_MODULE));
                pStubSink->Release();
                pStubUnk->Release();
                pSink->Release();
                return false;
            }
            kLog.Add(Info, C("�ɹ������¼�: " + eventType), C(CURRENT_MODULE));
        }

        return true;
    }

    // ֹͣ WMI ���
    void stopWMI() {
        kLog.Add(Info, C("��ʼֹͣWMI���"), C(CURRENT_MODULE));
        if (pSvc && pStubSink) {
            HRESULT hres = pSvc->CancelAsyncCall(pStubSink);
            if (FAILED(hres)) {
                kLog.Add(Warn, C("ȡ��WMI�첽����ʧ�ܣ��������: " + std::to_string(hres)), C(CURRENT_MODULE));
            }
            else {
                kLog.Add(Info, C("�ɹ�ȡ��WMI�첽����"), C(CURRENT_MODULE));
            }
        }

        // �ͷ���Դ�������ͷţ�
        if (pStubSink) {
            pStubSink->Release();
            pStubSink = nullptr;
            kLog.Add(Info, C("�ͷ�pStubSink�ɹ�"), C(CURRENT_MODULE));
        }
        if (pStubUnk) {
            pStubUnk->Release();
            pStubUnk = nullptr;
            kLog.Add(Info, C("�ͷ�pStubUnk�ɹ�"), C(CURRENT_MODULE));
        }
        if (pUnsecApp) {
            pUnsecApp->Release();
            pUnsecApp = nullptr;
            kLog.Add(Info, C("�ͷ�pUnsecApp�ɹ�"), C(CURRENT_MODULE));
        }
        if (pSvc) {
            pSvc->Release();
            pSvc = nullptr;
            kLog.Add(Info, C("�ͷ�pSvc�ɹ�"), C(CURRENT_MODULE));
        }
        if (pLoc) {
            pLoc->Release();
            pLoc = nullptr;
            kLog.Add(Info, C("�ͷ�pLoc�ɹ�"), C(CURRENT_MODULE));
        }

        // ����ͷ�COM��ʼ����ȷ��ֻ����һ�Σ�
        static int comCount = 0;
        comCount--;
        if (comCount <= 0) {
            CoUninitialize();
            comCount = 0;
            kLog.Add(Info, C("CoUninitialize �ɹ�"), C(CURRENT_MODULE));
        }
    }

    // ETW �¼��ص�����
    static VOID WINAPI EtwEventCallback(EVENT_RECORD* pEventRecord) {
        if (pEventRecord == nullptr) {
            kLog.Add(Warn, C("EtwEventCallback ���տ��¼���¼"), C(CURRENT_MODULE));
            return;
        }

        // ��ȡ��ǰʵ��ָ��
        EventMonitor* monitor = static_cast<EventMonitor*>(pEventRecord->UserContext);
        if (monitor == nullptr) {
            kLog.Add(Warn, C("EtwEventCallback �޷���ȡEventMonitorʵ��"), C(CURRENT_MODULE));
            return;
        }

        // ��ʽ��ʱ���
        SYSTEMTIME st;
        FILETIME ft;
        ULARGE_INTEGER uli;
        uli.QuadPart = pEventRecord->EventHeader.TimeStamp.QuadPart;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        if (!FileTimeToSystemTime(&ft, &st)) {
            kLog.Add(Warn, C("FileTimeToSystemTime ת��ʧ��"), C(CURRENT_MODULE));
            return;
        }

        std::stringstream ss;
        ss << std::setw(4) << std::setfill('0') << st.wYear << "-"
            << std::setw(2) << std::setfill('0') << st.wMonth << "-"
            << std::setw(2) << std::setfill('0') << st.wDay << " "
            << std::setw(2) << std::setfill('0') << st.wHour << ":"
            << std::setw(2) << std::setfill('0') << st.wMinute << ":"
            << std::setw(2) << std::setfill('0') << st.wSecond;

        // ��¼�¼���Ϣ
        std::string type = "ETW Event";
        std::string source = "TCP/IP";
        std::string description = "Network event captured";

        monitor->addEvent(EventRecord(ss.str(), type, source, description));
    }
    void generateUniqueSessionName() {
        // ��������
        const WCHAR baseName[] = L"KswordETWMonitor_";

        // ��ȡ��ǰϵͳʱ��
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm* localTime = std::localtime(&nowTime);

        // ��ȡ���ڣ���-��-�գ�������
        WCHAR timeSuffix[64] = { 0 };
        swprintf_s(
            timeSuffix,
            L"%04d%02d%02d_%02d",  // ��ʽ��������_�루��20240730_15��
            localTime->tm_year + 1900,
            localTime->tm_mon + 1,
            localTime->tm_mday,
            localTime->tm_sec  // ����ȷ��ͬһ�����ڵ�Ψһ��
        );

        // ƴ�������Ự������������+ʱ���׺��
        swprintf_s(
            etwSessionName,
            L"%s%s",
            baseName,
            timeSuffix
        );

        //kLog.Add(Info, C(std::wstring(L"����ΨһETW�Ự��: " )+ std::wstring(etwSessionName)));
    }

    // ��ʼ�� ETW ��ػỰ
    bool initializeETW() {
        kLog.Add(Info, C("��ʼ��ʼ��ETW���"), C(CURRENT_MODULE));
        // 1. �����ڴ��С�����ڷ���Ự����
        DWORD bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(etwSessionName);
        etwProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, bufferSize);
        if (etwProps == nullptr) {
            kLog.Add(Err, C("����ETW�����ڴ�ʧ��"), C(CURRENT_MODULE));
            return false;
        }
        kLog.Add(Info, C("ETW�����ڴ����ɹ�"), C(CURRENT_MODULE));

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
            kLog.Add(Err, C("����ETW�Ựʧ�ܣ��������: " + std::to_string(status)), C(CURRENT_MODULE));
            LocalFree(etwProps);
            etwProps = nullptr;
            return false;
        }
        kLog.Add(Info, C("ETW�Ự�����ɹ�"), C(CURRENT_MODULE));

        // 4. ָ��TCP/IP�¼��ṩ��
        const GUID providerGuid = { 0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA} };


        // 5. ��ʼ��ENABLE_TRACE_PARAMETERS�ṹ��������µ�ϵͳ��
        ENABLE_TRACE_PARAMETERS enableParams = { 0 };
        enableParams.Version = 1;  // ��Ӧ�汾�ṹ�İ汾�ţ�����EnableInfo��Ա
        enableParams.EnableProperty = EVENT_ENABLE_PROPERTY_STACK_TRACE;  // ���ö�ջ����

        // 6. �����¼��ṩ��
        status = EnableTraceEx2(
            etwSession,                          // ���ٻỰ���
            &providerGuid,                       // �ṩ��GUID
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,  // �����ṩ��
            TRACE_LEVEL_INFORMATION,             // �¼���������Ϊ��Ϣ����
            0x1,                                 // MatchAnyKeyword��ɸѡ�ؼ��֣�
            0x0,                                 // MatchAllKeyword
            0,                                   // ��ʱʱ��
            &enableParams                        // �ṹ������������չ��Ϣ��
        );

        if (status != ERROR_SUCCESS) {
            kLog.Add(Err, C("����TCP/IP�¼��ṩ��ʧ�ܣ��������: " + std::to_string(status)), C(CURRENT_MODULE));
            ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            LocalFree(etwProps);
            etwProps = nullptr;
            etwSession = 0;
            return false;
        }
        kLog.Add(Info, C("TCP/IP�¼��ṩ�����óɹ�"), C(CURRENT_MODULE));

        // 7. �����¼������߳�
        std::thread etwThread([this]() {
            EVENT_TRACE_LOGFILE traceLogFile = { 0 };
            traceLogFile.LoggerName = etwSessionName;
            traceLogFile.EventRecordCallback = EtwEventCallback;
            traceLogFile.Context = this;
            traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;

            TRACEHANDLE traceHandle = OpenTrace(&traceLogFile);
            if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
                kLog.Add(Err, C("��ETW����ʧ�ܣ��������: " + std::to_string(GetLastError())), C(CURRENT_MODULE));
                return;
            }
            kLog.Add(Info, C("ETW���ٴ򿪳ɹ�"), C(CURRENT_MODULE));

            // �����¼�ֱ��ֹͣ
            while (isRunning) {
                ULONG processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
                if (processStatus != ERROR_SUCCESS && processStatus != ERROR_MORE_DATA) {
                    kLog.Add(Err, C("����ETW�¼�ʧ�ܣ��������: " + std::to_string(processStatus)), C(CURRENT_MODULE));
                    break;
                }
            }

            CloseTrace(traceHandle);
            kLog.Add(Info, C("ETW�����ѹر�"), C(CURRENT_MODULE));
            });

        etwThread.detach();
        kLog.Add(Info, C("ETW�¼������߳������ɹ�"), C(CURRENT_MODULE));
        return true;
    }

    // ֹͣ ETW ���
    void stopETW() {
        kLog.Add(Info, C("��ʼֹͣETW���"), C(CURRENT_MODULE));
        if (etwSession != 0 && etwProps != nullptr) {
            // ֹͣ���ٻỰ
            ULONG status = ControlTrace(etwSession, etwSessionName, etwProps, EVENT_TRACE_CONTROL_STOP);
            if (status != ERROR_SUCCESS) {
                kLog.Add(Warn, C("ֹͣETW�Ựʧ�ܣ��������: " + std::to_string(status)), C(CURRENT_MODULE));
            }
            else {
                kLog.Add(Info, C("ETW�Ựֹͣ�ɹ�"), C(CURRENT_MODULE));
            }
            etwSession = 0;
        }

        if (etwProps != nullptr) {
            LocalFree(etwProps);
            etwProps = nullptr;
            kLog.Add(Info, C("ETW�����ڴ��ͷųɹ�"), C(CURRENT_MODULE));
        }
    }

    // ��ȫ����¼����б�
    void addEvent(const EventRecord& event) {
        try {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.insert(events.begin(), event);
            if (events.size() > 1000) {  // �����¼�����
                events.pop_back();
            }
        }
        catch (const std::exception& e) {
            kLog.Add(Err, C("����¼����б�ʧ��: " + std::string(e.what())), C(CURRENT_MODULE));
        }
        catch (...) {
            kLog.Add(Err, C("����¼����б�ʱ����δ֪����"), C(CURRENT_MODULE));
        }
    }

public:
    EventMonitor() = default;
    ~EventMonitor() {
        stopMonitoring();
    }

    // ������أ�WMI + ETW��
    void startMonitoring(const std::vector<std::string>& eventTypes) {
        if (isRunning) {
            kLog.Add(Warn, C("����������У������ظ�����"), C(CURRENT_MODULE));
            return;
        }

        if (eventTypes.empty()) {
            kLog.Add(Warn, C("δѡ���κ��¼����ͣ��������ʧ��"), C(CURRENT_MODULE));
            return;
        }

        selectedEventTypes = eventTypes;
        isRunning = true;
        kLog.Add(Info, C("��ʼ��������߳�"), C(CURRENT_MODULE));

        monitoringThread = std::thread([this]() {
            bool wmiOk = initializeWMI() && subscribeToEvents();
            bool etwOk = initializeETW();

            if (!wmiOk && !etwOk) {
                kLog.Add(Err, C("WMI��ETW��ؾ���ʼ��ʧ�ܣ�����޷�����"), C(CURRENT_MODULE));
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
            kLog.Add(Info, C("����߳����˳�"), C(CURRENT_MODULE));
            });
    }

    // ֹͣ���
    void stopMonitoring() {
        if (!isRunning) {
            kLog.Add(Warn, C("���δ�����У�����ֹͣ"), C(CURRENT_MODULE));
            return;
        }

        kLog.Add(Info, C("����ֹͣ���"), C(CURRENT_MODULE));
        isRunning = false;
        if (monitoringThread.joinable()) {
            monitoringThread.join();
            kLog.Add(Info, C("����߳��ѳɹ�join"), C(CURRENT_MODULE));
        }
        else {
            kLog.Add(Warn, C("����̲߳���join"), C(CURRENT_MODULE));
        }
    }

    // ����¼��б�
    void clearEvents() {
        try {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.clear();
            kLog.Add(Info, C("������¼��б�"), C(CURRENT_MODULE));
        }
        catch (const std::exception& e) {
            kLog.Add(Err, C("����¼��б�ʧ��: " + std::string(e.what())), C(CURRENT_MODULE));
        }
        catch (...) {
            kLog.Add(Err, C("����¼��б�ʱ����δ֪����"), C(CURRENT_MODULE));
        }
    }

    // ��ȡ��ǰ�¼��б�
    std::vector<EventRecord> getEvents() {
        try {
            std::lock_guard<std::mutex> lock(eventsMutex);
            return events;
        }
        catch (const std::exception& e) {
            kLog.Add(Err, C("��ȡ�¼��б�ʧ��: " + std::string(e.what())), C(CURRENT_MODULE));
            return {};
        }
        catch (...) {
            kLog.Add(Err, C("��ȡ�¼��б�ʱ����δ֪����"), C(CURRENT_MODULE));
            return {};
        }
    }

    // ��ѯ�Ƿ����ڼ��
    bool isMonitoring() const {
        return isRunning;
    }
};

// ȫ���¼������ʵ��
EventMonitor g_eventMonitor;

// ��չ�¼������б��ɸ�����Ҫ��ɾ��
std::vector<std::pair<std::string, bool>> eventTypes = {
    { "__InstanceCreationEvent", false },   // ʵ�������¼������̵ȣ�
    { "__InstanceDeletionEvent", false },   // ʵ��ɾ���¼�
    { "__InstanceModificationEvent", false },// ʵ���޸��¼�
    { "Win32_ProcessStartTrace", false },   // ������������
    { "Win32_ProcessStopTrace", false },    // ����ֹͣ����
    { "MSFT_NetConnectionCreate", false },  // �������Ӵ�����Win10+֧�֣�
    { "CIM_DataFile", false },              // �ļ�ϵͳ�¼�������/ɾ���ȣ�
    { "RegistryKeyChangeEvent", false },    // ע�������
    { "SecurityEvent", false },             // ��ȫ��־�¼�
};


void KswordMonitorMain() {
    if (ImGui::GetCurrentContext() == nullptr) {
        kLog.Add(Err, C("ImGui������δ��ʼ�����޷���ʾ��ؽ���"), C(CURRENT_MODULE));
        return;
    }

    ImGui::Text(C("ѡ��Ҫ��ص��¼�����:"), C(CURRENT_MODULE));
    ImGui::Separator();

    // ÿ����ʾ2��ѡ�� - ȷ��Columns��ȷʹ��
    {
        int columns = 2;
        ImGui::Columns(columns, NULL, false);  // Push columns

        for (size_t i = 0; i < eventTypes.size(); ++i) {
            if (!ImGui::Checkbox(eventTypes[i].first.c_str(), &eventTypes[i].second)) {
                // ����Ҫ������־��Checkboxʧ��ͨ����Ӱ������
            }
            if ((i + 1) % columns == 0) {
                ImGui::NextColumn();
            }
        }

        ImGui::Columns(1);  // Pop columns�ص�Ĭ��
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

    // �¼��б� - ȷ��Begin/End��ȷ���
    ImGui::Spacing();
    ImGui::Text(C("�¼��б�:"));
    ImGui::Separator();

    auto events = g_eventMonitor.getEvents();

    // �����ʾ�¼��б�
    if (ImGui::BeginTable("�¼��б�", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
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
    else {
        kLog.Add(Warn, C("�����¼��б���ʧ��"), C(CURRENT_MODULE));
    }
    ImGui::EndTabItem();
}

#undef CURRENT_MODULE