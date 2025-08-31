#pragma once
void DrawVerticalHexagon(
	float relativeX,     // 0~1，相对窗口水平位置
	float relativeY,     // 0~1，相对窗口竖直位置
	int radiusPx,        // 到顶点的绝对长度（像素）
	ImU32 color,         // ImGui 颜色（含透明度，如 IM_COL32(...)）
	float lineThickness  // 线条粗细
);
// 绘制带渐变背景的文本框
void DrawGradientBorderTextBox(
	float relX, float relY,      // 相对窗口的位置（0~1）
	float relWidth, float relHeight, // 相对窗口的宽高（0~1）
	ImU32 leftColor,             // 矩形最左侧颜色
	const char* text             // 要绘制的文本
);
void DrawGradientLabel(
	float relX, float relY,       // 相对窗口的位置（0~1）
	float relWidth, float relHeight, // 相对窗口的宽高（0~1）
	ImU32 leftColor, ImU32 rightColor, // 渐变左右颜色
	const char* text,             // 显示的文本
	ImU32 textColor               // 文本颜色（默认与左侧颜色一致）
);