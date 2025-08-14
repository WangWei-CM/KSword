#include "dll.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <windows.h>
#include <winver.h>
#pragma comment(lib, "version.lib")
// ��̬���ݴ洢
namespace KswordDebugger
{
    static DWORD s_currentPID = 0;
    static std::string s_processPath;
    static std::string s_processName;
    static bool s_is64BitProcess = false;
    static bool s_isProcessRunning = false;
    static bool s_autoRefresh = false;
    static float s_refreshInterval = 1.0f;
    static float s_lastRefreshTime = 0.0f;

    // UI״̬�洢
    static char s_filterText[256] = "";
    static char s_dllPath[512] = "";
    static char s_memoryAddr[32] = "0x00400000";
    static char s_breakpointAddr[32] = "0x00401000";
    static int s_sortColumn = -1;
    static bool s_sortAscending = true;
    static bool s_showModuleDetails = false;
    static KswordModuleInfo s_selectedModule;  // ʹ��ʵ�ʶ���Ľṹ��
    static bool s_showMemoryEditor = false;
    static KswordMemoryRegionInfo s_selectedMemoryRegion;  // ʹ��ʵ�ʶ���Ľṹ��

    // ���ݴ洢ʹ��vector����
    static std::vector<KswordModuleInfo> s_modules;
    static std::vector<KswordThreadInfo> s_threads;
    static std::vector<KswordMemoryRegionInfo> s_memoryRegions;
    static std::vector<KswordBreakpointInfo> s_breakpoints;
}

using namespace KswordDebugger;

// ��������
static void RefreshAllData();
static void FilterAndSortModules(std::vector<KswordModuleInfo>& filteredModules);
static void DrawModuleDetails();
static void DrawMemoryEditor();
static std::string FormatSize(size_t size);
static std::string FormatAddress(uintptr_t addr);
static bool AddBreakpoint(uintptr_t address);
static bool RemoveBreakpoint(uintptr_t address);
static bool ToggleBreakpoint(uintptr_t address);


// ж��ģ���ʵ��ʵ�ֺ���
bool UnloadModule(DWORD pid, HMODULE hModule) {
    if (pid == 0 || hModule == NULL) {
        kLog.err(C("��Ч�Ľ���ID��ģ����"), C("ModuleView"));
        return false;
    }

    // ��Ŀ�����
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "�򿪽���ʧ�� (PID: %u), ����: %u", pid, GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        return false;
    }

    // ��ȡFreeLibrary��ַ
    LPVOID freeLibAddr = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FreeLibrary");
    if (!freeLibAddr) {
        char errMsg[256];
        sprintf_s(errMsg, "��ȡFreeLibrary��ַʧ��, ����: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        CloseHandle(hProcess);
        return false;
    }

    // ����Զ���߳�ִ��ж��
    HANDLE hRemoteThread = CreateRemoteThread(
        hProcess,
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)freeLibAddr,
        hModule,  // ģ������Ϊ����
        0,
        NULL
    );

    if (!hRemoteThread || hRemoteThread == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "����ж���߳�ʧ��, ����: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        CloseHandle(hProcess);
        return false;
    }

    // �ȴ�ж�����
    DWORD waitResult = WaitForSingleObject(hRemoteThread, 5000); // 5�볬ʱ
    if (waitResult == WAIT_TIMEOUT) {
        kLog.warn(C("ж��ģ�鳬ʱ"), C("ModuleView"));
        TerminateThread(hRemoteThread, 0);
        CloseHandle(hRemoteThread);
        CloseHandle(hProcess);
        return false;
    }

    // ��ȡ�߳��˳����ж��Ƿ�ж�سɹ�
    DWORD exitCode;
    if (!GetExitCodeThread(hRemoteThread, &exitCode) || exitCode == 0) {
        kLog.err(C("ģ��ж��ʧ��"), C("ModuleView"));
        CloseHandle(hRemoteThread);
        CloseHandle(hProcess);
        return false;
    }

    // ������Դ
    CloseHandle(hRemoteThread);
    CloseHandle(hProcess);
    return true;
}

void BrowseDLLFile(char* outPath, size_t maxSize) {
    OPENFILENAMEA ofn = { 0 };
    char szFile[MAX_PATH] = { 0 };

    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "DLL Files (*.dll)\0*.dll\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = "ѡ��Ҫע���DLL�ļ�";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        strncpy_s(outPath, maxSize, szFile, _TRUNCATE);
        kLog.info(C("��ѡ��DLL�ļ�"), C("ModuleView"));
    }
    else {
        DWORD err = CommDlgExtendedError();
        if (err != 0) {
            kLog.err(C(("�ļ�ѡ��Ի������: " + std::to_string(err)).c_str()), C("ModuleView"));
        }
    }
}

// DLLע��ʵ��
bool InjectDLL(const char* dllPath) {
    if (!dllPath || strlen(dllPath) == 0) {
        kLog.err(C("��Ч��DLL·��"), C("ModuleView"));
        return false;
    }

    // ��ȡĿ�����ID (������Ҫ����ʵ������޸ģ������UIѡ��Ľ���)
    DWORD pid = s_currentPID; // �������д˺�����ȡĿ�����ID
    if (pid == 0) {
        kLog.err(C("δѡ��Ŀ�����"), C("ModuleView"));
        return false;
    }

    // ��Ŀ�����
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "�򿪽���ʧ�� (PID: %u), ����: %u", pid, GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        return false;
    }

    // ��Ŀ������з����ڴ�
    LPVOID remoteMem = VirtualAllocEx(hProcess, NULL, strlen(dllPath) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        char errMsg[256];
        sprintf_s(errMsg, "�ڴ����ʧ��, ����: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        CloseHandle(hProcess);
        return false;
    }

    // д��DLL·����Ŀ������ڴ�
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, strlen(dllPath) + 1, NULL)) {
        char errMsg[256];
        sprintf_s(errMsg, "д���ڴ�ʧ��, ����: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // ��ȡLoadLibraryA��ַ
    LPVOID loadLibAddr = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!loadLibAddr) {
        char errMsg[256];
        sprintf_s(errMsg, "��ȡLoadLibraryA��ַʧ��, ����: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // ����Զ���߳�ִ��DLLע��
    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibAddr, remoteMem, 0, NULL);
    if (!hRemoteThread || hRemoteThread == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "����Զ���߳�ʧ��, ����: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // �ȴ�ע�����
    WaitForSingleObject(hRemoteThread, INFINITE);

    // ������Դ
    CloseHandle(hRemoteThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    kLog.info(C("DLLע���߳�ִ�����"), C("ModuleView"));
    return true;
}
// UI��Ⱦ����
void KswordDLLMain()
{
    // ��ʼ����ǰ������Ϣ
    // ����PID�����UI���
        ImGui::Text(C("���������PID:"));
        static char pidBuffer[32] = "";  // ���ڴ洢PID����Ļ�����
        ImGui::InputText("##pidInputField", pidBuffer, IM_ARRAYSIZE(pidBuffer),
            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine();
        if (ImGui::Button(C("ȷ�ϲ�ˢ��"))) {
            if (strlen(pidBuffer) > 0) {
                // ת������ΪDWORD����PID
                DWORD targetPID = static_cast<DWORD>(atoi(pidBuffer));
                if (targetPID > 0) {
                    s_currentPID = targetPID;  // ���µ�ǰPID
                    kLog.info(C("�����õ�ǰPIDΪ: " + std::to_string(s_currentPID)), C("PIDInput"));
                    // ��ȡ����·��������
                    char path[MAX_PATH] = { 0 };
                    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
                        s_processPath = path;
                        size_t lastSlash = s_processPath.find_last_of("\\/");
                        s_processName = (lastSlash != std::string::npos) ? s_processPath.substr(lastSlash + 1) : s_processPath;
                        s_isProcessRunning = true;
                        // ���λ��
                    }
                    else {
                        kLog.warn(C("��Ч��PIDֵ��������������"), C("PIDInput"));
                    }
                }
                else {
                    kLog.warn(C("PID�������Ϊ��"), C("PIDInput"));
                }
            }
            if (s_currentPID != 0)  // ��ָ����Ŀ��PIDʱִ�г�ʼ��
            {
                // ��Ŀ����̣���Ҫ�㹻Ȩ�ޣ�
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE, FALSE, s_currentPID);
                if (hProcess == NULL)
                {
                    DWORD error = GetLastError();
                    kLog.err(C(std::string("�򿪽���ʧ�ܣ�PID: ") + std::to_string(s_currentPID) + std::string("��������: ") + std::to_string(error)), C("ProcessInfo"));
                    s_currentPID = 0;  // ������ЧPID
                    return;
                }

                // ��ȡ����·��������
                char path[MAX_PATH] = { 0 };
                if (GetModuleFileNameExA(hProcess, NULL, path, MAX_PATH) == 0)
                {
                    DWORD error = GetLastError();
                    kLog.err(C("��ȡ����·��ʧ�ܣ�������: " + std::to_string(error)), C("ProcessInfo"));
                    CloseHandle(hProcess);
                    s_currentPID = 0;
                    return;
                }
                s_processPath = path;
                size_t lastSlash = s_processPath.find_last_of("\\/");
                s_processName = (lastSlash != std::string::npos) ? s_processPath.substr(lastSlash + 1) : s_processPath;

                // ���Ŀ�����λ��
                BOOL isWow64 = FALSE;
                s_is64BitProcess = false;  // Ĭ��32λ

                // �жϵ�ǰϵͳ�Ƿ�Ϊ64λ
                SYSTEM_INFO sysInfo = { 0 };
                GetNativeSystemInfo(&sysInfo);
                bool isSystem64Bit = (sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);

                if (isSystem64Bit)
                {
                    // 64λϵͳ�ϣ�ͨ��IsWow64Process�ж�Ŀ������Ƿ�Ϊ32λ
                    if (IsWow64Process(hProcess, &isWow64))
                    {
                        // isWow64ΪTRUE �� Ŀ����32λ���̣�FALSE �� Ŀ����64λ����
                        s_is64BitProcess = !isWow64;
                    }
                    else
                    {
                        DWORD error = GetLastError();
                        kLog.warn(C("IsWow64Process����ʧ�ܣ�������: " + std::to_string(error) + "��Ĭ�ϰ�32λ����"), C("ProcessInfo"));
                    }
                }
                else
                {
                    // 32λϵͳֻ������32λ����
                    s_is64BitProcess = false;
                }

                // �������Ƿ�����
                DWORD exitCode = 0;
                s_isProcessRunning = GetExitCodeProcess(hProcess, &exitCode) && (exitCode == STILL_ACTIVE);
                if (!s_isProcessRunning)
                {
                    kLog.warn(C("Ŀ��������˳���PID: " + std::to_string(s_currentPID)), C("ProcessInfo"));
                }

                CloseHandle(hProcess);
                RefreshAllData();  // ˢ��Ŀ����̵�����
            }

        }

        ImGui::SameLine();
        if (ImGui::Button(C("���"))) {
            memset(pidBuffer, 0, sizeof(pidBuffer));
            kLog.info(C("�����PID�����"), C("PIDInput"));
        }

    // �Զ�ˢ���߼�
    float currentTime = ImGui::GetTime();
    if (s_autoRefresh && (currentTime - s_lastRefreshTime > s_refreshInterval))
    {
        RefreshAllData();
        s_lastRefreshTime = currentTime;
    }

    // ������Ϣ��ʾ
    ImGui::Text(C("������Ϣ"));
    ImGui::Separator();

    ImGui::BeginGroup();
    ImGui::Text(C("����ID: %d"), s_currentPID);
    ImGui::Text(C("��������: %s"), s_processName.c_str());
    ImGui::Text(C("����·��:"));
    ImGui::SameLine();
    static char s_processPathBuffer[MAX_PATH] = "";
        if (!s_processPath.empty())
            strncpy_s(s_processPathBuffer, sizeof(s_processPathBuffer), C(s_processPath.c_str()), _TRUNCATE);
        else
            s_processPathBuffer[0] = '\0';
    ImGui::InputText("##processPath", s_processPathBuffer, sizeof(s_processPathBuffer), ImGuiInputTextFlags_ReadOnly);
    
    ImGui::Text(C("����λ��: %s"), s_is64BitProcess ? C("64λ") : C("32λ"));
    ImGui::Text(C("����״̬: %s"), s_isProcessRunning ? C("������") : C("��ֹͣ"));
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button(C("���ļ�·��")))
    {
        std::string folderPath = s_processPath.substr(0, s_processPath.find_last_of("\\/"));
        kLog.info(C(("���ļ�·��: " + folderPath).c_str()), C("ModuleView"));
    }

    if (ImGui::Button(s_autoRefresh ? C("�ر��Զ�ˢ��") : C("�����Զ�ˢ��")))
    {
        s_autoRefresh = !s_autoRefresh;
        if (s_autoRefresh)
            kLog.info(C("�ѿ����Զ�ˢ��"), C("ModuleView"));
        else
            kLog.info(C("�ѹر��Զ�ˢ��"), C("ModuleView"));
    }

    ImGui::Text(C("ˢ�¼��:"));
    ImGui::SameLine();
    ImGui::SliderFloat("##refreshInterval", &s_refreshInterval, 0.5f, 10.0f, C("%.1fs"));

    if (ImGui::Button(C("�ֶ�ˢ��")))
    {
        RefreshAllData();
        kLog.info(C("�ֶ�ˢ������"), C("ModuleView"));
    }
    ImGui::EndGroup();

    ImGui::Separator();

    // ģ���б�ɸѡ��
    ImGui::Text(C("ģ���б�"));
    ImGui::InputTextWithHint("##filter", C("ɸѡģ��(��Сд������)"), s_filterText, IM_ARRAYSIZE(s_filterText));

    // �б������ť
    ImGui::SameLine();
    if (ImGui::Button(C("����б�")))
    {
        kLog.info(C("���ģ���б�"), C("ModuleView"));
    }

    ImGui::SameLine();
    if (ImGui::Button(C("ˢ��ģ��")))
    {
        kLog.info(C("ˢ��ģ���б�"), C("ModuleView"));
        RefreshAllData();
    }

    // ɸѡ������ģ��
    std::vector<KswordModuleInfo> filteredModules = s_modules;
    FilterAndSortModules(filteredModules);

    // ��ʾģ����
    float windowHeight = ImGui::GetContentRegionAvail().y;
    float halfHeight = windowHeight * 0.5f - ImGui::GetStyle().ItemSpacing.y;

    // ʹ���Ӵ������Ʊ��߶�Ϊ�������
    ImGui::BeginChild("ModuleTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    {
        if (ImGui::BeginTable("ModuleTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn(C("ģ��·��"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(C("����ַ"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("��С"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("�汾"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("״̬"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // �����߼�
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs && sortSpecs->Specs)
            {
                s_sortColumn = sortSpecs->Specs[0].ColumnIndex;
                s_sortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }

            for (size_t i = 0; i < filteredModules.size(); ++i)
            {
                const auto& mod = filteredModules[i];
                ImGui::TableNextRow();

                // ģ��·��
                ImGui::TableSetColumnIndex(0);
                bool isSelected = (s_showModuleDetails && s_selectedModule.path == mod.path);
                if (ImGui::Selectable(mod.path.c_str(), isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                {
                    s_selectedModule = mod;
                    s_showModuleDetails = true;
                }

                // ����ַ
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", C(FormatAddress(mod.baseAddr).c_str()));

                // ��С
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", FormatSize(mod.size).c_str());

                // �汾
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", mod.version.c_str());

                // ״̬
                ImGui::TableSetColumnIndex(4);
                if (mod.isSigned)
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), C("��ǩ��"));
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), C("δǩ��"));
                ImGui::SameLine();
                ImGui::Text("%s", mod.is64Bit ? C("64λ") : C("32λ"));

                // ������ť
                ImGui::TableSetColumnIndex(5);
                std::string locateBtnId = "��λ##" + std::to_string(i);
                if (ImGui::Button(C(locateBtnId.c_str()), ImVec2(40, 0)))
                {
                    // ��ʽ���ڴ��ַ��ʾ��ȷ����ȷ��ʮ�����Ƹ�ʽ��
                    sprintf_s(s_memoryAddr, "0x%p", (void*)mod.baseAddr);
                    s_showMemoryEditor = true;

                    // �Ż���־�����ʹ��sprintf����string��C()ֱ��ƴ��
                    char logMsg[512];
                    sprintf_s(logMsg, "��λģ���ڴ�: %s (��ַ: 0x%p)", mod.path.c_str(), (void*)mod.baseAddr);
                    kLog.info(C(logMsg), C("ModuleView"));
                }
                ImGui::SameLine();

                std::string unloadBtnId = "ж��##" + std::to_string(i);
                if (ImGui::Button(C(unloadBtnId.c_str()), ImVec2(40, 0)))
                {
                    char logMsg[512];
                    sprintf_s(logMsg, "����ж��ģ��: %s (��ַ: 0x%p)", mod.path.c_str(), (void*)mod.baseAddr);
                    kLog.info(C(logMsg), C("ModuleView"));

                    // ִ��ʵ��ж�ز���
                    bool unloadSuccess = UnloadModule(s_currentPID, (HMODULE)mod.baseAddr);

                    // ����ж�ؽ�������б���¼��־
                    if (unloadSuccess) {
                        // ���б����Ƴ�ģ��
                        auto it = std::find_if(s_modules.begin(), s_modules.end(),
                            [&](const KswordModuleInfo& m) {
                                return m.path == mod.path && m.baseAddr == mod.baseAddr;
                            });

                        if (it != s_modules.end()) {
                            s_modules.erase(it);
                            sprintf_s(logMsg, "�ѳɹ�ж��ģ��: %s", mod.path.c_str());
                            kLog.info(C(logMsg), C("ModuleView"));
                        }
                        else {
                            kLog.warn(C("ģ����ж�ص�δ���б����ҵ�"), C("ModuleView"));
                        }
                    }
                    else {
                        sprintf_s(logMsg, "ж��ģ��ʧ��: %s", mod.path.c_str());
                        kLog.err(C(logMsg), C("ModuleView"));
                    }
                }
            }
            ImGui::EndTable();
        }
		ImGui::EndChild();  // �����Ӵ���
    }

    // ��ʾģ����ϸ��Ϣ
    if (s_showModuleDetails)
    {
        DrawModuleDetails();
    }

    ImGui::Separator();

    // DLLע������
    ImGui::Text(C("DLLע��"));
    ImGui::InputText("##dllPath", s_dllPath, IM_ARRAYSIZE(s_dllPath));
    ImGui::SameLine();
    if (ImGui::Button(C("���")))
    {
        kLog.info(C("���DLL�ļ�"), C("ModuleView"));
        BrowseDLLFile(s_dllPath, MAX_PATH); // ����s_dllPath��ȫ�ֻ�����
    }
    ImGui::SameLine();
    if (ImGui::Button(C("ע��")))
    {
        if (strlen(s_dllPath) > 0)
        {
            if (InjectDLL(s_dllPath))
            {
                // �����ַ���ƴ�ӷ�ʽ������ֱ����C()��string���
                char successMsg[512];
                sprintf_s(successMsg, "DLLע��ɹ�: %s", s_dllPath);
                kLog.info(C(successMsg), C("ModuleView"));
                RefreshAllData();
            }
            else
            {
                char failMsg[512];
                sprintf_s(failMsg, "DLLע��ʧ��: %s", s_dllPath);
                kLog.err(C(failMsg), C("ModuleView"));
            }
        }
        else
        {
            kLog.warn(C("DLL·��Ϊ�գ�ע��ʧ��"), C("ModuleView"));
        }
    }

    // ���̿�������
    ImGui::Separator();
    ImGui::Text(C("���̿���"));
    ImGui::BeginGroup();
    if (ImGui::Button(C("��ֹ����"), ImVec2(100, 0)))
    {
        //if (ImGui::GetIO().KeyCtrl)
        //{
            if(kProcess(s_currentPID).Terminate())
            {
                kLog.info(C(("�ɹ���ֹ����: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
                s_isProcessRunning = false;
            }
            else
            {
                kLog.err(C(("��ֹ����ʧ��: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
			}
            

            kLog.info(C(("��ֹ����: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
        //}
        //else
        //{
        //    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), C("�밴סCtrl��ȷ����ֹ"));
        //}
    }

    ImGui::SameLine();
    if (ImGui::Button(C("��ͣ����"), ImVec2(100, 0)))
    {
        kProcess(s_currentPID).Suspend();
        kLog.info(C(("��ͣ����: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
    }

    ImGui::SameLine();
    if (ImGui::Button(C("��������"), ImVec2(100, 0)))
    {
		kProcess(s_currentPID).Resume();
        kLog.info(C(("��������: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
    }

    ImGui::SameLine();
    if (ImGui::Button(C("ˢ�½���"), ImVec2(100, 0)))
    {
        RefreshAllData();
        kLog.info(C("ˢ�½�����Ϣ"), C("ModuleView"));
    }
    ImGui::EndGroup();

    // �ڴ�鿴����
    if (ImGui::CollapsingHeader(C("�ڴ�鿴"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText(C("�ڴ��ַ"), s_memoryAddr, IM_ARRAYSIZE(s_memoryAddr));
        ImGui::SameLine();
        if (ImGui::Button(C("�鿴�ڴ�")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_memoryAddr, "%p", (void**)&addr);
            if (addr != 0)
            {
                s_showMemoryEditor = true;
                kLog.info(C(("�鿴�ڴ��ַ: " + std::string(s_memoryAddr)).c_str()), C("ModuleView"));
            }
            else
            {
                kLog.warn(C("��Ч���ڴ��ַ"), C("ModuleView"));
            }
        }

        // �ڴ������б�
        ImGui::BeginChild("MemoryTableViewCHildWindow", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar); {
            if (ImGui::BeginTable("MemoryTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn(C("����ַ"), ImGuiTableColumnFlags_WidthStretch, 2.0f);  // Ȩ��2
                ImGui::TableSetupColumn(C("��С"), ImGuiTableColumnFlags_WidthStretch, 1.0f);     // Ȩ��1
                ImGui::TableSetupColumn(C("��������"), ImGuiTableColumnFlags_WidthStretch, 2.0f); // Ȩ��2
                ImGui::TableSetupColumn(C("״̬"), ImGuiTableColumnFlags_WidthStretch, 1.0f);     // Ȩ��1
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < s_memoryRegions.size(); ++i)
                {
                    const KswordMemoryRegionInfo& region = s_memoryRegions[i];  // ʵ��ʹ��
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    bool isSelected = (s_showMemoryEditor && s_selectedMemoryRegion.baseAddr == region.baseAddr);
                    if (ImGui::Selectable(FormatAddress(region.baseAddr).c_str(), isSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                    {
                        s_selectedMemoryRegion = region;
                        s_showMemoryEditor = true;
                        sprintf_s(s_memoryAddr, "%p", (void*)region.baseAddr);
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", FormatSize(region.size).c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", region.protection.c_str());

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", region.state.c_str());
                }
                ImGui::EndTable();
			}ImGui::EndChild();  // �����Ӵ���
        }
    }

    // ��ʾ�ڴ�༭����
    if (s_showMemoryEditor)
    {
        DrawMemoryEditor();
    }

    // �߳���Ϣ����
    if (ImGui::CollapsingHeader(C("�߳���Ϣ")))
    {
        ImGui::BeginChild("ThreadTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar); {

            //std::cout << s_threads.size() << std::endl;
            if (ImGui::BeginTable("ThreadTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn(C("�߳�ID"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(C("״̬"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn(C("���ȼ�"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(C("��ڵ�ַ"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (const auto& thread : s_threads)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", thread.id);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", thread.status.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", thread.priority);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", thread.entryAddr.c_str());
                }
                ImGui::EndTable();
            }	ImGui::EndChild();  // �����Ӵ���
        }
    }

    // �ϵ��������
    if (ImGui::CollapsingHeader(C("�ϵ����")))
    {
        ImGui::InputText(C("�ϵ��ַ"), s_breakpointAddr, IM_ARRAYSIZE(s_breakpointAddr));
        ImGui::SameLine();
        if (ImGui::Button(C("��Ӷϵ�")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_breakpointAddr, "%p", (void**)&addr);
            if (addr != 0)
            {
                if (AddBreakpoint(addr))
                {
                    kLog.info(C(("��Ӷϵ�: " + std::string(s_breakpointAddr)).c_str()), C("ModuleView"));
                }
                else
                {
                    kLog.err(C(("��Ӷϵ�ʧ��: " + std::string(s_breakpointAddr)).c_str()), C("ModuleView"));
                }
            }
            else
            {
                kLog.warn(C("��Ч�Ķϵ��ַ"), C("ModuleView"));
            }
        }
        ImGui::BeginChild("BreakPointTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar); {
            if (ImGui::BeginTable("BreakPointTable2", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn(C("��ַ"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(C("ģ��"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(C("���д���"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                std::cout<< s_breakpoints.size() << std::endl;
                for (size_t i = 0; i < s_breakpoints.size(); ++i)
                {
                    kLog.dbg(C("������һ���ϵ�"), C("�ϵ����"));
                    auto& bp = s_breakpoints[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", C(FormatAddress(bp.address).c_str()));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", C(bp.module.c_str()));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", bp.hitCount);
                    ImGui::TableSetColumnIndex(3);
                    std::string toggleBtnId = (bp.enabled ? "����##bp" : "����##bp") + std::to_string(i);
                    if (ImGui::Button(C(toggleBtnId.c_str()), ImVec2(40, 0)))
                    {
                        ToggleBreakpoint(bp.address);
                    }
                    ImGui::SameLine();
                    std::string delBtnId = "ɾ��##bp" + std::to_string(i);
                    if (ImGui::Button(C(delBtnId.c_str()), ImVec2(40, 0)))
                    {
                        RemoveBreakpoint(bp.address);
                        kLog.info(C(("ɾ���ϵ�: " + FormatAddress(bp.address)).c_str()), C("ModuleView"));
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
    }

    ImGui::EndTabItem();
}

// ����ˢ��ʵ��
static void RefreshAllData()
{
    // �����������
    s_modules.clear();
    s_threads.clear();
    s_memoryRegions.clear();

    // ��Ŀ����̣���Ҫ�㹻Ȩ�ޣ�
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        s_currentPID
    );

    if (hProcess == NULL)
    {
        DWORD error = GetLastError();
        kLog.err(C("��Ŀ�����ʧ�ܣ��޷�ˢ�����ݣ�������: " + std::to_string(error)), C("DataRefresh"));
        return;
    }

    // 1. ˢ��ģ���б���ʵö��Ŀ�����ģ�飩
    HMODULE* moduleList = nullptr;
    DWORD bytesNeeded = 0;

    // ��һ�ε��û�ȡ���軺������С
    if (EnumProcessModulesEx(hProcess, moduleList, 0, &bytesNeeded, LIST_MODULES_ALL))
    {
        // ���仺����
        moduleList = (HMODULE*)LocalAlloc(LPTR, bytesNeeded);
        if (moduleList)
        {
            // �ڶ��ε��û�ȡģ���б�
            if (EnumProcessModulesEx(hProcess, moduleList, bytesNeeded, &bytesNeeded, LIST_MODULES_ALL))
            {
                DWORD moduleCount = bytesNeeded / sizeof(HMODULE);
                for (DWORD i = 0; i < moduleCount; ++i)
                {
                    KswordModuleInfo module;
                    char modulePath[MAX_PATH] = { 0 };

                    // ��ȡģ��·��
                    if (GetModuleFileNameExA(hProcess, moduleList[i], modulePath, MAX_PATH))
                    {
                        module.path = modulePath;

                        // ��ȡģ�����ַ�ʹ�С
                        MODULEINFO modInfo = { 0 };
                        if (GetModuleInformation(hProcess, moduleList[i], &modInfo, sizeof(MODULEINFO)))
                        {
                            module.baseAddr = (uintptr_t)modInfo.lpBaseOfDll;
                            module.size = modInfo.SizeOfImage;
                        }

                        // ��ȡģ��汾��Ϣ
                        DWORD verHandle = 0;
                        DWORD verSize = GetFileVersionInfoSizeA(modulePath, &verHandle);
                        if (verSize > 0)
                        {
                            std::vector<BYTE> verData(verSize);
                            if (GetFileVersionInfoA(modulePath, verHandle, verSize, verData.data()))
                            {
                                VS_FIXEDFILEINFO* verInfo = nullptr;
                                UINT infoSize = 0;
                                if (VerQueryValueA(verData.data(), "\\", (void**)&verInfo, &infoSize) && infoSize > 0)
                                {
                                    module.version = std::to_string((verInfo->dwFileVersionMS >> 16) & 0xFFFF) + "."
                                        + std::to_string(verInfo->dwFileVersionMS & 0xFFFF) + "."
                                        + std::to_string((verInfo->dwFileVersionLS >> 16) & 0xFFFF) + "."
                                        + std::to_string(verInfo->dwFileVersionLS & 0xFFFF);
                                }
                            }
                        }
                        if (module.version.empty())
                            module.version = C("δ֪�汾");

                        // �򻯴�������ϵͳģ����ǩ����ʵ�������ǩ����֤API��
                        module.isSigned = (module.path.find("Windows\\system32") != std::string::npos);
                        module.is64Bit = s_is64BitProcess;  // �����λ��һ��

                        s_modules.push_back(module);
                    }
                }
            }
            LocalFree(moduleList);
        }
    }
    else
    {
        DWORD error = GetLastError();
        kLog.warn(C("ö��ģ��ʧ�ܣ�������: " + std::to_string(error)), C("DataRefresh"));
    }

    // 2. ˢ���߳��б���ʵö��Ŀ������̣߳�
    HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hThreadSnap != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te32 = { 0 };
        te32.dwSize = sizeof(THREADENTRY32);

        if (Thread32First(hThreadSnap, &te32))
        {
            do
            {

                // ֻ����Ŀ����̵��߳�
                if (te32.th32OwnerProcessID == s_currentPID)
                {
                    KswordThreadInfo thread;
                    thread.id = te32.th32ThreadID;
				kLog.dbg(C("�߳�ID: " + std::to_string(te32.th32ThreadID)), C("DataRefresh"));
                    // ���̻߳�ȡ��ϸ��Ϣ
                    HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread.id);
                    if (hThread != NULL)
                    {
                        // ��ȡ�߳����ȼ�
                        thread.priority = GetThreadPriority(hThread);

                        // �ж��߳�״̬���򻯰棺ͨ���˳����ж��Ƿ������У�
                        DWORD exitCode = 0;
                        GetExitCodeThread(hThread, &exitCode);
                        thread.status = (exitCode == STILL_ACTIVE) ? C("������") : C("���˳�");

                        // ��ȡ�߳���ڵ�ַ�����ض�Ȩ�ޣ�����ʧ�ܣ�
                        uintptr_t entryAddr = 0;
#ifdef _WIN64
                        if (s_is64BitProcess)
                            GetThreadContext(hThread, (LPCONTEXT)&entryAddr);  // �򻯴���ʵ������ȷ��ȡ������
#else
                        if (!s_is64BitProcess)
                            GetThreadContext(hThread, (LPCONTEXT)&entryAddr);
#endif
                        thread.entryAddr = FormatAddress(entryAddr);

                        CloseHandle(hThread);
                    }
                    else
                    {
                        thread.status = C("Ȩ�޲���");
                        thread.priority = 0;
                        thread.entryAddr = C("δ֪");
                    }

                    s_threads.push_back(thread);
                }
            } while (Thread32Next(hThreadSnap, &te32));
        }
        CloseHandle(hThreadSnap);
    }
    else
    {
        DWORD error = GetLastError();
        kLog.warn(C("ö���߳�ʧ�ܣ�������: " + std::to_string(error)), C("DataRefresh"));
    }

    // 3. ˢ���ڴ�������ʵ��ѯĿ������ڴ棩
    MEMORY_BASIC_INFORMATION mbi = { 0 };
    uintptr_t addr = 0;

    while (VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        KswordMemoryRegionInfo region;
        region.baseAddr = (uintptr_t)mbi.BaseAddress;
        region.size = mbi.RegionSize;

        // ת���ڴ���������
        switch (mbi.Type)
        {
        case MEM_IMAGE: region.type = C("ӳ���ļ�"); break;
        case MEM_MAPPED: region.type = C("ӳ���ڴ�"); break;
        case MEM_PRIVATE: region.type = C("˽���ڴ�"); break;
        default: region.type = C("δ֪����");
        }

        // ת��������������
        std::string protectStr;
        if (mbi.Protect & PAGE_EXECUTE) protectStr += C("ִ��");
        if (mbi.Protect & PAGE_READONLY) protectStr += C("ֻ��");
        if (mbi.Protect & PAGE_READWRITE) protectStr += C("��д");
        if (mbi.Protect & PAGE_WRITECOPY) protectStr += C("дʱ����");
        if (mbi.Protect & PAGE_EXECUTE_READ) protectStr += C("ִ��+��");
        if (mbi.Protect & PAGE_EXECUTE_READWRITE) protectStr += C("ִ��+��д");
        if (mbi.Protect & PAGE_EXECUTE_WRITECOPY) protectStr += C("ִ��+дʱ����");
        if (protectStr.empty()) protectStr = C("δ֪����");
        region.protection = protectStr;

        // ת���ڴ�״̬����
        switch (mbi.State)
        {
        case MEM_COMMIT: region.state = C("���ύ"); break;
        case MEM_RESERVE: region.state = C("�ѱ���"); break;
        case MEM_FREE: region.state = C("����"); break;
        default: region.state = C("δ֪״̬");
        }

        s_memoryRegions.push_back(region);
        addr += mbi.RegionSize;  // �ƶ�����һ���ڴ�����

        // ��ֹ����ѭ��������64λ���ַ�ռ䣩
        if (addr < (uintptr_t)mbi.BaseAddress)
            break;
    }

    CloseHandle(hProcess);
    kLog.info(C("��ˢ��Ŀ��������ݣ�PID: " + std::to_string(s_currentPID)), C("DataRefresh"));
}
static void FilterAndSortModules(std::vector<KswordModuleInfo>& filteredModules)
{
    // ɸѡ�߼�
    std::string filterLower = s_filterText;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    if (!filterLower.empty())
    {
        auto it = std::remove_if(filteredModules.begin(), filteredModules.end(),
            [&](const KswordModuleInfo& mod) {  // ʹ��ʵ�ʽṹ��
                std::string pathLower = mod.path;
                std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
                return pathLower.find(filterLower) == std::string::npos;
            });
        filteredModules.erase(it, filteredModules.end());
    }

    // �����߼�(ʹ��lambda���ʽʵ��)
    if (s_sortColumn >= 0)
    {
        switch (s_sortColumn)
        {
        case 0: // ģ��·��
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.path < b.path : a.path > b.path;
                });
            break;
        case 1: // ����ַ
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.baseAddr < b.baseAddr : a.baseAddr > b.baseAddr;
                });
            break;
        case 2: // ��С
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.size < b.size : a.size > b.size;
                });
            break;
        case 3: // �汾
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.version < b.version : a.version > b.version;
                });
            break;
        }
    }
}

static void DrawModuleDetails()
{
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(C("ģ������"), &s_showModuleDetails))
    {
        ImGui::Text(C("ģ��·��: %s"), s_selectedModule.path.c_str());
        ImGui::Text(C("����ַ: %s"), FormatAddress(s_selectedModule.baseAddr).c_str());
        ImGui::Text(C("��С: %s"), FormatSize(s_selectedModule.size).c_str());
        ImGui::Text(C("�汾: %s"), s_selectedModule.version.c_str());
        ImGui::Text(C("����ʱ��: %s"), s_selectedModule.loadTime.c_str());
        ImGui::Text(C("У���: 0x%X"), s_selectedModule.checksum);
        ImGui::Text(C("ǩ��״̬: %s"), s_selectedModule.isSigned ? C("��ǩ��") : C("δǩ��"));
        ImGui::Text(C("λ��: %s"), s_selectedModule.is64Bit ? C("64λ") : C("32λ"));

        ImGui::Separator();

        if (ImGui::Button(C("�鿴������")))
        {
            kLog.info(C(("�鿴ģ�鵼����: " + s_selectedModule.path).c_str()), C("ModuleView"));
        }
        ImGui::SameLine();
        if (ImGui::Button(C("�鿴�����")))
        {
            kLog.info(C(("�鿴ģ�鵼���: " + s_selectedModule.path).c_str()), C("ModuleView"));
        }
        ImGui::SameLine();
        if (ImGui::Button(C("���ڴ��в鿴")))
        {
            sprintf_s(s_memoryAddr, "%p", (void*)s_selectedModule.baseAddr);
            s_showMemoryEditor = true;
        }
    }
    ImGui::End();
}
static bool first_show_memory_window = 1;
static void DrawMemoryEditor()
{
    if(first_show_memory_window)
    {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(50, 50));
    first_show_memory_window = 0;
	}
    if (ImGui::Begin(C("�ڴ�༭��"), &s_showMemoryEditor, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text(C("��ַ: %s"), s_memoryAddr);
        ImGui::SameLine();
        if (ImGui::Button(C("ˢ��")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_memoryAddr, "%p", (void**)&addr);
            if (addr == 0)
            {
                kLog.warn(C("��Ч���ڴ��ַ���޷�ˢ��"), C("MemoryEditor"));
                return;
            }

            // ��ȡĿ������ڴ�
            HANDLE hProcess = OpenProcess(PROCESS_VM_READ, FALSE, s_currentPID);
            if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
            {
                kLog.err(C(("�򿪽���ʧ�ܣ��޷���ȡ�ڴ�: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
                return;
            }

            // ��ȡ0x100�ֽ��ڴ�����
            static std::vector<uint8_t> memoryData(0x100, 0);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(hProcess, (LPCVOID)addr, memoryData.data(), memoryData.size(), &bytesRead))
            {
                kLog.info(C(("�ɹ���ȡ�ڴ�: " + FormatAddress(addr) + " ����: " + std::to_string(bytesRead)).c_str()), C("MemoryEditor"));
            }
            else
            {
                kLog.err(C(("��ȡ�ڴ�ʧ��: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
            }
            CloseHandle(hProcess);
        }

        ImGui::SameLine();
        static char modifyValue[32] = "";
        ImGui::InputTextWithHint("##modifyValue", C("������ֵ(ʮ������)"), modifyValue, IM_ARRAYSIZE(modifyValue));
        ImGui::SameLine();
        if (ImGui::Button(C("�޸�")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_memoryAddr, "%p", (void**)&addr);
            if (addr == 0)
            {
                kLog.warn(C("��Ч���ڴ��ַ���޷��޸�"), C("MemoryEditor"));
                return;
            }

            // ���������ʮ������ֵ
            uint8_t value = 0;
            if (sscanf_s(modifyValue, "%hhX", &value) != 1)
            {
                kLog.warn(C("��Ч��ʮ������ֵ"), C("MemoryEditor"));
                return;
            }

            // д��Ŀ������ڴ�
            HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, s_currentPID);
            if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
            {
                kLog.err(C(("�򿪽���ʧ�ܣ��޷�д���ڴ�: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
                return;
            }

            SIZE_T bytesWritten = 0;
            if (WriteProcessMemory(hProcess, (LPVOID)addr, &value, sizeof(value), &bytesWritten) && bytesWritten == sizeof(value))
            {
                kLog.info(C(("�ɹ��޸��ڴ�: " + FormatAddress(addr) + " ��ֵ: 0x" + std::to_string(value)).c_str()), C("MemoryEditor"));
            }
            else
            {
                kLog.err(C(("д���ڴ�ʧ��: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
            }
            CloseHandle(hProcess);
        }

        ImGui::Separator();

        // ��ʾ�ڴ����ݣ�ʹ��ʵ�ʶ�ȡ�����ݣ�
        ImGui::BeginChild(C("MemoryView"), ImVec2(0, 0), true);
        ImGuiListClipper clipper;
        static std::vector<uint8_t> memoryData(0x100, 0); // �洢ʵ�ʶ�ȡ���ڴ�����
        clipper.Begin(memoryData.size() / 16);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                uintptr_t addr = 0;
                sscanf_s(s_memoryAddr, "%p", (void**)&addr);
                addr += i * 16;

                ImGui::Text("%s: ", FormatAddress(addr).c_str());
                ImGui::SameLine();

                // ʮ��������ʾ
                for (int j = 0; j < 16; j++)
                {
                    int idx = i * 16 + j;
                    if (idx < memoryData.size())
                        ImGui::Text("%02X ", memoryData[idx]);
                    else
                        ImGui::Text("   ");
                    if (j == 7) ImGui::SameLine();
                }

                ImGui::SameLine();

                // ASCII��ʾ
                std::string ascii;
                for (int j = 0; j < 16; j++)
                {
                    int idx = i * 16 + j;
                    if (idx < memoryData.size())
                    {
                        uint8_t c = memoryData[idx];
                        ascii += (c >= 32 && c <= 126) ? (char)c : '.';
                    }
                    else
                    {
                        ascii += ' ';
                    }
                }
                ImGui::Text("%s", ascii.c_str());
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
static std::string FormatSize(size_t size)
{
    if (size >= 1024 * 1024 * 1024)
        return std::to_string((double)size / (1024 * 1024 * 1024)) + " GB";
    else if (size >= 1024 * 1024)
        return std::to_string((double)size / (1024 * 1024)) + " MB";
    else if (size >= 1024)
        return std::to_string((double)size / 1024) + " KB";
    else
        return std::to_string(size) + " B";
}
// ��ʽ����ַ��0xXXXXXXXX �� 0xXXXXXXXXXXXXXXXX ��ʽ��
static std::string FormatAddress(uintptr_t addr)
{
    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase
        << std::setw(s_is64BitProcess ? 16 : 8)  // 64λ��ַ16��ʮ������λ��32λ8��
        << std::setfill('0') << addr;
    return ss.str();
}

// ��Ӷϵ㣨�����ԱȨ�ޣ�ʵ���޸�Ŀ������ڴ棩
static bool AddBreakpoint(uintptr_t address)
{
    // ���ϵ��Ƿ��Ѵ���
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
        [address](const KswordBreakpointInfo& bp) { return bp.address == address; });
    if (it != s_breakpoints.end())
    {
        kLog.warn(C("�ϵ��Ѵ���: " + FormatAddress(address)), C("Breakpoint"));
        return false;
    }

    // ��Ŀ����̻�ȡ�ڴ����Ȩ��
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, s_currentPID);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        kLog.err(C("�޷��򿪽�������Ӷϵ㣬����: " + std::to_string(GetLastError())), C("Breakpoint"));
        return false;
    }

    // ��ȡԭʼָ���ֽڣ��ϵ���滻ΪINT3ָ��0xCC��
    BYTE originalByte;
    SIZE_T bytesRead;
    if (!ReadProcessMemory(hProcess, (LPCVOID)address, &originalByte, 1, &bytesRead) || bytesRead != 1)
    {
        kLog.err(C("��ȡ�ڴ�ʧ�ܣ��޷���Ӷϵ�: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    // д��INT3ָ�����öϵ�
    BYTE int3 = 0xCC;
    SIZE_T bytesWritten;
    if (!WriteProcessMemory(hProcess, (LPVOID)address, &int3, 1, &bytesWritten) || bytesWritten != 1)
    {
        kLog.err(C("д���ڴ�ʧ�ܣ��޷���Ӷϵ�: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    // ���ҵ�ַ����ģ��
    std::string moduleName = "δ֪ģ��";
    for (const auto& mod : s_modules)
    {
        if (address >= mod.baseAddr && address < mod.baseAddr + mod.size)
        {
            size_t lastSlash = mod.path.find_last_of("\\/");
            moduleName = (lastSlash != std::string::npos) ? mod.path.substr(lastSlash + 1) : mod.path;
            break;
        }
    }

    // ����ϵ���Ϣ������ԭʼ�ֽ����ڻָ���
    s_breakpoints.push_back({
        address,
        moduleName,
        "����ϵ� (INT3)",
        true,
        0,
        originalByte  // ����������ԭʼָ���ֽ�
        });

    kLog.info(C("����Ӷϵ�: " + FormatAddress(address)), C("Breakpoint"));
    CloseHandle(hProcess);
    return true;
}

// �Ƴ��ϵ㣨�ָ�ԭʼָ�
static bool RemoveBreakpoint(uintptr_t address)
{
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
        [address](const KswordBreakpointInfo& bp) { return bp.address == address; });
    if (it == s_breakpoints.end())
    {
        kLog.warn(C("δ�ҵ��ϵ�: " + FormatAddress(address)), C("Breakpoint"));
        return false;
    }

    // �򿪽��ָ̻�ԭʼָ��
    HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, s_currentPID);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        kLog.err(C("�޷��򿪽������Ƴ��ϵ㣬����: " + std::to_string(GetLastError())), C("Breakpoint"));
        return false;
    }

    // �ָ�ԭʼ�ֽ�
    SIZE_T bytesWritten;
    if (!WriteProcessMemory(hProcess, (LPVOID)address, &it->originalByte, 1, &bytesWritten) || bytesWritten != 1)
    {
        kLog.err(C("�ָ��ڴ�ʧ�ܣ��޷��Ƴ��ϵ�: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    s_breakpoints.erase(it);
    kLog.info(C("���Ƴ��ϵ�: " + FormatAddress(address)), C("Breakpoint"));
    CloseHandle(hProcess);
    return true;
}

// �л��ϵ�����/����״̬����̬�޸��ڴ棩
static bool ToggleBreakpoint(uintptr_t address)
{
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
        [address](const KswordBreakpointInfo& bp) { return bp.address == address; });
    if (it == s_breakpoints.end())
    {
        kLog.warn(C("δ�ҵ��ϵ�: " + FormatAddress(address)), C("Breakpoint"));
        return false;
    }

    // �򿪽���׼���޸��ڴ�
    HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, s_currentPID);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        kLog.err(C("�޷��򿪽������л��ϵ㣬����: " + std::to_string(GetLastError())), C("Breakpoint"));
        return false;
    }

    bool newState = !it->enabled;
    BYTE targetByte = newState ? 0xCC : it->originalByte;  // ������д��INT3��������ָ�ԭʼ�ֽ�

    SIZE_T bytesWritten;
    if (!WriteProcessMemory(hProcess, (LPVOID)address, &targetByte, 1, &bytesWritten) || bytesWritten != 1)
    {
        kLog.err(C("�л��ϵ�״̬ʧ��: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    it->enabled = newState;
    kLog.info(C((newState ? "�����öϵ�: " : "�ѽ��öϵ�: ") + FormatAddress(address)), C("Breakpoint"));
    CloseHandle(hProcess);
    return true;
}