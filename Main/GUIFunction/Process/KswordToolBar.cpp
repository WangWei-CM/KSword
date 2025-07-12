#include "../../KswordTotalHead.h"

static bool Ksword_tool_bar_already_top = 0;
static bool first_done = 1;

inline void KswordToolBar() {
    // ���ô���λ�úʹ�С
    //const ImGuiViewport* viewport = ImGui::GetMainViewport();
    //ImVec2 window_pos = ImVec2(viewport->WorkPos.x + 10, viewport->WorkPos.y + 10);
    ImVec2 window_size = ImVec2(200, 150);

    // ���ô��ڱ�־ - �����벼�ֱ���
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        //ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        //ImGuiWindowFlags_Tooltip |
        //ImGuiWindowFlags_TopMost|
        //ImGuiWindowFlags_NoFocusOnAppearing /*|*/  // ��ֹ�Զ���ȡ����
        //ImGuiWindowFlags_NoDecoration /*|*/       // �ޱ��������ޱ߿�
        ImGuiWindowFlags_NoBringToFrontOnFocus; // ��ֹ���������ڸ���
        ;

        //ImGui::SetNextWindowFocus();
    //ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    //ImGuiViewport* viewport = ImGui::GetWindowViewport(); // ��ǰ���������ӿ�
    //HWND hwnd = (HWND)viewport->PlatformHandle;          // ת��Ϊ Windows ���
    //if (!Ksword_tool_bar_already_top) {
    //    //2. ����ϵͳ API �ö�
    //    SetWindowPos(
    //        hwnd,
    //        HWND_TOPMOST,     // �ö���־
    //        0, 0, 0, 0,       // ����ԭλ�úͳߴ�
    //        SWP_NOMOVE | SWP_NOSIZE
    //    );
    //    Ksword_tool_bar_already_top=1;
    //}
    // ��ʼ���ƴ���
    if (ImGui::Begin("Control Panel", nullptr, window_flags))
    {
        ImGuiViewport* viewport = ImGui::GetWindowViewport(); // ��ǰ���������ӿ�
        HWND hwnd = (HWND)viewport->PlatformHandle;          // ת��Ϊ Windows ���
        if (first_done) {
            //2. ����ϵͳ API �ö�
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,     // �ö���־
                0, 0, 0, 0,       // ����ԭλ�úͳߴ�
                SWP_NOMOVE | SWP_NOSIZE
            );
            //first_done = 0;
        }
        // 1. �˳���ť - ����
        if (ImGui::Button("Exit", ImVec2(-1, 0)))
        {
            Ksword_main_should_exit = 1;
        }

        ImGui::Separator();

        // 2. �۵��� - ���ڿ��ư�ť
        if (ImGui::CollapsingHeader("Window Controls", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // ��־���ڰ�ť
            if (ImGui::Button(KswordShowLogWindow ? "Hide Log Window" : "Show Log Window", ImVec2(-1, 0)))
            {
                KswordShowLogWindow = !KswordShowLogWindow;
            }

            // ָ�봰�ڰ�ť
            if (ImGui::Button(KswordShowPointerWindow ? "Hide Pointer Window" : "Show Pointer Window", ImVec2(-1, 0)))
            {
                KswordShowPointerWindow = !KswordShowPointerWindow;
            }

            // ���±����ڰ�ť
            if (ImGui::Button(KswordShowNotpadWindow ? "Hide Notepad Window" : "Show Notepad Window", ImVec2(-1, 0)))
            {
                KswordShowNotpadWindow = !KswordShowNotpadWindow;
            }

            // ��������ť
            if (ImGui::Button(KswordShowToolBar ? "Hide Tool Bar" : "Show Tool Bar", ImVec2(-1, 0)))
            {
                KswordShowToolBar = !KswordShowToolBar;
            }
        }
    }
    ImGui::End();
}
