#pragma once
#include "../KswordTotalHead.h"
// �����������Ƕ�ת����
// �����������Ƕ�ת����
static float DegToRad(float deg) {
    return deg * 3.1415926535f / 180.0f;
}
void drawGradientRectangle(ImDrawList* drawList, ImVec2 pos, ImVec2 size, ImU32 leftColor, ImU32 rightColor) {
    int steps = 50; // ���䲽��������Խ�ཥ��Խƽ��
    float stepWidth = size.x / steps;

    for (int i = 0; i < steps; ++i) {
        float t = static_cast<float>(i) / steps;
        ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(
                // ���Բ�ֵ������ɫ
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
// ���ƴ��߿���ı��򣨽��߿���ɫ������͸�����ı�����
// ���������������ɫͨ������ȡRGBA������
// ���������������ɫͨ������ȡ RGBA ������
static void GetColorChannels(ImU32 color, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (color >> 24) & 0xFF;
    g = (color >> 16) & 0xFF;
    b = (color >> 8) & 0xFF;
    a = color & 0xFF;
}

// ��������������ˮƽ�����߶Σ�������͸���Ƚ��䣩
static void DrawHorizontalGradientSegment(
    ImDrawList* drawList,
    const ImVec2& start,    // �߶���㣨��
    const ImVec2& end,      // �߶��յ㣨�ң�
    ImU32 baseColor,        // ��׼��ɫ��RGB ���䣬������ Alpha��
    float startAlphaRatio,  // ���͸���ȱ�����0~1��
    float endAlphaRatio,    // �յ�͸���ȱ�����0~1��
    float thickness         // �߶δ�ϸ
) {
    uint8_t r, g, b, baseA;
    GetColorChannels(baseColor, r, g, b, baseA);

    const int segments = 30; // ��ֶ�����Խ�ཥ��Խƽ����
    float segmentLen = (end.x - start.x) / segments;

    for (int i = 0; i < segments; ++i) {
        // ���㵱ǰ�ε�͸���ȱ��������Բ�ֵ��
        float t = static_cast<float>(i) / segments;
        float alphaRatio = startAlphaRatio + t * (endAlphaRatio - startAlphaRatio);
        uint8_t currentA = static_cast<uint8_t>(baseA * alphaRatio);

        // ���㵱ǰ�ε������յ�
        ImVec2 segStart = ImVec2(start.x + i * segmentLen, start.y);
        ImVec2 segEnd = ImVec2(start.x + (i + 1) * segmentLen, end.y);

        // ���Ƶ�ǰ��
        ImU32 segColor = IM_COL32(r, g, b, currentA);
        drawList->AddLine(segStart, segEnd, segColor, thickness);
    }
}

void ExtractColorChannels(ImU32 color, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (color) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = (color >> 16) & 0xFF;
    a = (color >> 24) & 0xFF;
}

void DrawGradientBorderTextBox(
    float relX, float relY,
    float relWidth, float relHeight,
    ImU32 borderColor,
    const char* text
) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;

    ImDrawList* drawList = window->DrawList;
    ImVec2 windowPos = window->Pos;
    ImVec2 windowSize = ImGui::GetWindowSize();
    const float borderThickness = 2.0f;
    const int segments = 50;  // ����Խ�ཥ��Խƽ��

    // �����ı����������
    ImVec2 rectMin(
        windowPos.x + relX * windowSize.x,
        windowPos.y + relY * windowSize.y
    );
    ImVec2 rectMax(
        windowPos.x + (relX + relWidth) * windowSize.x,
        windowPos.y + (relY + relHeight) * windowSize.y
    );
    const float totalWidth = rectMax.x - rectMin.x;

    // ��ȡ������ɫ�� RGB ͨ��������ԭʼ alpha�����¿��ƽ��䣩
    uint8_t targetR, targetG, targetB, unusedA;
    ExtractColorChannels(borderColor, targetR, targetG, targetB, unusedA);

    // 1. ������߿�ʵɫ��alpha ��Ϊ 255��
    drawList->AddLine(
        rectMin,
        ImVec2(rectMin.x, rectMax.y),
        IM_COL32(targetR, targetG, targetB, 255),
        borderThickness
    );

    // 2. �����ϱ߿�ˮƽ���䣺��ʵɫ �� ��͸����
    for (int i = 0; i < segments; ++i) {
        float x1 = rectMin.x + (i * totalWidth) / segments;
        float x2 = rectMin.x + ((i + 1) * totalWidth) / segments;
        // ͸���ȴ� 1 �� 0 ����
        float alphaRatio = 1.0f - static_cast<float>(i) / segments;
        uint8_t currentAlpha = static_cast<uint8_t>(alphaRatio * 255);

        drawList->AddLine(
            ImVec2(x1, rectMin.y),
            ImVec2(x2, rectMin.y),
            IM_COL32(targetR, targetG, targetB, currentAlpha),
            borderThickness
        );
    }

    // 3. �����±߿�ˮƽ���䣺��ʵɫ �� ��͸����
    for (int i = 0; i < segments; ++i) {
        float x1 = rectMin.x + (i * totalWidth) / segments;
        float x2 = rectMin.x + ((i + 1) * totalWidth) / segments;
        float alphaRatio = 1.0f - static_cast<float>(i) / segments;
        uint8_t currentAlpha = static_cast<uint8_t>(alphaRatio * 255);

        drawList->AddLine(
            ImVec2(x1, rectMax.y),
            ImVec2(x2, rectMax.y),
            IM_COL32(targetR, targetG, targetB, currentAlpha),
            borderThickness
        );
    }

    // 4. �����ı�����ɫ����߿�һ�£�ʵɫ��
    ImVec2 textPos;
    textPos.x = rectMin.x + 8.0f;
    textPos.y = rectMin.y + (rectMax.y - rectMin.y - ImGui::GetTextLineHeight()) * 0.5f;

    ImGui::SetCursorScreenPos(textPos);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(targetR, targetG, targetB, 255));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void DrawVerticalHexagon(
    float relativeX,     // 0~1����Դ��ڵ�ˮƽλ�ã����ڴ������Ͻǣ�
    float relativeY,     // 0~1����Դ��ڵ���ֱλ�ã����ڴ������Ͻǣ�
    int radiusPx,        // ������ľ��Գ��ȣ����أ����Բ�뾶��
    ImU32 color,         // ImGui ��ɫ����͸���ȣ��� IM_COL32(...)��
    float lineThickness  // ������ϸ
) {
    // 1. ��ȡ���ھ���λ�� + ���ڴ�С
    ImVec2 windowPos = ImGui::GetWindowPos();     // �������Ͻ�����Ļ�ľ�������
    ImVec2 windowSize = ImGui::GetWindowSize();   // ���ڵĿ�ߣ����أ�

    // 2. ���������������ĵ��**������Ļ����**
    //    relativeX/Y ������ڴ��ڵı���������Ӵ��������λ��
    float centerX = windowPos.x + relativeX * windowSize.x;
    float centerY = windowPos.y + relativeY * windowSize.y;

    // 3. ���� 6 ������ľ�����Ļ���꣨�ϸ�պϣ�
    ImVec2 points[6];
    for (int i = 0; i < 6; ++i) {
        // ����Ƕȣ������Ϸ���90�㣩��ʼ��ÿ�ε��� 60��
        float angleDeg = 90.0f + 60.0f * i;
        float angleRad = DegToRad(angleDeg);

        points[i].x = centerX + radiusPx * cos(angleRad);
        points[i].y = centerY + radiusPx * sin(angleRad);
    }

    // 4. ��ȡ�����б������������Σ��ϸ�պϣ�
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddPolyline(
        points,
        6,          // ����������6 ������ = �պ�ͼ�Σ�
        color,
        true,      // �Ƿ�Ϊ�պ��ߣ�AddPolyline ���Զ��պ���β��
        lineThickness
    );
}
void DrawGradientLabel(
    float relX, float relY,       // ��Դ��ڵ�λ�ã�0~1��
    float relWidth, float relHeight, // ��Դ��ڵĿ�ߣ�0~1��
    ImU32 leftColor, ImU32 rightColor, // ����������ɫ
    const char* text,             // ��ʾ���ı�
    ImU32 textColor = 0           // �ı���ɫ��Ĭ���������ɫһ�£�
) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;

    ImDrawList* drawList = window->DrawList;
    ImVec2 windowPos = window->Pos;
    ImVec2 windowSize = ImGui::GetWindowSize();

    // 1. ���㽥����εľ���λ�úʹ�С
    ImVec2 rectPos(
        windowPos.x + relX * windowSize.x,
        windowPos.y + relY * windowSize.y
    );
    ImVec2 rectSize(
        relWidth * windowSize.x,
        relHeight * windowSize.y
    );

    // 2. ����ˮƽ���䱳��
    drawGradientRectangle(drawList, rectPos, rectSize, leftColor, rightColor);

    // 3. �����ı�λ�ã����� + ��ֱ���У�
    ImVec2 textPos;
    textPos.x = rectPos.x + 8.0f; // ����ڱ߾�
    textPos.y = rectPos.y + (rectSize.y - ImGui::GetTextLineHeight()) * 0.5f; // ��ֱ����

    // 4. �����ı���ɫ��Ĭ���������ɫһ�£�
    if (textColor == 0) {
        textColor = leftColor;
    }

    // 5. �����ı�
    ImGui::SetCursorScreenPos(textPos);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}