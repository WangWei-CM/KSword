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

// 内存相关数据存储
namespace KswordMemoryDebugger {
    static DWORD s_currentPID = 0;
    static std::string s_processPath;
    static std::string s_processName;
    static bool s_is64BitProcess = false;
    static bool s_isProcessRunning = false;
    static bool s_autoRefresh = false;
    static float s_refreshInterval = 1.0f;
    static float s_lastRefreshTime = 0.0f;

    // UI状态存储
    static char s_memoryAddr[32] = "0x00400000";
    static char s_breakpointAddr[32] = "0x00401000";
    static bool s_showMemoryEditor = false;
    static KswordMemoryRegionInfo s_selectedMemoryRegion;

    // 数据存储
    static std::vector<KswordThreadInfo> s_threads;
    static std::vector<KswordMemoryRegionInfo> s_memoryRegions;
    static std::vector<KswordBreakpointInfo> s_breakpoints;
}
using namespace KswordMemoryDebugger;

// 函数声明（实现见下方）
static void RefreshAllData();
static void DrawMemoryEditor();
static std::string FormatSize(size_t size);
static std::string FormatAddress(uintptr_t addr);
static bool AddBreakpoint(uintptr_t address);
static bool RemoveBreakpoint(uintptr_t address);
static bool ToggleBreakpoint(uintptr_t address);

void KswordMemoryMain()
{
    // 进程PID输入区
    ImGui::Text(C("请输入进程PID:"));
    static char pidBuffer[32] = "";
    ImGui::InputText("##pidInputField", pidBuffer, IM_ARRAYSIZE(pidBuffer), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(C("确认并刷新"))) {
        if (strlen(pidBuffer) > 0) {
            DWORD targetPID = static_cast<DWORD>(atoi(pidBuffer));
            if (targetPID > 0) {
                s_currentPID = targetPID;
                RefreshAllData();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(C("清空"))) {
        memset(pidBuffer, 0, sizeof(pidBuffer));
    }
    ImGui::Separator();

    // 进程信息显示
    ImGui::Text(C("进程信息"));
    ImGui::Text(C("进程ID: %d"), s_currentPID);
    ImGui::Text(C("进程名: %s"), s_processName.c_str());
    ImGui::Text(C("进程路径: %s"), s_processPath.c_str());
    ImGui::Text(C("位数: %s"), s_is64BitProcess ? C("64位") : C("32位"));
    ImGui::Text(C("状态: %s"), s_isProcessRunning ? C("运行中") : C("已停止"));
    ImGui::Separator();

    // 内存区域
    float windowHeight = ImGui::GetContentRegionAvail().y;
    float halfHeight = windowHeight * 0.5f - ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::CollapsingHeader(C("内存区域"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("MemoryTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ImGui::BeginTable("MemoryTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(C("基址"));
            ImGui::TableSetupColumn(C("大小"));
            ImGui::TableSetupColumn(C("保护"));
            ImGui::TableSetupColumn(C("状态"));
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

    // 断点管理
    if (ImGui::CollapsingHeader(C("断点管理"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(C("断点地址"), s_breakpointAddr, IM_ARRAYSIZE(s_breakpointAddr));
        ImGui::SameLine();
        if (ImGui::Button(C("添加断点"))) {
            uintptr_t addr = 0;
            sscanf_s(s_breakpointAddr, "%p", (void**)&addr);
            if (addr != 0) AddBreakpoint(addr);
        }
        ImGui::BeginChild("BreakPointTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ImGui::BeginTable("BreakPointTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(C("地址"));
            ImGui::TableSetupColumn(C("模块"));
            ImGui::TableSetupColumn(C("命中次数"));
            ImGui::TableSetupColumn(C("状态"));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < s_breakpoints.size(); ++i) {
                auto& bp = s_breakpoints[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(C("%s"), FormatAddress(bp.address).c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text(C("%s"), bp.module.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text(C("%d"), bp.hitCount);
                ImGui::TableSetColumnIndex(3);
                std::string btnId = (bp.enabled ? "禁用##bp" : "启用##bp") + std::to_string(i);
                if (ImGui::Button(C(btnId.c_str()), ImVec2(40, 0))) ToggleBreakpoint(bp.address);
                ImGui::SameLine();
                std::string delBtnId = "删除##bp" + std::to_string(i);
                if (ImGui::Button(C(delBtnId.c_str()), ImVec2(40, 0))) RemoveBreakpoint(bp.address);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // 线程信息
    if (ImGui::CollapsingHeader(C("线程信息"))) {
        ImGui::BeginChild("ThreadTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ImGui::BeginTable("ThreadTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(C("线程ID"));
            ImGui::TableSetupColumn(C("状态"));
            ImGui::TableSetupColumn(C("优先级"));
            ImGui::TableSetupColumn(C("入口地址"));
            ImGui::TableSetupColumn(C("所属模块"));
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

    // 内存编辑器
    if (s_showMemoryEditor) {
        DrawMemoryEditor();
    }

    ImGui::EndTabItem();
    return;
}

// 格式化内存大小（如 1.23 MB）
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

// 格式化地址（如 0x00400000）
static std::string FormatAddress(uintptr_t addr) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(sizeof(uintptr_t)*2) << std::setfill('0') << addr;
    return ss.str();
}

// 内存区域刷新（枚举目标进程所有内存区域）
static void RefreshAllData() {
    // 清空现有数据
    s_threads.clear();
    s_memoryRegions.clear();

    // 打开目标进程（需要足够权限）
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, s_currentPID);
    // 忽略API失败，继续执行

    // 刷新线程列表（详细状态）
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
                        thread.status = "未知"; // Or use other logic to determine status
                        thread.priority = te32.tpBasePri; // Use tpBasePri for priority
                        thread.entryAddr = "";
                        thread.moduleName = "";
                        CloseHandle(hThread);
                    } else {
                        thread.status = "未知";
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

    // 刷新内存区域
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

// 添加断点（写入0xCC，保存原字节）
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

// 移除断点（恢复原字节）
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

// 切换断点状态（启用/禁用，写入/恢复字节）
static bool ToggleBreakpoint(uintptr_t address) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, s_currentPID);
    if (!hProcess) return false;
    for (auto& bp : s_breakpoints) {
        if (bp.address == address) {
            SIZE_T written = 0;
            if (bp.enabled) {
                // 禁用：恢复原字节
                WriteProcessMemory(hProcess, (LPVOID)address, &bp.originalByte, 1, &written);
                bp.enabled = false;
            } else {
                // 启用：写入0xCC
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

// 内存编辑器（十六进制查看器，支持查看和编辑指定地址的内存）
static void DrawMemoryEditor() {
    static std::vector<uint8_t> memBuffer;
    static uintptr_t lastAddr = 0;
    ImGui::InputText("地址", s_memoryAddr, IM_ARRAYSIZE(s_memoryAddr));
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
        ImGui::Text("内存内容:");
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