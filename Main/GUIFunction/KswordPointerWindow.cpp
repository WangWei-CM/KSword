#include "../KswordTotalHead.h"

inline void RenderPointerWindowInfo(HWND hwnd) {
    if (!hwnd) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[δ�ҵ�����]");
        return;
    }

    // ��ȡ���ڱ���
    //ImGui::Columns(2, "two_columns");
    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    ImGui::Text(GBKtoUTF8("����: %s").c_str(), title[0] ? GBKtoUTF8(title).c_str() : C("<�ޱ���>"),500);

    // ��ȡ��������
    char class_name[256];
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    ImGui::Text(GBKtoUTF8("����: %s").c_str(), GBKtoUTF8(class_name).c_str());

    // ��ȡ����λ�úͳߴ�
    RECT rect;
    GetWindowRect(hwnd, &rect);
    ImGui::Text(GBKtoUTF8("λ��: (%d, %d)").c_str(), rect.left, rect.top);
    ImGui::Text(GBKtoUTF8("�ߴ�: %dx%d").c_str(), rect.right - rect.left, rect.bottom - rect.top);

    // ��ȡ����ID����չ���ܣ�
    //ImGui::NextColumn();
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    ImGui::Separator();
    ImGui::Text(GBKtoUTF8("����ID: %d").c_str(), pid);
    ImGui::Text(GBKtoUTF8("��������: %s").c_str(), C(GetProcessNameByPID(pid)));

    //ImGui::Columns(1);
}


extern void PointerWindow() {
    POINT mouse_pos_windows;
    GetCursorPos(&mouse_pos_windows);

    // ��ȡ��ǰ���ھ��
    HWND hwnd = WindowFromPoint(mouse_pos_windows);
    //ImVec2 mouse_pos = ImGui::GetMousePos();
    ImVec2 mouse_pos = ImVec2((float)(mouse_pos_windows.x+1), (float)(mouse_pos_windows.y+1));
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
        | ImGuiWindowFlags_NoSavedSettings    // �����洰��״̬
        //| ImGuiWindowFlags_NoFocusOnAppearing // ����ʱ����ռ����
        | ImGuiWindowFlags_NoNav              // ��ֹ��������������̿��ƣ�
        | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoMove
        | ImGuiViewportFlags_TopMost        // ϵͳ���ö�
        | ImGuiViewportFlags_NoAutoMerge
        | ImGuiWindowFlags_Tooltip
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings
        ;            // ��ֹ�ֶ��϶����ڣ���ͨ���������λ�ã�
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.3f));

    // -------------------------------
    // ���ƴ�������
    // -------------------------------
    // ����͸����������ѡ��
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f)); // ��͸����ɫ����

    // ��ʼ���ƴ���
    if (ImGui::Begin("Mouse Following Window", nullptr, window_flags)) {
        // ��ʾ��ǰ������꣨ʾ�����ݣ�
        /*
        if (my_texture) {
            ImVec4 tint_color = ImVec4(1.0f, 1.0f, 1.0f, 0.5f); // RGB=��ɫ, Alpha=0.3
            ImVec2 window_size = ImGui::GetContentRegionAvail();
            window_size.x += 0;
            window_size.y += 30;
            // ����ͼƬԭʼ��߱�
            float image_aspect = (float)my_image_width / (float)my_image_height;

            // ���㴰�ڿ�߱�
            float window_aspect = window_size.x / window_size.y;

            // ���㸲�����Ĳü��ߴ�
            ImVec2 display_size;
            if (window_aspect > image_aspect) {
                // ���ڸ��� -> �������䣬�߶Ȳü�
                display_size.x = window_size.x;
                display_size.y = window_size.x / image_aspect;  // ͼƬ�߶���Ҫ��չ

                // �������ڸ߶ȵĲ��ֻᱻ�ü�
            }
            else {
                // ���ڸ��� -> ���߶���䣬��Ȳü�
                display_size.y = window_size.y;
                display_size.x = window_size.y * image_aspect;  // ͼƬ�����Ҫ��չ

                // �������ڿ�ȵĲ��ֻᱻ�ü�
            }

            // ����ƫ������ʹ�ü����У�
            ImVec2 offset = ImVec2(
                (display_size.x - window_size.x) * 0.5f,
                (display_size.y - window_size.y) * 0.5f
            );

            // ��תƫ�����õ�UV����
            ImVec2 uv0 = ImVec2(
                offset.x > 0 ? offset.x / display_size.x : 0.0f,
                offset.y > 0 ? offset.y / display_size.y : 0.0f
            );

            ImVec2 uv1 = ImVec2(
                offset.x > 0 ? 1.0f - offset.x / display_size.x : 1.0f,
                offset.y > 0 ? 1.0f - offset.y / display_size.y : 1.0f
            );

            // ʹ�������Ĵ��ڴ�С����ͼƬ���Զ��ü��������֣�
            ImGui::SetCursorPos(ImVec2(0, 0));
            ImGui::Image(
                (ImTextureID)(intptr_t)my_texture,
                window_size,   // ʹ�ô��ڳߴ磨�ü�Ч����Դ�ڴˣ�
                uv0,           // UV��ʼ���꣨����ü�����
                uv1,            // UV�������꣨����ü�����
                tint_color,
                ImVec4(0, 0, 0, 0)
            );
        }
        ImGui::SetCursorPos(ImVec2(0, 30)); // ���õ�ͬһλ��
        */
        ImGui::Text("Mouse Position: (%.0f, %.0f)", mouse_pos.x, mouse_pos.y);
        RenderPointerWindowInfo(hwnd);

        // ��ѡ�����������̬����
    }
    ImGui::PopStyleColor();
    ImGui::End();

    // �ָ���ʽ
    ImGui::PopStyleColor();
}