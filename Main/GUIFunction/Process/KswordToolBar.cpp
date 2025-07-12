#include "../../KswordTotalHead.h"

static bool Ksword_tool_bar_already_top = 0;
static bool first_done = 1;

inline void KswordToolBar() {
    // 设置窗口位置和大小
    //const ImGuiViewport* viewport = ImGui::GetMainViewport();
    //ImVec2 window_pos = ImVec2(viewport->WorkPos.x + 10, viewport->WorkPos.y + 10);
    ImVec2 window_size = ImVec2(200, 150);

    // 设置窗口标志 - 不参与布局保存
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        //ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        //ImGuiWindowFlags_Tooltip |
        //ImGuiWindowFlags_TopMost|
        //ImGuiWindowFlags_NoFocusOnAppearing /*|*/  // 防止自动获取焦点
        //ImGuiWindowFlags_NoDecoration /*|*/       // 无标题栏、无边框
        ImGuiWindowFlags_NoBringToFrontOnFocus; // 防止被其他窗口覆盖
        ;

        //ImGui::SetNextWindowFocus();
    //ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    //ImGuiViewport* viewport = ImGui::GetWindowViewport(); // 当前窗口所属视口
    //HWND hwnd = (HWND)viewport->PlatformHandle;          // 转换为 Windows 句柄
    //if (!Ksword_tool_bar_already_top) {
    //    //2. 调用系统 API 置顶
    //    SetWindowPos(
    //        hwnd,
    //        HWND_TOPMOST,     // 置顶标志
    //        0, 0, 0, 0,       // 保留原位置和尺寸
    //        SWP_NOMOVE | SWP_NOSIZE
    //    );
    //    Ksword_tool_bar_already_top=1;
    //}
    // 开始绘制窗口
    if (ImGui::Begin("Control Panel", nullptr, window_flags))
    {
        ImGuiViewport* viewport = ImGui::GetWindowViewport(); // 当前窗口所属视口
        HWND hwnd = (HWND)viewport->PlatformHandle;          // 转换为 Windows 句柄
        if (first_done) {
            //2. 调用系统 API 置顶
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,     // 置顶标志
                0, 0, 0, 0,       // 保留原位置和尺寸
                SWP_NOMOVE | SWP_NOSIZE
            );
            //first_done = 0;
        }
        // 1. 退出按钮 - 顶部
        if (ImGui::Button("Exit", ImVec2(-1, 0)))
        {
            Ksword_main_should_exit = 1;
        }

        ImGui::Separator();

        // 2. 折叠栏 - 窗口控制按钮
        if (ImGui::CollapsingHeader("Window Controls", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // 日志窗口按钮
            if (ImGui::Button(KswordShowLogWindow ? "Hide Log Window" : "Show Log Window", ImVec2(-1, 0)))
            {
                KswordShowLogWindow = !KswordShowLogWindow;
            }

            // 指针窗口按钮
            if (ImGui::Button(KswordShowPointerWindow ? "Hide Pointer Window" : "Show Pointer Window", ImVec2(-1, 0)))
            {
                KswordShowPointerWindow = !KswordShowPointerWindow;
            }

            // 记事本窗口按钮
            if (ImGui::Button(KswordShowNotpadWindow ? "Hide Notepad Window" : "Show Notepad Window", ImVec2(-1, 0)))
            {
                KswordShowNotpadWindow = !KswordShowNotpadWindow;
            }

            // 工具栏按钮
            if (ImGui::Button(KswordShowToolBar ? "Hide Tool Bar" : "Show Tool Bar", ImVec2(-1, 0)))
            {
                KswordShowToolBar = !KswordShowToolBar;
            }
        }
    }
    ImGui::End();
}
