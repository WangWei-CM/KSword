#include "../KswordTotalHead.h"
static float SelectedWindowAlpha = 1.0f;
static bool showPointerWindowDetail = false;
extern LPDIRECT3DTEXTURE9 GetCachedProcessIcon(DWORD pid);
static inline BOOL CALLBACK EnumWorkerWProc(_In_ HWND hwndWorkerW, _In_ LPARAM lParam) {
    char className[256] = { 0 };
    GetClassNameA(hwndWorkerW, className, sizeof(className));

    // ֻ��������Ϊ"WorkerW"�Ĵ���
    if (strcmp(className, "WorkerW") != 0) {
        return TRUE;
    }

    // ���ҵ�ǰWorkerW���Ƿ����"SHELLDLL_DefView"�Ӵ��ڣ�����Ŀ��WorkerW��
    HWND* pHideWorkerW = (HWND*)lParam;
    HWND hwndShellDefView = FindWindowExA(hwndWorkerW, NULL, "SHELLDLL_DefView", NULL);

    if (hwndShellDefView != NULL) {
        // ��¼��Ҫ������WorkerW�����������أ�
        *pHideWorkerW = hwndWorkerW;
    }
    else {
        // ���ز���SHELLDLL_DefView��WorkerW������������ʾ�쳣��
        ShowWindow(hwndWorkerW, SW_HIDE);
    }

    return TRUE;
}
inline void RenderPointerWindowInfo(HWND hwnd) {
    if (!hwnd) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[δ�ҵ�����]");
        return;
    }


    // ===================== ����У�������Ϣ���Ȼ��ƣ��Զ���Ӧ��ȣ� =====================
    {
        float left_col_start_x = ImGui::GetCursorPosX(); // ��¼�������ʼX����

        // 1. ���ڱ���
        char title[256];
        GetWindowTextA(hwnd, title, sizeof(title));
        ImGui::Text(GBKtoUTF8("����: %s").c_str(), title[0] ? GBKtoUTF8(title).c_str() : C("<�ޱ���>"));

        // 2. ��������
        char class_name[256];
        GetClassNameA(hwnd, class_name, sizeof(class_name));
        ImGui::Text(GBKtoUTF8("����: %s").c_str(), GBKtoUTF8(class_name).c_str());

        // 3. ����λ�úͳߴ�
        RECT rect;
        GetWindowRect(hwnd, &rect);
        ImGui::Text(GBKtoUTF8("λ��: (%d, %d)").c_str(), rect.left, rect.top);
        ImGui::Text(GBKtoUTF8("�ߴ�: %dx%d").c_str(), rect.right - rect.left, rect.bottom - rect.top);

        // 4. ������Ϣ
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        ImGui::Separator();
        ImGui::Text(GBKtoUTF8("����ID: %d").c_str(), pid);
		ImGui::SameLine();
        LPDIRECT3DTEXTURE9 pTexture = GetCachedProcessIcon(pid);
        if (pTexture) {
            // ImGui::Image����������ָ�루ת��ΪImTextureID�����ߴ�
            ImGui::Image(
                (ImTextureID)pTexture,  // DX9����ָ��ֱ����ΪImTextureID
                ImVec2(16, 16)          // 16��16��ʾ�ߴ�
            );
        }
		ImGui::SameLine();
        if (ImGui::Button(("-"), ImVec2(16, 16))) {
            showPointerWindowDetail = 0;
        };
        ImGui::Text(GBKtoUTF8("��������: %s").c_str(), C(GetProcessNameByPID(pid)));

        // ���㲢��������п�ȣ�����+�ڱ߾ࣩ
        float left_col_width = ImGui::GetCursorPosX() - left_col_start_x;
        left_col_width += 10.0f; // ����10px�ڱ߾࣬�������Ҳ����
    }

    {

        // 1. �ö���ť
        if (ImGui::Button(GBKtoUTF8("�ö�").c_str(), ImVec2(50, 25))) {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            );
        }
        ImGui::SameLine();

        // 2. ȡ���ö���ť
        if (ImGui::Button(GBKtoUTF8("ȡ��").c_str(), ImVec2(50, 25))) {
            SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            );
        }
        ImGui::SameLine();

        // 3. ���ذ�ť
        if (ImGui::Button(C("����"), ImVec2(50, 25))) {
            ShowWindow(hwnd, SW_HIDE);
        }
        ImGui::SameLine();
#define CURRENT_MODULE "���ڹ���ָ�봰��"
        if (ImGui::Button(C("��Ϊ�����Ӵ���"), ImVec2(100, 25))) {
            if (hwnd) {
                kLog.info(C("��ʼ���Խ�������Ϊ�����Ӵ���"), C(CURRENT_MODULE));

                // ����1����ȡ���������Progman
                HWND hwndProgman = FindWindowA("Progman", "Program Manager");
                if (hwndProgman == NULL) {
                    kLog.err(C("δ�ҵ����������Progman��������ֹ"), C(CURRENT_MODULE));
                    return;
                }
                kLog.dbg(C("���ҵ�Progman����"), C(CURRENT_MODULE));

                // ����2������0x052c��Ϣ��Progman
                LRESULT sendResult = SendMessageTimeoutA(
                    hwndProgman,
                    0x052C,
                    0,
                    0,
                    SMTO_NORMAL,
                    1000,
                    NULL
                );
                if (sendResult == 0) {
                    kLog.err(C("����0x052C��Ϣ��Progmanʧ�ܣ�����δ�ܴ���WorkerW����"), C(CURRENT_MODULE));
                    // �������ѡ��return�����
                }
                else {
                    kLog.dbg(C("�ѷ���0x052C��Ϣ��Progman"), C(CURRENT_MODULE));
                }

                // ����3��ö������WorkerW����
                HWND hwndKeepWorkerW = NULL;
                if (!EnumWindows((WNDENUMPROC)EnumWorkerWProc, (LPARAM)&hwndKeepWorkerW)) {
                    kLog.err(C("ö��WorkerW����ʧ��"), C(CURRENT_MODULE));
                    // �������ѡ��return�����
                }
                else {
                    kLog.dbg(C("��ö��WorkerW����"), C(CURRENT_MODULE));
                }

                // ����4����Ŀ�괰����ΪProgman���Ӵ���
                HWND hRet = SetParent(hwnd, hwndProgman);
                if (hRet == NULL) {
                    DWORD err = GetLastError();
                    char msg[256];
                    snprintf(msg, sizeof(msg), "SetParentʧ�ܣ�������: %lu, hwnd=0x%p, hwndProgman=0x%p", err, hwnd, hwndProgman);
                    kLog.fatal(C(msg), C(CURRENT_MODULE));
                }
                else {
                    kLog.info(C("�ɹ���Ŀ�괰����Ϊ�����Ӵ���"), C(CURRENT_MODULE));
                }
            }
            else {
                kLog.warn(C("hwnd��Ч���޷���Ϊ�����Ӵ���"), C(CURRENT_MODULE));
            }
        }
#undef CURRENT_MODULE

        ImGui::Separator();

        // 4. ͸���ȵ��ڣ�ȫ�ֱ���`SelectedWindowAlpha`����״̬��
        ImGui::Text(C("͸����"));
        if (ImGui::SliderFloat("##window_alpha", &SelectedWindowAlpha, 0.01f, 1.0f, "%.1f",0)) {
            LONG ex_style = GetWindowLong(hwnd, GWL_EXSTYLE);
            if (SelectedWindowAlpha < 1.0f) {
                SetWindowLong(hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
                SetLayeredWindowAttributes(hwnd, 0, (BYTE)(SelectedWindowAlpha * 255), LWA_ALPHA);
            }
            else {
                SetWindowLong(hwnd, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
                RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            }
        }
    }
}static    HWND hwnd = NULL;
    ImVec2 mouse_pos = {};
extern void PointerWindow() {
    POINT mouse_pos_windows;

    //ImVec2 mouse_pos = ImGui::GetMousePos();
    if (!GetAsyncKeyState(VK_CONTROL)) {
        GetCursorPos(&mouse_pos_windows);

    // ��ȡ��ǰ���ھ��
        hwnd = WindowFromPoint(mouse_pos_windows);
        mouse_pos = ImVec2((float)(mouse_pos_windows.x + 2), (float)(mouse_pos_windows.y + 2));
    }
    // -------------------------------
    // �������ã��ô��ڸ������λ��
    // -------------------------------
    // ���ô��ڵ�����λ��Ϊ��굱ǰλ��
    //ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        ImGui::SetNextWindowPos(mouse_pos);
        float max_width = 1000;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0),           // ��С��ȸ߶ȣ�0��ʾ����Ӧ��
            ImVec2(max_width, FLT_MAX) // ����Ȳ��޸߶�
        );
    

    // -------------------------------
    // ���崰�ڵ���ʾ��������ʽ
    // -------------------------------
    // ��϶����־ȷ�������޷�������͸��
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration       // �ޱ��������ޱ߿�
          | ImGuiWindowFlags_AlwaysAutoResize   // �Զ��������ڴ�С����Ӧ����
         | ImGuiWindowFlags_NoSavedSettings
        ;            // ��ֹ�ֶ��϶����ڣ���ͨ���������λ�ã�
        //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.3f));

    // -------------------------------
    // ���ƴ�������
    // -------------------------------
    // ����͸����������ѡ��
    //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f)); // ��͸����ɫ����

    // ��ʼ���ƴ���
    if (ImGui::Begin("Mouse Following Window", nullptr, window_flags)) {
                    // ǰ��������Ϊ RGB��ʾ��Ϊ��͸����ɫ���ɸ������������
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 0.8f));
        {
                ImGuiWindow* current_window = ImGui::GetCurrentWindow();
                if (current_window && current_window->Viewport) {

                    HWND current_imgui_hwnd = (HWND)current_window->Viewport->PlatformHandle;
                    //SetLayeredWindowAttributes(current_imgui_hwnd, 0, 128, LWA_ALPHA);
                    LONG exStyle = GetWindowLongPtr(current_imgui_hwnd, GWL_EXSTYLE);
                    if (!(exStyle & WS_EX_LAYERED))
                    {
                        SetWindowLongPtr(current_imgui_hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
                        SetLayeredWindowAttributes(current_imgui_hwnd, 0, (BYTE)(255 * 0.65f), LWA_ALPHA);
                    }

                    // 3. ����͸���ȣ�80% ��͸���ȶ�Ӧ alpha ֵΪ 204����Χ 0~255��
                    // ���һ������ LWA_ALPHA ��ʾ�޸� alpha ͨ��͸����

                    SetWindowPos(
                        current_imgui_hwnd,
                        HWND_TOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
                    );
                }
            }
        if(showPointerWindowDetail){


            RenderPointerWindowInfo(hwnd);
            ImGui::Text(C("��ס")); ImGui::SameLine(); if (ImGui::Button("Ctrl")); ImGui::SameLine(); ImGui::Text(C("�Թ̶��˴���"));
        }// ��ѡ�����������̬����
        else {
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            ImGui::Text(GBKtoUTF8("   %d").c_str(), pid); ImGui::SameLine();


            LPDIRECT3DTEXTURE9 pTexture = GetCachedProcessIcon(pid);
            if (pTexture) {
                // ImGui::Image����������ָ�루ת��ΪImTextureID�����ߴ�
                ImGui::Image(
                    (ImTextureID)pTexture,  // DX9����ָ��ֱ����ΪImTextureID
                    ImVec2(16, 16)          // 16��16��ʾ�ߴ�
                );
            }
			ImGui::SameLine();
            if(ImGui::Button(("+"), ImVec2(16, 16))) {
                showPointerWindowDetail = 1;
            };
            ImGui::Text(GBKtoUTF8("%s").c_str(), C(GetProcessNameByPID(pid)));

        }
    }
    // ����֮ǰ���͵���ʽ���ָ�ԭ���ڱ�����
    ImGui::PopStyleColor();
    //ImGui::PopStyleColor();
    ImGui::End();

    // �ָ���ʽ
    //ImGui::PopStyleColor();
}