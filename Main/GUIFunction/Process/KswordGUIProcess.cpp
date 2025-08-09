#include "../../KswordTotalHead.h"
#include <shlobj.h> 
#define CURRENT_MODULE "进程管理"
//struct ProcessInfo {
//    std::string name;
//    int    pid;
//    std::string user;
//};

// 排序类型
enum SortType {
    SortType_Name,
    SortType_PID,
    SortType_User,
    SortType_None
};
std::vector<kProcess> GetProcessList();
static char filter_text[128] = "";
static SortType current_sort = SortType_PID;
static bool sort_ascending = true;
static int selected_pid = -1;


// CreateProcess参数存储
static std::wstring g_modulePath;          // 模块路径（宽字符，适配Windows API）
static std::wstring g_cmdLineArgs;         // 命令行参数
static DWORD g_creationFlags = 0;          // dwCreationFlags
static std::wstring g_environment;         // 环境变量（用户输入）
static std::wstring g_workingDir;          // 工作目录
static STARTUPINFOW g_startupInfo = { 0 };   // STARTUPINFO
static PROCESS_INFORMATION g_procInfo = { 0 };// PROCESS_INFORMATION
// 全局输入框变量（扩大作用域）
static char modulePathUtf8[1024] = "C:\\Windows\\System32\\cmd.exe";
static char cmdLineUtf8[1024] = "";
static char workDirUtf8[1024] = "";
static char envPid[32] = "";

// 标志位状态变量（修复左值问题）
static bool flagUseDefaultEnv = true;
static bool flagCreateNewConsole = false;           // CREATE_NEW_CONSOLE
static bool flagCreateSuspended = false;            // CREATE_SUSPENDED
static bool flagCreateNoWindow = false;             // CREATE_NO_WINDOW
static bool flagDebugProcess = false;               // DEBUG_PROCESS
static bool flagDebugOnlyThis = false;              // DEBUG_ONLY_THIS_PROCESS
static bool flagNewProcessGroup = false;            // CREATE_NEW_PROCESS_GROUP
static bool flagUnicodeEnv = false;                 // CREATE_UNICODE_ENVIRONMENT
static bool flagSeparateWowVdm = false;             // CREATE_SEPARATE_WOW_VDM
static bool flagSharedWowVdm = false;               // CREATE_SHARED_WOW_VDM
static bool flagInheritAffinity = false;            // INHERIT_PARENT_AFFINITY
static bool flagProtectedProcess = false;           // CREATE_PROTECTED_PROCESS
static bool flagExtendedStartup = false;            // EXTENDED_STARTUPINFO_PRESENT
static bool flagBreakawayFromJob = false;           // CREATE_BREAKAWAY_FROM_JOB
static bool flagPreserveCodeAuthz = false;          // CREATE_PRESERVE_CODE_AUTHZ_LEVEL

static char lpDesktopUtf8[256] = "";
static char lpTitleUtf8[256] = "";
static bool flagStartfUseShowWindow = false;
static bool flagStartfUseSize = false;
static bool flagStartfUsePosition = false;
static bool flagStartfUseCountChars = false;
static bool flagStartfUseFillAttribute = false;
static bool flagStartfForceOnFeedback = false;
static bool flagStartfForceOffFeedback = false;
static bool flagStartfUseStdHandles = false;
static int showWindowMode = SW_SHOW;

// 全局状态
static std::vector<kProcess> dummy_processes = GetProcessList();
bool SelectExecutableFile(wchar_t* outPath, size_t maxLen) {
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    wchar_t fileName[1024] = L"";

    ofn.hwndOwner = GetForegroundWindow();  // 使用当前活跃窗口作为父窗口
    ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";  // 文件筛选器
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = sizeof(fileName) / sizeof(wchar_t);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;  // 确保文件存在
    ofn.lpstrTitle = L"选择可执行文件";  // 对话框标题

    if (GetOpenFileNameW(&ofn)) {
        wcsncpy_s(outPath, maxLen, fileName, _TRUNCATE);
        return true;
    }
    return false;
}

// 文件夹选择对话框函数
bool SelectDirectory(wchar_t* outPath, size_t maxLen) {
    BROWSEINFOW bi = { 0 };
    LPITEMIDLIST pidl;
    wchar_t displayName[MAX_PATH] = L"";

    bi.hwndOwner = GetForegroundWindow();
    bi.lpszTitle = L"选择工作目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;  // 只返回文件系统目录

    pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr) {
        if (SHGetPathFromIDListW(pidl, displayName)) {
            wcsncpy_s(outPath, maxLen, displayName, _TRUNCATE);
            CoTaskMemFree(pidl);  // 释放内存
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

std::string WCharToUTF8(const WCHAR* wstr) {
    if (!wstr || *wstr == L'\0')
        return std::string();

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::system_error(GetLastError(), std::system_category(), "WideCharToMultiByte failed");
    }

    std::string str(len, 0);
    int result = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str.data(), len, nullptr, nullptr);
    if (result == 0) {
        throw std::system_error(GetLastError(), std::system_category(), "WideCharToMultiByte failed");
    }

    str.resize(len - 1); // 移除末尾的null
    return str;
}



std::vector<kProcess> GetProcessList() {
    std::vector<kProcess> processes;

    // 创建进程快照
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return processes;
    }

    do {
        kProcess info(pe32.th32ProcessID);
        processes.push_back(std::move(info));
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return processes;
}


static void UpdateFilterAndSort(std::vector<kProcess*>& filtered,
    const char* filter, SortType sort_type, bool ascending)
{
    filtered.clear();

    // 过滤
    for (auto& proc : dummy_processes) {
        if (filter[0] == '\0' ||
            strstr(proc.Name().data(), filter) ||
            strstr(proc.User().data(), filter)) {
            filtered.push_back(&proc);
        }
    }

    // 排序
    if (sort_type != SortType_None) {
        std::sort(filtered.begin(), filtered.end(),
            [sort_type, ascending](const kProcess* a, const kProcess* b) {
                int cmp = 0;
                switch (sort_type) {
                case SortType_Name: cmp = strcmp(a->Name().data(), b->Name().data()); break;
                case SortType_PID:  cmp = a->pid() - b->pid(); break;
                case SortType_User: cmp = strcmp(a->User().data(), b->User().data()); break;
                default: break;
                }
                return ascending ? (cmp < 0) : (cmp > 0);
            });
    }
}

static void KCreateProcessWithSuspendFollower(DWORD pid) {
    int Kpid = kItem.AddProcess(C(std::string("PID为" + std::to_string(pid) + "的进程启动")),
        std::string(C("取消挂起PID为" + std::to_string(pid) + "的进程启动")), NULL, 0.98f);
    kItem.UI(Kpid,C( "点击按钮以继续运行该进程"), 1);
    UnSuspendProcess(pid);
    kItem.SetProcess(Kpid,"",1.00f);
    return;
    
}

void KswordGUIProcess() {
    if (ImGui::TreeNode("Process List"))
    {
        // 设置表格（三列，带边框）
// 假设的进程数据结构
                        // 过滤输入框
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15);
        if (ImGui::InputTextWithHint("##filter", "Search (name/user)...",
            filter_text, IM_ARRAYSIZE(filter_text))) {
            // 过滤文本变化时自动更新
        }

        // 准备过滤和排序后的数据
        static std::vector< kProcess*> filtered_processes;
        UpdateFilterAndSort(filtered_processes, filter_text, current_sort, sort_ascending);

        // 表格设置
        const float footer_height = ImGui::GetStyle().ItemSpacing.y +
            ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##table_container",
            ImVec2(0, -footer_height), // 留出底部空间
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar);

        if (ImGui::BeginTable("ProcessTable", 3,
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit |

            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable))
        {
            // 表头

            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f, SortType_Name);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f, SortType_PID);
            ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthStretch, 0.0f, SortType_User);
            ImGui::TableSetupScrollFreeze(0, 1);
            if (ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs())
            {
                if (sorts_specs->SpecsDirty)
                {
                    // 获取最新的排序设置
                    current_sort = static_cast<SortType>(sorts_specs->Specs[0].ColumnUserID);
                    sort_ascending = (sorts_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                    sorts_specs->SpecsDirty = false;
                }
            }
            ImGui::TableHeadersRow();

            // 排序指示
            if (current_sort != SortType_None) {
                ImGui::TableSetColumnIndex(current_sort);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                const char* sort_order = sort_ascending ? " (ASC)" : " (DESC)";
                ImGui::TextDisabled(sort_order);
                ImGui::PopStyleVar();
            }

            // 内容行（使用clipper优化滚动性能）
            ImGuiListClipper clipper;
            clipper.Begin(filtered_processes.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                   
                    kProcess* proc = filtered_processes[row];

                    ImGui::TableNextRow();
                    // 高亮选中行
                    bool is_selected = (selected_pid == proc->pid());
                    if (is_selected) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                            ImGui::GetColorU32(ImGuiCol_Header));
                    }
                    ImGui::PushID(row);  // 为每行设置唯一标识
                   

                    // 定义右键菜单内容
                    if (ImGui::BeginPopupContextWindow("RowContextMenu")) {
                        if (ImGui::BeginMenu("Terminate")) {
                            if (ImGui::MenuItem("taskkill")) {
                                std::thread(KillProcessByTaskkill,proc->pid()).detach();
                            }
                            if (ImGui::MenuItem("taskkill /f")) {
                                // 优雅关闭逻辑
                                proc->Terminate(kTaskkillF);
								kLog.Add(Info, C(("使用taskkill /f终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()), C(CURRENT_MODULE));
                            }
                            if (ImGui::MenuItem("Terminate Process")) {

								kLog.Add(Info, C(("使用Terminate终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()), C(CURRENT_MODULE));
                           		proc->Terminate(kTerminate);
                            }
                            if (ImGui::MenuItem("Terminate Thread")) {
								kLog.Add(Info, C(("使用TerminateThread终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()), C(CURRENT_MODULE));
								proc->Terminate(kTerminateThread);
                            }
                            if (ImGui::MenuItem("NT Terminate")) {
								kLog.Add(Info, C(("使用NT Terminate终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()), C(CURRENT_MODULE));
								proc->Terminate(kNTTerminate);
                            }
                            ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem("Suspend")) {
                            // 执行编辑逻辑，例如弹出输入框
                        }
                        if (ImGui::MenuItem(C("取消挂起"))) {
                            // 执行编辑逻辑，例如弹出输入框
                        }
                        if (ImGui::MenuItem("Information")) {
                            // 执行编辑逻辑，例如弹出输入框
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::PopID();


                    // 名称列
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", proc->Name().c_str());

                    // PID列（右对齐）
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", proc->pid());

                    // 用户列
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", proc->User().c_str());

                }
            }
            clipper.End();

            ImGui::EndTable();
        }
        ImGui::EndChild();

        // 底部状态栏
        ImGui::Separator();
        ImGui::Text("Processes: %d / %d", filtered_processes.size(), dummy_processes.size());
        ImGui::SameLine(ImGui::GetWindowWidth() - 120);
        if (ImGui::SmallButton("Refresh")) {
            dummy_processes = GetProcessList();
            kLog.Add(Info, C("刷新进程列表"), C(CURRENT_MODULE));
            // 刷新逻辑
        }

        ImGui::TreePop();

    }
    if (ImGui::TreeNode("CreateProcess")){
    ImGui::InputTextWithHint(C("##ModulePath"), C("模块路径（如C:\\Windows\\notepad.exe）"), modulePathUtf8, sizeof(modulePathUtf8));
    ImGui::SameLine();
    if (ImGui::Button(C("浏览文件 ##CreateProcess1"))) {
        wchar_t selectedPath[1024] = L"";
        if (SelectExecutableFile(selectedPath, _countof(selectedPath))) {
            g_modulePath = selectedPath;
            WideCharToMultiByte(CP_UTF8, 0, selectedPath, -1, modulePathUtf8, sizeof(modulePathUtf8), nullptr, nullptr);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(C("粘贴 ##CreateProcess2")))
    {
        const wchar_t* demoPath = L"C:\\粘贴的路径.exe";
        g_modulePath = demoPath;
        WideCharToMultiByte(CP_UTF8, 0, demoPath, -1, modulePathUtf8, sizeof(modulePathUtf8), nullptr, nullptr);
    }

    // 2. 命令行参数
    ImGui::InputTextWithHint(C("##CmdLine"), C("命令行参数（如test.txt）"), cmdLineUtf8, sizeof(cmdLineUtf8));
    ImGui::SameLine();
    if (ImGui::Button(C("粘贴 ##CreateProcess3")))
    {
        const wchar_t* demoArgs = L"粘贴的参数";
        g_cmdLineArgs = demoArgs;
        WideCharToMultiByte(CP_UTF8, 0, demoArgs, -1, cmdLineUtf8, sizeof(cmdLineUtf8), nullptr, nullptr);
    }

    // 3. dwCreationFlags选项（修复左值问题）
    ImGui::Separator();
    ImGui::Text(C("进程创建标志:"));
    ImGui::Columns(2, nullptr, false); // 分为两栏
    if (ImGui::Checkbox(C("CREATE_NEW_CONSOLE（新控制台）"), &flagCreateNewConsole))
    {
        g_creationFlags = flagCreateNewConsole ? (g_creationFlags | CREATE_NEW_CONSOLE) : (g_creationFlags & ~CREATE_NEW_CONSOLE);
    }
    if (ImGui::Checkbox(C("CREATE_SUSPENDED（挂起进程）"), &flagCreateSuspended))
    {
        g_creationFlags = flagCreateSuspended ? (g_creationFlags | CREATE_SUSPENDED) : (g_creationFlags & ~CREATE_SUSPENDED);
    }
    if (ImGui::Checkbox(C("CREATE_NO_WINDOW（无窗口）"), &flagCreateNoWindow))
    {
        g_creationFlags = flagCreateNoWindow ? (g_creationFlags | CREATE_NO_WINDOW) : (g_creationFlags & ~CREATE_NO_WINDOW);
    }
    if (ImGui::Checkbox(C("DEBUG_PROCESS（调试进程）"), &flagDebugProcess))
    {
        g_creationFlags = flagDebugProcess ? (g_creationFlags | DEBUG_PROCESS) : (g_creationFlags & ~DEBUG_PROCESS);
    }
    if (ImGui::Checkbox(C("DEBUG_ONLY_THIS_PROCESS"), &flagDebugOnlyThis))
    {
        g_creationFlags = flagDebugOnlyThis ? (g_creationFlags | DEBUG_ONLY_THIS_PROCESS) : (g_creationFlags & ~DEBUG_ONLY_THIS_PROCESS);
    }
    if (ImGui::Checkbox(C("CREATE_NEW_PROCESS_GROUP"), &flagNewProcessGroup))
    {
        g_creationFlags = flagNewProcessGroup ? (g_creationFlags | CREATE_NEW_PROCESS_GROUP) : (g_creationFlags & ~CREATE_NEW_PROCESS_GROUP);
    }
    if (ImGui::Checkbox(C("CREATE_UNICODE_ENVIRONMENT"), &flagUnicodeEnv))
    {
        g_creationFlags = flagUnicodeEnv ? (g_creationFlags | CREATE_UNICODE_ENVIRONMENT) : (g_creationFlags & ~CREATE_UNICODE_ENVIRONMENT);
    }

    ImGui::NextColumn(); // 切换到第二栏

    // 第二栏标志位
    if (ImGui::Checkbox(C("CREATE_SEPARATE_WOW_VDM"), &flagSeparateWowVdm))
    {
        g_creationFlags = flagSeparateWowVdm ? (g_creationFlags | CREATE_SEPARATE_WOW_VDM) : (g_creationFlags & ~CREATE_SEPARATE_WOW_VDM);
    }
    if (ImGui::Checkbox(C("CREATE_SHARED_WOW_VDM"), &flagSharedWowVdm))
    {
        g_creationFlags = flagSharedWowVdm ? (g_creationFlags | CREATE_SHARED_WOW_VDM) : (g_creationFlags & ~CREATE_SHARED_WOW_VDM);
    }
    if (ImGui::Checkbox(C("INHERIT_PARENT_AFFINITY"), &flagInheritAffinity))
    {
        g_creationFlags = flagInheritAffinity ? (g_creationFlags | INHERIT_PARENT_AFFINITY) : (g_creationFlags & ~INHERIT_PARENT_AFFINITY);
    }
    if (ImGui::Checkbox(C("CREATE_PROTECTED_PROCESS"), &flagProtectedProcess))
    {
        g_creationFlags = flagProtectedProcess ? (g_creationFlags | CREATE_PROTECTED_PROCESS) : (g_creationFlags & ~CREATE_PROTECTED_PROCESS);
    }
    if (ImGui::Checkbox(C("EXTENDED_STARTUPINFO_PRESENT"), &flagExtendedStartup))
    {
        g_creationFlags = flagExtendedStartup ? (g_creationFlags | EXTENDED_STARTUPINFO_PRESENT) : (g_creationFlags & ~EXTENDED_STARTUPINFO_PRESENT);
    }
    if (ImGui::Checkbox(C("CREATE_BREAKAWAY_FROM_JOB"), &flagBreakawayFromJob))
    {
        g_creationFlags = flagBreakawayFromJob ? (g_creationFlags | CREATE_BREAKAWAY_FROM_JOB) : (g_creationFlags & ~CREATE_BREAKAWAY_FROM_JOB);
    }
    if (ImGui::Checkbox(C("CREATE_PRESERVE_CODE_AUTHZ_LEVEL"), &flagPreserveCodeAuthz))
    {
        g_creationFlags = flagPreserveCodeAuthz ? (g_creationFlags | CREATE_PRESERVE_CODE_AUTHZ_LEVEL) : (g_creationFlags & ~CREATE_PRESERVE_CODE_AUTHZ_LEVEL);
    }

    ImGui::Columns(1); // 恢复单栏布局

    // 4. 环境变量
    ImGui::Separator();
    ImGui::Text(C("环境变量:"));
    ImGui::InputText(C("##PID"), envPid, sizeof(envPid));
    ImGui::SameLine();
    if (ImGui::Button(C("获取进程环境")))
    {
        g_environment = L"自定义环境变量=值";
    }
    if (ImGui::Checkbox(C("使用默认环境（NULL）"), &flagUseDefaultEnv))
    {
        if (flagUseDefaultEnv) g_environment.clear();
    }

    // 5. 工作目录
    ImGui::Separator();
    ImGui::InputTextWithHint(C("##WorkDir"), C("工作目录"), workDirUtf8, sizeof(workDirUtf8)); ImGui::SameLine();
    if (ImGui::Button(C("浏览 ##CreateProcess4"))) {
        wchar_t selectedPath[1024] = L"";
        if (SelectExecutableFile(selectedPath, _countof(selectedPath))) {
            g_modulePath = selectedPath;
            WideCharToMultiByte(CP_UTF8, 0, selectedPath, -1, modulePathUtf8, sizeof(modulePathUtf8), nullptr, nullptr);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(C("粘贴 ##CreateProcess5")))
    {
        const wchar_t* demoDir = L"C:\\粘贴的目录";
        g_workingDir = demoDir;
        WideCharToMultiByte(CP_UTF8, 0, demoDir, -1, workDirUtf8, sizeof(workDirUtf8), nullptr, nullptr);
    }

    // 基础信息
    ImGui::Text(C("结构体大小: %d 字节"), g_startupInfo.cb);
    ImGui::InputText(C("桌面名称 (lpDesktop)"), lpDesktopUtf8, sizeof(lpDesktopUtf8));
    ImGui::InputText(C("窗口标题 (lpTitle)"), lpTitleUtf8, sizeof(lpTitleUtf8));

    // 窗口位置
    ImGui::Separator();
    ImGui::Checkbox(C("使用窗口位置 (STARTF_USEPOSITION)"), &flagStartfUsePosition);
    ImGui::SameLine();
    if (ImGui::Button(C("应用位置"))) {
        g_startupInfo.dwFlags = flagStartfUsePosition ? (g_startupInfo.dwFlags | STARTF_USEPOSITION) : (g_startupInfo.dwFlags & ~STARTF_USEPOSITION);
    }

    ImGui::PushItemWidth(150);
    ImGui::InputInt(C("窗口X坐标 (dwX)"), (int*)&g_startupInfo.dwX); ImGui::SameLine();
    ImGui::InputInt(C("窗口Y坐标 (dwY)"), (int*)&g_startupInfo.dwY);
    ImGui::PopItemWidth();

    // 窗口大小
    ImGui::Separator();
    ImGui::Checkbox(C("使用窗口大小 (STARTF_USESIZE)"), &flagStartfUseSize);
    ImGui::SameLine();
    if (ImGui::Button(C("应用大小"))) {
        g_startupInfo.dwFlags = flagStartfUseSize ? (g_startupInfo.dwFlags | STARTF_USESIZE) : (g_startupInfo.dwFlags & ~STARTF_USESIZE);
    }

    ImGui::PushItemWidth(150);
    ImGui::InputInt(C("窗口宽度 (dwXSize)"), (int*)&g_startupInfo.dwXSize); ImGui::SameLine();
    ImGui::InputInt(C("窗口高度 (dwYSize)"), (int*)&g_startupInfo.dwYSize);
    ImGui::PopItemWidth();

    // 控制台设置
    ImGui::Separator();
    ImGui::Text(C("控制台窗口设置:"));

    ImGui::Checkbox(C("使用字符计数 (STARTF_USECOUNTCHARS)"), &flagStartfUseCountChars);
    ImGui::SameLine();
    if (ImGui::Button(C("应用字符计数"))) {
        g_startupInfo.dwFlags = flagStartfUseCountChars ? (g_startupInfo.dwFlags | STARTF_USECOUNTCHARS) : (g_startupInfo.dwFlags & ~STARTF_USECOUNTCHARS);
    }

    ImGui::PushItemWidth(150);
    ImGui::InputInt(C("字符宽度 (dwXCountChars)"), (int*)&g_startupInfo.dwXCountChars); ImGui::SameLine();
    ImGui::InputInt(C("字符高度 (dwYCountChars)"), (int*)&g_startupInfo.dwYCountChars);
    ImGui::PopItemWidth();

    ImGui::Checkbox(C("使用填充属性 (STARTF_USEFILLATTRIBUTE)"), &flagStartfUseFillAttribute);
    ImGui::SameLine();
    if (ImGui::Button(C("应用填充属性"))) {
        g_startupInfo.dwFlags = flagStartfUseFillAttribute ? (g_startupInfo.dwFlags | STARTF_USEFILLATTRIBUTE) : (g_startupInfo.dwFlags & ~STARTF_USEFILLATTRIBUTE);
    }

    ImGui::ColorEdit4(C("填充颜色 (dwFillAttribute)"), (float*)&g_startupInfo.dwFillAttribute, ImGuiColorEditFlags_Uint8);

    // 窗口显示设置
    ImGui::Separator();
    ImGui::Checkbox(C("使用显示设置 (STARTF_USESHOWWINDOW)"), &flagStartfUseShowWindow);
    ImGui::SameLine();
    if (ImGui::Button(C("应用显示设置"))) {
        g_startupInfo.dwFlags = flagStartfUseShowWindow ? (g_startupInfo.dwFlags | STARTF_USESHOWWINDOW) : (g_startupInfo.dwFlags & ~STARTF_USESHOWWINDOW);
        g_startupInfo.wShowWindow = showWindowMode;
    }

    ImGui::Combo(C("显示模式 (wShowWindow)"), &showWindowMode,
        C("SW_HIDE=0\0SW_SHOWNORMAL=1\0SW_NORMAL=1\0SW_SHOWMINIMIZED=2\0SW_SHOWMAXIMIZED=3\0SW_MAXIMIZE=3\0SW_SHOWNOACTIVATE=4\0SW_SHOW=5\0SW_MINIMIZE=6\0SW_SHOWMINNOACTIVE=7\0SW_SHOWNA=8\0SW_RESTORE=9\0SW_SHOWDEFAULT=10\0SW_FORCEMINIMIZE=11\0"));

    // 其他标志
    ImGui::Separator();
    ImGui::Text(C("其他标志:"));
    if (ImGui::Checkbox(C("STARTF_FORCEONFEEDBACK"), &flagStartfForceOnFeedback)) {
        g_startupInfo.dwFlags = flagStartfForceOnFeedback ? (g_startupInfo.dwFlags | STARTF_FORCEONFEEDBACK) : (g_startupInfo.dwFlags & ~STARTF_FORCEONFEEDBACK);
    }
    if (ImGui::Checkbox(C("STARTF_FORCEOFFFEEDBACK"), &flagStartfForceOffFeedback)) {
        g_startupInfo.dwFlags = flagStartfForceOffFeedback ? (g_startupInfo.dwFlags | STARTF_FORCEOFFFEEDBACK) : (g_startupInfo.dwFlags & ~STARTF_FORCEOFFFEEDBACK);
    }
    if (ImGui::Checkbox(C("STARTF_USESTDHANDLES"), &flagStartfUseStdHandles)) {
        g_startupInfo.dwFlags = flagStartfUseStdHandles ? (g_startupInfo.dwFlags | STARTF_USESTDHANDLES) : (g_startupInfo.dwFlags & ~STARTF_USESTDHANDLES);
    }

    //// 标准句柄设置（仅作展示，实际使用需要设置有效句柄）
    //if (flagStartfUseStdHandles) {
    //    ImGui::Indent();
    //    ImGui::Text(C("标准输入句柄 (hStdInput): 0x%p"), g_startupInfo.hStdInput);
    //    ImGui::Text(C("标准输出句柄 (hStdOutput): 0x%p"), g_startupInfo.hStdOutput);
    //    ImGui::Text(C("标准错误句柄 (hStdError): 0x%p"), g_startupInfo.hStdError);
    //    ImGui::Unindent();
    //}

    // 应用按钮
    ImGui::Separator();
    if (ImGui::Button(C("应用所有设置"), ImVec2(-1, 0))) {
        // 同步字符串到宽字符
        MultiByteToWideChar(CP_UTF8, 0, lpDesktopUtf8, -1, g_startupInfo.lpDesktop, 256);
        MultiByteToWideChar(CP_UTF8, 0, lpTitleUtf8, -1, g_startupInfo.lpTitle, 256);

        // 同步标志位
        g_startupInfo.dwFlags = 0;
        if (flagStartfUsePosition) g_startupInfo.dwFlags |= STARTF_USEPOSITION;
        if (flagStartfUseSize) g_startupInfo.dwFlags |= STARTF_USESIZE;
        if (flagStartfUseCountChars) g_startupInfo.dwFlags |= STARTF_USECOUNTCHARS;
        if (flagStartfUseFillAttribute) g_startupInfo.dwFlags |= STARTF_USEFILLATTRIBUTE;
        if (flagStartfUseShowWindow) {
            g_startupInfo.dwFlags |= STARTF_USESHOWWINDOW;
            g_startupInfo.wShowWindow = showWindowMode;
        }
        if (flagStartfForceOnFeedback) g_startupInfo.dwFlags |= STARTF_FORCEONFEEDBACK;
        if (flagStartfForceOffFeedback) g_startupInfo.dwFlags |= STARTF_FORCEOFFFEEDBACK;
        if (flagStartfUseStdHandles) g_startupInfo.dwFlags |= STARTF_USESTDHANDLES;

        kLog.Add(Info, C("STARTUPINFOW 设置已应用"), C(CURRENT_MODULE));
    }
    // 6. 调用CreateProcess
    ImGui::Separator();
    if (ImGui::Button(C("调用CreateProcess")))
    {
        // 转换UTF8输入到宽字符（修复类型不匹配）
        wchar_t modulePathW[1024] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, modulePathUtf8, -1, modulePathW, _countof(modulePathW));

        wchar_t cmdLineW[1024] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, cmdLineUtf8, -1, cmdLineW, _countof(cmdLineW));

        wchar_t workingDirW[1024] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, workDirUtf8, -1, workingDirW, _countof(workingDirW));

        // 调用CreateProcess
        STARTUPINFOW si = g_startupInfo;
        PROCESS_INFORMATION pi = { 0 };
        BOOL bSuccess = CreateProcessW(
            modulePathW,                  // 模块路径
            cmdLineW,                     // 命令行参数
            nullptr,                      // 进程安全属性
            nullptr,                      // 线程安全属性
            FALSE,                        // 不继承句柄
            g_creationFlags,              // 创建标志
            flagUseDefaultEnv ? nullptr : (LPVOID)g_environment.c_str(),  // 环境变量
            *workDirUtf8 ? workingDirW : nullptr,  // 工作目录
            &si,                          // 启动信息
            &pi                           // 进程信息
        );

        if (bSuccess)
        {
            kLog.Add(Info, C(("CreateProcess 调用成功！PID:" + std::to_string(pi.dwProcessId)).c_str()), C(CURRENT_MODULE));
            g_procInfo = pi;
            CloseHandle(pi.hThread); // 及时释放不需要的句柄
        }
        else
        {
            kLog.Add(Err, C(("CreateProcess 调用失败！错误码:" + std::to_string(GetLastError())).c_str()), C(CURRENT_MODULE));
        }
        if (flagCreateSuspended == true) {
            std::thread(KCreateProcessWithSuspendFollower, pi.dwProcessId).detach();
        }
    }
    ImGui::TreePop();
    }    
    ImGui::EndTabItem();

}

#undef CURRENT_MODULE