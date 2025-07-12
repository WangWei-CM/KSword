#pragma once
extern bool isGUI;
#include "KswordTotalHead.h"
#include "TextEditor/TextEditor.h"
namespace ImGuiColors
{
    // 基础颜色
    const ImVec4 Red = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    const ImVec4 Green = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
    const ImVec4 Blue = ImVec4(0.00f, 0.00f, 1.00f, 1.00f);
    const ImVec4 Yellow = ImVec4(1.00f, 1.00f, 0.00f, 1.00f);
    const ImVec4 White = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 Black = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    // 扩展颜色
    const ImVec4 Cyan = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 Magenta = ImVec4(1.00f, 0.00f, 1.00f, 1.00f);
    const ImVec4 Orange = ImVec4(1.00f, 0.65f, 0.00f, 1.00f);
    const ImVec4 Purple = ImVec4(0.50f, 0.00f, 0.50f, 1.00f);
    const ImVec4 Pink = ImVec4(1.00f, 0.75f, 0.80f, 1.00f);
    const ImVec4 Gray = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    const ImVec4 DarkGreen = ImVec4(0.00f, 0.39f, 0.00f, 1.00f);
}
#define KSWORD_BLUE_STYLE_R 61
#define KSWORD_BLUE_STYLE_G 162
#define KSWORD_BLUE_STYLE_B 234


enum LogLevel { Info, Warn, Err };

// 日志条目结构
struct LogEntry {
    LogLevel level;
    std::string message;
    float timestamp;

    LogEntry(LogLevel lvl, const std::string& msg, float ts)
        : level(lvl), message(msg), timestamp(ts) {
    }
};

// 日志管理类
class Logger {
private:
    std::vector<LogEntry> logs;    // 日志存储
    std::mutex mtx;               // 线程安全锁
    bool level_visible[3] = { true, true, true }; // 等级过滤开关

public:
    /**
     * @brief 添加日志条目
     * @param level 日志等级
     * @param fmt 格式化字符串（类似printf）
     * @param ... 可变参数
     */
    void Add(LogLevel level, const char* fmt, ...);

    /**
     * @brief 绘制日志窗口
     */
    void Draw();
};
extern Logger kLog;
extern ImFont* LOGOfont;



struct WorkItemUI {
    int PID;
    std::string Info;
    int OperateNum;
};

struct WorkItem {
    std::string name;        // 工序名称
    float progress;          // 进度 (0.0~1.0)
    std::string currentStep; // 当前步骤
    bool* canceled;//应该指向一个bool，执行线程定期检查这个变量以确定是否被取消。
};

class WorkProgressManager {
public:
    int  AddProcess(WorkItem);
    int  AddProcess(std::string Name, std::string StepName, bool* cancel, float Process=0.0f);
    void SetProcess(int pid, WorkItem);
    void SetProcess(int pid, std::string, float);
    int UI(int pid, WorkItemUI);//等待用户回应
    int UI(int pid, std::string Info, int OperateNum);
    void Render();//渲染
private:
    static std::vector<WorkItem> ProcessList;//每次刷新一帧，检查对应的
    static std::vector<WorkItemUI>ProcessUI;
    static std::vector<int>ShowUI;
    static std::vector<int>UIreturnValue;
    static std::mutex data_mutex;  // 互斥锁
};
extern WorkProgressManager kItem;

extern TextEditor m_editor;

enum KswordStyleIn
{
    Ksword,
    Dark,
    Light,
};
extern int KswordStyle;