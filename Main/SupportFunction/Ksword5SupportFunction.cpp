#include "../KswordTotalHead.h"

// 初始化静态成员
std::vector<WorkItem> WorkProgressManager::ProcessList;
std::vector<WorkItemUI> WorkProgressManager::ProcessUI;
std::vector<int> WorkProgressManager::ShowUI;
std::vector<int> WorkProgressManager::UIreturnValue;
std::mutex WorkProgressManager::data_mutex;

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
        ImGui::Begin("任务进度监控", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        for (size_t i = 0; i < ProcessList.size(); ++i) {
            if (ProcessList[i].progress == 1.0f)continue;
            ImGui::PushID(static_cast<int>(i));

            // 显示任务信息
            ImGui::Text(C("正在执行: %s"), C(ProcessList[i].currentStep.c_str()));
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