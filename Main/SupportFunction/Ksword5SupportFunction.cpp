#include "../KswordTotalHead.h"

// ��ʼ����̬��Ա
std::vector<WorkItem> WorkProgressManager::ProcessList;
std::vector<WorkItemUI> WorkProgressManager::ProcessUI;
std::vector<int> WorkProgressManager::ShowUI;
std::vector<int> WorkProgressManager::UIreturnValue;
std::mutex WorkProgressManager::data_mutex;
bool DeleteReleasedGUIINIFile() {
    // ��ȡ��ǰ����ִ��Ŀ¼
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // ��ȡ·��ʧ��
    }

    // ��ȡĿ¼·��
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // ·����ʽ����
    }
    *pLastSlash = '\0'; // �ض�ΪĿ¼·��

    // ����INI�ļ�����·��
    std::string strIniPath = std::string(szExePath) + "\\KswordGUI.ini";

    // ����ļ��Ƿ����
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // �ļ�������
    }

    // ɾ���ļ�
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // ɾ���ɹ�
    }
    else {
        return false; // ɾ��ʧ��
    }
}
bool DeleteReleasedD3DX9DLLFile() {
    // ��ȡ��ǰ����ִ��Ŀ¼
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // ��ȡ·��ʧ��
    }

    // ��ȡĿ¼·��
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // ·����ʽ����
    }
    *pLastSlash = '\0'; // �ض�ΪĿ¼·��

    // ����INI�ļ�����·��
    std::string strIniPath = std::string(szExePath) + "\\d3dx9_43.dll";

    // ����ļ��Ƿ����
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // �ļ�������
    }

    // ɾ���ļ�
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // ɾ���ɹ�
    }
    else {
        return false; // ɾ��ʧ��
    }
}

// ���ַ��汾��֧��Unicode·����
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
    // ��ȡ��ǰ����ִ��Ŀ¼
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0)
        return false;

    // ��ȡĿ¼·��
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL)
        return false;
    *pLastSlash = '\0';

    // ����Ŀ���ļ�·��
    std::string strOutputPath = std::string(szExePath) + "\\KswordGUI.ini";

    // ����ļ��Ƿ��Ѵ��ڣ�������ֱ�ӷ���true
    if (GetFileAttributesA(strOutputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    // ������Դ
    HRSRC hResource = FindResource(NULL, MAKEINTRESOURCE(IDR_INI1), L"INI");
    if (hResource == NULL)
        return false;

    // ������Դ
    HGLOBAL hLoadedResource = LoadResource(NULL, hResource);
    if (hLoadedResource == NULL)
        return false;

    // ������Դ����ȡ����ָ��
    LPVOID pResourceData = LockResource(hLoadedResource);
    if (pResourceData == NULL)
        return false;

    // ��ȡ��Դ��С
    DWORD dwResourceSize = SizeofResource(NULL, hResource);
    if (dwResourceSize == 0)
        return false;

    // д���ļ�
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
    ProcessUI   .push_back(WorkItemUI());            // ���Ĭ��UI�ṹ
    ShowUI      .push_back(0);                       // ��ʼ����ʾUI
    UIreturnValue.push_back(0);                      // ��ʼ����ֵ0
    return static_cast<int>(ProcessList.size() - 1); // ��������ӵ�����
                                                     // �Ժ�ͨ��[pid]ֱ�ӷ�������
}
int WorkProgressManager::AddProcess(std::string Name,std::string StepName,bool* cancel,float Progress) {
    std::lock_guard<std::mutex> lock(data_mutex);
    WorkItem temp;
    temp.canceled = cancel;
    temp.currentStep = StepName;
    temp.name = Name;
    temp.progress = Progress;
    ProcessList.push_back(temp);
    ProcessUI.push_back(WorkItemUI());            // ���Ĭ��UI�ṹ
    ShowUI.push_back(0);                       // ��ʼ����ʾUI
    UIreturnValue.push_back(0);                      // ��ʼ����ֵ0
    return static_cast<int>(ProcessList.size() - 1); // ��������ӵ�����
    // �Ժ�ͨ��[pid]ֱ�ӷ�������
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
        ImGui::Begin(C("������ȼ��"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        for (size_t i = 0; i < ProcessList.size(); ++i) {
            if (ProcessList[i].progress == 1.0f)continue;
            ImGui::PushID(static_cast<int>(i));

            // ��ʾ������Ϣ
            ImGui::Text(C("����ִ��: "));
            ImGui::SameLine();
            ImGui::Text(ProcessList[i].currentStep.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));
            ImGui::ProgressBar(ProcessList[i].progress, ImVec2(200, 20));
            ImGui::PopStyleColor();

            // ����Ƿ���Ҫ��ʾUI
            if (ShowUI[i]) {
                //ImGui::Separator();
                ImGui::Text("%s", ProcessUI[i].Info.c_str());

                // ��Ⱦ������ť
                for (int btn = 0; btn < ProcessUI[i].OperateNum; ++btn) {
                    if (ImGui::Button((C("����") + std::to_string(btn + 1)).c_str())) {
                        UIreturnValue[i] = btn + 1; // ���÷���ֵ
                        ShowUI[i] = false;          // ����UI
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