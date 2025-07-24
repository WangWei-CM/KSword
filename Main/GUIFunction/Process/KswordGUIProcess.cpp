#include "../../KswordTotalHead.h"

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

// 全局状态
static std::vector<kProcess> dummy_processes = GetProcessList();

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
								kLog.Add(Info, C(("使用taskkill /f终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()));
                            }
                            if (ImGui::MenuItem("Terminate Process")) {

								kLog.Add(Info, C(("使用Terminate终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()));
                           		proc->Terminate(kTerminate);
                            }
                            if (ImGui::MenuItem("Terminate Thread")) {
								kLog.Add(Info, C(("使用TerminateThread终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()));
								proc->Terminate(kTerminateThread);
                            }
                            if (ImGui::MenuItem("NT Terminate")) {
								kLog.Add(Info, C(("使用NT Terminate终止pid为" + std::to_string(proc->pid()) + "的进程").c_str()));
								proc->Terminate(kNTTerminate);
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
            kLog.Add(Info, C("刷新进程列表"));
            // 刷新逻辑
        }

        ImGui::TreePop();

    }
    ImGui::EndTabItem();

}

