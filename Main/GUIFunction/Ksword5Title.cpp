#include "../KswordTotalHead.h"

static ImVec2 TitleHighLightStartPos;

//imgui绘制矩形


void drawGradientRectangle(ImDrawList* drawList, ImVec2 pos, ImVec2 size, ImU32 leftColor, ImU32 rightColor) {
    int steps = 20; // 渐变步数，步数越多渐变越平滑
    float stepWidth = size.x / steps;

    for (int i = 0; i < steps; ++i) {
        float t = static_cast<float>(i) / steps;
        ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(
                // 线性插值计算颜色
                ImGui::ColorConvertU32ToFloat4(leftColor).x + t * (ImGui::ColorConvertU32ToFloat4(rightColor).x - ImGui::ColorConvertU32ToFloat4(leftColor).x),
                ImGui::ColorConvertU32ToFloat4(leftColor).y + t * (ImGui::ColorConvertU32ToFloat4(rightColor).y - ImGui::ColorConvertU32ToFloat4(leftColor).y),
                ImGui::ColorConvertU32ToFloat4(leftColor).z + t * (ImGui::ColorConvertU32ToFloat4(rightColor).z - ImGui::ColorConvertU32ToFloat4(leftColor).z),
                ImGui::ColorConvertU32ToFloat4(leftColor).w + t * (ImGui::ColorConvertU32ToFloat4(rightColor).w - ImGui::ColorConvertU32ToFloat4(leftColor).w)
            )
        );
        drawList->AddRectFilled(
            ImVec2(pos.x + i * stepWidth, pos.y),
            ImVec2(pos.x + (i + 1) * stepWidth, pos.y + size.y),
            color
        );
    }
}

void MinimizeMainWindow() {
    HWND hwnd = (HWND)ImGui::GetMainViewport()->PlatformHandle;
    ShowWindow(hwnd, SW_MINIMIZE);
}
extern IDirect3DTexture9* g_IconTexture;
extern ImTextureID g_IconImTextureID ;

static bool is_fullscreen = false;
static ImVec2 original_pos = ImVec2(100, 100);
static ImVec2 original_size = ImVec2(0, 0);
static bool first_run = true;
static bool should_switch_to_full = false;
static bool should_switch_to_normal = false;
inline void ToggleFullscreen()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    if (first_run) {
        TitleHighLightStartPos = ImVec2(ImGui::GetWindowPos().x - 300, ImGui::GetWindowPos().y);
        // 首次运行时设置默认大小（屏幕的80%）
        original_size = ImVec2(viewport->Size.x * 0.8f, viewport->Size.y * 0.8f);
        first_run = false;
    }

    if (!is_fullscreen) {
        // 进入全屏前保存当前状态
        should_switch_to_full = 1;
    }
    else {
        // ✅ 关键修改：使用 SetNextWindow 系列函数
        should_switch_to_normal = 1;
    }
}
ImVec2 KswordTitleHighLightSize(0, 0);
inline void Ksword5Title() {
        ImGui::SetNextWindowSizeConstraints(
        ImVec2(200,30),  // 最小宽度为0（完全自适应）
        ImVec2(FLT_MAX, 30) // 最大宽度无限制
    );


        if (should_switch_to_full == 1) {
            original_pos = ImGui::GetWindowPos();
            original_size = ImGui::GetWindowSize();
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            // ✅ 关键修改：使用 SetNextWindow 系列函数
            ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
            is_fullscreen = true;
            should_switch_to_full = 0;
        }
        else if (should_switch_to_normal == 1) {
            ImGui::SetNextWindowPos(original_pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(original_size, ImGuiCond_Always);
            is_fullscreen = false;
            should_switch_to_normal = 0;
        }

        ImGui::Begin("Windows Style Window", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | 
        ImGuiWindowFlags_NoCollapse);
    const float window_height = ImGui::GetFrameHeight();
    ImVec2 currentSize = ImGui::GetWindowSize();
    if (currentSize.y != 30) {
        ImGui::SetWindowSize(ImVec2(currentSize.x, 30));
    }

    // 获取当前窗口位置和大小
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();

    // 计算图标在窗口内的绝对坐标（窗口内 (5, 5) 位置）
    ImVec2 iconPos = ImVec2(windowPos.x + 5, windowPos.y + 5);
    ImVec2 iconSize = ImVec2(20, 20);  // 图标尺寸

    // 使用 ImGui 的绘制列表在指定位置绘制图标
    ImGui::GetWindowDrawList()->AddImage(
        g_IconImTextureID,
        iconPos,
        ImVec2(iconPos.x + iconSize.x, iconPos.y + iconSize.y),
        ImVec2(0, 0),  // UV 坐标起点
        ImVec2(1, 1)   // UV 坐标终点
    );
    ImGui::SetCursorPos(ImVec2(30, 8)); // 图标右侧(5+30+5=40)
    ImGui::Text("Ksword 5.0");
    // 获取当前主题颜色并转换为 ImVec4
    ImVec4 bgColor = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_WindowBg));
    ImVec4 textColor = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_Text));

    // 基于背景色动态计算按钮颜色
    ImVec4 buttonBg = ImLerp(bgColor, ImVec4(0.2f, 0.4f, 0.8f, 1.0f), 0.8f);
    ImVec4 buttonHover = ImLerp(bgColor, ImVec4(0.3f, 0.5f, 0.9f, 1.0f), 0.7f);
    ImVec4 buttonActive = ImLerp(bgColor, ImVec4(0.1f, 0.3f, 0.7f, 1.0f), 0.6f);

    // 应用动态颜色
    ImGui::PushStyleColor(ImGuiCol_Button, buttonBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buttonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, buttonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);

    // 绘制"Developer"按钮（点击不响应）
    ImGui::SameLine(); // 靠右对齐
    ImGui::Button("Developer", ImVec2(80, 20));

    // 恢复ImGui样式
    ImGui::PopStyleColor(4);

    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    const float button_size_x = 50.0f;  // 按钮尺寸
    const float button_size_y = 30.0f;  // 按钮尺寸
    const float spacing = 2.0f;       // 按钮间距

    // 计算按钮组位置（右上角）
    ImVec2 button_group_pos = ImVec2(
        window_pos.x + window_size.x - (button_size_x * 2 + spacing * 2),
        window_pos.y
    );

    // 保存原始样式
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 orig_button = style.Colors[ImGuiCol_Button];
    ImVec4 orig_hover = style.Colors[ImGuiCol_ButtonHovered];
    ImVec4 orig_active = style.Colors[ImGuiCol_ButtonActive];
    float orig_rounding = style.FrameRounding;
    ImVec2 orig_spacing = style.ItemSpacing;

    ImVec4 accentColor = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_TitleBgActive));
    // 设置直角无间距样式
    style.ScrollbarSize = 0.0f;  // 完全隐藏滚动条[7,8](@ref)
    style.FrameRounding = 0.0f;         // 完全禁用圆角
    style.ItemSpacing = ImVec2(0, 0);    // 消除按钮间距
    ImVec4 btnNormal = ImLerp(bgColor, accentColor, 0.6f); // 背景融合主题色
    ImVec4 btnHover = ImLerp(bgColor, accentColor, 0.8f);
    ImVec4 btnActive = ImLerp(bgColor, accentColor, 0.4f);

    ImGui::PushStyleColor(ImGuiCol_Button, btnNormal);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnActive);


    // === 最小化按钮 ===
    ImGui::SetCursorScreenPos(button_group_pos);
    if (ImGui::Button("##Minimize", ImVec2(button_size_x, button_size_y))) {
        MinimizeMainWindow();
    }
    if (ImGui::IsItemVisible()) {
        ImVec2 rect_min = ImGui::GetItemRectMin();
        ImVec2 rect_max = ImGui::GetItemRectMax();
        float line_height = 1.8f;  // 横线高度
        float line_width = button_size_x * 0.2f;  // 横线宽度

        ImVec2 center = ImVec2(
            (rect_min.x + rect_max.x) * 0.5f,
            (rect_min.y + rect_max.y) * 0.5f
        );
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(
            ImVec2(center.x - line_width / 2, center.y - line_height / 2),
            ImVec2(center.x + line_width / 2, center.y + line_height / 2),
            ImGui::ColorConvertFloat4ToU32(textColor)
        );
    }

    // === 最大化/还原按钮 ===
    //ImGui::SameLine(0, spacing);
    //if (ImGui::Button(C("##Maxniumize"), ImVec2(button_size_x, button_size_y))) {
    //    ToggleFullscreen();
    //}
    //if (ImGui::IsItemVisible()) {
    //    ImVec2 rect_min = ImGui::GetItemRectMin();
    //    ImVec2 rect_max = ImGui::GetItemRectMax();
    //    float square_size = (button_size_y - 20) * 0.5f; // 正方形大小

    //    ImVec2 center = ImVec2(
    //        (rect_min.x + rect_max.x) * 0.5f,
    //        (rect_min.y + rect_max.y) * 0.5f
    //    );

    //    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    //    draw_list->AddRect(
    //        ImVec2(center.x - square_size, center.y - square_size),
    //        ImVec2(center.x + square_size, center.y + square_size),
    //        IM_COL32(KSWORD_BLUE_STYLE_R, KSWORD_BLUE_STYLE_G,KSWORD_BLUE_STYLE_B, 255), // 黑色边框
    //        0.0f, // 无圆角
    //        0, // 无特殊标志
    //        0.5f // 线宽
    //    );
    //}
    //ImGui::PopStyleColor(3);
    // === 关闭按钮（特殊红色样式）===
    ImGui::SameLine(0, spacing);
    //// 设置关闭按钮颜色
    //ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.1f, 0.1f, 1.0f)); // 红色
    //ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f)); // 悬停加深
    //ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.0f, 0.0f, 1.0f)); // 按下加深

    if (ImGui::Button(C("##Exittitle"), ImVec2(button_size_x, button_size_y))) {
        Ksword_main_should_exit = 1;
    }
    if (ImGui::IsItemVisible()) {
        ImVec2 rect_min = ImGui::GetItemRectMin();
        ImVec2 rect_max = ImGui::GetItemRectMax();
        float line_width = 0.3f;  // 线条宽度
        float cross_size = button_size_x * 0.1f;  // 交叉大小

        ImVec2 center = ImVec2(
            (rect_min.x + rect_max.x) * 0.5f,
            (rect_min.y + rect_max.y) * 0.5f
        );

        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        // 第一条斜线（左上到右下）
        draw_list->AddLine(
            ImVec2(center.x - cross_size, center.y - cross_size),
            ImVec2(center.x + cross_size, center.y + cross_size),
            ImGui::ColorConvertFloat4ToU32(textColor),
            line_width
        );

        // 第二条斜线（右上到左下）
        draw_list->AddLine(
            ImVec2(center.x + cross_size, center.y - cross_size),
            ImVec2(center.x - cross_size, center.y + cross_size),
            ImGui::ColorConvertFloat4ToU32(textColor)
        );
    }
    ImGui::PopStyleColor(3); // 恢复颜色


    // 恢复原始样式
    style.Colors[ImGuiCol_Button] = orig_button;
    style.Colors[ImGuiCol_ButtonHovered] = orig_hover;
    style.Colors[ImGuiCol_ButtonActive] = orig_active;
    style.FrameRounding = orig_rounding;
    style.ItemSpacing = orig_spacing;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    TitleHighLightStartPos.x += 30;
    if (TitleHighLightStartPos.x >= (ImGui::GetWindowSize().x + ImGui::GetWindowPos().x))TitleHighLightStartPos.x = ImGui::GetWindowPos().x - 300;
    TitleHighLightStartPos.y = ImGui::GetWindowPos().y;
    ImVec2 size(150, 30);

    // 定义左边和右边的颜色，这里左边透明度为0，右边透明度为1// 左边：透明的霓虹蓝（带Alpha渐变）
    ImU32 leftColor = ImGui::ColorConvertFloat4ToU32(ImVec4(StyleColor.w, StyleColor.x, StyleColor.y, 0.00f));
    // 右边：不透明的亮紫色
    ImU32 rightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(StyleColor.w, StyleColor.x, StyleColor.y, 0.50f));

    drawGradientRectangle(drawList, TitleHighLightStartPos, size, leftColor, rightColor);
    drawGradientRectangle(drawList, ImVec2(TitleHighLightStartPos.x+150,TitleHighLightStartPos.y), size, rightColor, leftColor);
    // 绘制渐变矩形
    ImGui::End();
}