#pragma once
#include "../../KswordTotalHead.h"
#include "process.h"

struct OpenProcessTestItem {
    std::string name;               // 权限名称
    DWORD accessRight;              // 权限值
    std::string result;             // 测试结果
    bool tested;                    // 是否已测试
};


class ProcessOpenTest {
private:
    std::vector<OpenProcessTestItem> testItems;  // 权限测试列表
    DWORD targetPID;                             // 目标进程PID
    bool showTestWindow;                         // 窗口显示标志

    // 执行单个权限测试
    void RunTest(OpenProcessTestItem& item);

    // 错误码转换为可读字符串
    std::string GetErrorString(DWORD errorCode);

public:
    ProcessOpenTest();  // 构造函数，初始化权限列表

    // 显示测试窗口
    void ShowWindow();

    // 设置目标PID
    void SetTargetPID(DWORD pid) { targetPID = pid; }

    // 显示/隐藏窗口
    void ToggleWindow() { showTestWindow = !showTestWindow; }
};


class kProcessDetail : public kProcess {
private:
    std::string processUser;       // 进程所属用户
    std::string processExePath;    // 进程完整路径
    bool isAdmin;                  // 是否为管理员权限
    std::string processName;       // 进程名称
    bool firstShow = true;
	std::string commandLine;           // 命令行参数
public:
    std::string GetCommandLine();
    // 仅支持PID初始化
    explicit kProcessDetail(DWORD pid);

    // 渲染进程信息窗口
    void Render();
	ProcessOpenTest openTest; // 进程权限测试实例

private:
    // 初始化详细进程信息
    void InitDetailInfo();
};

class ProcessDetailManager {
private:
    std::vector<std::unique_ptr<kProcessDetail>> processDetails;
public:
    // 添加进程到管理列表（通过PID）
    void add(DWORD pid);

    // 移除指定PID的进程（返回是否成功移除）
    bool remove(DWORD pid);

    // 渲染所有进程的详细信息窗口
    void renderAll();
};

