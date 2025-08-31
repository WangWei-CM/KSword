#include "../../KswordTotalHead.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <windows.h>
#include <winver.h>
#pragma comment(lib, "version.lib")
#include <wintrust.h>
#include <softpub.h>
#pragma comment(lib, "wintrust.lib")
#include "Memory.h"

// �ڴ�������ݴ洢
namespace KswordMemoryDebugger {
    static DWORD s_currentPID = 0;
    static std::string s_processPath;
    static std::string s_processName;
    static bool s_is64BitProcess = false;
    static bool s_isProcessRunning = false;
    static bool s_autoRefresh = false;
    static float s_refreshInterval = 1.0f;
    static float s_lastRefreshTime = 0.0f;

    // UI״̬�洢
    static char s_memoryAddr[32] = "0x00400000";
    static char s_breakpointAddr[32] = "0x00401000";
    static bool s_showMemoryEditor = false;
    static KswordMemoryRegionInfo s_selectedMemoryRegion;

    // ���ݴ洢
    static std::vector<KswordThreadInfo> s_threads;
    static std::vector<KswordMemoryRegionInfo> s_memoryRegions;
    static std::vector<KswordBreakpointInfo> s_breakpoints;
}
using namespace KswordMemoryDebugger;

// ����������ʵ�ּ��·���
static void RefreshAllData();
static void DrawMemoryEditor();
static std::string FormatSize(size_t size);
static std::string FormatAddress(uintptr_t addr);
static bool AddBreakpoint(uintptr_t address);
static bool RemoveBreakpoint(uintptr_t address);
static bool ToggleBreakpoint(uintptr_t address);

void KswordMemoryMain()
{
    // ����PID������
    ImGui::Text(C("���������PID:"));
    static char pidBuffer[32] = "";
    ImGui::InputText("##pidInputField", pidBuffer, IM_ARRAYSIZE(pidBuffer), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(C("ȷ�ϲ�ˢ��"))) {
        if (strlen(pidBuffer) > 0) {
            DWORD targetPID = static_cast<DWORD>(atoi(pidBuffer));
            if (targetPID > 0) {
                s_currentPID = targetPID;
                RefreshAllData();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(C("���"))) {
        memset(pidBuffer, 0, sizeof(pidBuffer));
    }
    ImGui::Separator();

    // ������Ϣ��ʾ
    ImGui::Text(C("������Ϣ"));
    ImGui::Text(C("����ID: %d"), s_currentPID);
    ImGui::Text(C("������: %s"), s_processName.c_str());
    ImGui::Text(C("����·��: %s"), s_processPath.c_str());
    ImGui::Text(C("λ��: %s"), s_is64BitProcess ? C("64λ") : C("32λ"));
    ImGui::Text(C("״̬: %s"), s_isProcessRunning ? C("������") : C("��ֹͣ"));
    ImGui::Separator();

    // �ڴ�����
    float windowHeight = ImGui::GetContentRegionAvail().y;
    float halfHeight = windowHeight * 0.5f - ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::CollapsingHeader(C("�ڴ�����"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("MemoryTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ImGui::BeginTable("MemoryTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(C("��ַ"));
            ImGui::TableSetupColumn(C("��С"));
            ImGui::TableSetupColumn(C("����"));
            ImGui::TableSetupColumn(C("״̬"));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < s_memoryRegions.size(); ++i) {
                const auto& region = s_memoryRegions[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(C("%s"), FormatAddress(region.baseAddr).c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text(C("%s"), FormatSize(region.size).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text(C("%s"), region.protection.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::Text(C("%s"), region.state.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // �ϵ����
    if (ImGui::CollapsingHeader(C("�ϵ����"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(C("�ϵ��ַ"), s_breakpointAddr, IM_ARRAYSIZE(s_breakpointAddr));
        ImGui::SameLine();
        if (ImGui::Button(C("��Ӷϵ�"))) {
            uintptr_t addr = 0;
            sscanf_s(s_breakpointAddr, "%p", (void**)&addr);
            if (addr != 0) AddBreakpoint(addr);
        }
        ImGui::BeginChild("BreakPointTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ImGui::BeginTable("BreakPointTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(C("��ַ"));
            ImGui::TableSetupColumn(C("ģ��"));
            ImGui::TableSetupColumn(C("���д���"));
            ImGui::TableSetupColumn(C("״̬"));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < s_breakpoints.size(); ++i) {
                auto& bp = s_breakpoints[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(C("%s"), FormatAddress(bp.address).c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text(C("%s"), bp.module.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text(C("%d"), bp.hitCount);
                ImGui::TableSetColumnIndex(3);
                std::string btnId = (bp.enabled ? "����##bp" : "����##bp") + std::to_string(i);
                if (ImGui::Button(C(btnId.c_str()), ImVec2(40, 0))) ToggleBreakpoint(bp.address);
                ImGui::SameLine();
                std::string delBtnId = "ɾ��##bp" + std::to_string(i);
                if (ImGui::Button(C(delBtnId.c_str()), ImVec2(40, 0))) RemoveBreakpoint(bp.address);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // �߳���Ϣ
    if (ImGui::CollapsingHeader(C("�߳���Ϣ"))) {
        ImGui::BeginChild("ThreadTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ImGui::BeginTable("ThreadTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(C("�߳�ID"));
            ImGui::TableSetupColumn(C("״̬"));
            ImGui::TableSetupColumn(C("���ȼ�"));
            ImGui::TableSetupColumn(C("��ڵ�ַ"));
            ImGui::TableSetupColumn(C("����ģ��"));
            ImGui::TableHeadersRow();
            for (const auto& thread : s_threads) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(C("%d"), thread.id);
                ImGui::TableSetColumnIndex(1); ImGui::Text(C("%s"), thread.status.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text(C("%d"), thread.priority);
                ImGui::TableSetColumnIndex(3); ImGui::Text(C("%s"), thread.entryAddr.c_str());
                ImGui::TableSetColumnIndex(4); ImGui::Text(C("%s"), thread.moduleName.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // �ڴ�༭��
    if (s_showMemoryEditor) {
        DrawMemoryEditor();
    }

    ImGui::EndTabItem();
    return;
}

// ��ʽ���ڴ��С���� 1.23 MB��
static std::string FormatSize(size_t size) {
    char buf[32];
    if (size < 1024)
        sprintf_s(buf, "%zu B", size);
    else if (size < 1024 * 1024)
        sprintf_s(buf, "%.2f KB", size / 1024.0);
    else if (size < 1024 * 1024 * 1024)
        sprintf_s(buf, "%.2f MB", size / 1024.0 / 1024.0);
    else
        sprintf_s(buf, "%.2f GB", size / 1024.0 / 1024.0 / 1024.0);
    return buf;
}

// ��ʽ����ַ���� 0x00400000��
static std::string FormatAddress(uintptr_t addr) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(sizeof(uintptr_t)*2) << std::setfill('0') << addr;
    return ss.str();
}

// �ڴ�����ˢ�£�ö��Ŀ����������ڴ�����
static void RefreshAllData() {
    // �����������
    s_threads.clear();
    s_memoryRegions.clear();

    // ��Ŀ����̣���Ҫ�㹻Ȩ�ޣ�
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, s_currentPID);
    // ����APIʧ�ܣ�����ִ��

    // ˢ���߳��б���ϸ״̬��
    HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hThreadSnap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te32 = { 0 };
        te32.dwSize = sizeof(THREADENTRY32);
        if (Thread32First(hThreadSnap, &te32)) {
            do {
                if (te32.th32OwnerProcessID == s_currentPID) {
                    KswordThreadInfo thread;
                    thread.id = te32.th32ThreadID;
                    HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT, FALSE, te32.th32ThreadID);
                    if (hThread) {
                        thread.status = "δ֪"; // Or use other logic to determine status
                        thread.priority = te32.tpBasePri; // Use tpBasePri for priority
                        thread.entryAddr = "";
                        thread.moduleName = "";
                        CloseHandle(hThread);
                    } else {
                        thread.status = "δ֪";
                        thread.priority = 0;
                        thread.entryAddr = "";
                        thread.moduleName = "";
                    }
                    s_threads.push_back(thread);
                }
            } while (Thread32Next(hThreadSnap, &te32));
        }
        CloseHandle(hThreadSnap);
    }

    // ˢ���ڴ�����
    if (hProcess) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t addr = 0;
        while (VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            KswordMemoryRegionInfo info;
            info.baseAddr = (uintptr_t)mbi.BaseAddress;
            info.size = mbi.RegionSize;
            info.type = (mbi.Type == MEM_IMAGE) ? "Image" : (mbi.Type == MEM_MAPPED) ? "Mapped" : (mbi.Type == MEM_PRIVATE) ? "Private" : "Unknown";
            info.protection = (mbi.Protect & PAGE_READONLY) ? "R" : (mbi.Protect & PAGE_READWRITE) ? "RW" : (mbi.Protect & PAGE_EXECUTE_READ) ? "RX" : (mbi.Protect & PAGE_EXECUTE_READWRITE) ? "RWX" : "?";
            info.state = (mbi.State == MEM_COMMIT) ? "Commit" : (mbi.State == MEM_RESERVE) ? "Reserve" : (mbi.State == MEM_FREE) ? "Free" : "Unknown";
            s_memoryRegions.push_back(info);
            addr += mbi.RegionSize;
            if (addr <= (uintptr_t)mbi.BaseAddress) break;
        }
        CloseHandle(hProcess);
    }
}

// ��Ӷϵ㣨д��0xCC������ԭ�ֽڣ�
static bool AddBreakpoint(uintptr_t address) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, s_currentPID);
    if (!hProcess) return false;
    for (const auto& bp : s_breakpoints) {
        if (bp.address == address) { CloseHandle(hProcess); return false; }
    }
    uint8_t orig = 0;
    SIZE_T read = 0;
    ReadProcessMemory(hProcess, (LPCVOID)address, &orig, 1, &read);
    uint8_t cc = 0xCC;
    SIZE_T written = 0;
    WriteProcessMemory(hProcess, (LPVOID)address, &cc, 1, &written);
    KswordBreakpointInfo info;
    info.address = address;
    info.module = "";
    info.symbol = "";
    info.enabled = true;
    info.hitCount = 0;
    info.originalByte = orig;
    s_breakpoints.push_back(info);
    CloseHandle(hProcess);
    return true;
}

// �Ƴ��ϵ㣨�ָ�ԭ�ֽڣ�
static bool RemoveBreakpoint(uintptr_t address) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, s_currentPID);
    if (!hProcess) return false;
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(), [&](const KswordBreakpointInfo& bp) {
        return bp.address == address;
    });
    if (it != s_breakpoints.end()) {
        uint8_t orig = it->originalByte;
        SIZE_T written = 0;
        WriteProcessMemory(hProcess, (LPVOID)address, &orig, 1, &written);
        s_breakpoints.erase(it);
        CloseHandle(hProcess);
        return true;
    }
    CloseHandle(hProcess);
    return false;
}

// �л��ϵ�״̬������/���ã�д��/�ָ��ֽڣ�
static bool ToggleBreakpoint(uintptr_t address) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, s_currentPID);
    if (!hProcess) return false;
    for (auto& bp : s_breakpoints) {
        if (bp.address == address) {
            SIZE_T written = 0;
            if (bp.enabled) {
                // ���ã��ָ�ԭ�ֽ�
                WriteProcessMemory(hProcess, (LPVOID)address, &bp.originalByte, 1, &written);
                bp.enabled = false;
            } else {
                // ���ã�д��0xCC
                uint8_t cc = 0xCC;
                WriteProcessMemory(hProcess, (LPVOID)address, &cc, 1, &written);
                bp.enabled = true;
            }
            CloseHandle(hProcess);
            return true;
        }
    }
    CloseHandle(hProcess);
    return false;
}

// �ڴ�༭����ʮ�����Ʋ鿴����֧�ֲ鿴�ͱ༭ָ����ַ���ڴ棩
static void DrawMemoryEditor() {
    static std::vector<uint8_t> memBuffer;
    static uintptr_t lastAddr = 0;
    ImGui::InputText("��ַ", s_memoryAddr, IM_ARRAYSIZE(s_memoryAddr));
    uintptr_t addr = 0;
    sscanf_s(s_memoryAddr, "%p", (void**)&addr);
    if (addr != 0 && addr != lastAddr) {
        memBuffer.clear();
        HANDLE hProcess = OpenProcess(PROCESS_VM_READ, FALSE, s_currentPID);
        if (hProcess) {
            memBuffer.resize(256);
            SIZE_T read = 0;
            ReadProcessMemory(hProcess, (LPCVOID)addr, memBuffer.data(), memBuffer.size(), &read);
            memBuffer.resize(read);
            CloseHandle(hProcess);
        }
        lastAddr = addr;
    }
    if (!memBuffer.empty()) {
        ImGui::Text("�ڴ�����:");
        for (size_t i = 0; i < memBuffer.size(); i += 16) {
            std::stringstream ss;
            ss << FormatAddress(addr + i) << ": ";
            for (size_t j = 0; j < 16 && i + j < memBuffer.size(); ++j) {
                ss << std::setw(2) << std::setfill('0') << std::hex << (int)memBuffer[i + j] << " ";
            }
            ImGui::Text("%s", ss.str().c_str());
        }
    }
}