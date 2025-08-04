#include "../KswordTotalHead.h"

ImVec4 StyleColor;


void StyleColorsRedBlack(ImGuiStyle* dst/* = nullptr*/)
{
    StyleColor = ImVec4(0.50f, 0.00f, 0.00f, 0.50f);
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();
    ImVec4* colors = style->Colors;

    // ===== ������ɫ =====
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.85f, 0.85f, 1.00f);      // ǳ���ı�[2](@ref)
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.00f, 0.00f, 0.94f);   // ���ڱ���[2](@ref)
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.02f, 0.02f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.02f, 0.02f, 0.94f);

    // ===== �߿���ָ��� =====
    colors[ImGuiCol_Border] = ImVec4(0.50f, 0.00f, 0.00f, 0.50f);    // ���߿�[2](@ref)
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.50f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.70f, 0.15f, 0.15f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.85f, 0.20f, 0.20f, 1.00f);

    // ===== �ؼ���ɫ =====
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.02f, 0.02f, 0.54f);   // ����򱳾�[2](@ref)
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.03f, 0.03f, 0.54f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.04f, 0.04f, 0.54f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.25f, 0.00f, 0.00f, 1.00f);   // �ǻ����[2](@ref)
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.05f, 0.05f, 1.00f); // �����[2](@ref)
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.20f, 0.00f, 0.00f, 0.75f);

    // ===== ��ť�뽻��Ԫ�� =====
    colors[ImGuiCol_Button] = ImVec4(0.35f, 0.00f, 0.00f, 0.70f);    // ��ť����[2](@ref)
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.65f, 0.10f, 0.10f, 0.90f); // ��ͣ[2](@ref)
    colors[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);  // ���[2](@ref)
    colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.20f, 0.20f, 1.00f); // ��ѡ����[2](@ref)

    // ===== Tab��ר����ɫ =====
    colors[ImGuiCol_Tab] = ImLerp(colors[ImGuiCol_Header], colors[ImGuiCol_TitleBgActive], 0.80f);
    colors[ImGuiCol_TabHovered] = colors[ImGuiCol_HeaderHovered];
    colors[ImGuiCol_TabSelected] = ImLerp(colors[ImGuiCol_HeaderActive], colors[ImGuiCol_TitleBgActive], 0.60f);
    colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_HeaderActive];
    colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
    colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);

    // ===== ������б� =====
    colors[ImGuiCol_Header] = ImVec4(0.55f, 0.05f, 0.05f, 0.90f);    // ��ͷ/ѡ����[2](@ref)
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.10f, 0.10f, 0.90f); // ��ͣ��[2](@ref)
    colors[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);  // ������[2](@ref)
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.30f, 0.00f, 0.00f, 0.90f); // ��ͷ����[2](@ref)

    // ===== ����Ԫ��ͳһ =====
    colors[ImGuiCol_SliderGrab] = ImVec4(0.65f, 0.10f, 0.10f, 0.90f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.65f, 0.10f, 0.10f, 0.90f);    // �����ֱ�[2](@ref)
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.85f, 0.15f, 0.15f, 0.35f);

    // ===== �Ӿ���ǿ =====
    style->WindowRounding = 5.0f;   // ����Բ��[2](@ref)
    style->FrameRounding = 3.0f;    // �ؼ�Բ��[2](@ref)
    style->TabRounding = 4.0f;      // Tab��Բ��
}

inline void KswordInitAdminMode() {
	if (IsAdmin()) {
		EnableDebugPrivilege(TRUE);
	}
}