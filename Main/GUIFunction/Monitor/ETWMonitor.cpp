#define CURRENT_MODULE C("ETW���")
#include "../../KswordTotalHead.h"

#include <windows.h>
#include <evntrace.h>
#include <tdh.h> // ������ȷ������tdh.h
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

// UI����ʾ�����������̵߳��ã�
void ETWMonitorMain() {
    if (ImGui::CollapsingHeader(C("ETW���"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::GetCurrentContext() == nullptr) return;

        ImGui::Text(C("ETW�¼����"));
        ImGui::Separator();

        // ���ư�ť
        if (ImGui::Button(C("�������")) && !g_etwMonitor.IsRunning()) {
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
        if (ImGui::Button(C("ֹͣ���")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(C("����¼�"))) {
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
            ImGui::EndTable();
        }
    }
}

#undef CURRENT_MODULE