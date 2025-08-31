#include "../KswordTotalHead.h"
#include "ConsolePainter.h"
extern bool EasyMode;

// minValue 就是你要的结果
void RenderEasyModeFrame() {
	ImU32 Color = IM_COL32(
		(uint8_t)(StyleColor.x * 255.0f),  // 红色分量转换
		(uint8_t)(StyleColor.y * 255.0f),  // 绿色分量转换
		(uint8_t)(StyleColor.z * 255.0f),  // 蓝色分量转换
		255);
	ImGui::Begin("Easy Mode", nullptr);
	float height = ImGui::GetWindowSize().y;
	float width = ImGui::GetWindowSize().x;
	float Size = max(width * 9.0f / 16.0f, height);
	if(ImGui::Button("Switch to Normal Mode"))
		EasyMode = false;
    // 颜色与基础定义
    ImU32 titleLeft = IM_COL32(50, 100, 150, 255);
    ImU32 titleRight = IM_COL32(50, 100, 150, 0);
    ImU32 contentLeft = IM_COL32(30, 80, 130, 255);
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    ImVec2 winSize = ImGui::GetWindowSize();
    float centerWidth = winSize.x * 0.3f; // 中间区域宽度为窗口30%

    // ====================== 1. 操作系统模块 ======================
    DrawGradientLabel(
        0.25f, 0.05f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("操作系统"), textColor
    );
    DrawGradientBorderTextBox(
        0.25f, 0.1f, 0.25f, 0.1f,
        contentLeft,
        C("Windows10 1909专业工作站 12415.4153")
    );

    // ====================== 2. 硬件模块 ======================
    DrawGradientLabel(
        0.6f, 0.05f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("硬件"), textColor
    );
    DrawGradientBorderTextBox(
        0.6f, 0.1f, 0.25f, 0.1f,
        contentLeft,
        C("CPU: Intel(R) Xeon E5 2682 v4\n内存: 32GB\n显卡: GT 750")
    );

    // ====================== 3. 网络模块（拆分文本框） ======================
    DrawGradientLabel(
        0.7f, 0.3f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("网络"), textColor
    );
    DrawGradientBorderTextBox(0.7f, 0.35f, 0.2f, 0.06f, contentLeft, C("上传流量: 120kb/s"));
    DrawGradientBorderTextBox(0.7f, 0.42f, 0.2f, 0.06f, contentLeft, C("下载流量: 114kb/s"));
    DrawGradientBorderTextBox(0.7f, 0.49f, 0.2f, 0.06f, contentLeft, C("TCP连接数: 114"));

    // ====================== 4. 内存模块（拆分文本框） ======================
    DrawGradientLabel(
        0.6f, 0.7f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("内存"), textColor
    );
    DrawGradientBorderTextBox(0.6f, 0.75f, 0.25f, 0.07f, contentLeft, C("总内存: 32GB+0GB"));
    DrawGradientBorderTextBox(0.6f, 0.83f, 0.25f, 0.07f, contentLeft, C("已使用: 7GB"));

    // ====================== 5. 用户模块（拆分文本框） ======================
    DrawGradientLabel(
        0.25f, 0.7f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("用户"), textColor
    );
    DrawGradientBorderTextBox(0.25f, 0.75f, 0.25f, 0.07f, contentLeft, C("Administrator 已连接 本地登录"));
    DrawGradientBorderTextBox(0.25f, 0.83f, 0.25f, 0.07f, contentLeft, C("User 未连接 远程登陆"));

    // ====================== 6. 进程模块（拆分文本框） ======================
    DrawGradientLabel(
        0.05f, 0.3f, 0.2f, 0.05f,
        titleLeft, titleRight,
        C("进程"), textColor
    );
    DrawGradientBorderTextBox(0.05f, 0.35f, 0.2f, 0.07f, contentLeft, C("总进程: 250"));
    DrawGradientBorderTextBox(0.05f, 0.43f, 0.2f, 0.07f, contentLeft, C("句柄数: 114514"));
    DrawGradientBorderTextBox(0.05f, 0.51f, 0.2f, 0.07f, contentLeft, C("线程数: 1919"));

    // ====================== 中间区域与六边形轮廓 ======================
    float centerX = winSize.x * 0.5f;
    float centerY = winSize.y * 0.5f;
    // 中间文本
    float hexRadius = 250; // 大幅增加半径，确保六边形足够大
    ImU32 hexColor = IM_COL32(30, 80, 130, 255); // 统一颜色（可根据需要微调透明度
    ImGui::SetCursorPos(ImVec2(centerX - 40, centerY - 20));
    ImGui::TextColored(ImVec4(0.2f, 0.4f, 0.6f, 1.0f), C("RAM: 23%%"));
    ImGui::SetCursorPos(ImVec2(centerX + 20, centerY - 20));
    ImGui::TextColored(ImVec4(0.2f, 0.4f, 0.6f, 1.0f), C("CPU: 9.2%%"));
    ImGui::SetCursorPos(ImVec2(centerX - 30, centerY + 10));
    ImGui::TextColored(ImVec4(0.2f, 0.4f, 0.6f, 1.0f), C("Ksword5.0 开发者模式"));
    DrawVerticalHexagon(0.3f, 0.0f, hexRadius, hexColor, 2.0f);  // 左上区域
    DrawVerticalHexagon(0.7f, 0.0f, hexRadius, hexColor, 2.0f);  // 右上区域
    DrawVerticalHexagon(0.3f, 1.0f, hexRadius, hexColor, 2.0f);  // 左下区域
    DrawVerticalHexagon(0.7f, 1.0f, hexRadius, hexColor, 2.0f);  // 右下区域
    DrawVerticalHexagon(0.12f, 0.5f, hexRadius, hexColor, 2.0f);  // 左侧中间区域
    DrawVerticalHexagon(0.88f, 0.5f, hexRadius, hexColor, 2.0f);  // 右侧中间区域
    DrawVerticalHexagon(0.5f, 0.5f, 25+hexRadius, hexColor, 2.0f);  // 正中间区域
	ImGui::End();
}