#include "../KswordTotalHead.h"

inline void RenderPointerWindowInfo(HWND hwnd) {
    if (!hwnd) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[未找到窗口]");
        return;
    }

    // 获取窗口标题
    //ImGui::Columns(2, "two_columns");
    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    ImGui::Text(GBKtoUTF8("标题: %s").c_str(), title[0] ? GBKtoUTF8(title).c_str() : C("<无标题>"),500);

    // 获取窗口类名
    char class_name[256];
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    ImGui::Text(GBKtoUTF8("类名: %s").c_str(), GBKtoUTF8(class_name).c_str());

    // 获取窗口位置和尺寸
    RECT rect;
    GetWindowRect(hwnd, &rect);
    ImGui::Text(GBKtoUTF8("位置: (%d, %d)").c_str(), rect.left, rect.top);
    ImGui::Text(GBKtoUTF8("尺寸: %dx%d").c_str(), rect.right - rect.left, rect.bottom - rect.top);

    // 获取进程ID（扩展功能）
    //ImGui::NextColumn();
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    ImGui::Separator();
    ImGui::Text(GBKtoUTF8("进程ID: %d").c_str(), pid);
    ImGui::Text(GBKtoUTF8("进程名称: %s").c_str(), C(GetProcessNameByPID(pid)));

    //ImGui::Columns(1);
}


extern void PointerWindow() {
    POINT mouse_pos_windows;
    GetCursorPos(&mouse_pos_windows);

    // 获取当前窗口句柄
    HWND hwnd = WindowFromPoint(mouse_pos_windows);
    //ImVec2 mouse_pos = ImGui::GetMousePos();
    ImVec2 mouse_pos = ImVec2((float)(mouse_pos_windows.x+1), (float)(mouse_pos_windows.y+1));
    // -------------------------------
    // 核心设置：让窗口跟随鼠标位置
    // -------------------------------
    // 设置窗口的期望位置为鼠标当前位置
    //ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowPos(mouse_pos);
    float max_width = 1000;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0, 0),           // 最小宽度高度（0表示自适应）
        ImVec2(max_width, FLT_MAX) // 最大宽度不限高度
    );

    // -------------------------------
    // 定义窗口的显示参数和样式
    // -------------------------------
    // 组合多个标志确保窗口无法交互且透明
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration       // 无标题栏、无边框
        | ImGuiWindowFlags_AlwaysAutoResize   // 自动调整窗口大小以适应内容
        | ImGuiWindowFlags_NoSavedSettings    // 不保存窗口状态
        //| ImGuiWindowFlags_NoFocusOnAppearing // 出现时不抢占焦点
        | ImGuiWindowFlags_NoNav              // 禁止导航交互（如键盘控制）
        | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoMove
        | ImGuiViewportFlags_TopMost        // 系统级置顶
        | ImGuiViewportFlags_NoAutoMerge
        | ImGuiWindowFlags_Tooltip
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings
        ;            // 禁止手动拖动窗口（已通过代码控制位置）
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.3f));

    // -------------------------------
    // 绘制窗口内容
    // -------------------------------
    // 设置透明背景（可选）
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f)); // 半透明黑色背景

    // 开始绘制窗口
    if (ImGui::Begin("Mouse Following Window", nullptr, window_flags)) {
        // 显示当前鼠标坐标（示例内容）
        /*
        if (my_texture) {
            ImVec4 tint_color = ImVec4(1.0f, 1.0f, 1.0f, 0.5f); // RGB=白色, Alpha=0.3
            ImVec2 window_size = ImGui::GetContentRegionAvail();
            window_size.x += 0;
            window_size.y += 30;
            // 计算图片原始宽高比
            float image_aspect = (float)my_image_width / (float)my_image_height;

            // 计算窗口宽高比
            float window_aspect = window_size.x / window_size.y;

            // 计算覆盖填充的裁剪尺寸
            ImVec2 display_size;
            if (window_aspect > image_aspect) {
                // 窗口更宽 -> 按宽度填充，高度裁剪
                display_size.x = window_size.x;
                display_size.y = window_size.x / image_aspect;  // 图片高度需要扩展

                // 超出窗口高度的部分会被裁剪
            }
            else {
                // 窗口更高 -> 按高度填充，宽度裁剪
                display_size.y = window_size.y;
                display_size.x = window_size.y * image_aspect;  // 图片宽度需要扩展

                // 超出窗口宽度的部分会被裁剪
            }

            // 计算偏移量（使裁剪居中）
            ImVec2 offset = ImVec2(
                (display_size.x - window_size.x) * 0.5f,
                (display_size.y - window_size.y) * 0.5f
            );

            // 反转偏移量得到UV坐标
            ImVec2 uv0 = ImVec2(
                offset.x > 0 ? offset.x / display_size.x : 0.0f,
                offset.y > 0 ? offset.y / display_size.y : 0.0f
            );

            ImVec2 uv1 = ImVec2(
                offset.x > 0 ? 1.0f - offset.x / display_size.x : 1.0f,
                offset.y > 0 ? 1.0f - offset.y / display_size.y : 1.0f
            );

            // 使用完整的窗口大小绘制图片（自动裁剪超出部分）
            ImGui::SetCursorPos(ImVec2(0, 0));
            ImGui::Image(
                (ImTextureID)(intptr_t)my_texture,
                window_size,   // 使用窗口尺寸（裁剪效果来源于此）
                uv0,           // UV起始坐标（定义裁剪区域）
                uv1,            // UV结束坐标（定义裁剪区域）
                tint_color,
                ImVec4(0, 0, 0, 0)
            );
        }
        ImGui::SetCursorPos(ImVec2(0, 30)); // 重置到同一位置
        */
        ImGui::Text("Mouse Position: (%.0f, %.0f)", mouse_pos.x, mouse_pos.y);
        RenderPointerWindowInfo(hwnd);

        // 可选：添加其他动态内容
    }
    ImGui::PopStyleColor();
    ImGui::End();

    // 恢复样式
    ImGui::PopStyleColor();
}