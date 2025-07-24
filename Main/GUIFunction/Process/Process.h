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
	// 移动构造和移动赋值（允许移动）
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

private:
	int PID;
	std::string AuthName;
	std::string exePath;
	bool isAdmin;
	HANDLE handle;
	std::string name;
};