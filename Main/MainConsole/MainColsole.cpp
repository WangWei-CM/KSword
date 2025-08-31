#include "../KswordTotalHead.h"
#include "ConsolePainter.h"
extern bool EasyMode;

// minValue ������Ҫ�Ľ��
void RenderEasyModeFrame() {
	ImU32 Color = IM_COL32(
		(uint8_t)(StyleColor.x * 255.0f),  // ��ɫ����ת��
		(uint8_t)(StyleColor.y * 255.0f),  // ��ɫ����ת��
		(uint8_t)(StyleColor.z * 255.0f),  // ��ɫ����ת��
		255);
	ImGui::Begin("Easy Mode", nullptr);
	float height = ImGui::GetWindowSize().y;
	float width = ImGui::GetWindowSize().x;
	float Size = max(width * 9.0f / 16.0f, height);
	if(ImGui::Button("Switch to Normal Mode"))
		EasyMode = false;
    // ��ɫ���������
    ImU32 titleLeft = IM_COL32(50, 100, 150, 255);
    ImU32 titleRight = IM_COL32(50, 100, 150, 0);
    ImU32 contentLeft = IM_COL32(30, 80, 130, 255);
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    ImVec2 winSize = ImGui::GetWindowSize();
    float centerWidth = winSize.x * 0.3f; // �м�������Ϊ����30%

    // ====================== 1. ����ϵͳģ�� ======================
    DrawGradientLabel(
        0.25f, 0.05f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("����ϵͳ"), textColor
    );
    DrawGradientBorderTextBox(
        0.25f, 0.1f, 0.25f, 0.1f,
        contentLeft,
        C("Windows10 1909רҵ����վ 12415.4153")
    );

    // ====================== 2. Ӳ��ģ�� ======================
    DrawGradientLabel(
        0.6f, 0.05f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("Ӳ��"), textColor
    );
    DrawGradientBorderTextBox(
        0.6f, 0.1f, 0.25f, 0.1f,
        contentLeft,
        C("CPU: Intel(R) Xeon E5 2682 v4\n�ڴ�: 32GB\n�Կ�: GT 750")
    );

    // ====================== 3. ����ģ�飨����ı��� ======================
    DrawGradientLabel(
        0.7f, 0.3f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("����"), textColor
    );
    DrawGradientBorderTextBox(0.7f, 0.35f, 0.2f, 0.06f, contentLeft, C("�ϴ�����: 120kb/s"));
    DrawGradientBorderTextBox(0.7f, 0.42f, 0.2f, 0.06f, contentLeft, C("��������: 114kb/s"));
    DrawGradientBorderTextBox(0.7f, 0.49f, 0.2f, 0.06f, contentLeft, C("TCP������: 114"));

    // ====================== 4. �ڴ�ģ�飨����ı��� ======================
    DrawGradientLabel(
        0.6f, 0.7f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("�ڴ�"), textColor
    );
    DrawGradientBorderTextBox(0.6f, 0.75f, 0.25f, 0.07f, contentLeft, C("���ڴ�: 32GB+0GB"));
    DrawGradientBorderTextBox(0.6f, 0.83f, 0.25f, 0.07f, contentLeft, C("��ʹ��: 7GB"));

    // ====================== 5. �û�ģ�飨����ı��� ======================
    DrawGradientLabel(
        0.25f, 0.7f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("�û�"), textColor
    );
    DrawGradientBorderTextBox(0.25f, 0.75f, 0.25f, 0.07f, contentLeft, C("Administrator ������ ���ص�¼"));
    DrawGradientBorderTextBox(0.25f, 0.83f, 0.25f, 0.07f, contentLeft, C("User δ���� Զ�̵�½"));

    // ====================== 6. ����ģ�飨����ı��� ======================
    DrawGradientLabel(
        0.05f, 0.3f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("����"), textColor
    );
    DrawGradientBorderTextBox(0.05f, 0.35f, 0.2f, 0.07f, contentLeft, C("�ܽ���: 250"));
    DrawGradientBorderTextBox(0.05f, 0.43f, 0.2f, 0.07f, contentLeft, C("�����: 114514"));
    DrawGradientBorderTextBox(0.05f, 0.51f, 0.2f, 0.07f, contentLeft, C("�߳���: 1919"));

    // ====================== �м����������������� ======================
    float centerX = winSize.x * 0.5f;
    float centerY = winSize.y * 0.5f;
    // �м��ı�
    float hexRadius = 250; // ������Ӱ뾶��ȷ���������㹻��
    ImU32 hexColor = IM_COL32(30, 80, 130, 255); // ͳһ��ɫ���ɸ�����Ҫ΢��͸����
    ImGui::SetCursorPos(ImVec2(centerX - 40, centerY - 20));
    ImGui::TextColored(ImVec4(0.2f, 0.4f, 0.6f, 1.0f), C("RAM: 23%%"));
    ImGui::SetCursorPos(ImVec2(centerX + 20, centerY - 20));
    ImGui::TextColored(ImVec4(0.2f, 0.4f, 0.6f, 1.0f), C("CPU: 9.2%%"));
    ImGui::SetCursorPos(ImVec2(centerX - 30, centerY + 10));
    ImGui::TextColored(ImVec4(0.2f, 0.4f, 0.6f, 1.0f), C("Ksword5.0 ������ģʽ"));
    DrawVerticalHexagon(0.3f, 0.0f, hexRadius, hexColor, 2.0f);  // ��������
    DrawVerticalHexagon(0.7f, 0.0f, hexRadius, hexColor, 2.0f);  // ��������
    DrawVerticalHexagon(0.3f, 1.0f, hexRadius, hexColor, 2.0f);  // ��������
    DrawVerticalHexagon(0.7f, 1.0f, hexRadius, hexColor, 2.0f);  // ��������
    DrawVerticalHexagon(0.12f, 0.5f, hexRadius, hexColor, 2.0f);  // ����м�����
    DrawVerticalHexagon(0.88f, 0.5f, hexRadius, hexColor, 2.0f);  // �Ҳ��м�����
    DrawVerticalHexagon(0.5f, 0.5f, 25+hexRadius, hexColor, 2.0f);  // ���м�����
	ImGui::End();
}