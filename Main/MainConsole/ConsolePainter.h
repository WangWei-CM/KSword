#pragma once
void DrawVerticalHexagon(
	float relativeX,     // 0~1����Դ���ˮƽλ��
	float relativeY,     // 0~1����Դ�����ֱλ��
	int radiusPx,        // ������ľ��Գ��ȣ����أ�
	ImU32 color,         // ImGui ��ɫ����͸���ȣ��� IM_COL32(...)��
	float lineThickness  // ������ϸ
);
// ���ƴ����䱳�����ı���
void DrawGradientBorderTextBox(
	float relX, float relY,      // ��Դ��ڵ�λ�ã�0~1��
	float relWidth, float relHeight, // ��Դ��ڵĿ�ߣ�0~1��
	ImU32 leftColor,             // �����������ɫ
	const char* text             // Ҫ���Ƶ��ı�
);
void DrawGradientLabel(
	float relX, float relY,       // ��Դ��ڵ�λ�ã�0~1��
	float relWidth, float relHeight, // ��Դ��ڵĿ�ߣ�0~1��
	ImU32 leftColor, ImU32 rightColor, // ����������ɫ
	const char* text,             // ��ʾ���ı�
	ImU32 textColor               // �ı���ɫ��Ĭ���������ɫһ�£�
);