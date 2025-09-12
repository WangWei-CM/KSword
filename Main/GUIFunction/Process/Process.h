#pragma once
#include "../../KswordTotalHead.h"
enum kProcKillMethod{
	kTaskkill,
	kTaskkillF,
	kTerminate,
	kTerminateThread,
	kNTTerminate,
	kJobTer
};
enum kEnumProcMethod {
	kEnumProcByCreateSnapTool
};

class kThread {
private:
	HANDLE hThread;         // 线程句柄
	DWORD TID;              // 线程ID
	int priority;           // 线程优先级（-2到2对应Windows的THREAD_PRIORITY_*）
	int statusCode;         // 线程操作状态码（0表示成功，其他为错误码）
	std::string modulePath; // 线程所属模块路径




public:
	// 构造函数：通过线程ID初始化
	kThread(DWORD tid);
	// 构造函数：通过进程句柄初始化（获取进程的主线程）
	kThread(HANDLE hProcess);
	// 析构函数：释放资源
	~kThread();

	// 终止线程
	int Terminate(UINT exitCode = 0);
	// 挂起线程
	int Suspend();
	// 恢复线程
	int UnSuspend();

	// 获取私有成员的函数
	HANDLE GetHandle() const;
	DWORD GetTID() const;
	int GetPriority() const;
	int GetStatusCode() const;
	std::string GetModulePath() const;

private:
	// 初始化线程信息（内部使用）
	void InitThreadInfo();
};

class kProcess {
public:

	kProcess(DWORD pid);

	kProcess(HANDLE hProc);

	bool Terminate(kProcKillMethod method = kTerminateThread, UINT uExitCode = 0);
	
	bool Suspend();
	bool Resume();
	
	bool SetKeyProc();
	bool CancelKeyProc();

	std::string GetProcessName();
	kProcess(kProcess&& other) noexcept;
	kProcess& operator=(kProcess&& other) noexcept;

	// 禁止拷贝（句柄不能被拷贝）
	kProcess(const kProcess&) = delete;
	kProcess& operator=(const kProcess&) = delete;
	//辅助函数
	static HANDLE GetProcessHandleByPID(DWORD pid);
	static DWORD GetProcessIdByName(const std::wstring& procName);

	~kProcess();

	DWORD pid() const;
	std::string Name() const;
	std::string User() const;
	std::string ExePath() const;
	HANDLE Handle() const;
	bool IsAdmin() const;

	double GetCPUUsage() const { return cpuUsage; }

	void UpdateMemoryInfo();
	double UpdateCPUUsage();
	void UpdateStats();
protected:
	int PID;
	std::string AuthName;
	std::string exePath;
	bool isAdmin;
	HANDLE handle;
	std::string name;

	//性能计数器
	double cpuUsage;

};
struct ServiceInfo {
	std::wstring serviceName;       // 服务名称（短名）
	std::wstring userName;          // 服务运行账户名
	std::wstring displayName;       // 服务显示名称
	std::wstring description;       // 服务描述
	DWORD pid;                      // 服务对应的进程PID
};