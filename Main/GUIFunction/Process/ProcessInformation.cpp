#include "../../KswordTotalHead.h"
#include "Process.h"
#include "ProcessDetail.h"
#include <winnt.h>  // 补充Windows权限常量定义
#define CURRENT_MODULE "进程详细信息管理"


typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(
    HANDLE           ProcessHandle,
    DWORD            ProcessInformationClass,
    PVOID            ProcessInformation,
    ULONG            ProcessInformationLength,
    PULONG           ReturnLength);
typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    PVOID BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;
typedef struct _PEB {
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
} PEB, * PPEB;
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING;
typedef struct _RTL_USER_PROCESS_PARAMETERS {
    BYTE           Reserved1[16];
    PVOID          Reserved2[10];
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
} RTL_USER_PROCESS_PARAMETERS, * PRTL_USER_PROCESS_PARAMETERS;


std::string kProcessDetail::GetCommandLine() {
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
    RTL_USER_PROCESS_PARAMETERS upp;
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
    InitDetailInfo();
}

void kProcessDetail::InitDetailInfo() {
    // 从父类获取进程详细信息
    processName = Name();
    processExePath = ExePath();
    processUser = User();
    isAdmin = IsAdmin();
	commandLine = GetCommandLine();
}


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
    ImGui::Text(C("启动参数:"));ImGui::SameLine();
    std::string cmdLine = commandLine;
    char cmdLineBuf[1024] = { 0 };
    strncpy(cmdLineBuf, C(cmdLine.c_str()), sizeof(cmdLineBuf) - 1);
    ImGui::InputText("", cmdLineBuf, sizeof(cmdLineBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(C("复制##cmd"))) {
        ImGui::SetClipboardText(C(cmdLine.c_str()));
    }
    if(ImGui::Button(C("销毁详细信息实例"))) {
		kProcDtl.remove(pid());
    };
	openTest.SetTargetPID(pid());
	openTest.ShowWindow();
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
#undef CURRENT_MODULE
