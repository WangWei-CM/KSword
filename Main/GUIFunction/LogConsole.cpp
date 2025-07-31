#include "../KswordTotalHead.h"
#include "GUIfunction.h"
static int LastConsoleEvent;//上一个控制台事件，用于确认控制台标题的矩形颜色
static int LogConsoleColorAttribute;//透明度，每一帧-1
void Logger::Add(LogLevel level, const char* fmt, ...) {

    // 这里应该做一下防御性编程，当部分对象持有 ImGui UI 对象的指针引用时候，
    // 如果不正确处理 ImGui 的生命周期，上下文在其他功能模块析构前就已经被释放，
    // 析构函数中调用日志记录等操作会导致访问空指针，导致空指针异常
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
        ImGui::SameLine();
        if (ImGui::Button(C("复制全部内容"))) {
            std::string all_content;
            {
                std::lock_guard<std::mutex> lock(mtx); // 仅在访问logs时加锁
                int index = 0;
                for (const auto& log : logs) {
                    if (!level_visible[log.level]) continue;

                    const char* level_str = (log.level == Info) ? "Info" :
                        (log.level == Warn) ? "Warn" : "Error";

                    // 用\t分割列，每行末尾添加换行
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
            kLog.Add(Info, C("已复制全部内容到剪贴板"));
        }


        // 日志显示区
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar);
        // 表格设置：4列，前三列窄，第四列自适应
        if (ImGui::BeginTable("LogTable", 4,
            ImGuiTableFlags_Borders |          // 边框
            ImGuiTableFlags_RowBg |           // 行背景
            ImGuiTableFlags_SizingFixedFit |  // 固定列宽模式
            ImGuiTableFlags_NoSavedSettings)) // 不保存设置
        {
            // 配置列宽：前三列窄，第四列占剩余空间
            ImGui::TableSetupColumn(C("序号"), ImGuiTableColumnFlags_WidthFixed, 50);    // 序号列（极窄）
            ImGui::TableSetupColumn(C("等级"), ImGuiTableColumnFlags_WidthFixed, 80);    // 严重程度列（窄）
            ImGui::TableSetupColumn(C("时间"), ImGuiTableColumnFlags_WidthFixed, 80);    // 时间列（窄）
            ImGui::TableSetupColumn(C("内容"), ImGuiTableColumnFlags_WidthStretch);      // 内容列（自适应拉伸）
            ImGui::TableHeadersRow();

            // 等级颜色定义（作为行背景色）
            const ImVec4 bg_colors[] = {
                ImVec4(0.4f, 1.0f, 0.6f, 0.9f),  // 信息-浅绿色背景
                ImVec4(1.0f, 0.8f, 0.0f, 0.9f),  // 警告-浅黄色背景
                ImVec4(1.0f, 0.3f, 0.3f, 0.9f)   // 错误-浅红色背景
            };
            // 文字统一用黑色
            const ImVec4 text_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);  // 黑色文字

            // 渲染日志行
            
            int log_index = 0;  // 序号计数器
            std::vector<LogEntry> local_logs; // 假设日志条目类型为LogEntry
            {
                std::lock_guard<std::mutex> lock(mtx); // 仅在此作用域加锁
                local_logs = logs; // 复制到局部容器
            }
            for (const auto& log : local_logs) {
                if (!level_visible[log.level]) continue;

                // 开始新行
                ImGui::TableNextRow();
                // 为当前行创建唯一ID（用于右键菜单）
                ImGui::PushID(log_index);

                // 设置行背景色
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(bg_colors[log.level]));

                // 1. 序号列
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                ImGui::Text("%d", log_index++);
                ImGui::PopStyleColor();

                // 2. 严重程度列
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                switch (log.level) {
                case Info:  ImGui::Text("Info"); break;
                case Warn:  ImGui::Text("Warn"); break;
                case Err:   ImGui::Text("Error"); break;
                }
                ImGui::PopStyleColor();

                // 3. 时间列
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                ImGui::Text("%.1fs", log.timestamp);
                ImGui::PopStyleColor();

                // 4. 内容列
                ImGui::TableSetColumnIndex(3);
                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                ImGui::TextWrapped("%s", log.message.c_str());  // 自动换行
                ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem("LogMenuRightClick")) {  // 使用当前ID的上下文菜单
                    if (ImGui::MenuItem(C("复制内容"))) {
                        // 将日志内容复制到剪贴板
                        ImGui::SetClipboardText(log.message.c_str());
                        kLog.Add(Info, C("已复制到剪贴板"));
                    }

                    ImGui::EndPopup();
                }ImGui::PopID();
            }

            ImGui::EndTable();
        }


        // 自动滚动到底部
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

}
