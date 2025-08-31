#pragma once
#include "../KswordTotalHead.h"
// 辅助函数：角度转弧度
// 辅助函数：角度转弧度
static float DegToRad(float deg) {
    return deg * 3.1415926535f / 180.0f;
}
void drawGradientRectangle(ImDrawList* drawList, ImVec2 pos, ImVec2 size, ImU32 leftColor, ImU32 rightColor) {
    int steps = 50; // 渐变步数，步数越多渐变越平滑
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
// 绘制带边框的文本框（仅边框有色，背景透明，文本居左）
// 辅助函数：拆分颜色通道（获取RGBA分量）
// 辅助函数：拆分颜色通道（获取 RGBA 分量）
static void GetColorChannels(ImU32 color, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (color >> 24) & 0xFF;
    g = (color >> 16) & 0xFF;
    b = (color >> 8) & 0xFF;
    a = color & 0xFF;
}

// 辅助函数：绘制水平渐变线段（从左到右透明度渐变）
static void DrawHorizontalGradientSegment(
    ImDrawList* drawList,
    const ImVec2& start,    // 线段起点（左）
    const ImVec2& end,      // 线段终点（右）
    ImU32 baseColor,        // 基准颜色（RGB 不变，仅调整 Alpha）
    float startAlphaRatio,  // 起点透明度比例（0~1）
    float endAlphaRatio,    // 终点透明度比例（0~1）
    float thickness         // 线段粗细
) {
    uint8_t r, g, b, baseA;
    GetColorChannels(baseColor, r, g, b, baseA);

    const int segments = 30; // 拆分段数（越多渐变越平滑）
    float segmentLen = (end.x - start.x) / segments;

    for (int i = 0; i < segments; ++i) {
        // 计算当前段的透明度比例（线性插值）
        float t = static_cast<float>(i) / segments;
        float alphaRatio = startAlphaRatio + t * (endAlphaRatio - startAlphaRatio);
        uint8_t currentA = static_cast<uint8_t>(baseA * alphaRatio);

        // 计算当前段的起点和终点
        ImVec2 segStart = ImVec2(start.x + i * segmentLen, start.y);
        ImVec2 segEnd = ImVec2(start.x + (i + 1) * segmentLen, end.y);

        // 绘制当前段
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
    const int segments = 50;  // 段数越多渐变越平滑

    // 计算文本框绝对坐标
    ImVec2 rectMin(
        windowPos.x + relX * windowSize.x,
        windowPos.y + relY * windowSize.y
    );
    ImVec2 rectMax(
        windowPos.x + (relX + relWidth) * windowSize.x,
        windowPos.y + (relY + relHeight) * windowSize.y
    );
    const float totalWidth = rectMax.x - rectMin.x;

    // 提取传入颜色的 RGB 通道（忽略原始 alpha，重新控制渐变）
    uint8_t targetR, targetG, targetB, unusedA;
    ExtractColorChannels(borderColor, targetR, targetG, targetB, unusedA);

    // 1. 绘制左边框（实色，alpha 设为 255）
    drawList->AddLine(
        rectMin,
        ImVec2(rectMin.x, rectMax.y),
        IM_COL32(targetR, targetG, targetB, 255),
        borderThickness
    );

    // 2. 绘制上边框（水平渐变：左实色 → 右透明）
    for (int i = 0; i < segments; ++i) {
        float x1 = rectMin.x + (i * totalWidth) / segments;
        float x2 = rectMin.x + ((i + 1) * totalWidth) / segments;
        // 透明度从 1 到 0 渐变
        float alphaRatio = 1.0f - static_cast<float>(i) / segments;
        uint8_t currentAlpha = static_cast<uint8_t>(alphaRatio * 255);

        drawList->AddLine(
            ImVec2(x1, rectMin.y),
            ImVec2(x2, rectMin.y),
            IM_COL32(targetR, targetG, targetB, currentAlpha),
            borderThickness
        );
    }

    // 3. 绘制下边框（水平渐变：左实色 → 右透明）
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

    // 4. 绘制文本（颜色与左边框一致，实色）
    ImVec2 textPos;
    textPos.x = rectMin.x + 8.0f;
    textPos.y = rectMin.y + (rectMax.y - rectMin.y - ImGui::GetTextLineHeight()) * 0.5f;

    ImGui::SetCursorScreenPos(textPos);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(targetR, targetG, targetB, 255));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void DrawVerticalHexagon(
    float relativeX,     // 0~1，相对窗口的水平位置（基于窗口左上角）
    float relativeY,     // 0~1，相对窗口的竖直位置（基于窗口左上角）
    int radiusPx,        // 到顶点的绝对长度（像素，外接圆半径）
    ImU32 color,         // ImGui 颜色（含透明度，如 IM_COL32(...)）
    float lineThickness  // 线条粗细
) {
    // 1. 获取窗口绝对位置 + 窗口大小
    ImVec2 windowPos = ImGui::GetWindowPos();     // 窗口左上角在屏幕的绝对坐标
    ImVec2 windowSize = ImGui::GetWindowSize();   // 窗口的宽高（像素）

    // 2. 计算正六边形中心点的**绝对屏幕坐标**
    //    relativeX/Y 是相对于窗口的比例，需叠加窗口自身的位置
    float centerX = windowPos.x + relativeX * windowSize.x;
    float centerY = windowPos.y + relativeY * windowSize.y;

    // 3. 计算 6 个顶点的绝对屏幕坐标（严格闭合）
    ImVec2 points[6];
    for (int i = 0; i < 6; ++i) {
        // 顶点角度：从正上方（90°）开始，每次递增 60°
        float angleDeg = 90.0f + 60.0f * i;
        float angleRad = DegToRad(angleDeg);

        points[i].x = centerX + radiusPx * cos(angleRad);
        points[i].y = centerY + radiusPx * sin(angleRad);
    }

    // 4. 获取绘制列表，绘制正六边形（严格闭合）
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddPolyline(
        points,
        6,          // 顶点数量（6 个顶点 = 闭合图形）
        color,
        true,      // 是否为闭合线（AddPolyline 会自动闭合首尾）
        lineThickness
    );
}
void DrawGradientLabel(
    float relX, float relY,       // 相对窗口的位置（0~1）
    float relWidth, float relHeight, // 相对窗口的宽高（0~1）
    ImU32 leftColor, ImU32 rightColor, // 渐变左右颜色
    const char* text,             // 显示的文本
    ImU32 textColor = 0           // 文本颜色（默认与左侧颜色一致）
) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;

    ImDrawList* drawList = window->DrawList;
    ImVec2 windowPos = window->Pos;
    ImVec2 windowSize = ImGui::GetWindowSize();

    // 1. 计算渐变矩形的绝对位置和大小
    ImVec2 rectPos(
        windowPos.x + relX * windowSize.x,
        windowPos.y + relY * windowSize.y
    );
    ImVec2 rectSize(
        relWidth * windowSize.x,
        relHeight * windowSize.y
    );

    // 2. 绘制水平渐变背景
    drawGradientRectangle(drawList, rectPos, rectSize, leftColor, rightColor);

    // 3. 计算文本位置（左间距 + 垂直居中）
    ImVec2 textPos;
    textPos.x = rectPos.x + 8.0f; // 左侧内边距
    textPos.y = rectPos.y + (rectSize.y - ImGui::GetTextLineHeight()) * 0.5f; // 垂直居中

    // 4. 设置文本颜色（默认与左侧颜色一致）
    if (textColor == 0) {
        textColor = leftColor;
    }

    // 5. 绘制文本
    ImGui::SetCursorScreenPos(textPos);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}