#pragma once
extern bool isGUI;
#include "KswordTotalHead.h"
#include "TextEditor/TextEditor.h"
namespace ImGuiColors
{
    // ������ɫ
    const ImVec4 Red = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    const ImVec4 Green = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
    const ImVec4 Blue = ImVec4(0.00f, 0.00f, 1.00f, 1.00f);
    const ImVec4 Yellow = ImVec4(1.00f, 1.00f, 0.00f, 1.00f);
    const ImVec4 White = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 Black = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    // ��չ��ɫ
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


enum LogLevel { Debug,Info, Warn, Err,Fatal };

// ��־��Ŀ�ṹ
struct LogEntry {
    LogLevel level;
    std::string message;
	std::string module;
    float timestamp;

    LogEntry(LogLevel lvl, const std::string& msg, const std::string& Module, float ts)
        : level(lvl), message(msg), module(Module), timestamp(ts) {
    }
};

// ��־������
class Logger : public ImGuiDependentModule {
private:
    std::vector<LogEntry> logs;    // ��־�洢
    std::mutex mtx;               // �̰߳�ȫ��
    bool level_visible[5] = { true,true ,true, true, true }; // �ȼ����˿���
protected:
    void onInitialize() override {
        // ��ʼ����־ϵͳ
    }

    void onShutdown() override {
        // ������־��Դ
        std::lock_guard<std::mutex> lock(mtx);
        logs.clear();
    }
public:
    Logger() : ImGuiDependentModule("Logger") {}
    /**
     * @brief �����־��Ŀ
     * @param level ��־�ȼ�
     * @param fmt ��ʽ���ַ���������printf��
     * @param ... �ɱ����
     */
    void Add(LogLevel level, const char* fmt, const char* Module = "Unknown");

    /**
     * @brief ������־����
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
    std::string name;        // ��������
    float progress;          // ���� (0.0~1.0)
    std::string currentStep; // ��ǰ����
    bool* canceled;//Ӧ��ָ��һ��bool��ִ���̶߳��ڼ�����������ȷ���Ƿ�ȡ����
};

class WorkProgressManager {
public:
    int  AddProcess(WorkItem);
    int  AddProcess(std::string Name, std::string StepName, bool* cancel, float Process=0.0f);
    void SetProcess(int pid, WorkItem);
    void SetProcess(int pid, std::string, float);
    int UI(int pid, WorkItemUI);//�ȴ��û���Ӧ
    int UI(int pid, std::string Info, int OperateNum);
    void Render();//��Ⱦ
private:
    static std::vector<WorkItem> ProcessList;//ÿ��ˢ��һ֡������Ӧ��
    static std::vector<WorkItemUI>ProcessUI;
    static std::vector<int>ShowUI;
    static std::vector<int>UIreturnValue;
    static std::mutex data_mutex;  // ������
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