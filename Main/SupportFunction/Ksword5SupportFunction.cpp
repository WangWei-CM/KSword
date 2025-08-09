#include "../KswordTotalHead.h"

// 初始化静态成员
std::vector<WorkItem> WorkProgressManager::ProcessList;
std::vector<WorkItemUI> WorkProgressManager::ProcessUI;
std::vector<int> WorkProgressManager::ShowUI;
std::vector<int> WorkProgressManager::UIreturnValue;
std::mutex WorkProgressManager::data_mutex;
bool DeleteReleasedGUIINIFile() {
    // 获取当前程序执行目录
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // 获取路径失败
    }

    // 提取目录路径
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // 路径格式错误
    }
    *pLastSlash = '\0'; // 截断为目录路径

    // 构建INI文件完整路径
    std::string strIniPath = std::string(szExePath) + "\\KswordGUI.ini";

    // 检查文件是否存在
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // 文件不存在
    }

    // 删除文件
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // 删除成功
    }
    else {
        return false; // 删除失败
    }
}
bool DeleteReleasedD3DX9DLLFile() {
    // 获取当前程序执行目录
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // 获取路径失败
    }

    // 提取目录路径
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // 路径格式错误
    }
    *pLastSlash = '\0'; // 截断为目录路径

    // 构建INI文件完整路径
    std::string strIniPath = std::string(szExePath) + "\\d3dx9_43.dll";

    // 检查文件是否存在
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // 文件不存在
    }

    // 删除文件
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // 删除成功
    }
    else {
        return false; // 删除失败
    }
}

// 宽字符版本（支持Unicode路径）
bool DeleteReleasedIniFileW() {
    WCHAR szExePath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szExePath, MAX_PATH) == 0) {
        return false;
    }

    WCHAR* pLastSlash = wcsrchr(szExePath, L'\\');
    if (pLastSlash == NULL) {
        return false;
    }
    *pLastSlash = L'\0';

    std::wstring strIniPath = std::wstring(szExePath) + L"\\KswordGUI.ini";

    if (GetFileAttributesW(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    return DeleteFileW(strIniPath.c_str()) ? true : false;
}


bool ExtractGUIINIResourceToFile()
{
    // 获取当前程序执行目录
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0)
        return false;

    // 提取目录路径
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL)
        return false;
    *pLastSlash = '\0';

    // 构建目标文件路径
    std::string strOutputPath = std::string(szExePath) + "\\KswordGUI.ini";

    // 检查文件是否已存在，存在则直接返回true
    if (GetFileAttributesA(strOutputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    // 查找资源
    HRSRC hResource = FindResource(NULL, MAKEINTRESOURCE(IDR_INI1), L"INI");
    if (hResource == NULL)
        return false;

    // 加载资源
    HGLOBAL hLoadedResource = LoadResource(NULL, hResource);
    if (hLoadedResource == NULL)
        return false;

    // 锁定资源并获取数据指针
    LPVOID pResourceData = LockResource(hLoadedResource);
    if (pResourceData == NULL)
        return false;

    // 获取资源大小
    DWORD dwResourceSize = SizeofResource(NULL, hResource);
    if (dwResourceSize == 0)
        return false;

    // 写入文件
    std::ofstream outFile(strOutputPath, std::ios::binary);
    if (!outFile)
        return false;

    outFile.write(static_cast<const char*>(pResourceData), dwResourceSize);
    outFile.close();

    return true;
}
int WorkProgressManager::AddProcess(WorkItem item) {
    std::lock_guard<std::mutex> lock(data_mutex);
    ProcessList .push_back(item);
    ProcessUI   .push_back(WorkItemUI());            // 添加默认UI结构
    ShowUI      .push_back(0);                       // 初始不显示UI
    UIreturnValue.push_back(0);                      // 初始返回值0
    return static_cast<int>(ProcessList.size() - 1); // 返回新添加的索引
                                                     // 以后通过[pid]直接访问数组
}
int WorkProgressManager::AddProcess(std::string Name,std::string StepName,bool* cancel,float Progress) {
    std::lock_guard<std::mutex> lock(data_mutex);
    WorkItem temp;
    temp.canceled = cancel;
    temp.currentStep = StepName;
    temp.name = Name;
    temp.progress = Progress;
    ProcessList.push_back(temp);
    ProcessUI.push_back(WorkItemUI());            // 添加默认UI结构
    ShowUI.push_back(0);                       // 初始不显示UI
    UIreturnValue.push_back(0);                      // 初始返回值0
    return static_cast<int>(ProcessList.size() - 1); // 返回新添加的索引
    // 以后通过[pid]直接访问数组
}


void WorkProgressManager::SetProcess(int pid,WorkItem item) {
    std::lock_guard<std::mutex> lock(data_mutex);
    ProcessList[pid] = item;
}

void WorkProgressManager::SetProcess(int pid, std::string StepName, float Progress)

{
    std::lock_guard<std::mutex> lock(data_mutex);
    ProcessList[pid].currentStep = StepName;
    ProcessList[pid].progress = Progress;
}

int WorkProgressManager::UI(int pid, WorkItemUI ui) {

    ShowUI[pid] = 1;
    ProcessUI[pid] = ui;
    while (UIreturnValue[pid] == 0)
    {
        Sleep(10);
    }
    ShowUI[pid] = 0;
    int returnValue = UIreturnValue[pid];
    UIreturnValue[pid] = 0;
    return returnValue;
}


int WorkProgressManager::UI(int pid, std::string Info, int OperateNum) {
    //std::lock_guard<std::mutex> lock(data_mutex);
    ShowUI[pid] = 1;
    ProcessUI[pid].Info = Info;
    ProcessUI[pid].OperateNum = OperateNum;
    while (UIreturnValue[pid] == 0)
    {
        Sleep(10);
    }
    ShowUI[pid] = 0;
    int returnValue = UIreturnValue[pid];
    UIreturnValue[pid] = 0;
    return returnValue;
}

void WorkProgressManager::Render() {

    bool showWorkWindow = 0;
    //std::lock_guard<std::mutex> lock(data_mutex);
    for (size_t i = 0; i < ProcessList.size(); ++i) {
        if (ProcessList[i].progress != 1.0f)showWorkWindow = 1;
    }
    if (showWorkWindow) {
        ImGui::Begin(C("任务进度监控"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        for (size_t i = 0; i < ProcessList.size(); ++i) {
            if (ProcessList[i].progress == 1.0f)continue;
            ImGui::PushID(static_cast<int>(i));

            // 显示任务信息
            ImGui::Text(C("正在执行: "));
            ImGui::SameLine();
            ImGui::Text(ProcessList[i].currentStep.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));
            ImGui::ProgressBar(ProcessList[i].progress, ImVec2(200, 20));
            ImGui::PopStyleColor();

            // 检查是否需要显示UI
            if (ShowUI[i]) {
                //ImGui::Separator();
                ImGui::Text("%s", ProcessUI[i].Info.c_str());

                // 渲染操作按钮
                for (int btn = 0; btn < ProcessUI[i].OperateNum; ++btn) {
                    if (ImGui::Button((C("操作") + std::to_string(btn + 1)).c_str())) {
                        UIreturnValue[i] = btn + 1; // 设置返回值
                        ShowUI[i] = false;          // 隐藏UI
                    }
                    if (btn < ProcessUI[i].OperateNum - 1) ImGui::SameLine();
                }
                //ImGui::Separator();
            }
            ImGui::PopID();
            ImGui::Spacing();
        }
        ImGui::End();
    }
}