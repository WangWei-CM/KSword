#include "../KswordTotalHead.h"
#include "GUIfunction.h"
static int LastConsoleEvent;//��һ������̨�¼�������ȷ�Ͽ���̨����ľ�����ɫ
static int LogConsoleColorAttribute;//͸���ȣ�ÿһ֡-1
void Logger::Add(LogLevel level, const char* fmt, ...) {

    // ����Ӧ����һ�·����Ա�̣������ֶ������ ImGui UI �����ָ������ʱ��
    // �������ȷ���� ImGui ���������ڣ�����������������ģ������ǰ���Ѿ����ͷţ�
    // ���������е�����־��¼�Ȳ����ᵼ�·��ʿ�ָ�룬���¿�ָ���쳣
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

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
        ImGui::SameLine();
        if (ImGui::Button(C("����ȫ������"))) {
            std::string all_content;
            {
                std::lock_guard<std::mutex> lock(mtx); // ���ڷ���logsʱ����
                int index = 0;
                for (const auto& log : logs) {
                    if (!level_visible[log.level]) continue;

                    const char* level_str = (log.level == Info) ? "Info" :
                        (log.level == Warn) ? "Warn" : "Error";

                    // ��\t�ָ��У�ÿ��ĩβ��ӻ���
                    char line[512];
                    sprintf(line, "%d\t%s\t%.1fs\t%s\n",
                        index++,
                        level_str,
                        log.timestamp,
                        log.message.c_str());
                    all_content += line;
                }
            }
            ImGui::SetClipboardText(all_content.c_str());
            kLog.Add(Info, C("�Ѹ���ȫ�����ݵ�������"));
        }


        // ��־��ʾ��
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar);
        // ������ã�4�У�ǰ����խ������������Ӧ
        if (ImGui::BeginTable("LogTable", 4,
            ImGuiTableFlags_Borders |          // �߿�
            ImGuiTableFlags_RowBg |           // �б���
            ImGuiTableFlags_SizingFixedFit |  // �̶��п�ģʽ
            ImGuiTableFlags_NoSavedSettings)) // ����������
        {
            // �����п�ǰ����խ��������ռʣ��ռ�
            ImGui::TableSetupColumn(C("���"), ImGuiTableColumnFlags_WidthFixed, 50);    // ����У���խ��
            ImGui::TableSetupColumn(C("�ȼ�"), ImGuiTableColumnFlags_WidthFixed, 80);    // ���س̶��У�խ��
            ImGui::TableSetupColumn(C("ʱ��"), ImGuiTableColumnFlags_WidthFixed, 80);    // ʱ���У�խ��
            ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch);      // �����У�����Ӧ���죩
            ImGui::TableHeadersRow();

            // �ȼ���ɫ���壨��Ϊ�б���ɫ��
            const ImVec4 bg_colors[] = {
                ImVec4(0.4f, 1.0f, 0.6f, 0.9f),  // ��Ϣ-ǳ��ɫ����
                ImVec4(1.0f, 0.8f, 0.0f, 0.9f),  // ����-ǳ��ɫ����
                ImVec4(1.0f, 0.3f, 0.3f, 0.9f)   // ����-ǳ��ɫ����
            };
            // ����ͳһ�ú�ɫ
            const ImVec4 text_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);  // ��ɫ����

            // ��Ⱦ��־��
            
            int log_index = 0;  // ��ż�����
            std::vector<LogEntry> local_logs; // ������־��Ŀ����ΪLogEntry
            {
                std::lock_guard<std::mutex> lock(mtx); // ���ڴ����������
                local_logs = logs; // ���Ƶ��ֲ�����
            }
            for (const auto& log : local_logs) {
                if (!level_visible[log.level]) continue;

                // ��ʼ����
                ImGui::TableNextRow();
                // Ϊ��ǰ�д���ΨһID�������Ҽ��˵���
                ImGui::PushID(log_index);

                // �����б���ɫ
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(bg_colors[log.level]));

                // 1. �����
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                ImGui::Text("%d", log_index++);
                ImGui::PopStyleColor();

                // 2. ���س̶���
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                switch (log.level) {
                case Info:  ImGui::Text("Info"); break;
                case Warn:  ImGui::Text("Warn"); break;
                case Err:   ImGui::Text("Error"); break;
                }
                ImGui::PopStyleColor();

                // 3. ʱ����
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                ImGui::Text("%.1fs", log.timestamp);
                ImGui::PopStyleColor();

                // 4. ������
                ImGui::TableSetColumnIndex(3);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                ImGui::TextWrapped("%s", log.message.c_str());  // �Զ�����
                ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem("LogMenuRightClick")) {  // ʹ�õ�ǰID�������Ĳ˵�
                    if (ImGui::MenuItem(C("��������"))) {
                        // ����־���ݸ��Ƶ�������
                        ImGui::SetClipboardText(log.message.c_str());
                        kLog.Add(Info, C("�Ѹ��Ƶ�������"));
                    }

                    ImGui::EndPopup();
                }ImGui::PopID();
            }

            ImGui::EndTable();
        }


        // �Զ��������ײ�
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

}
