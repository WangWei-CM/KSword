#include "../KswordTotalHead.h"
#include "GUIfunction.h"
static int LastConsoleEvent;//��һ������̨�¼�������ȷ�Ͽ���̨����ľ�����ɫ
static int LogConsoleColorAttribute;//͸���ȣ�ÿһ֡-1
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
        // ������
                // ��ȡ��ǰ����
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return;

        // ��ȡ���ڳߴ�
        ImVec2 windowSize = window->Size;
        ImVec2 windowPos = window->Pos;

        // ���þ�����ɫ
        int R = 0, G = 0, B = 0;
        if (LastConsoleEvent == Info) {
            R = static_cast<int>(255 * 0.4f); G = 255; B = static_cast<int>(255 * 0.6f); // ��ɫ
        }
        else if (LastConsoleEvent == Warn) {
            R = 255; G = static_cast<int>(255 * 0.8f); B = 0; // ��ɫ
        }
        else if (LastConsoleEvent == Err) {
            R = 255; G = static_cast<int>(255 * 0.3f); B = static_cast<int>(255 * 0.3f); // ��ɫ
        }

        // ͸���ȴ���
        if (LogConsoleColorAttribute > 0) {
            LogConsoleColorAttribute-=4;
        }

        // ���Ƹ������Σ�ʹ�ô��ڱ�������ϵͳ��
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

        // ��հ�ť
        if (ImGui::Button("Clear")) {
            std::lock_guard<std::mutex> lock(mtx);
            logs.clear();
        }

        // ��־��ʾ��
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar);

        // �ȼ���ɫ����
        const ImVec4 colors[] = {
            ImVec4(0.4f, 1.0f, 0.6f, 1.0f), // ��Ϣ-��
            ImVec4(1.0f, 0.8f, 0.0f, 1.0f), // ����-��
            ImVec4(1.0f, 0.3f, 0.3f, 1.0f)  // ����-��
        };

        // ��Ⱦ��־
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

        // �Զ��������ײ�
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

}
