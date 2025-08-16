#include "dll.h"
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

// 静态数据存储
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

    // UI状态存储
    static char s_filterText[256] = "";
    static char s_dllPath[512] = "";
    static char s_memoryAddr[32] = "0x00400000";
    static char s_breakpointAddr[32] = "0x00401000";
    static int s_sortColumn = -1;
    static bool s_sortAscending = true;
    static bool s_showModuleDetails = false;
    static KswordModuleInfo s_selectedModule;  // 使用实际定义的结构体
    static bool s_showMemoryEditor = false;
    static KswordMemoryRegionInfo s_selectedMemoryRegion;  // 使用实际定义的结构体

    // 数据存储使用vector容器
    static std::vector<KswordModuleInfo> s_modules;
    static std::vector<KswordThreadInfo> s_threads;
    static std::vector<KswordMemoryRegionInfo> s_memoryRegions;
    static std::vector<KswordBreakpointInfo> s_breakpoints;
}

using namespace KswordDebugger;

// 声明函数
static void RefreshAllData();
static void FilterAndSortModules(std::vector<KswordModuleInfo>& filteredModules);
static void DrawModuleDetails();
static void DrawMemoryEditor();
static std::string FormatSize(size_t size);
static std::string FormatAddress(uintptr_t addr);
static bool AddBreakpoint(uintptr_t address);
static bool RemoveBreakpoint(uintptr_t address);
static bool ToggleBreakpoint(uintptr_t address);


// 卸载模块的实际实现函数
bool UnloadModule(DWORD pid, HMODULE hModule) {
    if (pid == 0 || hModule == NULL) {
        kLog.err(C("无效的进程ID或模块句柄"), C("ModuleView"));
        return false;
    }

    // 打开目标进程
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "打开进程失败 (PID: %u), 错误: %u", pid, GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        return false;
    }

    // 获取FreeLibrary地址
    LPVOID freeLibAddr = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FreeLibrary");
    if (!freeLibAddr) {
        char errMsg[256];
        sprintf_s(errMsg, "获取FreeLibrary地址失败, 错误: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        CloseHandle(hProcess);
        return false;
    }

    // 创建远程线程执行卸载
    HANDLE hRemoteThread = CreateRemoteThread(
        hProcess,
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)freeLibAddr,
        hModule,  // 模块句柄作为参数
        0,
        NULL
    );

    if (!hRemoteThread || hRemoteThread == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "创建卸载线程失败, 错误: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        CloseHandle(hProcess);
        return false;
    }

    // 等待卸载完成
    DWORD waitResult = WaitForSingleObject(hRemoteThread, 5000); // 5秒超时
    if (waitResult == WAIT_TIMEOUT) {
        kLog.warn(C("卸载模块超时"), C("ModuleView"));
        TerminateThread(hRemoteThread, 0);
        CloseHandle(hRemoteThread);
        CloseHandle(hProcess);
        return false;
    }

    // 获取线程退出码判断是否卸载成功
    DWORD exitCode;
    if (!GetExitCodeThread(hRemoteThread, &exitCode) || exitCode == 0) {
        kLog.err(C("模块卸载失败"), C("ModuleView"));
        CloseHandle(hRemoteThread);
        CloseHandle(hProcess);
        return false;
    }

    // 清理资源
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
    ofn.lpstrTitle = "选择要注入的DLL文件";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        strncpy_s(outPath, maxSize, szFile, _TRUNCATE);
        kLog.info(C("已选择DLL文件"), C("ModuleView"));
    }
    else {
        DWORD err = CommDlgExtendedError();
        if (err != 0) {
            kLog.err(C(("文件选择对话框错误: " + std::to_string(err)).c_str()), C("ModuleView"));
        }
    }
}

// DLL注入实现
bool InjectDLL(const char* dllPath) {
    if (!dllPath || strlen(dllPath) == 0) {
        kLog.err(C("无效的DLL路径"), C("ModuleView"));
        return false;
    }

    // 获取目标进程ID (这里需要根据实际情况修改，例如从UI选择的进程)
    DWORD pid = s_currentPID; // 假设已有此函数获取目标进程ID
    if (pid == 0) {
        kLog.err(C("未选择目标进程"), C("ModuleView"));
        return false;
    }

    // 打开目标进程
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "打开进程失败 (PID: %u), 错误: %u", pid, GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        return false;
    }

    // 在目标进程中分配内存
    LPVOID remoteMem = VirtualAllocEx(hProcess, NULL, strlen(dllPath) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        char errMsg[256];
        sprintf_s(errMsg, "内存分配失败, 错误: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        CloseHandle(hProcess);
        return false;
    }

    // 写入DLL路径到目标进程内存
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, strlen(dllPath) + 1, NULL)) {
        char errMsg[256];
        sprintf_s(errMsg, "写入内存失败, 错误: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 获取LoadLibraryA地址
    LPVOID loadLibAddr = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!loadLibAddr) {
        char errMsg[256];
        sprintf_s(errMsg, "获取LoadLibraryA地址失败, 错误: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 创建远程线程执行DLL注入
    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibAddr, remoteMem, 0, NULL);
    if (!hRemoteThread || hRemoteThread == INVALID_HANDLE_VALUE) {
        char errMsg[256];
        sprintf_s(errMsg, "创建远程线程失败, 错误: %u", GetLastError());
        kLog.err(C(errMsg), C("ModuleView"));
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 等待注入完成
    WaitForSingleObject(hRemoteThread, INFINITE);

    // 清理资源
    CloseHandle(hRemoteThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    kLog.info(C("DLL注入线程执行完成"), C("ModuleView"));
    return true;
}
// UI渲染函数
void KswordDLLMain()
{
    // 初始化当前进程信息
    // 进程PID输入框UI组件
        ImGui::Text(C("请输入进程PID:"));
        static char pidBuffer[32] = "";  // 用于存储PID输入的缓冲区
        ImGui::InputText("##pidInputField", pidBuffer, IM_ARRAYSIZE(pidBuffer),
            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine();
        if (ImGui::Button(C("确认并刷新"))) {
            if (strlen(pidBuffer) > 0) {
                // 转换输入为DWORD类型PID
                DWORD targetPID = static_cast<DWORD>(atoi(pidBuffer));
                if (targetPID > 0) {
                    s_currentPID = targetPID;  // 更新当前PID
                    kLog.info(C("已设置当前PID为: " + std::to_string(s_currentPID)), C("PIDInput"));
                    // 获取进程路径和名称
                    char path[MAX_PATH] = { 0 };
                    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
                        s_processPath = path;
                        size_t lastSlash = s_processPath.find_last_of("\\/");
                        s_processName = (lastSlash != std::string::npos) ? s_processPath.substr(lastSlash + 1) : s_processPath;
                        s_isProcessRunning = true;
                        // 检测位数
                    }
                    else {
                        kLog.warn(C("无效的PID值，请输入正整数"), C("PIDInput"));
                    }
                }
                else {
                    kLog.warn(C("PID输入框不能为空"), C("PIDInput"));
                }
            }
            if (s_currentPID != 0)  // 当指定了目标PID时执行初始化
            {
                // 打开目标进程（需要足够权限）
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE, FALSE, s_currentPID);
                if (hProcess == NULL)
                {
                    DWORD error = GetLastError();
                    kLog.err(C(std::string("打开进程失败，PID: ") + std::to_string(s_currentPID) + std::string("，错误码: ") + std::to_string(error)), C("ProcessInfo"));
                    s_currentPID = 0;  // 重置无效PID
                    return;
                }

                // 获取进程路径与名称
                char path[MAX_PATH] = { 0 };
                if (GetModuleFileNameExA(hProcess, NULL, path, MAX_PATH) == 0)
                {
                    DWORD error = GetLastError();
                    kLog.err(C("获取进程路径失败，错误码: " + std::to_string(error)), C("ProcessInfo"));
                    CloseHandle(hProcess);
                    s_currentPID = 0;
                    return;
                }
                s_processPath = path;
                size_t lastSlash = s_processPath.find_last_of("\\/");
                s_processName = (lastSlash != std::string::npos) ? s_processPath.substr(lastSlash + 1) : s_processPath;

                // 检测目标进程位数
                BOOL isWow64 = FALSE;
                s_is64BitProcess = false;  // 默认32位

                // 判断当前系统是否为64位
                SYSTEM_INFO sysInfo = { 0 };
                GetNativeSystemInfo(&sysInfo);
                bool isSystem64Bit = (sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);

                if (isSystem64Bit)
                {
                    // 64位系统上，通过IsWow64Process判断目标进程是否为32位
                    if (IsWow64Process(hProcess, &isWow64))
                    {
                        // isWow64为TRUE → 目标是32位进程；FALSE → 目标是64位进程
                        s_is64BitProcess = !isWow64;
                    }
                    else
                    {
                        DWORD error = GetLastError();
                        kLog.warn(C("IsWow64Process调用失败，错误码: " + std::to_string(error) + "，默认按32位处理"), C("ProcessInfo"));
                    }
                }
                else
                {
                    // 32位系统只能运行32位进程
                    s_is64BitProcess = false;
                }

                // 检测进程是否运行
                DWORD exitCode = 0;
                s_isProcessRunning = GetExitCodeProcess(hProcess, &exitCode) && (exitCode == STILL_ACTIVE);
                if (!s_isProcessRunning)
                {
                    kLog.warn(C("目标进程已退出，PID: " + std::to_string(s_currentPID)), C("ProcessInfo"));
                }

                CloseHandle(hProcess);
                RefreshAllData();  // 刷新目标进程的数据
            }

        }

        ImGui::SameLine();
        if (ImGui::Button(C("清空"))) {
            memset(pidBuffer, 0, sizeof(pidBuffer));
            kLog.info(C("已清空PID输入框"), C("PIDInput"));
        }

    // 自动刷新逻辑
    float currentTime = ImGui::GetTime();
    if (s_autoRefresh && (currentTime - s_lastRefreshTime > s_refreshInterval))
    {
        RefreshAllData();
        s_lastRefreshTime = currentTime;
    }

    // 进程信息显示
    ImGui::Text(C("进程信息"));
    ImGui::Separator();

    ImGui::BeginGroup();
    ImGui::Text(C("进程ID: %d"), s_currentPID);
    ImGui::Text(C("进程名称: %s"), s_processName.c_str());
    ImGui::Text(C("进程路径:"));
    ImGui::SameLine();
    static char s_processPathBuffer[MAX_PATH] = "";
        if (!s_processPath.empty())
            strncpy_s(s_processPathBuffer, sizeof(s_processPathBuffer), C(s_processPath.c_str()), _TRUNCATE);
        else
            s_processPathBuffer[0] = '\0';
    ImGui::InputText("##processPath", s_processPathBuffer, sizeof(s_processPathBuffer), ImGuiInputTextFlags_ReadOnly);
    
    ImGui::Text(C("进程位数: %s"), s_is64BitProcess ? C("64位") : C("32位"));
    ImGui::Text(C("进程状态: %s"), s_isProcessRunning ? C("运行中") : C("已停止"));
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button(C("打开文件路径")))
    {
        std::string folderPath = s_processPath.substr(0, s_processPath.find_last_of("\\/"));
        kLog.info(C(("打开文件路径: " + folderPath).c_str()), C("ModuleView"));
    }

    if (ImGui::Button(s_autoRefresh ? C("关闭自动刷新") : C("开启自动刷新")))
    {
        s_autoRefresh = !s_autoRefresh;
        if (s_autoRefresh)
            kLog.info(C("已开启自动刷新"), C("ModuleView"));
        else
            kLog.info(C("已关闭自动刷新"), C("ModuleView"));
    }

    ImGui::Text(C("刷新间隔:"));
    ImGui::SameLine();
    ImGui::SliderFloat("##refreshInterval", &s_refreshInterval, 0.5f, 10.0f, C("%.1fs"));

    if (ImGui::Button(C("手动刷新")))
    {
        RefreshAllData();
        kLog.info(C("手动刷新数据"), C("ModuleView"));
    }
    ImGui::EndGroup();

    ImGui::Separator();

    // 模块列表筛选区
    ImGui::Text(C("模块列表"));
    ImGui::InputTextWithHint("##filter", C("筛选模块(大小写不敏感)"), s_filterText, IM_ARRAYSIZE(s_filterText));

    // 列表操作按钮
    ImGui::SameLine();
    if (ImGui::Button(C("清空列表")))
    {
        kLog.info(C("清空模块列表"), C("ModuleView"));
    }

    ImGui::SameLine();
    if (ImGui::Button(C("刷新模块")))
    {
        kLog.info(C("刷新模块列表"), C("ModuleView"));
        RefreshAllData();
    }

    // 筛选和排序模块
    std::vector<KswordModuleInfo> filteredModules = s_modules;
    FilterAndSortModules(filteredModules);

    // 显示模块表格
    float windowHeight = ImGui::GetContentRegionAvail().y;
    float halfHeight = windowHeight * 0.5f - ImGui::GetStyle().ItemSpacing.y;

    // 使用子窗口限制表格高度为半个窗口
    ImGui::BeginChild("ModuleTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    {
        if (ImGui::BeginTable("ModuleTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn(C("模块路径"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(C("基地址"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("大小"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("版本"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("状态"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(C("操作"), ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // 排序逻辑
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs && sortSpecs->Specs)
            {
                s_sortColumn = sortSpecs->Specs[0].ColumnIndex;
                s_sortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            for (size_t i = 0; i < filteredModules.size(); ++i)
            {
                ImGui::PushID(i);
                const auto& mod = filteredModules[i];
                ImGui::TableNextRow();

                // 模块路径
                ImGui::TableSetColumnIndex(0);
                bool isSelected = (s_showModuleDetails && s_selectedModule.path == mod.path);
                if (ImGui::Selectable(C(mod.path.c_str()), isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                {
                    s_selectedModule = mod;
                    s_showModuleDetails = true;
                }
                if (ImGui::BeginPopupContextItem("DLLModuleRightClick")) {  // 使用当前ID的上下文菜单
                    if (ImGui::MenuItem(C("显示详情"))) {
                        s_showModuleDetails = true;
                    }
                    if (ImGui::MenuItem(C("复制模块路径"))) {
                        ImGui::SetClipboardText(C(mod.path.c_str()));
                        kLog.info(C("已复制模块路径到剪贴板"), C("ModuleView"));
                    }
                    if (ImGui::MenuItem(C("复制基地址"))) {
                        char addrStr[32];
                        sprintf_s(addrStr, "0x%p", (void*)mod.baseAddr);
                        ImGui::SetClipboardText(addrStr);
                    }
                    if (ImGui::MenuItem(C("复制大小"))) {
                        ImGui::SetClipboardText(FormatSize(mod.size).c_str());
                    }
                    if (ImGui::MenuItem(C("复制版本"))) {
                        ImGui::SetClipboardText(mod.version.c_str());
                    }
                    if (ImGui::MenuItem(C("复制整行"))) {
                        std::stringstream ss;
                        ss << mod.path << "\t"
                            << FormatAddress(mod.baseAddr) << "\t"
                            << FormatSize(mod.size) << "\t"
                            << mod.version << "\t"
                            << (mod.isSigned ? C("已签名") : C("未签名")) << "\t"
                            << (mod.is64Bit ? C("64位") : C("32位"));
                        ImGui::SetClipboardText(ss.str().c_str());
                        kLog.info(C("已复制模块整行到剪贴板"), C("ModuleView"));
                    }
                    if (ImGui::MenuItem(C("复制全部"))) {
                        std::stringstream ss;
                        for (const auto& mod : filteredModules) {
                            ss << C(mod.path) << "\t"
                                << FormatAddress(mod.baseAddr) << "\t"
                                << FormatSize(mod.size) << "\t"
                                << mod.version << "\t"
                                << (mod.isSigned ? C("已签名") : C("未签名")) << "\t"
                                << (mod.is64Bit ? C("64位") : C("32位")) << "\n";
                        }
                        ImGui::SetClipboardText(ss.str().c_str());
                        kLog.info(C("已复制全部模块信息到剪贴板"), C("ModuleView"));
                    }

                    ImGui::EndPopup();
                }ImGui::PopID();

                // 基地址
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", C(FormatAddress(mod.baseAddr).c_str()));

                // 大小
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", FormatSize(mod.size).c_str());

                // 版本
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", mod.version.c_str());

                // 状态
                ImGui::TableSetColumnIndex(4);
                if (mod.isSigned)
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), C("已签名"));
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), C("未签名"));
                ImGui::SameLine();
                ImGui::Text("%s", mod.is64Bit ? C("64位") : C("32位"));

                // 操作按钮
                ImGui::TableSetColumnIndex(5);
                std::string locateBtnId = "定位##" + std::to_string(i);
                if (ImGui::Button(C(locateBtnId.c_str()), ImVec2(40, 0)))
                {
                    // 格式化内存地址显示（确保正确的十六进制格式）
                    sprintf_s(s_memoryAddr, "0x%p", (void*)mod.baseAddr);
                    s_showMemoryEditor = true;

                    // 优化日志输出，使用sprintf避免string与C()直接拼接
                    char logMsg[512];
                    sprintf_s(logMsg, "定位模块内存: %s (地址: 0x%p)", mod.path.c_str(), (void*)mod.baseAddr);
                    kLog.info(C(logMsg), C("ModuleView"));
                }
                ImGui::SameLine();

                std::string unloadBtnId = "卸载##" + std::to_string(i);
                if (ImGui::Button(C(unloadBtnId.c_str()), ImVec2(40, 0)))
                {
                    char logMsg[512];
                    sprintf_s(logMsg, "尝试卸载模块: %s (地址: 0x%p)", mod.path.c_str(), (void*)mod.baseAddr);
                    kLog.info(C(logMsg), C("ModuleView"));

                    // 执行实际卸载操作
                    bool unloadSuccess = UnloadModule(s_currentPID, (HMODULE)mod.baseAddr);

                    // 根据卸载结果更新列表并记录日志
                    if (unloadSuccess) {
                        // 从列表中移除模块
                        auto it = std::find_if(s_modules.begin(), s_modules.end(),
                            [&](const KswordModuleInfo& m) {
                                return m.path == mod.path && m.baseAddr == mod.baseAddr;
                            });

                        if (it != s_modules.end()) {
                            s_modules.erase(it);
                            sprintf_s(logMsg, "已成功卸载模块: %s", mod.path.c_str());
                            kLog.info(C(logMsg), C("ModuleView"));
                        }
                        else {
                            kLog.warn(C("模块已卸载但未在列表中找到"), C("ModuleView"));
                        }
                    }
                    else {
                        sprintf_s(logMsg, "卸载模块失败: %s", mod.path.c_str());
                        kLog.err(C(logMsg), C("ModuleView"));
                    }
                }

            }
            ImGui::EndTable();
        }
		ImGui::EndChild();  // 结束子窗口
    }

    // 显示模块详细信息
    if (s_showModuleDetails)
    {
        DrawModuleDetails();
    }

    ImGui::Separator();

    // DLL注入区域
    ImGui::Text(C("DLL注入"));
    ImGui::InputText("##dllPath", s_dllPath, IM_ARRAYSIZE(s_dllPath));
    ImGui::SameLine();
    if (ImGui::Button(C("浏览")))
    {
        kLog.info(C("浏览DLL文件"), C("ModuleView"));
        BrowseDLLFile(s_dllPath, MAX_PATH); // 假设s_dllPath是全局缓冲区
    }
    ImGui::SameLine();
    if (ImGui::Button(C("注入")))
    {
        if (strlen(s_dllPath) > 0)
        {
            if (InjectDLL(s_dllPath))
            {
                // 修正字符串拼接方式，避免直接用C()与string相加
                char successMsg[512];
                sprintf_s(successMsg, "DLL注入成功: %s", s_dllPath);
                kLog.info(C(successMsg), C("ModuleView"));
                RefreshAllData();
            }
            else
            {
                char failMsg[512];
                sprintf_s(failMsg, "DLL注入失败: %s", s_dllPath);
                kLog.err(C(failMsg), C("ModuleView"));
            }
        }
        else
        {
            kLog.warn(C("DLL路径为空，注入失败"), C("ModuleView"));
        }
    }

    // 进程控制区域
    ImGui::Separator();
    ImGui::Text(C("进程控制"));
    ImGui::BeginGroup();
    if (ImGui::Button(C("终止进程"), ImVec2(100, 0)))
    {
        //if (ImGui::GetIO().KeyCtrl)
        //{
            if(kProcess(s_currentPID).Terminate())
            {
                kLog.info(C(("成功终止进程: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
                s_isProcessRunning = false;
            }
            else
            {
                kLog.err(C(("终止进程失败: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
			}
            

            kLog.info(C(("终止进程: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
        //}
        //else
        //{
        //    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), C("请按住Ctrl以确认终止"));
        //}
    }

    ImGui::SameLine();
    if (ImGui::Button(C("暂停进程"), ImVec2(100, 0)))
    {
        kProcess(s_currentPID).Suspend();
        kLog.info(C(("暂停进程: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
    }

    ImGui::SameLine();
    if (ImGui::Button(C("继续进程"), ImVec2(100, 0)))
    {
		kProcess(s_currentPID).Resume();
        kLog.info(C(("继续进程: " + std::to_string(s_currentPID)).c_str()), C("ModuleView"));
    }

    ImGui::SameLine();
    if (ImGui::Button(C("刷新进程"), ImVec2(100, 0)))
    {
        RefreshAllData();
        kLog.info(C("刷新进程信息"), C("ModuleView"));
    }
    ImGui::EndGroup();

    // 内存查看区域
    if (ImGui::CollapsingHeader(C("内存查看"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText(C("内存地址"), s_memoryAddr, IM_ARRAYSIZE(s_memoryAddr));
        ImGui::SameLine();
        if (ImGui::Button(C("查看内存")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_memoryAddr, "%p", (void**)&addr);
            if (addr != 0)
            {
                s_showMemoryEditor = true;
                kLog.info(C(("查看内存地址: " + std::string(s_memoryAddr)).c_str()), C("ModuleView"));
            }
            else
            {
                kLog.warn(C("无效的内存地址"), C("ModuleView"));
            }
        }

        // 内存区域列表
        ImGui::BeginChild("MemoryTableViewCHildWindow", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar); {
            if (ImGui::BeginTable("MemoryTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn(C("基地址"), ImGuiTableColumnFlags_WidthStretch, 2.0f);  // 权重2
                ImGui::TableSetupColumn(C("大小"), ImGuiTableColumnFlags_WidthStretch, 1.0f);     // 权重1
                ImGui::TableSetupColumn(C("保护属性"), ImGuiTableColumnFlags_WidthStretch, 2.0f); // 权重2
                ImGui::TableSetupColumn(C("状态"), ImGuiTableColumnFlags_WidthStretch, 1.0f);     // 权重1
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < s_memoryRegions.size(); ++i)
                {
                    const KswordMemoryRegionInfo& region = s_memoryRegions[i];  // 实际使用
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
			}ImGui::EndChild();  // 结束子窗口
        }
    }

    // 显示内存编辑窗口
    if (s_showMemoryEditor)
    {
        DrawMemoryEditor();
    }

    // 线程信息区域
    if (ImGui::CollapsingHeader(C("线程信息")))
    {
        ImGui::BeginChild("ThreadTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar); {

            //std::cout << s_threads.size() << std::endl;
            if (ImGui::BeginTable("ThreadTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn(C("线程ID"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(C("状态"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn(C("优先级"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(C("入口地址"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
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
            }	ImGui::EndChild();  // 结束子窗口
        }
    }

    // 断点管理区域
    if (ImGui::CollapsingHeader(C("断点管理")))
    {
        ImGui::InputText(C("断点地址"), s_breakpointAddr, IM_ARRAYSIZE(s_breakpointAddr));
        ImGui::SameLine();
        if (ImGui::Button(C("添加断点")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_breakpointAddr, "%p", (void**)&addr);
            if (addr != 0)
            {
                if (AddBreakpoint(addr))
                {
                    kLog.info(C(("添加断点: " + std::string(s_breakpointAddr)).c_str()), C("ModuleView"));
                }
                else
                {
                    kLog.err(C(("添加断点失败: " + std::string(s_breakpointAddr)).c_str()), C("ModuleView"));
                }
            }
            else
            {
                kLog.warn(C("无效的断点地址"), C("ModuleView"));
            }
        }
        ImGui::BeginChild("BreakPointTableContainer", ImVec2(0, halfHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar); {
            if (ImGui::BeginTable("BreakPointTable2", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn(C("地址"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(C("模块"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(C("命中次数"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(C("操作"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                std::cout<< s_breakpoints.size() << std::endl;
                for (size_t i = 0; i < s_breakpoints.size(); ++i)
                {
                    kLog.dbg(C("遍历到一个断点"), C("断点管理"));
                    auto& bp = s_breakpoints[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", C(FormatAddress(bp.address).c_str()));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", C(bp.module.c_str()));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", bp.hitCount);
                    ImGui::TableSetColumnIndex(3);
                    std::string toggleBtnId = (bp.enabled ? "禁用##bp" : "启用##bp") + std::to_string(i);
                    if (ImGui::Button(C(toggleBtnId.c_str()), ImVec2(40, 0)))
                    {
                        ToggleBreakpoint(bp.address);
                    }
                    ImGui::SameLine();
                    std::string delBtnId = "删除##bp" + std::to_string(i);
                    if (ImGui::Button(C(delBtnId.c_str()), ImVec2(40, 0)))
                    {
                        RemoveBreakpoint(bp.address);
                        kLog.info(C(("删除断点: " + FormatAddress(bp.address)).c_str()), C("ModuleView"));
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
    }

    ImGui::EndTabItem();
}

// 数据刷新实现
static void RefreshAllData()
{
    // 清空现有数据
    s_modules.clear();
    s_threads.clear();
    s_memoryRegions.clear();

    // 打开目标进程（需要足够权限）
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        s_currentPID
    );

    if (hProcess == NULL)
    {
        DWORD error = GetLastError();
        kLog.err(C("打开目标进程失败，无法刷新数据，错误码: " + std::to_string(error)), C("DataRefresh"));
        return;
    }

    // 1. 刷新模块列表（真实枚举目标进程模块）
    HMODULE* moduleList = nullptr;
    DWORD bytesNeeded = 0;

    // 第一次调用获取所需缓冲区大小
    if (EnumProcessModulesEx(hProcess, moduleList, 0, &bytesNeeded, LIST_MODULES_ALL))
    {
        // 分配缓冲区
        moduleList = (HMODULE*)LocalAlloc(LPTR, bytesNeeded);
        if (moduleList)
        {
            // 第二次调用获取模块列表
            if (EnumProcessModulesEx(hProcess, moduleList, bytesNeeded, &bytesNeeded, LIST_MODULES_ALL))
            {
                DWORD moduleCount = bytesNeeded / sizeof(HMODULE);
                for (DWORD i = 0; i < moduleCount; ++i)
                {
                    KswordModuleInfo module;
                    char modulePath[MAX_PATH] = { 0 };

                    // 获取模块路径
                    if (GetModuleFileNameExA(hProcess, moduleList[i], modulePath, MAX_PATH))
                    {
                        module.path = modulePath;

                        // 获取模块基地址和大小
                        MODULEINFO modInfo = { 0 };
                        if (GetModuleInformation(hProcess, moduleList[i], &modInfo, sizeof(MODULEINFO)))
                        {
                            module.baseAddr = (uintptr_t)modInfo.lpBaseOfDll;
                            module.size = modInfo.SizeOfImage;
                        }

                        // 获取模块版本信息
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
                            module.version = C("未知版本");

                            auto isModuleSigned = [](const std::string& filePath) -> bool {
                            WINTRUST_FILE_INFO fileInfo = { 0 };
                            fileInfo.cbStruct = sizeof(WINTRUST_FILE_INFO);
                            fileInfo.pcwszFilePath = std::wstring(filePath.begin(), filePath.end()).c_str();
                            fileInfo.hFile = NULL;
                            fileInfo.pgKnownSubject = NULL;

                            WINTRUST_DATA winTrustData = { 0 };
                            winTrustData.cbStruct = sizeof(WINTRUST_DATA);
                            winTrustData.dwUIChoice = WTD_UI_NONE;
                            winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
                            winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
                            winTrustData.pFile = &fileInfo;
                            winTrustData.dwStateAction = WTD_STATEACTION_IGNORE;
                            winTrustData.dwProvFlags = WTD_SAFER_FLAG;
                            winTrustData.hWVTStateData = NULL;
                            winTrustData.pwszURLReference = NULL;

                            GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
                            LONG status = WinVerifyTrust(NULL, &policyGUID, &winTrustData);
                            return status == ERROR_SUCCESS;
                            };

                        // 替换原有判断
                        module.isSigned = isModuleSigned(module.path);
                        module.is64Bit = s_is64BitProcess;  // 与进程位数一致

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
        kLog.warn(C("枚举模块失败，错误码: " + std::to_string(error)), C("DataRefresh"));
    }

    // 2. 刷新线程列表（真实枚举目标进程线程）
    HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hThreadSnap != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te32 = { 0 };
        te32.dwSize = sizeof(THREADENTRY32);

        if (Thread32First(hThreadSnap, &te32))
        {
            do
            {

                // 只处理目标进程的线程
                if (te32.th32OwnerProcessID == s_currentPID)
                {
                    KswordThreadInfo thread;
                    thread.id = te32.th32ThreadID;
				kLog.dbg(C("线程ID: " + std::to_string(te32.th32ThreadID)), C("DataRefresh"));
                    // 打开线程获取详细信息
                    HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread.id);
                    if (hThread != NULL)
                    {
                        // 获取线程优先级
                        thread.priority = GetThreadPriority(hThread);

                        // 判断线程状态（简化版：通过退出码判断是否运行中）
                        DWORD exitCode = 0;
                        GetExitCodeThread(hThread, &exitCode);
                        thread.status = (exitCode == STILL_ACTIVE) ? C("运行中") : C("已退出");

                        // 获取线程入口地址（需特定权限，可能失败）
                        uintptr_t entryAddr = 0;
#ifdef _WIN64
                        if (s_is64BitProcess)
                            GetThreadContext(hThread, (LPCONTEXT)&entryAddr);  // 简化处理，实际需正确获取上下文
#else
                        if (!s_is64BitProcess)
                            GetThreadContext(hThread, (LPCONTEXT)&entryAddr);
#endif
                        thread.entryAddr = FormatAddress(entryAddr);

                        CloseHandle(hThread);
                    }
                    else
                    {
                        thread.status = C("权限不足");
                        thread.priority = 0;
                        thread.entryAddr = C("未知");
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
        kLog.warn(C("枚举线程失败，错误码: " + std::to_string(error)), C("DataRefresh"));
    }

    // 3. 刷新内存区域（真实查询目标进程内存）
    MEMORY_BASIC_INFORMATION mbi = { 0 };
    uintptr_t addr = 0;

    while (VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        KswordMemoryRegionInfo region;
        region.baseAddr = (uintptr_t)mbi.BaseAddress;
        region.size = mbi.RegionSize;

        // 转换内存类型描述
        switch (mbi.Type)
        {
        case MEM_IMAGE: region.type = C("映射文件"); break;
        case MEM_MAPPED: region.type = C("映射内存"); break;
        case MEM_PRIVATE: region.type = C("私有内存"); break;
        default: region.type = C("未知类型");
        }

        // 转换保护属性描述
        std::string protectStr;
        if (mbi.Protect & PAGE_EXECUTE) protectStr += C("执行");
        if (mbi.Protect & PAGE_READONLY) protectStr += C("只读");
        if (mbi.Protect & PAGE_READWRITE) protectStr += C("读写");
        if (mbi.Protect & PAGE_WRITECOPY) protectStr += C("写时复制");
        if (mbi.Protect & PAGE_EXECUTE_READ) protectStr += C("执行+读");
        if (mbi.Protect & PAGE_EXECUTE_READWRITE) protectStr += C("执行+读写");
        if (mbi.Protect & PAGE_EXECUTE_WRITECOPY) protectStr += C("执行+写时复制");
        if (protectStr.empty()) protectStr = C("未知保护");
        region.protection = protectStr;

        // 转换内存状态描述
        switch (mbi.State)
        {
        case MEM_COMMIT: region.state = C("已提交"); break;
        case MEM_RESERVE: region.state = C("已保留"); break;
        case MEM_FREE: region.state = C("空闲"); break;
        default: region.state = C("未知状态");
        }

        s_memoryRegions.push_back(region);
        addr += mbi.RegionSize;  // 移动到下一个内存区域

        // 防止无限循环（处理64位大地址空间）
        if (addr < (uintptr_t)mbi.BaseAddress)
            break;
    }

    CloseHandle(hProcess);
    kLog.info(C("已刷新目标进程数据，PID: " + std::to_string(s_currentPID)), C("DataRefresh"));
}
static void FilterAndSortModules(std::vector<KswordModuleInfo>& filteredModules)
{
    // 筛选逻辑
    std::string filterLower = s_filterText;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    if (!filterLower.empty())
    {
        auto it = std::remove_if(filteredModules.begin(), filteredModules.end(),
            [&](const KswordModuleInfo& mod) {  // 使用实际结构体
                std::string pathLower = mod.path;
                std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
                return pathLower.find(filterLower) == std::string::npos;
            });
        filteredModules.erase(it, filteredModules.end());
    }

    // 排序逻辑(使用lambda表达式实现)
    if (s_sortColumn >= 0)
    {
        switch (s_sortColumn)
        {
        case 0: // 模块路径
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.path < b.path : a.path > b.path;
                });
            break;
        case 1: // 基地址
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.baseAddr < b.baseAddr : a.baseAddr > b.baseAddr;
                });
            break;
        case 2: // 大小
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.size < b.size : a.size > b.size;
                });
            break;
        case 3: // 版本
            std::sort(filteredModules.begin(), filteredModules.end(),
                [](const KswordModuleInfo& a, const KswordModuleInfo& b) {
                    return s_sortAscending ? a.version < b.version : a.version > b.version;
                });
            break;
        }
    }
}

static DWORD RvaToFileOffset(LPVOID base, DWORD rva) {
    if (rva == 0) return 0;

    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(base) + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER* section = &sections[i];
        if (rva >= section->VirtualAddress &&
            rva < section->VirtualAddress + section->Misc.VirtualSize) {
            return rva - section->VirtualAddress + section->PointerToRawData;
        }
    }
    return 0;
}

static void ShowExportTable(const std::string& modulePath) {
#define CURRENT_MODULE C("导出表解析")
    std::vector<std::string> exportNames;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = NULL;
    LPVOID base = NULL;

    kLog.info(C("开始解析模块导出表: " + modulePath), CURRENT_MODULE);

    // 打开文件
    hFile = CreateFileA(modulePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        kLog.err(C("文件打开失败，错误码: " + std::to_string(GetLastError())), CURRENT_MODULE);
        // 直接进入资源清理流程
    }
    else {
        // 创建文件映射
        hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMapping) {
            kLog.err(C("文件映射创建失败，错误码: " + std::to_string(GetLastError())), CURRENT_MODULE);
        }
        else {
            // 映射视图
            base = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
            if (!base) {
                kLog.err(C("文件视图映射失败，错误码: " + std::to_string(GetLastError())), CURRENT_MODULE);
            }
            else {
                // 解析DOS头
                IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                    kLog.err(C("无效的DOS签名 (不是PE文件)"), CURRENT_MODULE);
                }
                else {
                    // 解析NT头
                    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                        reinterpret_cast<BYTE*>(base) + dos->e_lfanew);
                    if (nt->Signature != IMAGE_NT_SIGNATURE) {
                        kLog.err(C("无效的NT签名 (PE结构损坏)"), CURRENT_MODULE);
                    }
                    else {
                        // 检查导出表目录
                        IMAGE_DATA_DIRECTORY& exportDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                        if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) {
                            kLog.info(C("模块没有导出表"), CURRENT_MODULE);
                        }
                        else {
                            // 转换导出表RVA为文件偏移
                            DWORD exportTableOffset = RvaToFileOffset(base, exportDir.VirtualAddress);
                            if (exportTableOffset == 0) {
                                kLog.err(C("导出表RVA转换文件偏移失败 (无效地址)"), CURRENT_MODULE);
                            }
                            else {
                                // 验证导出表地址有效性
                                SIZE_T fileSize = GetFileSize(hFile, NULL);
                                if (exportTableOffset + sizeof(IMAGE_EXPORT_DIRECTORY) > fileSize) {
                                    kLog.err(C("导出表地址超出文件范围 (文件损坏)"), CURRENT_MODULE);
                                }
                                else {
                                    // 获取导出表
                                    IMAGE_EXPORT_DIRECTORY* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
                                        reinterpret_cast<BYTE*>(base) + exportTableOffset);

                                    // 转换AddressOfNames的RVA为文件偏移
                                    DWORD namesOffset = RvaToFileOffset(base, exp->AddressOfNames);
                                    if (namesOffset == 0) {
                                        kLog.err(C("AddressOfNames RVA转换失败 (无效地址)"), CURRENT_MODULE);
                                    }
                                    else if (namesOffset + exp->NumberOfNames * sizeof(DWORD) > fileSize) {
                                        kLog.err(C("导出名称表超出文件范围 (文件损坏)"), CURRENT_MODULE);
                                    }
                                    else {
                                        // 解析导出名称
                                        DWORD* names = reinterpret_cast<DWORD*>(
                                            reinterpret_cast<BYTE*>(base) + namesOffset);
                                        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                                            DWORD nameOffset = RvaToFileOffset(base, names[i]);
                                            if (nameOffset == 0 || nameOffset >= fileSize) {
                                                kLog.warn(C("跳过无效的导出名称 (索引: " + std::to_string(i) + ")"), CURRENT_MODULE);
                                                continue;
                                            }
                                            exportNames.push_back(reinterpret_cast<char*>(base) + nameOffset);
                                        }
                                        kLog.info(C("导出表解析成功，共找到 " + std::to_string(exportNames.size()) + " 个导出项"), CURRENT_MODULE);
                                    }
                                }
                            }
                        }
                    }
                }
                // 释放映射视图
                UnmapViewOfFile(base);
                base = NULL;
            }
            // 关闭文件映射
            CloseHandle(hMapping);
            hMapping = NULL;
        }
        // 关闭文件句柄
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }

    // 显示弹窗
    ImGui::OpenPopup(C("导出表"));
    if (ImGui::BeginPopupModal(C("导出表"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(C("模块: %s"), modulePath.c_str());
        ImGui::Separator();

        if (exportNames.empty()) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), C("未找到导出表或解析失败"));
        }
        else {
            for (const auto& name : exportNames) {
                ImGui::Text("%s", name.c_str());
            }
        }

        if (ImGui::Button(C("关闭"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#undef CURRENT_MODULE
}
static void ShowImportTable(const std::string& modulePath) {
    // 简单PE导入表解析，仅显示导入DLL和函数名
    std::vector<std::string> importList;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = NULL;
    LPVOID base = NULL;

    // 打开文件
    hFile = CreateFileA(modulePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        importList.push_back("无法打开文件");
    }
    else {
        // 创建文件映射
        hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMapping) {
            importList.push_back("无法创建文件映射");
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
        else {
            // 映射文件视图
            base = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
            if (!base) {
                importList.push_back("无法映射文件视图");
                CloseHandle(hMapping);
                CloseHandle(hFile);
                hMapping = NULL;
                hFile = INVALID_HANDLE_VALUE;
            }
            else {
                // 解析PE结构
                IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                    importList.push_back("不是有效的DOS签名");
                }
                else {
                    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                        reinterpret_cast<BYTE*>(base) + dos->e_lfanew);

                    if (nt->Signature != IMAGE_NT_SIGNATURE) {
                        importList.push_back("不是有效的NT签名");
                    }
                    else {
                        // 获取导入表目录项
                        IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                        if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
                            importList.push_back("没有导入表数据");
                        }
                        else {
                            // 将虚拟地址转换为文件中的实际偏移
                            // 获取节表
                            IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
                            DWORD numSections = nt->FileHeader.NumberOfSections;

                            // 找到包含导入表的节
                            DWORD importRva = importDir.VirtualAddress;
                            DWORD importOffset = 0;
                            bool foundSection = false;

                            for (DWORD i = 0; i < numSections; ++i) {
                                if (importRva >= sections[i].VirtualAddress &&
                                    importRva < sections[i].VirtualAddress + sections[i].SizeOfRawData) {
                                    importOffset = importRva - sections[i].VirtualAddress + sections[i].PointerToRawData;
                                    foundSection = true;
                                    break;
                                }
                            }

                            if (!foundSection) {
                                importList.push_back("找不到导入表所在的节");
                            }
                            else {
                                // 计算导入描述符的实际地址
                                IMAGE_IMPORT_DESCRIPTOR* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                                    reinterpret_cast<BYTE*>(base) + importOffset);

                                // 遍历导入描述符，最多遍历合理数量的描述符防止无限循环
                                DWORD maxImports = 1024; // 合理的上限
                                DWORD importCount = 0;

                                // 导入表以全零的描述符结束
                                while ((imp->Name != 0 || imp->FirstThunk != 0) && importCount < maxImports) {
                                    if (imp->Name != 0) {
                                        // 计算DLL名称的实际地址
                                        DWORD nameRva = imp->Name;
                                        DWORD nameOffset = 0;
                                        bool foundNameSection = false;

                                        for (DWORD i = 0; i < numSections; ++i) {
                                            if (nameRva >= sections[i].VirtualAddress &&
                                                nameRva < sections[i].VirtualAddress + sections[i].SizeOfRawData) {
                                                nameOffset = nameRva - sections[i].VirtualAddress + sections[i].PointerToRawData;
                                                foundNameSection = true;
                                                break;
                                            }
                                        }

                                        if (foundNameSection) {
                                            char* dllNamePtr = reinterpret_cast<char*>(
                                                reinterpret_cast<BYTE*>(base) + nameOffset);
                                            std::string dllName = dllNamePtr;

                                            // 处理导入函数
                                            if (imp->OriginalFirstThunk != 0) {
                                                DWORD thunkRva = imp->OriginalFirstThunk;
                                                DWORD thunkOffset = 0;
                                                bool foundThunkSection = false;

                                                for (DWORD i = 0; i < numSections; ++i) {
                                                    if (thunkRva >= sections[i].VirtualAddress &&
                                                        thunkRva < sections[i].VirtualAddress + sections[i].SizeOfRawData) {
                                                        thunkOffset = thunkRva - sections[i].VirtualAddress + sections[i].PointerToRawData;
                                                        foundThunkSection = true;
                                                        break;
                                                    }
                                                }

                                                if (foundThunkSection) {
                                                    IMAGE_THUNK_DATA* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                                                        reinterpret_cast<BYTE*>(base) + thunkOffset);

                                                    // 遍历导入函数，限制最大数量防止无限循环
                                                    DWORD maxFunctions = 4096;
                                                    DWORD funcCount = 0;

                                                    while (thunk->u1.AddressOfData != 0 && funcCount < maxFunctions) {
                                                        // 检查是否是按名称导入(最高位为0)
                                                        if (!(thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                                                            DWORD funcRva = thunk->u1.AddressOfData;
                                                            DWORD funcOffset = 0;
                                                            bool foundFuncSection = false;

                                                            for (DWORD i = 0; i < numSections; ++i) {
                                                                if (funcRva >= sections[i].VirtualAddress &&
                                                                    funcRva < sections[i].VirtualAddress + sections[i].SizeOfRawData) {
                                                                    funcOffset = funcRva - sections[i].VirtualAddress + sections[i].PointerToRawData;
                                                                    foundFuncSection = true;
                                                                    break;
                                                                }
                                                            }

                                                            if (foundFuncSection) {
                                                                IMAGE_IMPORT_BY_NAME* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                                                                    reinterpret_cast<BYTE*>(base) + funcOffset);

                                                                if (importByName && importByName->Name[0] != '\0') {
                                                                    importList.push_back(dllName + " : " + reinterpret_cast<char*>(importByName->Name));
                                                                }
                                                                else {
                                                                    importList.push_back(dllName + " : [未知函数]");
                                                                }
                                                            }
                                                            else {
                                                                importList.push_back(dllName + " : [函数地址无效]");
                                                            }
                                                        }
                                                        else {
                                                            // 按序号导入
                                                            DWORD ordinal = thunk->u1.Ordinal & 0xFFFF;
                                                            importList.push_back(dllName + " : #" + std::to_string(ordinal));
                                                        }

                                                        ++thunk;
                                                        ++funcCount;
                                                    }
                                                }
                                                else {
                                                    importList.push_back(dllName + " : [导入函数表地址无效]");
                                                }
                                            }
                                            else {
                                                importList.push_back(dllName + " : [没有原始导入函数表]");
                                            }
                                        }
                                        else {
                                            importList.push_back("[DLL名称地址无效]");
                                        }
                                    }

                                    ++imp;
                                    ++importCount;
                                }

                                if (importCount >= maxImports) {
                                    importList.push_back("[导入表项数量超出合理范围，可能是畸形PE文件]");
                                }
                            }
                        }
                    }
                }

                // 清理资源
                UnmapViewOfFile(base);
                base = NULL;
                CloseHandle(hMapping);
                hMapping = NULL;
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;
            }
        }
    }

    // 显示导入表弹窗
    ImGui::OpenPopup(C("导入表"));
    if (ImGui::BeginPopupModal(C("导入表"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(C("模块: %s"), modulePath.c_str());
        ImGui::Separator();

        if (importList.empty()) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), C("未找到导入表或解析失败"));
        }
        else {
            for (const auto& info : importList) {
                ImGui::Text("%s", info.c_str());
            }
        }

        if (ImGui::Button(C("关闭"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
static void DrawModuleDetails()
{
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(C("模块详情"), &s_showModuleDetails))
    {
        ImGui::Text(C("模块路径: %s"), s_selectedModule.path.c_str());
        ImGui::Text(C("基地址: %s"), FormatAddress(s_selectedModule.baseAddr).c_str());
        ImGui::Text(C("大小: %s"), FormatSize(s_selectedModule.size).c_str());
        ImGui::Text(C("版本: %s"), s_selectedModule.version.c_str());
        ImGui::Text(C("加载时间: %s"), s_selectedModule.loadTime.c_str());
        ImGui::Text(C("校验和: 0x%X"), s_selectedModule.checksum);
        ImGui::Text(C("签名状态: %s"), s_selectedModule.isSigned ? C("已签名") : C("未签名"));
        ImGui::Text(C("位数: %s"), s_selectedModule.is64Bit ? C("64位") : C("32位"));

        ImGui::Separator();

        if (ImGui::Button(C("查看导出表")))
        {
            ShowExportTable(s_selectedModule.path);
        }
        ImGui::SameLine();
        if (ImGui::Button(C("查看导入表")))
        {
            ShowImportTable(s_selectedModule.path);
        }
        ImGui::SameLine();
        if (ImGui::Button(C("在内存中查看")))
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
    if (ImGui::Begin(C("内存编辑器"), &s_showMemoryEditor, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text(C("地址: %s"), s_memoryAddr);
        ImGui::SameLine();
        if (ImGui::Button(C("刷新")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_memoryAddr, "%p", (void**)&addr);
            if (addr == 0)
            {
                kLog.warn(C("无效的内存地址，无法刷新"), C("MemoryEditor"));
                return;
            }

            // 读取目标进程内存
            HANDLE hProcess = OpenProcess(PROCESS_VM_READ, FALSE, s_currentPID);
            if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
            {
                kLog.err(C(("打开进程失败，无法读取内存: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
                return;
            }

            // 读取0x100字节内存数据
            static std::vector<uint8_t> memoryData(0x100, 0);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(hProcess, (LPCVOID)addr, memoryData.data(), memoryData.size(), &bytesRead))
            {
                kLog.info(C(("成功读取内存: " + FormatAddress(addr) + " 长度: " + std::to_string(bytesRead)).c_str()), C("MemoryEditor"));
            }
            else
            {
                kLog.err(C(("读取内存失败: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
            }
            CloseHandle(hProcess);
        }

        ImGui::SameLine();
        static char modifyValue[32] = "";
        ImGui::InputTextWithHint("##modifyValue", C("输入新值(十六进制)"), modifyValue, IM_ARRAYSIZE(modifyValue));
        ImGui::SameLine();
        if (ImGui::Button(C("修改")))
        {
            uintptr_t addr = 0;
            sscanf_s(s_memoryAddr, "%p", (void**)&addr);
            if (addr == 0)
            {
                kLog.warn(C("无效的内存地址，无法修改"), C("MemoryEditor"));
                return;
            }

            // 解析输入的十六进制值
            uint8_t value = 0;
            if (sscanf_s(modifyValue, "%hhX", &value) != 1)
            {
                kLog.warn(C("无效的十六进制值"), C("MemoryEditor"));
                return;
            }

            // 写入目标进程内存
            HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, s_currentPID);
            if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
            {
                kLog.err(C(("打开进程失败，无法写入内存: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
                return;
            }

            SIZE_T bytesWritten = 0;
            if (WriteProcessMemory(hProcess, (LPVOID)addr, &value, sizeof(value), &bytesWritten) && bytesWritten == sizeof(value))
            {
                kLog.info(C(("成功修改内存: " + FormatAddress(addr) + " 新值: 0x" + std::to_string(value)).c_str()), C("MemoryEditor"));
            }
            else
            {
                kLog.err(C(("写入内存失败: " + std::to_string(GetLastError())).c_str()), C("MemoryEditor"));
            }
            CloseHandle(hProcess);
        }

        ImGui::Separator();

        // 显示内存数据（使用实际读取的数据）
        ImGui::BeginChild(C("MemoryView"), ImVec2(0, 0), true);
        ImGuiListClipper clipper;
        static std::vector<uint8_t> memoryData(0x100, 0); // 存储实际读取的内存数据
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

                // 十六进制显示
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

                // ASCII显示
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
// 格式化地址（0xXXXXXXXX 或 0xXXXXXXXXXXXXXXXX 格式）
static std::string FormatAddress(uintptr_t addr)
{
    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase
        << std::setw(s_is64BitProcess ? 16 : 8)  // 64位地址16个十六进制位，32位8个
        << std::setfill('0') << addr;
    return ss.str();
}

// 添加断点（需管理员权限，实际修改目标进程内存）
static bool AddBreakpoint(uintptr_t address)
{
    // 检查断点是否已存在
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
        [address](const KswordBreakpointInfo& bp) { return bp.address == address; });
    if (it != s_breakpoints.end())
    {
        kLog.warn(C("断点已存在: " + FormatAddress(address)), C("Breakpoint"));
        return false;
    }

    // 打开目标进程获取内存操作权限
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, s_currentPID);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        kLog.err(C("无法打开进程以添加断点，错误: " + std::to_string(GetLastError())), C("Breakpoint"));
        return false;
    }

    // 读取原始指令字节（断点会替换为INT3指令0xCC）
    BYTE originalByte;
    SIZE_T bytesRead;
    if (!ReadProcessMemory(hProcess, (LPCVOID)address, &originalByte, 1, &bytesRead) || bytesRead != 1)
    {
        kLog.err(C("读取内存失败，无法添加断点: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    // 写入INT3指令设置断点
    BYTE int3 = 0xCC;
    SIZE_T bytesWritten;
    if (!WriteProcessMemory(hProcess, (LPVOID)address, &int3, 1, &bytesWritten) || bytesWritten != 1)
    {
        kLog.err(C("写入内存失败，无法添加断点: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    // 查找地址所属模块
    std::string moduleName = "未知模块";
    for (const auto& mod : s_modules)
    {
        if (address >= mod.baseAddr && address < mod.baseAddr + mod.size)
        {
            size_t lastSlash = mod.path.find_last_of("\\/");
            moduleName = (lastSlash != std::string::npos) ? mod.path.substr(lastSlash + 1) : mod.path;
            break;
        }
    }

    // 保存断点信息（包含原始字节用于恢复）
    s_breakpoints.push_back({
        address,
        moduleName,
        "软件断点 (INT3)",
        true,
        0,
        originalByte  // 新增：保存原始指令字节
        });

    kLog.info(C("已添加断点: " + FormatAddress(address)), C("Breakpoint"));
    CloseHandle(hProcess);
    return true;
}

// 移除断点（恢复原始指令）
static bool RemoveBreakpoint(uintptr_t address)
{
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
        [address](const KswordBreakpointInfo& bp) { return bp.address == address; });
    if (it == s_breakpoints.end())
    {
        kLog.warn(C("未找到断点: " + FormatAddress(address)), C("Breakpoint"));
        return false;
    }

    // 打开进程恢复原始指令
    HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, s_currentPID);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        kLog.err(C("无法打开进程以移除断点，错误: " + std::to_string(GetLastError())), C("Breakpoint"));
        return false;
    }

    // 恢复原始字节
    SIZE_T bytesWritten;
    if (!WriteProcessMemory(hProcess, (LPVOID)address, &it->originalByte, 1, &bytesWritten) || bytesWritten != 1)
    {
        kLog.err(C("恢复内存失败，无法移除断点: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    s_breakpoints.erase(it);
    kLog.info(C("已移除断点: " + FormatAddress(address)), C("Breakpoint"));
    CloseHandle(hProcess);
    return true;
}

// 切换断点启用/禁用状态（动态修改内存）
static bool ToggleBreakpoint(uintptr_t address)
{
    auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
        [address](const KswordBreakpointInfo& bp) { return bp.address == address; });
    if (it == s_breakpoints.end())
    {
        kLog.warn(C("未找到断点: " + FormatAddress(address)), C("Breakpoint"));
        return false;
    }

    // 打开进程准备修改内存
    HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, s_currentPID);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE)
    {
        kLog.err(C("无法打开进程以切换断点，错误: " + std::to_string(GetLastError())), C("Breakpoint"));
        return false;
    }

    bool newState = !it->enabled;
    BYTE targetByte = newState ? 0xCC : it->originalByte;  // 启用则写入INT3，禁用则恢复原始字节

    SIZE_T bytesWritten;
    if (!WriteProcessMemory(hProcess, (LPVOID)address, &targetByte, 1, &bytesWritten) || bytesWritten != 1)
    {
        kLog.err(C("切换断点状态失败: " + FormatAddress(address)), C("Breakpoint"));
        CloseHandle(hProcess);
        return false;
    }

    it->enabled = newState;
    kLog.info(C((newState ? "已启用断点: " : "已禁用断点: ") + FormatAddress(address)), C("Breakpoint"));
    CloseHandle(hProcess);
    return true;
}