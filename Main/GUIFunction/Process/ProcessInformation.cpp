#include "../../KswordTotalHead.h"
#include "Process.h"
#include "ProcessDetail.h"
#include <winnt.h>  // ����WindowsȨ�޳�������
#define CURRENT_MODULE "������ϸ��Ϣ����"

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(
    HANDLE           ProcessHandle,
    DWORD            ProcessInformationClass,
    PVOID            ProcessInformation,
    ULONG            ProcessInformationLength,
    PULONG           ReturnLength);
// ������� PROCESS_BASIC_INFORMATION ����
typedef struct _MY_PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    PVOID BasePriority;  // ����Ϊ PVOID ����
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} _MY_PROCESS_BASIC_INFORMATION;
typedef struct _MY_PEB {
    BYTE                          Reserved1[2];
    BYTE                          BeingDebugged;
    BYTE                          Reserved2[1];
    PVOID                         Reserved3[2];
    PVOID                         Ldr;
    PVOID                         ProcessParameters;
    PVOID                         Reserved4[3];
    PVOID                         AtlThunkSListPtr;
    PVOID                         Reserved5;
    ULONG                         Reserved6;
    PVOID                         Reserved7;
    ULONG                         Reserved8;
    ULONG                         AtlThunkSListPtr32;
    PVOID                         Reserved9[45];
    BYTE                          Reserved10[96];
    PVOID                         PostProcessInitRoutine;
    BYTE                          Reserved11[128];
    PVOID                         Reserved12[1];
    ULONG                         SessionId;
} _MY_PEB, * _MY_PPEB;
//typedef struct _UNICODE_STRING {
//    USHORT Length;
//    USHORT MaximumLength;
//    PWSTR  Buffer;
//} UNICODE_STRING;
typedef struct _MY_RTL_USER_PROCESS_PARAMETERS {
    BYTE           Reserved1[16];
    PVOID          Reserved2[10];
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
} _MY_RTL_USER_PROCESS_PARAMETERS, * _MY_PRTL_USER_PROCESS_PARAMETERS;
// ������NtQueryInformationProcess�������Ϣ�ೣ��
const DWORD ProcessThreadCount = 0x03;
const DWORD ProcessHandleCount = 0x04;
const DWORD ProcessBasePriority = 0x05;
const DWORD ProcessExitStatus = 0x01;
const DWORD ProcessAffinityMask = 0x1D;
const DWORD ProcessSessionInformation = 0x1D;  // ��ͬϵͳ�����в��죬�����

// �������߳�����Ϣ�ṹ��
typedef struct _PROCESS_THREAD_COUNT_INFORMATION {
    ULONG ThreadCount;
} PROCESS_THREAD_COUNT_INFORMATION, * PPROCESS_THREAD_COUNT_INFORMATION;

// �������������Ϣ�ṹ��
typedef struct _PROCESS_HANDLE_COUNT_INFORMATION {
    ULONG HandleCount;
} PROCESS_HANDLE_COUNT_INFORMATION, * PPROCESS_HANDLE_COUNT_INFORMATION;

// �������Ự��Ϣ�ṹ��
typedef struct _PROCESS_SESSION_INFORMATION {
    DWORD SessionId;
} PROCESS_SESSION_INFORMATION, * PPROCESS_SESSION_INFORMATION;
// ������FILETIMEת����ʱ���ַ���
std::string kProcessDetail::FileTimeToString(FILETIME ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
        return "N/A";  // δ���ã��������н��̵��˳�ʱ�䣩
    }

    SYSTEMTIME stLocal;
    FileTimeToLocalFileTime(&ft, &ft);  // ת��Ϊ����ʱ��
    FileTimeToSystemTime(&ft, &stLocal);

    char buf[64];
    sprintf_s(buf, sizeof(buf),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        stLocal.wYear, stLocal.wMonth, stLocal.wDay,
        stLocal.wHour, stLocal.wMinute, stLocal.wSecond,
        stLocal.wMilliseconds);
    return buf;
}
bool kProcessDetail::GetParentProcessInfo() {
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid()
    );
    if (!hProcess) {
        kLog.err(C("�޷��򿪽��̻�ȡ��������Ϣ"), CURRENT_MODULE);
        parentPid = 0;
        return false;
    }

    // ʹ��NtQueryInformationProcess��ȡPROCESS_BASIC_INFORMATION
    typedef struct _PROCESS_BASIC_INFORMATION {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;   // ��ǰ����PID����֤�ã�
        ULONG_PTR InheritedFromUniqueProcessId; // ������PID
    } PROCESS_BASIC_INFORMATION, * PPROCESS_BASIC_INFORMATION;

    typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(
        HANDLE, INT, PVOID, ULONG, PULONG);
    _NtQueryInformationProcess NtQIP = (_NtQueryInformationProcess)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");

    if (!NtQIP) {
        CloseHandle(hProcess);
        parentPid = 0;
        return false;
    }

    PROCESS_BASIC_INFORMATION pbi = { 0 };
    ULONG returnLength = 0;
    NTSTATUS status = NtQIP(
        hProcess,
        0,  // 0 = ProcessBasicInformation
        &pbi,
        sizeof(pbi),
        &returnLength
    );

    if (NT_SUCCESS(status)) {
        // ��֤��ǰ����PID�Ƿ�ƥ�䣨ȷ����ȡ��ȷ��
        if (pbi.UniqueProcessId == (ULONG_PTR)pid()) {
            parentPid = (DWORD)pbi.InheritedFromUniqueProcessId;
            // ͨ��������PID��ѯ����
            parentProcessName = GetProcessNameByPID(parentPid);
        }
        else {
            parentPid = 0;
            parentProcessName = "��ȡʧ�ܣ�PID��ƥ�䣩";
        }
    }
    else {
        parentPid = 0;
        parentProcessName = "��ȡʧ�ܣ�NtQuery����";
    }

    CloseHandle(hProcess);
    return true;
}
// ��������ȡ��չ������Ϣ
bool kProcessDetail::GetProcessExtendedInfo() {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid());
    if (!hProcess) {
        kLog.err(C("�޷��򿪽��̻�ȡ��չ��Ϣ"), CURRENT_MODULE);
        return false;
    }

    // ��ʼ��������ϢΪĬ��ֵ
    creationTime = { 0, 0 };
    exitTime = { 0, 0 };
    kernelTime = { 0, 0 };
    userTime = { 0, 0 };
    threadCount = 0;
    handleCount = 0;
    basePriority = -1;   // ����Ϊ -1 ��ʾδ֪
    exitStatus = -1;
    affinityMask = 0;
    sessionId = -1;

    // ��ȡ����ʱ�䣨�������˳����ںˡ��û�ʱ�䣩
    if (!GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime)) {
        kLog.warn(C("GetProcessTimes ����ʧ��"), CURRENT_MODULE);
    }

    // ʹ�� NtQueryInformationProcess ��ȡ��չ��Ϣ
    typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(
        HANDLE, INT, PVOID, ULONG, PULONG);
    _NtQueryInformationProcess NtQIP = (_NtQueryInformationProcess)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) {
        CloseHandle(hProcess);
        return false;
    }

    // ��ȡ PROCESS_BASIC_INFORMATION �Ի�ȡ�������ȼ����׺��������
    _MY_PROCESS_BASIC_INFORMATION pbi = { 0 };
    ULONG returnLength = 0;
    NTSTATUS status = NtQIP(hProcess, 0, &pbi, sizeof(pbi), &returnLength);
    if (NT_SUCCESS(status)) {
        // �� PVOID ���͵� BasePriority ת��Ϊʵ�����ȼ�ֵ
        // �� x86 ϵͳ��ֱ��ת��Ϊ LONG���� x64 ����Ҫ����ָ���С
#ifdef _WIN64
        basePriority = (LONG)((INT_PTR)pbi.BasePriority);
#else
        basePriority = (LONG)((DWORD)pbi.BasePriority);
#endif

        affinityMask = pbi.AffinityMask;
        exitStatus = pbi.ExitStatus;
    }


    // ��ȡ�߳��������������ʹ�� ToolHelp API��
    THREADENTRY32 te32;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        te32.dwSize = sizeof(THREADENTRY32);
        if (Thread32First(hSnapshot, &te32)) {
            do {
                if (te32.th32OwnerProcessID == pid()) {
                    threadCount++;
                }
            } while (Thread32Next(hSnapshot, &te32));
        }
        CloseHandle(hSnapshot);
    }

    // ��ȡ�������ʹ�ù���API��
    GetProcessHandleCount(hProcess, &handleCount);

    // ��ȡ�ỰID��ʹ�ù���API��
    ProcessIdToSessionId(pid(), &sessionId);    CloseHandle(hProcess);
    return true;
}std::string kProcessDetail::GetCommandLine() {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid());
    if (!hProcess) {
        return "�޷��򿪽��� (Ȩ�޲���)";
    }
    _NtQueryInformationProcess NtQIP = (_NtQueryInformationProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) {
        CloseHandle(hProcess);
        return "GetProcAddress ʧ��: NtQueryInformationProcess API ������";
    }

    // ��ȡ ProcessBasicInformation �Զ�λ PEB
    PROCESS_BASIC_INFORMATION pbi;
    ULONG len;
    NTSTATUS status = NtQIP(hProcess, 0, &pbi, sizeof(pbi), &len);
    if (status != 0) {
        CloseHandle(hProcess);
        return "�޷���ȡ������Ϣ";
    }

    // ��ȡ PEB ��ַ
    PEB peb;
    if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr)) {
        CloseHandle(hProcess);
        return "�޷���ȡ PEB";
    }

    // ��ȡ���̲���
    _MY_RTL_USER_PROCESS_PARAMETERS upp;
    if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &upp, sizeof(upp), nullptr)) {
        CloseHandle(hProcess);
        return "�޷���ȡ���̲���";
    }

    // ���ȼ��
    if (upp.CommandLine.Length == 0 ||
        upp.CommandLine.Length > 0xFFFF) {
        CloseHandle(hProcess);
        return "No command line";
    }

    // ��ȡ������
    std::wstring wcmd;
    wcmd.resize(upp.CommandLine.Length / sizeof(WCHAR));
    if (!ReadProcessMemory(hProcess, upp.CommandLine.Buffer, wcmd.data(), upp.CommandLine.Length, nullptr)) {
        CloseHandle(hProcess);
        return "�޷���ȡ������";
    }
    CloseHandle(hProcess);
    return std::string(wcmd.begin(), wcmd.end());
}


kProcessDetail::kProcessDetail(DWORD pid) : kProcess(pid) {
    InitDetailInfo();
}

void kProcessDetail::InitDetailInfo() {
    // �Ӹ����ȡ������ϸ��Ϣ
    processName = Name();
    processExePath = ExePath();
    processUser = User();
    isAdmin = IsAdmin();
	commandLine = GetCommandLine();
    GetProcessExtendedInfo();  // ��������ʼ����չ��Ϣ
	GetParentProcessInfo();    // ��������ȡ��������Ϣ
}


void kProcessDetail::Render() {
    // ���ڱ���ʹ�ý�������+PID
    std::string windowTitle = processName + " (" + std::to_string(pid()) + ")";

    if (firstShow) {
        ImGui::SetNextWindowPos(ImVec2(200, 200)); // ����Ĭ��λ��
		ImGui::SetNextWindowSize(ImVec2(800, 400)); // ����Ĭ�ϴ�С
        firstShow = false; // ���״���ʾʱ��Ч
    }
    ImGui::Begin(C(windowTitle.c_str()), nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);

    // ����������
    ImGui::Text(C("��������:"));
    ImGui::SameLine();
    // ʹ�ò��ɱ༭���������ʾ��������
    char processNameBuf[256]; // ����ʵ�����������������С
    strcpy(processNameBuf, processName.c_str());
    ImGui::InputText("##ReadOnlyInputBox", processNameBuf, IM_ARRAYSIZE(processNameBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("����"))) {
        ImGui::SetClipboardText(processName.c_str());
    }

    // PID��
    ImGui::Text(C("����PID:"));
    ImGui::SameLine();
    // ʹ�ò��ɱ༭���������ʾPID
    char pidBuf[32]; // PID ���Ƚ϶̣���������СһЩ
    sprintf(pidBuf, "%d", pid());
    ImGui::InputText("ProcessPID##ReadOnlyInputBox", pidBuf, IM_ARRAYSIZE(pidBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("����##pid"))) {
        ImGui::SetClipboardText(std::to_string(pid()).c_str());
    }

    // ����·����
    ImGui::Text(C("����λ��:"));
    ImGui::SameLine();
    // ʹ�ò��ɱ༭���������ʾ����·��
    char processExePathBuf[1024]; // ����ʵ�����������������С
    strcpy(processExePathBuf, C(processExePath.c_str()));
    ImGui::InputText("ProcessFilePath##ReadOnlyInputBox", processExePathBuf, IM_ARRAYSIZE(processExePathBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("����##path"))) {
        ImGui::SetClipboardText(C(processExePath.c_str()));
    }
    ImGui::SameLine();
    if (ImGui::Button(C("��λ��"))) {
        // ���ļ�����Ŀ¼
        size_t lastSlashPos = processExePath.find_last_of("\\/");
        if (lastSlashPos == std::string::npos) {
            kLog.Add(Err, C("�޷��ҵ��ļ�����Ŀ¼"), C(CURRENT_MODULE));
        }
        std::string folderPath = processExePath.substr(0, lastSlashPos);
        HINSTANCE result = ShellExecuteA(NULL, "explore", folderPath.c_str(), NULL, NULL, SW_SHOWDEFAULT);

    }

    // ������ϸ��Ϣ����չ���ݣ�
    ImGui::Separator();
    ImGui::Text(C("�����û�: %s"), C(processUser.c_str()));
    ImGui::Text(C("����ԱȨ��: %s"), isAdmin ? C("��") : C("��"));
    // ��Render()���������
    ImGui::Text(C("����������:"));ImGui::SameLine();
    std::string cmdLine = commandLine;
    char cmdLineBuf[1024] = { 0 };
    strncpy(cmdLineBuf, C(cmdLine.c_str()), sizeof(cmdLineBuf) - 1);
    ImGui::InputText("##SelectedProcessCommandLine", cmdLineBuf, sizeof(cmdLineBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("����##cmd"))) {
        ImGui::SetClipboardText(C(cmdLine.c_str()));
    }
    ImGui::Separator();
    // ʱ����Ϣ
    ImGui::Text(C("����ʱ��: %s"), C(FileTimeToString(creationTime).c_str()));
    ImGui::Text(C("�˳�ʱ��: %s"), C(FileTimeToString(exitTime).c_str()));
    ImGui::Text(C("�ں�̬ʱ��: %s"), C(FileTimeToString(kernelTime).c_str()));
    ImGui::Text(C("�û�̬ʱ��: %s"), C(FileTimeToString(userTime).c_str()));

    // ��Դ��Ϣ
    ImGui::Text(C("�߳���: %d"), threadCount);
    ImGui::Text(C("�����: %d"), handleCount);

    // ���ȼ���״̬
    ImGui::Text(C("�������ȼ�: %d"), basePriority != -1 ? basePriority : -1);
    ImGui::Text(C("�˳�״̬: 0x%08X"), exitStatus != -1 ? exitStatus : 0);

    // ������Ϣ
    ImGui::Text(C("�׺�������: 0x%08X"), affinityMask);
    ImGui::Text(C("�ỰID: %d"), sessionId);
    ImGui::Text(C("������PID: %u"), parentPid);
    ImGui::Text(C("����������: %s"), C(parentProcessName.c_str()));
    if (ImGui::Button(C("ˢ�½�����ϸ��Ϣ"))) {
		GetProcessExtendedInfo();
    }
    ImGui::Separator();
	openTest.SetTargetPID(pid());
	openTest.ShowWindow();
    if (ImGui::Button(C("������ϸ��Ϣʵ��"))) {
        kProcDtl.remove(pid());
    }
    ImGui::End();
}

//�������ƴ�Ȩ��===============================================================================================
// // ���캯������ʼ�����г�������Ȩ�޲�����
static const std::map<DWORD, std::string> errorCodeMap = {
    {ERROR_SUCCESS, "�����ɹ����"},
    {ERROR_ACCESS_DENIED, "���ʱ��ܾ�"},
    {ERROR_INVALID_HANDLE, "��Ч�ľ��"},
    {ERROR_INVALID_PARAMETER, "��Ч�Ĳ���"},
    //{ERROR_PROCESS_NOT_FOUND, "�Ҳ���ָ���Ľ���"},
    {ERROR_INSUFFICIENT_BUFFER, "����������"},
    {ERROR_NOT_ENOUGH_MEMORY, "�ڴ治��"},
    {ERROR_OPERATION_ABORTED, "��������ֹ"}
};

ProcessOpenTest::ProcessOpenTest() : targetPID(0), showTestWindow(true) {
    // ��ʼ������Windows����Ȩ�ޣ������б�
    testItems = {
        // ������ѯȨ��
        {"PROCESS_QUERY_INFORMATION", PROCESS_QUERY_INFORMATION, "", false},
        {"PROCESS_QUERY_LIMITED_INFORMATION", PROCESS_QUERY_LIMITED_INFORMATION, "", false},

        // �ڴ����Ȩ��
        {"PROCESS_VM_READ", PROCESS_VM_READ, "", false},
        {"PROCESS_VM_WRITE", PROCESS_VM_WRITE, "", false},
        {"PROCESS_VM_OPERATION", PROCESS_VM_OPERATION, "", false},

        // ���̿���Ȩ��
        {"PROCESS_TERMINATE", PROCESS_TERMINATE, "", false},
        {"PROCESS_CREATE_THREAD", PROCESS_CREATE_THREAD, "", false},
        {"PROCESS_SET_INFORMATION", PROCESS_SET_INFORMATION, "", false},
        {"PROCESS_SUSPEND_RESUME", PROCESS_SUSPEND_RESUME, "", false},

        // �����ģ��Ȩ��
        {"PROCESS_DUP_HANDLE", PROCESS_DUP_HANDLE, "", false},
        {"PROCESS_CREATE_PROCESS", PROCESS_CREATE_PROCESS, "", false},
        {"PROCESS_SET_QUOTA", PROCESS_SET_QUOTA, "", false},
        {"PROCESS_SET_SESSIONID", PROCESS_SET_SESSIONID, "", false},

        // �ۺ�Ȩ��
        {"PROCESS_ALL_ACCESS", PROCESS_ALL_ACCESS, "", false}
    };
}

std::string ProcessOpenTest::GetErrorString(DWORD errorCode) {
    auto it = errorCodeMap.find(errorCode);
    if (it != errorCodeMap.end()) {
        std::stringstream ss;
        ss << it->second << " (0x" << std::hex << errorCode << ")";
        return ss.str();
    }

    std::stringstream ss;
    ss << "δ֪���� (0x" << std::hex << errorCode << ")";
    return ss.str();
}

void ProcessOpenTest::RunTest(OpenProcessTestItem& item) {
    if (targetPID == 0) {
        item.result = "������Ŀ��PID";
        item.tested = true;
        return;
    }

    HANDLE hProcess = OpenProcess(
        item.accessRight,
        FALSE,
        targetPID
    );

    if (hProcess == NULL) {
        DWORD errorCode = GetLastError();
        item.result = "ʧ��: " + GetErrorString(errorCode);
    }
    else {
        std::stringstream ss;
        ss << "�ɹ�: ��� 0x" << std::hex << (UINT_PTR)hProcess;
        item.result = ss.str();
        CloseHandle(hProcess);
    }
    item.tested = true;
}

void ProcessOpenTest::ShowWindow() {
    if (!showTestWindow) return;
    if (ImGui::TreeNode(C("�򿪽��̾������"))) {

        // �����ʾ���
        if (ImGui::BeginTable("permission_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(C("Ȩ������"), ImGuiTableColumnFlags_WidthFixed, 220);
            ImGui::TableSetupColumn(C("Ȩ��ֵ(ʮ������)"), ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn(C("���Խ��"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < testItems.size(); ++i) {
                auto& item = testItems[i];
                ImGui::TableNextRow();

                // Ȩ������
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", item.name.c_str());

                // Ȩ��ֵ
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%08X", item.accessRight);

                // ���Խ��������ɫ��
                ImGui::TableSetColumnIndex(2);
                if (item.tested) {
                    if (item.result.find(C("�ɹ�")) != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", C(item.result.c_str()));
                    }
                    else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", C(item.result.c_str()));
                    }
                }
                else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), C("δ����"));
                }

                // �������԰�ť
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Button((C("����##" + std::to_string(i))))) {
                    RunTest(item);
                }
            }

            ImGui::EndTable();
        }

        // ���Կ�����
        ImGui::BeginGroup();
        if (ImGui::Button(C("ִ�����в���"))) {
            for (auto& item : testItems) {
                RunTest(item);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("��ս��"))) {
            for (auto& item : testItems) {
                item.result.clear();
                item.tested = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("�������н��"))) {
            std::string allResults;
            for (const auto& item : testItems) {
                allResults += item.name + ": " + item.result + "\n";
            }
            ImGui::SetClipboardText(C(allResults.c_str()));
        }
        ImGui::EndGroup();
		ImGui::TreePop();
    }
}


//��ϸ��Ϣ������===============================================================================================
void ProcessDetailManager::add(DWORD pid) {
    // ��������ظ�PID
    for (const auto& detail : processDetails) {
        if (detail->pid() == pid) {
            kLog.warn(C("���������б��У�PID: " + std::to_string(pid)), C("ProcessDetailManager"));
            return;
        }
    }
    // ��ʼ������ӽ�������
    processDetails.emplace_back(std::make_unique<kProcessDetail>(pid));
    kLog.info(C("��ӽ��̵������б�PID: " + std::to_string(pid)), C("ProcessDetailManager"));
}

bool ProcessDetailManager::remove(DWORD pid) {
    // ���Ҳ��Ƴ�ָ��PID�Ľ���
    auto it = std::remove_if(processDetails.begin(), processDetails.end(),
        [pid](const std::unique_ptr<kProcessDetail>& detail) {
            return detail->pid() == pid;
        });

    if (it != processDetails.end()) {
        processDetails.erase(it, processDetails.end());
        kLog.info(C("�������б��Ƴ����̣�PID: " + std::to_string(pid)), C("ProcessDetailManager"));
        return true;
    }

    kLog.warn(C("δ�ҵ����̣��Ƴ�ʧ�ܣ�PID: " + std::to_string(pid)), C("ProcessDetailManager"));
    return false;
}

void ProcessDetailManager::renderAll() {
    // ��Ⱦ���н��̵����鴰��
    for (const auto& detail : processDetails) {
        detail->Render();
    }
}
#undef CURRENT_MODULE
