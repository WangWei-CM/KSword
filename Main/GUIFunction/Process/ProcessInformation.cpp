#include "../../KswordTotalHead.h"
#include "Process.h"
#include "ProcessDetail.h"
#include <winnt.h>  // 补充Windows权限常量定义
#include <sddl.h>
#include <comutil.h>
#define CURRENT_MODULE "进程详细信息管理"

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(
    HANDLE           ProcessHandle,
    DWORD            ProcessInformationClass,
    PVOID            ProcessInformation,
    ULONG            ProcessInformationLength,
    PULONG           ReturnLength);
// 修正后的 PROCESS_BASIC_INFORMATION 定义
typedef struct _MY_PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    PVOID BasePriority;  // 保持为 PVOID 类型
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
// 新增：NtQueryInformationProcess所需的信息类常量
const DWORD ProcessThreadCount = 0x03;
const DWORD ProcessHandleCount = 0x04;
const DWORD ProcessBasePriority = 0x05;
const DWORD ProcessExitStatus = 0x01;
const DWORD ProcessAffinityMask = 0x1D;
const DWORD ProcessSessionInformation = 0x1D;  // 不同系统可能有差异，需测试

// 新增：线程数信息结构体
typedef struct _PROCESS_THREAD_COUNT_INFORMATION {
    ULONG ThreadCount;
} PROCESS_THREAD_COUNT_INFORMATION, * PPROCESS_THREAD_COUNT_INFORMATION;

// 新增：句柄数信息结构体
typedef struct _PROCESS_HANDLE_COUNT_INFORMATION {
    ULONG HandleCount;
} PROCESS_HANDLE_COUNT_INFORMATION, * PPROCESS_HANDLE_COUNT_INFORMATION;

// 新增：会话信息结构体
typedef struct _PROCESS_SESSION_INFORMATION {
    DWORD SessionId;
} PROCESS_SESSION_INFORMATION, * PPROCESS_SESSION_INFORMATION;
// 新增：FILETIME转本地时间字符串
std::string kProcessDetail::FileTimeToString(FILETIME ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
        return "N/A";  // 未设置（如运行中进程的退出时间）
    }

    SYSTEMTIME stLocal;
    FileTimeToLocalFileTime(&ft, &ft);  // 转换为本地时间
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
        kLog.err(C("无法打开进程获取父进程信息"), CURRENT_MODULE);
        parentPid = 0;
        return false;
    }

    // 使用NtQueryInformationProcess获取PROCESS_BASIC_INFORMATION
    typedef struct _PROCESS_BASIC_INFORMATION {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;   // 当前进程PID（验证用）
        ULONG_PTR InheritedFromUniqueProcessId; // 父进程PID
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
        // 验证当前进程PID是否匹配（确保获取正确）
        if (pbi.UniqueProcessId == (ULONG_PTR)pid()) {
            parentPid = (DWORD)pbi.InheritedFromUniqueProcessId;
            // 通过父进程PID查询名称
            parentProcessName = GetProcessNameByPID(parentPid);
        }
        else {
            parentPid = 0;
            parentProcessName = "获取失败（PID不匹配）";
        }
    }
    else {
        parentPid = 0;
        parentProcessName = "获取失败（NtQuery错误）";
    }

    CloseHandle(hProcess);
    return true;
}
// 新增：获取扩展进程信息
bool kProcessDetail::GetProcessExtendedInfo() {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid());
    if (!hProcess) {
        kLog.err(C("无法打开进程获取扩展信息"), CURRENT_MODULE);
        return false;
    }

    // 初始化所有信息为默认值
    creationTime = { 0, 0 };
    exitTime = { 0, 0 };
    kernelTime = { 0, 0 };
    userTime = { 0, 0 };
    threadCount = 0;
    handleCount = 0;
    basePriority = -1;   // 修正为 -1 表示未知
    exitStatus = -1;
    affinityMask = 0;
    sessionId = -1;

    // 获取基本时间（创建、退出、内核、用户时间）
    if (!GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime)) {
        kLog.warn(C("GetProcessTimes 调用失败"), CURRENT_MODULE);
    }

    // 使用 NtQueryInformationProcess 获取扩展信息
    typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(
        HANDLE, INT, PVOID, ULONG, PULONG);
    _NtQueryInformationProcess NtQIP = (_NtQueryInformationProcess)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) {
        CloseHandle(hProcess);
        return false;
    }

    // 获取 PROCESS_BASIC_INFORMATION 以获取基本优先级、亲和性掩码等
    _MY_PROCESS_BASIC_INFORMATION pbi = { 0 };
    ULONG returnLength = 0;
    NTSTATUS status = NtQIP(hProcess, 0, &pbi, sizeof(pbi), &returnLength);
    if (NT_SUCCESS(status)) {
        // 将 PVOID 类型的 BasePriority 转换为实际优先级值
        // 在 x86 系统上直接转换为 LONG，在 x64 上需要处理指针大小
#ifdef _WIN64
        basePriority = (LONG)((INT_PTR)pbi.BasePriority);
#else
        basePriority = (LONG)((DWORD)pbi.BasePriority);
#endif

        affinityMask = pbi.AffinityMask;
        exitStatus = pbi.ExitStatus;
    }


    // 获取线程数（替代方法：使用 ToolHelp API）
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

    // 获取句柄数（使用公开API）
    GetProcessHandleCount(hProcess, &handleCount);

    // 获取会话ID（使用公开API）
    ProcessIdToSessionId(pid(), &sessionId);    CloseHandle(hProcess);
    return true;
}std::string kProcessDetail::GetCommandLine() {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid());
    if (!hProcess) {
        return "无法打开进程 (权限不足)";
    }
    _NtQueryInformationProcess NtQIP = (_NtQueryInformationProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) {
        CloseHandle(hProcess);
        return "GetProcAddress 失败: NtQueryInformationProcess API 不可用";
    }

    // 获取 ProcessBasicInformation 以定位 PEB
    PROCESS_BASIC_INFORMATION pbi;
    ULONG len;
    NTSTATUS status = NtQIP(hProcess, 0, &pbi, sizeof(pbi), &len);
    if (status != 0) {
        CloseHandle(hProcess);
        return "无法获取进程信息";
    }

    // 读取 PEB 地址
    PEB peb;
    if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr)) {
        CloseHandle(hProcess);
        return "无法读取 PEB";
    }

    // 读取进程参数
    _MY_RTL_USER_PROCESS_PARAMETERS upp;
    if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &upp, sizeof(upp), nullptr)) {
        CloseHandle(hProcess);
        return "无法读取进程参数";
    }

    // 长度检查
    if (upp.CommandLine.Length == 0 ||
        upp.CommandLine.Length > 0xFFFF) {
        CloseHandle(hProcess);
        return "No command line";
    }

    // 读取命令行
    std::wstring wcmd;
    wcmd.resize(upp.CommandLine.Length / sizeof(WCHAR));
    if (!ReadProcessMemory(hProcess, upp.CommandLine.Buffer, wcmd.data(), upp.CommandLine.Length, nullptr)) {
        CloseHandle(hProcess);
        return "无法读取命令行";
    }
    CloseHandle(hProcess);
    return std::string(wcmd.begin(), wcmd.end());
}


kProcessDetail::kProcessDetail(DWORD pid) : kProcess(pid) {
    InitTokenInfo();
    InitDetailInfo();
}

void kProcessDetail::InitDetailInfo() {
    // 从父类获取进程详细信息
    processName = Name();
    processExePath = ExePath();
    processUser = User();
    isAdmin = IsAdmin();
	commandLine = GetCommandLine();
    GetProcessExtendedInfo();  // 新增：初始化扩展信息
	GetParentProcessInfo();    // 新增：获取父进程信息
}

static int is_selected = -1;
static int selectedToken = -1;

void kProcessDetail::Render() {
    // 窗口标题使用进程名称+PID
    std::string windowTitle = processName + " (" + std::to_string(pid()) + ")";

    if (firstShow) {
        ImGui::SetNextWindowPos(ImVec2(200, 200)); // 设置默认位置
		ImGui::SetNextWindowSize(ImVec2(800, 400)); // 设置默认大小
        firstShow = false; // 仅首次显示时生效
    }
    ImGui::Begin(C(windowTitle.c_str()), nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);

    // 进程名称行
    ImGui::Text(C("进程名称:"));
    ImGui::SameLine();
    // 使用不可编辑的输入框显示进程名称
    char processNameBuf[256]; // 根据实际需求调整缓冲区大小
    strcpy(processNameBuf, processName.c_str());
    ImGui::InputText("##ReadOnlyInputBox", processNameBuf, IM_ARRAYSIZE(processNameBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("复制"))) {
        ImGui::SetClipboardText(processName.c_str());
    }

    // PID行
    ImGui::Text(C("进程PID:"));
    ImGui::SameLine();
    // 使用不可编辑的输入框显示PID
    char pidBuf[32]; // PID 长度较短，缓冲区可小一些
    sprintf(pidBuf, "%d", pid());
    ImGui::InputText("ProcessPID##ReadOnlyInputBox", pidBuf, IM_ARRAYSIZE(pidBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("复制##pid"))) {
        ImGui::SetClipboardText(std::to_string(pid()).c_str());
    }

    // 进程路径行
    ImGui::Text(C("进程位置:"));
    ImGui::SameLine();
    // 使用不可编辑的输入框显示进程路径
    char processExePathBuf[1024]; // 根据实际需求调整缓冲区大小
    strcpy(processExePathBuf, C(processExePath.c_str()));
    ImGui::InputText("ProcessFilePath##ReadOnlyInputBox", processExePathBuf, IM_ARRAYSIZE(processExePathBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("复制##path"))) {
        ImGui::SetClipboardText(C(processExePath.c_str()));
    }
    ImGui::SameLine();
    if (ImGui::Button(C("打开位置"))) {
        // 打开文件所在目录
        size_t lastSlashPos = processExePath.find_last_of("\\/");
        if (lastSlashPos == std::string::npos) {
            kLog.Add(Err, C("无法找到文件所在目录"), C(CURRENT_MODULE));
        }
        std::string folderPath = processExePath.substr(0, lastSlashPos);
        HINSTANCE result = ShellExecuteA(NULL, "explore", folderPath.c_str(), NULL, NULL, SW_SHOWDEFAULT);

    }

    // 额外详细信息（扩展内容）
    ImGui::Separator();
    ImGui::Text(C("进程用户: %s"), C(processUser.c_str()));
    ImGui::Text(C("管理员权限: %s"), isAdmin ? C("是") : C("否"));
    // 在Render()函数中添加
    ImGui::Text(C("进程命令行:"));ImGui::SameLine();
    std::string cmdLine = commandLine;
    char cmdLineBuf[1024] = { 0 };
    strncpy(cmdLineBuf, C(cmdLine.c_str()), sizeof(cmdLineBuf) - 1);
    ImGui::InputText("##SelectedProcessCommandLine", cmdLineBuf, sizeof(cmdLineBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("复制##cmd"))) {
        ImGui::SetClipboardText(C(cmdLine.c_str()));
    }
    ImGui::Separator();
    // 时间信息
    ImGui::Text(C("创建时间: %s"), C(FileTimeToString(creationTime).c_str()));
    ImGui::Text(C("退出时间: %s"), C(FileTimeToString(exitTime).c_str()));
    ImGui::Text(C("内核态时间: %s"), C(FileTimeToString(kernelTime).c_str()));
    ImGui::Text(C("用户态时间: %s"), C(FileTimeToString(userTime).c_str()));

    // 资源信息
    ImGui::Text(C("线程数: %d"), threadCount);
    ImGui::Text(C("句柄数: %d"), handleCount);

    // 优先级和状态
    ImGui::Text(C("基本优先级: %d"), basePriority != -1 ? basePriority : -1);
    ImGui::Text(C("退出状态: 0x%08X"), exitStatus != -1 ? exitStatus : 0);

    // 其他信息
    ImGui::Text(C("亲和性掩码: 0x%08X"), affinityMask);
    ImGui::Text(C("会话ID: %d"), sessionId);
    ImGui::Text(C("父进程PID: %u"), parentPid);
    ImGui::Text(C("父进程名称: %s"), C(parentProcessName.c_str()));
    if (ImGui::Button(C("刷新进程详细信息"))) {
		GetProcessExtendedInfo();
    }
    ImGui::Separator();
	openTest.SetTargetPID(pid());
	openTest.ShowWindow();
    if (ImGui::TreeNode(C("令牌读取测试"))) {

    // 创建可滚动区域
        if (ImGui::BeginChild("TokenInfoScrollArea", ImVec2(0, 300), true))
        {
            // 创建表格
            if (ImGui::BeginTable("TokenInfoTable", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX))
            {
                // 设置表头
                ImGui::TableSetupColumn(C("枚举值"), ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn(C("描述"), ImGuiTableColumnFlags_WidthFixed, 200);
                ImGui::TableSetupColumn(C("数据类型"), ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn(C("值"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                ImGui::TableSetupScrollFreeze(0, 1);
                // 计算行高（根据实际内容调整，这里使用文本行高）
                const float rowHeight = ImGui::GetTextLineHeightWithSpacing();

                // 初始化裁剪器，指定总数据量和行高
                ImGuiListClipper clipper;
                clipper.Begin((int)m_tokenInfos.size(), rowHeight);

                // 只渲染可见区域的行
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        const auto& info = m_tokenInfos[i];
                        ImGui::TableNextRow();

                        // 枚举值列
                        ImGui::TableSetColumnIndex(0);
                        bool is_selected = (selectedToken == i);
                        bool row_clicked = ImGui::Selectable(
                            "##row_selectable",
                            is_selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                            ImGuiSelectableFlags_AllowItemOverlap,
                            ImVec2(0, ImGui::GetTextLineHeight())
                        );

                        // 处理行点击
                        if (row_clicked) {
                            selectedToken = i;
                        }
                        // 悬停效果
                        if (ImGui::IsItemHovered()) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                        }

                        // 右键菜单
                        // 定义右键菜单内容
                        if (ImGui::BeginPopupContextItem("##RowContextMenu")) {
							selectedToken = i; // 记录右键点击的行
                            if (ImGui::BeginMenu(C("复制"))) {
                                if (ImGui::MenuItem(C("Token名称"))) {
									ImGui::SetClipboardText(C(info.enumName.c_str()));
                                }
                                if (ImGui::MenuItem(C("描述"))) {
									ImGui::SetClipboardText(C(info.description.c_str()));
                                }
                                if (ImGui::MenuItem(C("数据类型"))) {
									ImGui::SetClipboardText(C(info.dataType.c_str()));
                                }
                                if (ImGui::MenuItem(C("值"))) {
									ImGui::SetClipboardText(C(info.value.c_str()));
                                }

                                ImGui::EndMenu();
                            }
                            ImGui::EndPopup();
                        }


                        ImGui::Text("%s", info.enumName.c_str());

                        // 描述列
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", C(info.description.c_str()));

                        // 数据类型列
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%s", info.dataType.c_str());

                        // 值列
                        ImGui::TableSetColumnIndex(3);
                        if (info.success)
                        {
                            if (info.value.empty())
                                ImGui::Text("No data");
                            else
                                ImGui::TextWrapped(C("%s"), C(info.value.c_str()));
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255)); // 红色错误文本
                            ImGui::Text(C("Error: %s (0x%08X)"), C(FormatError(info.errorCode).c_str()), info.errorCode);
                            ImGui::PopStyleColor();
                        }
                    }
                }

                ImGui::EndTable();
            }
        }    ImGui::EndChild();
		ImGui::TreePop();
    }

    // 右上角关闭按钮
    float buttonWidth = ImGui::CalcTextSize(C("销毁详细信息实例")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImGui::SetCursorPos(ImVec2(windowSize.x - buttonWidth - 10.0f, 10.0f)); // 10像素边距

    if (ImGui::Button(C("销毁详细信息实例"))) {
        kProcDtl.remove(pid());
        ImGui::End(); // 立即关闭窗口，防止后续渲染
        return;
    }
    ImGui::End();
}

//测试令牌打开权限===============================================================================================
// // 构造函数：初始化所有常见进程权限测试项
static const std::map<DWORD, std::string> errorCodeMap = {
    {ERROR_SUCCESS, "操作成功完成"},
    {ERROR_ACCESS_DENIED, "访问被拒绝"},
    {ERROR_INVALID_HANDLE, "无效的句柄"},
    {ERROR_INVALID_PARAMETER, "无效的参数"},
    //{ERROR_PROCESS_NOT_FOUND, "找不到指定的进程"},
    {ERROR_INSUFFICIENT_BUFFER, "缓冲区不足"},
    {ERROR_NOT_ENOUGH_MEMORY, "内存不足"},
    {ERROR_OPERATION_ABORTED, "操作已中止"}
};

ProcessOpenTest::ProcessOpenTest() : targetPID(0), showTestWindow(true) {
    // 初始化所有Windows进程权限（完整列表）
    testItems = {
        // 基础查询权限
        {"PROCESS_QUERY_INFORMATION", PROCESS_QUERY_INFORMATION, "", false},
        {"PROCESS_QUERY_LIMITED_INFORMATION", PROCESS_QUERY_LIMITED_INFORMATION, "", false},

        // 内存操作权限
        {"PROCESS_VM_READ", PROCESS_VM_READ, "", false},
        {"PROCESS_VM_WRITE", PROCESS_VM_WRITE, "", false},
        {"PROCESS_VM_OPERATION", PROCESS_VM_OPERATION, "", false},

        // 进程控制权限
        {"PROCESS_TERMINATE", PROCESS_TERMINATE, "", false},
        {"PROCESS_CREATE_THREAD", PROCESS_CREATE_THREAD, "", false},
        {"PROCESS_SET_INFORMATION", PROCESS_SET_INFORMATION, "", false},
        {"PROCESS_SUSPEND_RESUME", PROCESS_SUSPEND_RESUME, "", false},

        // 句柄和模块权限
        {"PROCESS_DUP_HANDLE", PROCESS_DUP_HANDLE, "", false},
        {"PROCESS_CREATE_PROCESS", PROCESS_CREATE_PROCESS, "", false},
        {"PROCESS_SET_QUOTA", PROCESS_SET_QUOTA, "", false},
        {"PROCESS_SET_SESSIONID", PROCESS_SET_SESSIONID, "", false},

        // 综合权限
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
    ss << "未知错误 (0x" << std::hex << errorCode << ")";
    return ss.str();
}

void ProcessOpenTest::RunTest(OpenProcessTestItem& item) {
    if (targetPID == 0) {
        item.result = "请输入目标PID";
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
        item.result = "失败: " + GetErrorString(errorCode);
    }
    else {
        std::stringstream ss;
        ss << "成功: 句柄 0x" << std::hex << (UINT_PTR)hProcess;
        item.result = ss.str();
        CloseHandle(hProcess);
    }
    item.tested = true;
}

void ProcessOpenTest::ShowWindow() {
    if (!showTestWindow) return;
    if (ImGui::TreeNode(C("打开进程句柄测试"))) {

        // 结果显示表格
        if (ImGui::BeginTable("permission_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(C("权限名称"), ImGuiTableColumnFlags_WidthFixed, 220);
            ImGui::TableSetupColumn(C("权限值(十六进制)"), ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn(C("测试结果"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(C("操作"), ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < testItems.size(); ++i) {
                auto& item = testItems[i];
                ImGui::TableNextRow();

                // 权限名称
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", item.name.c_str());

                // 权限值
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%08X", item.accessRight);

                // 测试结果（带颜色）
                ImGui::TableSetColumnIndex(2);
                if (item.tested) {
                    if (item.result.find(C("成功")) != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", C(item.result.c_str()));
                    }
                    else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", C(item.result.c_str()));
                    }
                }
                else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), C("未测试"));
                }

                // 单独测试按钮
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Button((C("测试##" + std::to_string(i))))) {
                    RunTest(item);
                }
            }

            ImGui::EndTable();
        }

        // 测试控制区
        ImGui::BeginGroup();
        if (ImGui::Button(C("执行所有测试"))) {
            for (auto& item : testItems) {
                RunTest(item);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("清空结果"))) {
            for (auto& item : testItems) {
                item.result.clear();
                item.tested = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("复制所有结果"))) {
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


//详细信息管理方案===============================================================================================
void ProcessDetailManager::add(DWORD pid) {
    // 避免添加重复PID
    for (const auto& detail : processDetails) {
        if (detail->pid() == pid) {
            kLog.warn(C("进程已在列表中，PID: " + std::to_string(pid)), C("ProcessDetailManager"));
            return;
        }
    }
    // 初始化并添加进程详情
    processDetails.emplace_back(std::make_unique<kProcessDetail>(pid));
    kLog.info(C("添加进程到详情列表，PID: " + std::to_string(pid)), C("ProcessDetailManager"));
}

bool ProcessDetailManager::remove(DWORD pid) {
    // 查找并移除指定PID的进程
    auto it = std::remove_if(processDetails.begin(), processDetails.end(),
        [pid](const std::unique_ptr<kProcessDetail>& detail) {
            return detail->pid() == pid;
        });

    if (it != processDetails.end()) {
        processDetails.erase(it, processDetails.end());
        kLog.info(C("从详情列表移除进程，PID: " + std::to_string(pid)), C("ProcessDetailManager"));
        return true;
    }

    kLog.warn(C("未找到进程，移除失败，PID: " + std::to_string(pid)), C("ProcessDetailManager"));
    return false;
}

void ProcessDetailManager::renderAll() {
    // 渲染所有进程的详情窗口
    for (const auto& detail : processDetails) {
        detail->Render();
    }
}

std::string kProcessDetail::SidToString(PSID sid)
{
    if (sid == nullptr)
        return "Invalid SID";

    // 显式使用 ANSI 版本函数，确保返回 char* 类型
    LPSTR sidString = nullptr;
    if (ConvertSidToStringSidA(sid, &sidString))  // 使用 A 后缀的 ANSI 版本
    {
        std::string result(sidString);  // 现在可以直接构造 std::string
        LocalFree(sidString);
        return result;
    }

    return "Failed to convert SID to string (Error: " + std::to_string(GetLastError()) + ")";
}

std::string kProcessDetail::LuidToString(LUID luid)
{
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << luid.HighPart
        << ":" << std::setw(8) << std::setfill('0') << luid.LowPart;
    return ss.str();
}

std::string kProcessDetail ::GetPrivilegeName(LUID luid)
{
    CHAR name[256] = { 0 };
    DWORD nameSize = sizeof(name);
    CHAR displayName[256] = { 0 };
    DWORD displayNameSize = sizeof(displayName);
    // 获取特权名称（4个参数）
    if (!LookupPrivilegeNameA(nullptr, &luid, name, &nameSize))
    {
        return "Unknown privilege (Error: " + std::to_string(GetLastError()) + ")";
    }

    DWORD languageId = 0;

    if (LookupPrivilegeDisplayNameA(nullptr, name, displayName, &displayNameSize, &languageId))
    {
        return std::string(name) + " (" + displayName + ")";
    }
    else
    {
        // 如果获取显示名称失败，至少返回基础名称
        return std::string(name);
    }
    return "Unknown privilege (LUID: " + LuidToString(luid) + ")";
}

std::string kProcessDetail::FormatError(DWORD errorCode)
{
    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&messageBuffer, 0, nullptr);

    std::string message(messageBuffer, size);

    LocalFree(messageBuffer);
    return message;
}

bool kProcessDetail::InitTokenInfo()
{

    // 打开进程
    
    // 打开进程令牌
    if (!OpenProcessToken(handle, TOKEN_QUERY, &m_hToken))
    {
        std::cerr << "OpenProcessToken failed. Error: " << GetLastError() << std::endl;
        return false;
    }

    // 定义所有需要查询的令牌信息类型
    struct TokenInfoType
    {
        TOKEN_INFORMATION_CLASS infoClass;
        std::string enumName;
        std::string description;
        std::string dataType;
    };

    std::vector<TokenInfoType> infoTypes = {
        {TokenUser, "TokenUser", "用户信息", "TOKEN_USER"},
        {TokenGroups, "TokenGroups", "组信息", "TOKEN_GROUPS"},
        {TokenPrivileges, "TokenPrivileges", "特权信息", "TOKEN_PRIVILEGES"},
        {TokenOwner, "TokenOwner", "所有者信息", "PSID"},
        {TokenPrimaryGroup, "TokenPrimaryGroup", "主要组信息", "PSID"},
        {TokenDefaultDacl, "TokenDefaultDacl", "默认DACL", "PACL"},
        {TokenSource, "TokenSource", "令牌源", "TOKEN_SOURCE"},
        {TokenType, "TokenType", "令牌类型", "TOKEN_TYPE"},
        {TokenImpersonationLevel, "TokenImpersonationLevel", "模拟级别", "SECURITY_IMPERSONATION_LEVEL"},
        {TokenStatistics, "TokenStatistics", "令牌统计", "TOKEN_STATISTICS"},
        {TokenRestrictedSids, "TokenRestrictedSids", "受限SID", "TOKEN_GROUPS"},
        {TokenSessionId, "TokenSessionId", "会话ID", "DWORD"},
        {TokenGroupsAndPrivileges, "TokenGroupsAndPrivileges", "组和特权", "TOKEN_GROUPS_AND_PRIVILEGES"},
        {TokenSessionReference, "TokenSessionReference", "会话引用", "DWORD"},
        {TokenSandBoxInert, "TokenSandBoxInert", "沙箱惰性", "BOOL"},
        {TokenAuditPolicy, "TokenAuditPolicy", "审计策略", "TOKEN_AUDIT_POLICY"},
        {TokenOrigin, "TokenOrigin", "令牌起源", "TOKEN_ORIGIN"},
        {TokenElevationType, "TokenElevationType", "提升类型", "TOKEN_ELEVATION_TYPE"},
        {TokenLinkedToken, "TokenLinkedToken", "链接令牌", "HANDLE"},
        {TokenElevation, "TokenElevation", "提升状态", "TOKEN_ELEVATION"},
        {TokenHasRestrictions, "TokenHasRestrictions", "有否限制", "BOOL"},
        {TokenAccessInformation, "TokenAccessInformation", "访问信息", "TOKEN_ACCESS_INFORMATION"},
        {TokenVirtualizationAllowed, "TokenVirtualizationAllowed", "允许虚拟化", "BOOL"},
        {TokenVirtualizationEnabled, "TokenVirtualizationEnabled", "已启用虚拟化", "BOOL"},
        {TokenIntegrityLevel, "TokenIntegrityLevel", "完整性级别", "DWORD"},
        {TokenUIAccess, "TokenUIAccess", "UI访问权限", "BOOL"},
        {TokenMandatoryPolicy, "TokenMandatoryPolicy", "强制策略", "TOKEN_MANDATORY_POLICY"},
        {TokenLogonSid, "TokenLogonSid", "登录SID", "PSID"},
        {TokenIsAppContainer, "TokenIsAppContainer", "是否为应用容器", "BOOL"},
        {TokenCapabilities, "TokenCapabilities", "功能", "TOKEN_CAPABILITIES"},
        {TokenAppContainerSid, "TokenAppContainerSid", "应用容器SID", "PSID"},
        {TokenAppContainerNumber, "TokenAppContainerNumber", "应用容器编号", "DWORD"},
        {TokenUserClaimAttributes, "TokenUserClaimAttributes", "用户声明属性", "CLAIM_SECURITY_ATTRIBUTES_INFORMATION"},
        {TokenDeviceClaimAttributes, "TokenDeviceClaimAttributes", "设备声明属性", "CLAIM_SECURITY_ATTRIBUTES_INFORMATION"},
        {TokenRestrictedUserClaimAttributes, "TokenRestrictedUserClaimAttributes", "受限用户声明属性", "CLAIM_SECURITY_ATTRIBUTES_INFORMATION"},
        {TokenRestrictedDeviceClaimAttributes, "TokenRestrictedDeviceClaimAttributes", "受限设备声明属性", "CLAIM_SECURITY_ATTRIBUTES_INFORMATION"},
        {TokenDeviceGroups, "TokenDeviceGroups", "设备组", "TOKEN_GROUPS"},
        {TokenRestrictedDeviceGroups, "TokenRestrictedDeviceGroups", "受限设备组", "TOKEN_GROUPS"},
        {TokenSecurityAttributes, "TokenSecurityAttributes", "安全属性", "SECURITY_ATTRIBUTES"},
        {TokenIsRestricted, "TokenIsRestricted", "是否受限", "BOOL"}
    };

    // 逐个查询令牌信息
    for (const auto& infoType : infoTypes)
    {
        TokenInfo tokenInfo;
        tokenInfo.enumName = infoType.enumName;
        tokenInfo.description = infoType.description;
        tokenInfo.dataType = infoType.dataType;
        tokenInfo.success = false;
        tokenInfo.errorCode = 0;

        // 首先获取所需缓冲区大小
        DWORD bufferSize = 0;
        GetTokenInformation(m_hToken, infoType.infoClass, nullptr, 0, &bufferSize);
        DWORD lastError = GetLastError();

        if (lastError != ERROR_INSUFFICIENT_BUFFER)
        {
            tokenInfo.errorCode = lastError;
            m_tokenInfos.push_back(tokenInfo);
            continue;
        }

        // 分配缓冲区
        std::vector<BYTE> buffer(bufferSize);

        // 获取令牌信息
        if (GetTokenInformation(m_hToken, infoType.infoClass, buffer.data(), bufferSize, &bufferSize))
        {
            tokenInfo.success = true;

            // 根据不同的信息类型解析数据
            switch (infoType.infoClass)
            {
            case TokenUser:
            {
                PTOKEN_USER pUser = reinterpret_cast<PTOKEN_USER>(buffer.data());
                tokenInfo.value = SidToString(pUser->User.Sid);
                break;
            }

            case TokenUIAccess:
            {
                BOOL* pUiAccess = reinterpret_cast<BOOL*>(buffer.data());
                tokenInfo.value = (*pUiAccess) ? "True" : "False";
                break;
            }

            case TokenType:
            {
                TOKEN_TYPE* pType = reinterpret_cast<TOKEN_TYPE*>(buffer.data());
                tokenInfo.value = (*pType == TokenPrimary) ? "TokenPrimary" : "TokenImpersonation";
                break;
            }

            case TokenSessionId:
            {
                DWORD* pSessionId = reinterpret_cast<DWORD*>(buffer.data());
                tokenInfo.value = std::to_string(*pSessionId);
                break;
            }

            case TokenIntegrityLevel:
            {
                PTOKEN_MANDATORY_LABEL pLabel = reinterpret_cast<PTOKEN_MANDATORY_LABEL>(buffer.data());
                tokenInfo.value = SidToString(pLabel->Label.Sid);
                break;
            }

            case TokenElevation:
            {
                PTOKEN_ELEVATION pElevation = reinterpret_cast<PTOKEN_ELEVATION>(buffer.data());
                tokenInfo.value = (pElevation->TokenIsElevated) ? "Elevated" : "Not elevated";
                break;
            }

            case TokenElevationType:
            {
                TOKEN_ELEVATION_TYPE* pType = reinterpret_cast<TOKEN_ELEVATION_TYPE*>(buffer.data());
                switch (*pType)
                {
                case TokenElevationTypeDefault: tokenInfo.value = "TokenElevationTypeDefault"; break;
                case TokenElevationTypeFull: tokenInfo.value = "TokenElevationTypeFull"; break;
                case TokenElevationTypeLimited: tokenInfo.value = "TokenElevationTypeLimited"; break;
                default: tokenInfo.value = "Unknown";
                }
                break;
            }

            case TokenPrivileges:
            {
                PTOKEN_PRIVILEGES pPrivileges = reinterpret_cast<PTOKEN_PRIVILEGES>(buffer.data());
                std::stringstream ss;

                for (DWORD i = 0; i < pPrivileges->PrivilegeCount; ++i)
                {
                    ss << GetPrivilegeName(pPrivileges->Privileges[i].Luid) << " - ";

                    switch (pPrivileges->Privileges[i].Attributes)
                    {
                    case SE_PRIVILEGE_ENABLED: ss << "Enabled"; break;
                    //case SE_PRIVILEGE_DISABLED: ss << "Disabled"; break;
                    case SE_PRIVILEGE_REMOVED: ss << "Removed"; break;
                    default: ss << "0x" << std::hex << pPrivileges->Privileges[i].Attributes;
                    }

                    if (i < pPrivileges->PrivilegeCount - 1)
                        ss << "\n";
                }

                tokenInfo.value = ss.str();
                break;
            }

            case TokenGroups:
            {
                PTOKEN_GROUPS pGroups = reinterpret_cast<PTOKEN_GROUPS>(buffer.data());
                std::stringstream ss;

                for (DWORD i = 0; i < pGroups->GroupCount; ++i)
                {
                    ss << SidToString(pGroups->Groups[i].Sid) << " - ";

                    if (pGroups->Groups[i].Attributes & SE_GROUP_ENABLED)
                        ss << "Enabled";
                    //else if (pGroups->Groups[i].Attributes & SE_GROUP_DISABLED)
                    //    ss << "Disabled";
                    else
                        ss << "0x" << std::hex << pGroups->Groups[i].Attributes;

                    if (i < pGroups->GroupCount - 1)
                        ss << "\n";
                }

                tokenInfo.value = ss.str();
                break;
            }

            case TokenIsAppContainer:
            {
                BOOL* pIsAppContainer = reinterpret_cast<BOOL*>(buffer.data());
                tokenInfo.value = (*pIsAppContainer) ? "True" : "False";
                break;
            }

            case TokenAppContainerSid:
            {
                PSID pSid = reinterpret_cast<PSID>(buffer.data());
                tokenInfo.value = SidToString(pSid);
                break;
            }

            case TokenOwner:
            {
                PSID pOwner = reinterpret_cast<PSID>(buffer.data());
                tokenInfo.value = SidToString(pOwner);
                break;
            }

            case TokenPrimaryGroup:
            {
                PSID pGroup = reinterpret_cast<PSID>(buffer.data());
                tokenInfo.value = SidToString(pGroup);
                break;
            }

            case TokenImpersonationLevel:
            {
                SECURITY_IMPERSONATION_LEVEL* pLevel = reinterpret_cast<SECURITY_IMPERSONATION_LEVEL*>(buffer.data());
                switch (*pLevel)
                {
                case SecurityAnonymous: tokenInfo.value = "SecurityAnonymous"; break;
                case SecurityIdentification: tokenInfo.value = "SecurityIdentification"; break;
                case SecurityImpersonation: tokenInfo.value = "SecurityImpersonation"; break;
                case SecurityDelegation: tokenInfo.value = "SecurityDelegation"; break;
                default: tokenInfo.value = "Unknown";
                }
                break;
            }

            // 对于其他类型，我们只显示"数据可用"
            default:
                tokenInfo.value = "Data available (not fully parsed)";
                break;
            }
        }
        else
        {
            tokenInfo.errorCode = GetLastError();
        }

        m_tokenInfos.push_back(tokenInfo);
    }

    m_isValid = true;
    return true;
}


#undef CURRENT_MODULE
