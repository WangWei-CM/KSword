#define CURRENT_MODULE C("ETW���")
#include <windows.h>
#include <tdh.h> // ������ȷ������tdh.h
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
#define MAX_GUID_SIZE 64 // GUID�ַ�����󳤶ȣ�������β��null�ַ���
static bool firstRun = true; // ���ڵ�һ������ʱ�ĳ�ʼ��
// Provider�����¼�ID�б�
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
// �洢����ETW Provider��������Ϣ��΢��ٷ��ֶ�+�¼�ID�б�
struct EtwProviderEvents {
    GUID guid;                          // Provider��GUID������TRACE_PROVIDER_INFO��
    std::wstring providerName;          // Provider���ƣ����ַ�������ԭʼ���룩
    bool isFromMOF;                     // ��Դ��true=WMI MOF�࣬false=XML manifest
    std::vector<uint32_t> eventIds;     // Provider�������¼�ID�б�
};

// �߳�����ṹ�壺���ݲ��д�������Ĳ���
struct ETWEnumThreadTask {
    size_t startIdx;                    // �̴߳������ʼ��������ӦTRACE_PROVIDER_INFO���飩
    size_t endIdx;                      // �̴߳���Ľ�����������ӦTRACE_PROVIDER_INFO���飩
    const TRACE_PROVIDER_INFO* traceProvArr; // �ٷ�TRACE_PROVIDER_INFO���飨�洢Provider������Ϣ��
    const BYTE* enumBufferBase;         // ö�ٻ���������ַ����������ƫ���ã�
    std::vector<EtwProviderEvents>* threadResult; // �̱߳��ؽ������
    int progressId;                     // �̶߳�Ӧ�Ľ�����ID
    std::atomic<size_t>& completedCount;// ȫ������ɼ�����ԭ�ӱ������̰߳�ȫ��
    size_t totalCount;                  // ��Provider�����������ܽ����ã�
	static int _TOTLA_PROGRESS_ID; // ȫ���ܽ�����ID����̬��Ա�������̹߳���
}; std::vector<EtwProviderEvents> etwProviders;
int ETWEnumThreadTask::_TOTLA_PROGRESS_ID = 0;
// �̴߳������������̴߳���һ��TRACE_PROVIDER_INFO����
void ETWEnumThreadProc(ETWEnumThreadTask task) {
    
    size_t threadTotal = task.endIdx - task.startIdx; // �߳��账���Provider����
    WCHAR stepTextW[512] = { 0 };                       // ���ַ������ı�������������⣩

    for (size_t i = task.startIdx; i < task.endIdx; ++i) {
        // 1. ���㵱ǰ�߳��ڵĽ��ȣ�0.0~1.0��
        float threadProgress = static_cast<float>(i - task.startIdx + 1) / threadTotal;

        // 2. ��ȡ��ǰProvider�Ļ�����Ϣ�����Թٷ�TRACE_PROVIDER_INFO��
        const TRACE_PROVIDER_INFO& currProv = task.traceProvArr[i];
        EtwProviderEvents providerData;

        // 2.1 ���GUID
        providerData.guid = currProv.ProviderGuid;

        // 2.2 ���Provider���ƣ�ͨ������������ַ+ƫ�Ƽ��㣬΢��ٷ��߼���
        if (currProv.ProviderNameOffset != 0) {
            LPWSTR nameW = reinterpret_cast<LPWSTR>(
                const_cast<BYTE*>(task.enumBufferBase) + currProv.ProviderNameOffset
                );
            providerData.providerName = (nameW != nullptr) ? nameW : L"Unknown Provider";
        }
        else {
            providerData.providerName = L"Unknown Provider";
        }

        // 2.3 �����Դ��MOF/manifest��΢��ٷ�SchemaSource�ֶΣ�
        providerData.isFromMOF = (currProv.SchemaSource != 0);

        // 3. ��ѯ��ǰProvider���¼�ID�б�����ԭTdhEnumerateManifestProviderEvents�߼���
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

        // 4. �����߳̽����������ַ�ת���ֽڣ�����Ksword��C()�꣩
        swprintf_s(stepTextW, L"�߳�%d��%s��%s��",
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

        // 5. ������������ȫ������ɼ���
        task.threadResult->push_back(providerData);
        task.completedCount++;

        // 6. �����ܽ�������10%��ʼ��+90%������ȣ�
        float totalProgress = 0.1f + 0.9f * static_cast<float>(task.completedCount) / task.totalCount;
        kItem.SetProcess(ETWEnumThreadTask::_TOTLA_PROGRESS_ID, C("����ö��ETW Provider..."), totalProgress);
    }
}
void EnumerateAllEtwProvidersWithEvents() {
    std::vector<EtwProviderEvents> finalResult;
    ULONG status = ERROR_SUCCESS;
    PROVIDER_ENUMERATION_INFO* pProvEnum = nullptr; // ΢��ٷ�ö�ٽ���ṹ��
    DWORD bufferSize = 0;
    const int THREAD_COUNT = 4;                     // �̶�4�߳�
    int TOTAL_PROGRESS_ID;                // �ܽ�����ID��ID=0��
    int threadProgressIds[THREAD_COUNT] = { 0 };      // 4���߳̽�����ID

    // --------------------------
    // ����1������5����������1��+4�̣߳�
    // --------------------------
    // �ܽ���������1������������״̬��
    TOTAL_PROGRESS_ID = kItem.AddProcess(
        C("ETW Providerö�٣��ܽ��ȣ�"),
        C("��ѯProvider�б��С..."),
        NULL,
        0.0f
    );
	ETWEnumThreadTask::_TOTLA_PROGRESS_ID = TOTAL_PROGRESS_ID; // ���þ�̬��Ա����
    // 4���߳̽���������2~5�����ֱ��Ӧ4���̣߳�
    for (int i = 0; i < THREAD_COUNT; ++i) {
        threadProgressIds[i] = kItem.AddProcess(
            C(std::string("�߳�") + std::to_string(i + 1) + "�������"),
            C("�ȴ��������..."),
            NULL,
            0.0f
        );
    }

    // --------------------------
    // ����2����΢��ٷ��߼�ö��Provider�б�
    // --------------------------
    // �״ε��ã���ȡ���軺������С��΢���Ƽ��߼���
    status = TdhEnumerateProviders(pProvEnum, &bufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER && status != ERROR_SUCCESS) {
        kItem.SetProcess(TOTAL_PROGRESS_ID, C("��ȡ��������Сʧ��"), 1.00f);
        kLog.err(C(std::string("TdhEnumerateProviders�״ε���ʧ�ܣ�") + HrToStr(status)), CURRENT_MODULE);
        if (pProvEnum != nullptr) {
            free(pProvEnum);
            pProvEnum = nullptr;
        }
        return; // ֱ�ӷ��أ������������
    }

    // ѭ�����仺����������Provider�б�̬�仯���´�С���㣬΢��ٷ��Ƽ���
    while (true) {
        // �ͷžɻ����������У��������»�����
        if (pProvEnum != nullptr) {
            free(pProvEnum);
            pProvEnum = nullptr;
        }
        pProvEnum = reinterpret_cast<PROVIDER_ENUMERATION_INFO*>(malloc(bufferSize));
        if (pProvEnum == nullptr) {
            kItem.SetProcess(TOTAL_PROGRESS_ID, C("���仺����ʧ��"), 1.00f);
            kLog.err(C(std::string("mallocʧ�ܣ���С��") + std::to_string(bufferSize)), CURRENT_MODULE);
            return;
        }

        // �ٴε��ã���ȡProviderö�ٽ��
        status = TdhEnumerateProviders(pProvEnum, &bufferSize);
        if (status == ERROR_SUCCESS) {
            // �ɹ���ȡ���������Ƿ���Provider
            if (pProvEnum->NumberOfProviders == 0) {
                kItem.SetProcess(TOTAL_PROGRESS_ID, C("δ�����κ�ETW Provider"), 1.00f);
                free(pProvEnum);
                pProvEnum = nullptr;
                return;
            }
            break; // ����ѭ���������������
        }
        else if (status != ERROR_INSUFFICIENT_BUFFER) {
            // �ǻ��������������ֹ����
            kItem.SetProcess(TOTAL_PROGRESS_ID, C("ö��Providerʧ��"), 1.00f);
            kLog.err(C(std::string("TdhEnumerateProvidersö��ʧ�ܣ�") + HrToStr(status)), CURRENT_MODULE);
            free(pProvEnum);
            pProvEnum = nullptr;
            return;
        }
        // ���������㣬����ѭ����bufferSize�ѱ�API����Ϊ������
    }

    // ��ȡ΢��ö�ٽ���ĺ�������
    size_t totalProvCount = pProvEnum->NumberOfProviders;
    const TRACE_PROVIDER_INFO* pTraceProvArr = pProvEnum->TraceProviderInfoArray;
    const BYTE* enumBufferBase = reinterpret_cast<BYTE*>(pProvEnum); // ����������ַ����������ƫ���ã�

    // --------------------------
    // ����3����ʼ������+�����߳�����
    // --------------------------
    // �����ܽ���������ʼ����ɣ�10%��
    kItem.SetProcess(TOTAL_PROGRESS_ID,
        C("��ʼ���д�����" + std::to_string(totalProvCount) + "��Provider��"),
        0.1f
    );

    // �����߳̽���������ʼ��������
    for (int i = 0; i < THREAD_COUNT; ++i) {
        kItem.SetProcess(threadProgressIds[i], C("������������"), 0.0f);
    }

    // �����������һ���̴߳���ʣ�ಿ�֣�������©��
    size_t perThreadCount = totalProvCount / THREAD_COUNT;
    std::atomic<size_t> completedCount(0); // ԭ�Ӽ�����ȷ���̰߳�ȫ
    std::vector<std::thread> threads;
    std::vector<std::vector<EtwProviderEvents>> threadLocalResults(THREAD_COUNT); // �̱߳��ؽ��

    // ����4���̣߳���������
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
    // ����4���ȴ��߳����+�ϲ����
    // --------------------------
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // �ϲ������̵߳ı��ؽ�������ս��
    for (auto& localResult : threadLocalResults) {
        finalResult.insert(finalResult.end(),
            std::make_move_iterator(localResult.begin()),
            std::make_move_iterator(localResult.end())
        );
    }

    // --------------------------
    // ����5�����½���Ϊ��ö����ɡ�
    // --------------------------
    kItem.SetProcess(TOTAL_PROGRESS_ID,
        C("ö����ɣ���" + std::to_string(finalResult.size()) + "��Provider��"),
        1.00f
    );
    for (int i = 0; i < THREAD_COUNT; ++i) {
        kItem.SetProcess(threadProgressIds[i], C("�������"), 1.00f);
    }

    // ��ֵ��ȫ�ֱ���
    etwProviders = std::move(finalResult);

    // �ͷ�΢��ö�ٽ����������΢��ٷ�ʾ�����ڴ��ͷ��߼���
    if (pProvEnum != nullptr) {
        free(pProvEnum);
        pProvEnum = nullptr;
    }
}


// �߳��������ݽṹ



// ��չ�¼��ṹ����������ϸ��Ϣ
struct ETWEventRecord {
    uint64_t id = 0;
    std::string timestamp;       // ��ȷ�������ʱ���
    std::string providerGuid;    // Provider GUID�ַ���
    std::string providerName;    // Provider���ƣ��Ѻ����ƣ�
    uint32_t eventId = 0;
    uint8_t level = 0;           // �¼�����1-5��
    uint8_t opcode = 0;          // ������
    std::string taskName;        // ��������
    std::string description;     // �¼���ϸ�������������ԣ�
};

class ETWMonitor {
public:
    ETWMonitor(size_t maxEvents = 2000) : maxEventCount(maxEvents) {}
    ~ETWMonitor() { Stop(); }

    // ������أ�֧�ֳ�ʼProvider�б�
    bool Start(const std::vector<GUID>& providers = {});
    // ֹͣ���
    void Stop();
    // ��ȡ�¼��б�֧�ֹ��ˣ�
    std::vector<ETWEventRecord> GetEvents(
        uint32_t eventId = 0,          // 0��ʾ������
        const std::string& provider = "",  // ���ַ�����ʾ������
        uint8_t minLevel = 0           // 0��ʾ������
    );
    // ��̬���Provider
    bool AddProvider(const GUID& provider, uint8_t level = TRACE_LEVEL_INFORMATION);
    // �Ƴ�Provider
    bool RemoveProvider(const GUID& provider);
    // ����¼��б�
    void ClearEvents() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.clear();
    }
    bool IsRunning() const { return running.load(); }
    static std::string GetProviderName(const GUID& providerGuid);
private:
    std::atomic<bool> running{false};
    std::atomic<uint64_t> nextId{1};
    size_t maxEventCount;          // ����¼�������
    std::vector<ETWEventRecord> events;
    std::mutex eventsMutex;
    std::thread traceThread;

    TRACEHANDLE sessionHandle = 0;
    EVENT_TRACE_PROPERTIES* sessionProps = nullptr;
    WCHAR sessionName[128] = L"KswordETWSession";
    std::vector<GUID> providerGuids;
    std::mutex providerMutex;      // ����providerGuids���̰߳�ȫ

    // �¼��ص�����
    static VOID WINAPI EventCallback(EVENT_RECORD* pEventRecord);
    // ����ѭ��
    void TraceLoop();
    // ����¼����б�
    void AddEvent(const ETWEventRecord& ev);
    // ����Provider���ƣ�GUID->���ƣ�

    // �����¼����飨�����������ơ����Եȣ�
    std::string ParseEventDetails(EVENT_RECORD* pEventRecord, std::string& taskName);
    // ��ʽ��ʱ�������ȷ�����룩
    std::string FormatTimestamp(ULONGLONG timeStamp);
};
std::string ETWMonitor::GetProviderName(const GUID& providerGuid) {
    // ֻ����GUID�ַ�������������Windows�汾
    return GuidToString(providerGuid);
}

// ����¼�������
void ETWMonitor::AddEvent(const ETWEventRecord& ev) {
    ETWEventRecord rec = ev;
    rec.id = nextId.fetch_add(1, std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(eventsMutex);
    events.insert(events.begin(), rec);  // ���¼�����ͷ��
    // �����������ʱ�Ƴ�����¼�
    if (events.size() > maxEventCount) {
        events.pop_back();
    }
}

// ��ʽ��ʱ�����FileTime->yyyy-MM-dd HH:mm:ss.fff��
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

// ��ȡProvider���ƣ�ͨ��GUID��



// �����¼����飨�������ơ����Եȣ�
std::string ETWMonitor::ParseEventDetails(EVENT_RECORD* pEventRecord, std::string& taskName) {
    std::stringstream ss;
    const auto& desc = pEventRecord->EventHeader.EventDescriptor;
    ss << "Level=" << (int)desc.Level 
       << ", Opcode=" << (int)desc.Opcode 
       << ", Task=" << desc.Task;
    // �����������ƣ�ʹ��TdhGetEventInformation��ȡTRACE_EVENT_INFO��
    ULONG infoSize = 0;
    ULONG status = TdhGetEventInformation(pEventRecord, 0, nullptr, nullptr, &infoSize);
    if (status != ERROR_INSUFFICIENT_BUFFER) {
        ss << " | ���Խ���ʧ��: " << HrToStr(status);
        taskName = "UnknownTask";
        return ss.str();
    }
    TRACE_EVENT_INFO* pInfo = (TRACE_EVENT_INFO*)LocalAlloc(LPTR, infoSize);
    if (!pInfo) {
        ss << " | �ڴ����ʧ��";
        taskName = "UnknownTask";
        return ss.str();
    }
    status = TdhGetEventInformation(pEventRecord, 0, nullptr, pInfo, &infoSize);
    if (status == ERROR_SUCCESS) {
        // ������
        if (pInfo->TaskNameOffset) {
            PWSTR taskW = (PWSTR)((BYTE*)pInfo + pInfo->TaskNameOffset);
            taskName = WstringToString(std::wstring(taskW));
            ss << " (" << taskName << ")";
        } else {
            taskName = "UnknownTask";
        }
        // �������б�
        ss << " | ����:";
        for (ULONG i = 0; i < pInfo->PropertyCount; ++i) {
            PWSTR propName = (PWSTR)((BYTE*)pInfo + pInfo->EventPropertyInfoArray[i].NameOffset);
            ss << " " << WstringToString(std::wstring(propName));
        }
    } else {
        ss << " | ���Խ���ʧ��: " << HrToStr(status);
        taskName = "UnknownTask";
    }
    LocalFree(pInfo);
    return ss.str();
}

// �������
bool ETWMonitor::Start(const std::vector<GUID>& providers) {
    if (running) {
        kLog.warn(C("ETW�������������"), CURRENT_MODULE);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        providerGuids = providers;
    }
    DWORD propsSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(sessionName);
    sessionProps = (EVENT_TRACE_PROPERTIES*)LocalAlloc(LPTR, propsSize);
    if (!sessionProps) {
        kLog.err(C("����EVENT_TRACE_PROPERTIESʧ��"), CURRENT_MODULE);
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
        ss << "�������ٻỰʧ��: " << HrToStr(status);
        if (status == ERROR_ALREADY_EXISTS) {
            ss << "���Ự�Ѵ��ڣ�����ֹͣ���лỰ��";
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
                ss << "����Providerʧ�ܣ�GUID: " << GuidToString(guid) << "������: " << HrToStr(status);
                kLog.warn(C(ss.str()), CURRENT_MODULE);
            } else {
                std::stringstream ss;
                ss << "������Provider: " << GetProviderName(guid);
                kLog.info(C(ss.str()), CURRENT_MODULE);
            }
        }
    }
    running = true;
    traceThread = std::thread(&ETWMonitor::TraceLoop, this);
    kLog.info(C("ETW���������"), CURRENT_MODULE);
    return true;
}

// ֹͣ���
void ETWMonitor::Stop() {
    if (!running) return;

    running = false;
    // ֹͣ���ٻỰ
    if (sessionHandle != 0 && sessionProps != nullptr) {
        ControlTrace(sessionHandle, sessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
        sessionHandle = 0;
        kLog.info(C("���ٻỰ��ֹͣ"), CURRENT_MODULE);
    }
    // �ͷ���Դ
    if (sessionProps != nullptr) {
        LocalFree(sessionProps);
        sessionProps = nullptr;
    }
    // �ȴ��߳̽���
    if (traceThread.joinable()) {
        traceThread.join();
    }
    // ���Provider�б�
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        providerGuids.clear();
    }
    kLog.info(C("ETW�����ֹͣ"), CURRENT_MODULE);
}

// ��̬���Provider
bool ETWMonitor::AddProvider(const GUID& provider, uint8_t level) {
    if (!running) {
        kLog.warn(C("���δ���У��޷����Provider"), CURRENT_MODULE);
        return false;
    }
    std::lock_guard<std::mutex> lock(providerMutex);
    // ����Ƿ��Ѵ���
    if (std::find(providerGuids.begin(), providerGuids.end(), provider) != providerGuids.end()) {
        std::stringstream ss;
        ss << "Provider�Ѵ���: " << GetProviderName(provider);
        kLog.info(C(ss.str()), CURRENT_MODULE);
        return true;
    }

    // ����Provider
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
        ss << "���Providerʧ��: " << GuidToString(provider) << "������: " << HrToStr(status);
        kLog.err(C(ss.str()), CURRENT_MODULE);
        return false;
    }

    providerGuids.push_back(provider);
    std::stringstream ss;
    ss << "�����Provider: " << GetProviderName(provider);
    kLog.info(C(ss.str()), CURRENT_MODULE);
    return true;
}

// �Ƴ�Provider
bool ETWMonitor::RemoveProvider(const GUID& provider) {
    if (!running) return false;

    std::lock_guard<std::mutex> lock(providerMutex);
    auto it = std::find(providerGuids.begin(), providerGuids.end(), provider);
    if (it == providerGuids.end()) {
        std::stringstream ss;
        ss << "Provider������: " << GuidToString(provider);
        kLog.warn(C(ss.str()), CURRENT_MODULE);
        return false;
    }

    // ����Provider
    ULONG status = EnableTraceEx2(
        sessionHandle,
        &provider,
        EVENT_CONTROL_CODE_DISABLE_PROVIDER,
        0, 0, 0, 0, nullptr
    );
    if (status != ERROR_SUCCESS) {
        std::stringstream ss;
        ss << "����Providerʧ��: " << GuidToString(provider) << "������: " << HrToStr(status);
        kLog.warn(C(ss.str()), CURRENT_MODULE);
    }

    providerGuids.erase(it);
    std::stringstream ss;
    ss << "���Ƴ�Provider: " << GetProviderName(provider);
    kLog.info(C(ss.str()), CURRENT_MODULE);
    return true;
}

// ��ȡ���˺���¼��б�
std::vector<ETWEventRecord> ETWMonitor::GetEvents(uint32_t eventId, const std::string& provider, uint8_t minLevel) {
    std::lock_guard<std::mutex> lock(eventsMutex);
    std::vector<ETWEventRecord> filtered;

    for (const auto& ev : events) {
        // �¼�ID����
        if (eventId != 0 && ev.eventId != eventId) continue;
        // Provider���ˣ�֧��GUID�����ƣ�
        if (!provider.empty() && ev.providerGuid != provider && ev.providerName != provider) continue;
        // ������ˣ�ֻ����>=minLevel���¼���
        if (minLevel != 0 && ev.level < minLevel) continue;

        filtered.push_back(ev);
    }

    return filtered;
}

// ����ѭ��������ʵʱ�¼���
void ETWMonitor::TraceLoop() {
    EVENT_TRACE_LOGFILE logFile = {0};
    logFile.LoggerName = sessionName;
    logFile.EventRecordCallback = ETWMonitor::EventCallback;  // ���ûص�
    logFile.Context = this;  // ���ݵ�ǰʵ����Ϊ������
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;

    // �򿪸��ٻỰ
    TRACEHANDLE handle = OpenTrace(&logFile);
    if (handle == INVALID_PROCESSTRACE_HANDLE) {
        ULONG err = GetLastError();
        kLog.err(C(std::string("�򿪸���ʧ��: ") + HrToStr(err)), CURRENT_MODULE);
        running = false;
        return;
    }

    kLog.info(C("��ʼ����ETW�¼�"), CURRENT_MODULE);
    // �����¼����������ã�ֱ��Stop�����ã�
    ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
        kLog.err(C(std::string("�¼��������������: ") + HrToStr(status)), CURRENT_MODULE);
    }

    // �رո���
    CloseTrace(handle);
}

// �¼��ص����������洢�¼���
VOID WINAPI ETWMonitor::EventCallback(EVENT_RECORD* pEventRecord) {
    if (!pEventRecord || !pEventRecord->UserContext) return;

    ETWMonitor* monitor = reinterpret_cast<ETWMonitor*>(pEventRecord->UserContext);
    ETWEventRecord ev;

    // ��������Ϣ
    ev.timestamp = monitor->FormatTimestamp(pEventRecord->EventHeader.TimeStamp.QuadPart);
    ev.providerGuid = GuidToString(pEventRecord->EventHeader.ProviderId);  // �������GUIDת�ַ�������
    ev.providerName = monitor->GetProviderName(pEventRecord->EventHeader.ProviderId);
    ev.eventId = pEventRecord->EventHeader.EventDescriptor.Id;
    ev.level = pEventRecord->EventHeader.EventDescriptor.Level;
    ev.opcode = pEventRecord->EventHeader.EventDescriptor.Opcode;

    // �����¼�����
    ev.description = monitor->ParseEventDetails(pEventRecord, ev.taskName);

    // ��ӵ��¼��б�
    monitor->AddEvent(ev);
}

// ���ߺ�����GUIDת�ַ���������ʵ�֣�

#include <atlstr.h>


// ȫ��ETW������ʵ��
static ETWMonitor g_etwMonitor(300000);  // ��󻺴�300000���¼�

// ʾ��������Provider GUID������չ��
namespace EtwProviders {
    // TCP/IP �¼�
    const GUID TcpIp = {0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xE8, 0x65, 0x94, 0x1E, 0xAB, 0x2B, 0xBA}};
    // ���̴���/�˳�
    const GUID Process = {0x22FB2CD6, 0x0E7B, 0x422B, {0x90, 0xC2, 0x34, 0x4F, 0x40, 0x9F, 0xAA, 0x3A}};
    // ע������
    const GUID Registry = {0x70EB4F03, 0x1435, 0x49E7, {0x9A, 0x20, 0x84, 0x71, 0x93, 0x38, 0xC7, 0x93}};
}
  static char filterText[256] = "";
  static EtwProviderEvents s_selectedGUID; // ��ǰѡ�е�Provider GUID
  static char customGuidInput[64] = { 0 }; // ��������GUID�ַ���
  static uint8_t customLevel = TRACE_LEVEL_INFORMATION; // �¼�����Ĭ��ֵ
// UI����ʾ�����������̵߳��ã�
void ETWMonitorMain() {
    if (ImGui::CollapsingHeader(C("ETW���"))) {
        // ���ڴ洢ɸѡ�ı��Ļ�����
        if (firstRun) {
            firstRun = false;
            std::thread(EnumerateAllEtwProvidersWithEvents).detach(); // �첽����ETW Providers
        }
        if (ImGui::BeginChild("ETWProviderScrolling", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetStyle().ItemSpacing.y), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            // ��ȡETW Providers����
            // ����ɸѡ�����
            ImGui::InputText(C("ɸѡ Provider ���¼�ID..."), filterText, IM_ARRAYSIZE(filterText));

            // �������
            if (ImGui::BeginTable(C("EtwProvidersTable"), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                // ���ñ�ͷ
                ImGui::TableSetupColumn(C("Provider GUID"), ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn(C("�¼�ID�б�"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // ��������Providers
                for (const auto& provider : etwProviders)
                {

                    // ��GUIDת��Ϊ�ַ����Ա���ʾ��ɸѡ
                    WCHAR guidStr[64];
                    StringFromGUID2(provider.guid, guidStr, IM_ARRAYSIZE(guidStr));
                    char ansiGuidStr[64];
                    WideCharToMultiByte(CP_ACP, 0, guidStr, -1, ansiGuidStr, sizeof(ansiGuidStr), NULL, NULL);

                    // ����Ƿ����ɸѡ����
                    bool matchesFilter = false;

                    if (strstr(ansiGuidStr, filterText) != nullptr) {
                        matchesFilter = true;
                    }
                    else {
                        // 2. ���������Provider Name�Ƿ�ƥ�䣨���ִ�Сд������
                        // ���ַ�Provider Nameת���ֽڣ���filterText����һ�£�
                        char providerNameA[512] = { 0 };
                        WideCharToMultiByte(
                            CP_ACP,
                            0,
                            provider.providerName.c_str(),  // ʹ��Provider�����ƣ����ַ���
                            -1,
                            providerNameA,
                            sizeof(providerNameA),
                            NULL,
                            NULL
                        );
                        // ���ִ�Сдģ��ƥ�䣨ʹ��_stricmp���ַ��Ƚ��Ӵ���
                        if (_strnicmp(providerNameA, filterText, strlen(filterText)) != 0) {
                            // ��ǰ׺��ƥ�䣬�ټ���Ƿ�������Զ��岻�ִ�Сдstrstr��
                            auto w2a = [](const wchar_t* wstr) { char b[512] = { 0 }; WideCharToMultiByte(CP_ACP, 0, wstr, -1, b, 512, nullptr, nullptr); return b; };
                            char* findPos = StrStrIA(w2a(provider.providerName.c_str()), filterText);
                            if (findPos != nullptr) {
                                matchesFilter = true;
                            }
                        }
                        else {
                            matchesFilter = true;
                        }

                        // 3. ԭ�У�����¼�ID�Ƿ�ƥ�䣨����ԭ�߼������ȼ�����Provider Name��
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
                    // ֻ��ʾ����ɸѡ��������
                    if (matchesFilter || filterText[0] == '\0')
                    {
                        ImGui::PushID(ansiGuidStr);
                        ImGui::TableNextRow();

                        // ��ʾGUID
                        ImGui::TableSetColumnIndex(0);
                        bool isSelected = (s_selectedGUID.guid == provider.guid);
                        if (ImGui::Selectable(C(ansiGuidStr), isSelected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                        {
                            s_selectedGUID = provider;
                        }
                        if (ImGui::BeginPopupContextItem("DLLModuleRightClick")) {  // ʹ�õ�ǰID�������Ĳ˵�

                            if (ImGui::MenuItem(C("����GUID##" + GuidToString(provider.guid)))) {
                                ImGui::SetClipboardText(ansiGuidStr);
                            }ImGui::EndPopup();  // �ؼ��޸�����Ӵ��йرյ����˵�
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(C(WCharToString(provider.providerName.c_str())));

                        // ��ʾ�¼�ID�б�
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
        ImGui::Text(C("ETW�¼����"));
        ImGui::Separator();
        ImGui::InputText(C("�Զ���GUID"), customGuidInput, sizeof(customGuidInput), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SameLine();
        ImGui::Text(C("��ʽ: {xxxxxxxx - xxxx - xxxx - xxxx - xxxxxxxxxxxx}"));

        ImGui::InputInt(C("�¼�����"), (int*)&customLevel, 1, 1);
        ImGui::Text(C("����Χ: 1(��ϸ) - 5(����)"));
        if (customLevel < 1) customLevel = 1;
        if (customLevel > 5) customLevel = 5;

        // ����������Զ���Provider��ť
        if (ImGui::Button(C("����Զ���Provider")) && g_etwMonitor.IsRunning()) {
            GUID customGuid;
            int wideSize = MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, nullptr, 0);
            std::vector<wchar_t> wideGuid(wideSize);
            MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, wideGuid.data(), wideSize);
            // ���Դ������ַ�������GUID
            HRESULT hr = IIDFromString(wideGuid.data(), &customGuid);
            if (SUCCEEDED(hr)) {
                bool result = g_etwMonitor.AddProvider(customGuid, customLevel);
                if (result) {
                    kLog.info(C("�ɹ�����Զ���Provider: " + std::string(customGuidInput)), CURRENT_MODULE);
                    // ��������
                    memset(customGuidInput, 0, sizeof(customGuidInput));
                }
            }
            else {
                kLog.err(C("GUID��ʽ��Ч����������"), CURRENT_MODULE);
            }
        }


        // ���ư�ť
        if (ImGui::Button(C("�������##ETWMonitor")) && !g_etwMonitor.IsRunning()) {
            // ��ʼ����TCP/IP�ͽ��̼��
            std::vector<GUID> initialProviders = {
                EtwProviders::TcpIp,
                EtwProviders::Process
            };
            if (g_etwMonitor.Start(initialProviders)) {
                kLog.info(C("ETW��������ɹ�"), CURRENT_MODULE);
            }
            else {
                kLog.err(C("ETW�������ʧ��"), CURRENT_MODULE);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("ֹͣ���##ETWMonitor")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(C("����¼�##ETWMonitor"))) {
            g_etwMonitor.ClearEvents();
        }

        // ��̬���Provider
        ImGui::Separator();
        ImGui::Text(C("��̬Provider����"));
        if (ImGui::Button(C("���ע�����")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.AddProvider(EtwProviders::Registry, TRACE_LEVEL_VERBOSE);
        }

        // �¼����˿ؼ�
        static uint32_t filterEventId = 0;
        static char filterProvider[256] = { 0 };
        static uint8_t filterMinLevel = 0;
        ImGui::Separator();
        ImGui::Text(C("�¼�����"));
        ImGui::InputInt(C("�¼�ID"), (int*)&filterEventId);
        ImGui::InputText(C("Provider(GUID/����)"), filterProvider, sizeof(filterProvider));
        ImGui::InputInt(C("��С����(1-5)"), (int*)&filterMinLevel, 1, 1);
        if (filterMinLevel > 5) filterMinLevel = 5;

        // �¼��б�չʾ
        ImGui::Separator();
        ImGui::Text(C("�¼��б� (����: %d)"), g_etwMonitor.GetEvents().size());
        if (ImGui::BeginChild("ETWEventTableescrolling", ImVec2(0, 300), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y - 10);
            if (ImGui::BeginTable("ETWEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, outerSize)) {
                ImGui::TableSetupColumn(C("ID"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("ʱ���"), ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn(C("Provider"), ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn(C("�¼�ID"), ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch);
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