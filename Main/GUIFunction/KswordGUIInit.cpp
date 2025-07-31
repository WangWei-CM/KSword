#include "../KswordTotalHead.h"

ImVec4 StyleColor;


void StyleColorsRedBlack(ImGuiStyle* dst/* = nullptr*/)
{
    StyleColor = ImVec4(0.50f, 0.00f, 0.00f, 0.50f);
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();
    ImVec4* colors = style->Colors;

    // ===== 基础配色 =====
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.85f, 0.85f, 1.00f);      // 浅粉文本[2](@ref)
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.00f, 0.00f, 0.94f);   // 深红黑背景[2](@ref)
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.02f, 0.02f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.02f, 0.02f, 0.94f);

    // ===== 边框与分隔线 =====
    colors[ImGuiCol_Border] = ImVec4(0.50f, 0.00f, 0.00f, 0.50f);    // 深红边框[2](@ref)
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.50f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.70f, 0.15f, 0.15f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.85f, 0.20f, 0.20f, 1.00f);

    // ===== 控件配色 =====
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.02f, 0.02f, 0.54f);   // 输入框背景[2](@ref)
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.03f, 0.03f, 0.54f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.04f, 0.04f, 0.54f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.25f, 0.00f, 0.00f, 1.00f);   // 非活动标题[2](@ref)
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.05f, 0.05f, 1.00f); // 活动标题[2](@ref)
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.20f, 0.00f, 0.00f, 0.75f);

    // ===== 按钮与交互元素 =====
    colors[ImGuiCol_Button] = ImVec4(0.35f, 0.00f, 0.00f, 0.70f);    // 按钮基础[2](@ref)
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.65f, 0.10f, 0.10f, 0.90f); // 悬停[2](@ref)
    colors[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);  // 点击[2](@ref)
    colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.20f, 0.20f, 1.00f); // 复选框标记[2](@ref)

    // ===== Tab栏专属配色 =====
    colors[ImGuiCol_Tab] = ImLerp(colors[ImGuiCol_Header], colors[ImGuiCol_TitleBgActive], 0.80f);
    colors[ImGuiCol_TabHovered] = colors[ImGuiCol_HeaderHovered];
    colors[ImGuiCol_TabSelected] = ImLerp(colors[ImGuiCol_HeaderActive], colors[ImGuiCol_TitleBgActive], 0.60f);
    colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_HeaderActive];
    colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
    colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);

    // ===== 表格与列表 =====
    colors[ImGuiCol_Header] = ImVec4(0.55f, 0.05f, 0.05f, 0.90f);    // 表头/选中行[2](@ref)
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.10f, 0.10f, 0.90f); // 悬停行[2](@ref)
    colors[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);  // 激活行[2](@ref)
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.30f, 0.00f, 0.00f, 0.90f); // 表头背景[2](@ref)

    // ===== 其他元素统一 =====
    colors[ImGuiCol_SliderGrab] = ImVec4(0.65f, 0.10f, 0.10f, 0.90f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.65f, 0.10f, 0.10f, 0.90f);    // 调整手柄[2](@ref)
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.85f, 0.15f, 0.15f, 0.35f);

    // ===== 视觉增强 =====
    style->WindowRounding = 5.0f;   // 窗口圆角[2](@ref)
    style->FrameRounding = 3.0f;    // 控件圆角[2](@ref)
    style->TabRounding = 4.0f;      // Tab栏圆角
}

inline void KswordInitAdminMode() {
	if (IsAdmin()) {
		EnableDebugPrivilege(TRUE);
	}
}