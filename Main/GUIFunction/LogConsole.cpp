#include "../KswordTotalHead.h"
#include "GUIfunction.h"
static int LastConsoleEvent;//上一个控制台事件，用于确认控制台标题的矩形颜色
static int LogConsoleColorAttribute;//透明度，每一帧-1
void Logger::Add(LogLevel level, const char* fmt, ...) {
    LastConsoleEvent = level;
    LogConsoleColorAttribute = 256;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(mtx);
    logs.emplace_back(level, std::string(buf), static_cast<float>(ImGui::GetTime()));
}

void Logger::Draw() {
    if (ImGui::Begin("Log Console")) {
        // 控制栏
                // 获取当前窗口
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return;

        // 获取窗口尺寸
        ImVec2 windowSize = window->Size;
        ImVec2 windowPos = window->Pos;

        // 设置矩形颜色
        int R = 0, G = 0, B = 0;
        if (LastConsoleEvent == Info) {
            R = static_cast<int>(255 * 0.4f); G = 255; B = static_cast<int>(255 * 0.6f); // 绿色
        }
        else if (LastConsoleEvent == Warn) {
            R = 255; G = static_cast<int>(255 * 0.8f); B = 0; // 黄色
        }
        else if (LastConsoleEvent == Err) {
            R = 255; G = static_cast<int>(255 * 0.3f); B = static_cast<int>(255 * 0.3f); // 红色
        }

        // 透明度处理
        if (LogConsoleColorAttribute > 0) {
            LogConsoleColorAttribute-=4;
        }

        // 绘制高亮矩形（使用窗口本地坐标系统）
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 rectMin = ImVec2(windowPos.x, windowPos.y);
        ImVec2 rectMax = ImVec2(windowPos.x + windowSize.x, windowPos.y + 65);

        drawList->AddRectFilled(
            rectMin,
            rectMax,
            IM_COL32(R, G, B, LogConsoleColorAttribute)
        );
        ImGui::Checkbox("Info", &level_visible[Info]);
        ImGui::SameLine();
        ImGui::Checkbox("Warn", &level_visible[Warn]);
        ImGui::SameLine();
        ImGui::Checkbox("Error", &level_visible[Err]);
        ImGui::SameLine();

        // 清空按钮
        if (ImGui::Button("Clear")) {
            std::lock_guard<std::mutex> lock(mtx);
            logs.clear();
        }

        // 日志显示区
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar);

        // 等级颜色定义
        const ImVec4 colors[] = {
            ImVec4(0.4f, 1.0f, 0.6f, 1.0f), // 信息-绿
            ImVec4(1.0f, 0.8f, 0.0f, 1.0f), // 警告-黄
            ImVec4(1.0f, 0.3f, 0.3f, 1.0f)  // 错误-红
        };

        // 渲染日志
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& log : logs) {
                if (!level_visible[log.level]) continue;

                ImGui::PushStyleColor(ImGuiCol_Text, colors[log.level]);
                ImGui::Text("[%.1fs] %s",
                    log.timestamp,
                    log.message.c_str());
                ImGui::PopStyleColor();
            }
        }

        // 自动滚动到底部
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

}
