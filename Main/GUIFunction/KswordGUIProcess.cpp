#include "../KswordTotalHead.h"

struct ProcessInfo {
    std::string name;
    int    pid;
    std::string user;
};

// 排序类型
enum SortType {
    SortType_Name,
    SortType_PID,
    SortType_User,
    SortType_None
};
std::vector<ProcessInfo> GetProcessList();
static char filter_text[128] = "";
static SortType current_sort = SortType_PID;
static bool sort_ascending = true;
static int selected_pid = -1;

// 全局状态
static std::vector<ProcessInfo> dummy_processes = GetProcessList();

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



std::vector<ProcessInfo> GetProcessList() {
    std::vector<ProcessInfo> processes;

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
        ProcessInfo info{};
        info.pid = pe32.th32ProcessID;

        if (!pe32.szExeFile) continue;
        info.name = WCharToUTF8(pe32.szExeFile);

        // 获取用户信息
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            FALSE, info.pid);
        if (hProcess) {
            HANDLE hToken;
            if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                DWORD neededSize = 0;
                GetTokenInformation(hToken, TokenUser, NULL, 0, &neededSize);

                if (neededSize > 0) {
                    std::vector<BYTE> tokenUserBuf(neededSize);
                    PTOKEN_USER pTokenUser = (PTOKEN_USER)tokenUserBuf.data();

                    if (GetTokenInformation(hToken, TokenUser, pTokenUser, neededSize, &neededSize)) {
                        WCHAR userName[256] = { 0 };
                        WCHAR domainName[256] = { 0 };
                        DWORD userNameSize = 256;
                        DWORD domainNameSize = 256;
                        SID_NAME_USE sidType;

                        if (LookupAccountSidW(
                            NULL,
                            pTokenUser->User.Sid,
                            userName,
                            &userNameSize,
                            domainName,
                            &domainNameSize,
                            &sidType))
                        {
                            info.user = WCharToUTF8(userName);
                        }
                    }
                }
                CloseHandle(hToken);
            }
            CloseHandle(hProcess);
        }

        processes.push_back(std::move(info));
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return processes;
}


static void UpdateFilterAndSort(std::vector<const ProcessInfo*>& filtered,
    const char* filter, SortType sort_type, bool ascending)
{
    filtered.clear();

    // 过滤
    for (const auto& proc : dummy_processes) {
        if (filter[0] == '\0' ||
            strstr(proc.name.data(), filter) ||
            strstr(proc.user.data(), filter)) {
            filtered.push_back(&proc);
        }
    }

    // 排序
    if (sort_type != SortType_None) {
        std::sort(filtered.begin(), filtered.end(),
            [sort_type, ascending](const ProcessInfo* a, const ProcessInfo* b) {
                int cmp = 0;
                switch (sort_type) {
                case SortType_Name: cmp = strcmp(a->name.data(), b->name.data()); break;
                case SortType_PID:  cmp = a->pid - b->pid; break;
                case SortType_User: cmp = strcmp(a->user.data(), b->user.data()); break;
                default: break;
                }
                return ascending ? (cmp < 0) : (cmp > 0);
            });
    }
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
        static std::vector<const ProcessInfo*> filtered_processes;
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
                   
                    const ProcessInfo* proc = filtered_processes[row];

                    ImGui::TableNextRow();
                    // 高亮选中行
                    bool is_selected = (selected_pid == proc->pid);
                    if (is_selected) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                            ImGui::GetColorU32(ImGuiCol_Header));
                    }
                    ImGui::PushID(row);  // 为每行设置唯一标识
                   

                    // 定义右键菜单内容
                    if (ImGui::BeginPopupContextWindow("RowContextMenu")) {
                        if (ImGui::BeginMenu("Terminate")) {
                            if (ImGui::MenuItem("taskkill")) {
                                std::thread(KillProcessByTaskkill,proc->pid).detach();
                            }
                            if (ImGui::MenuItem("taskkill /f")) {
                                // 优雅关闭逻辑

                            }
                            if (ImGui::MenuItem("Terminate Process")) {
                                // 优雅关闭逻辑
                            }
                            if (ImGui::MenuItem("Terminate Thread")) {
                                // 优雅关闭逻辑
                            }
                            if (ImGui::MenuItem("NT Terminate")) {
                                // 优雅关闭逻辑
                            }
                            ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem("Suspend")) {
                            // 执行编辑逻辑，例如弹出输入框
                        }
                        if (ImGui::MenuItem("UnSuspend")) {
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
                    ImGui::Text("%s", proc->name.c_str());

                    // PID列（右对齐）
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", proc->pid);

                    // 用户列
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", proc->user.c_str());

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
            // 刷新逻辑
        }

        ImGui::TreePop();

    }
    ImGui::EndTabItem();

}

