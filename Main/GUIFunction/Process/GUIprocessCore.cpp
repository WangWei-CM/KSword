#include "../../KswordTotalHead.h"
#include "Process.h"

extern BOOL TerminateAllThreads(HANDLE hProcess);
extern BOOL NtTerminate(HANDLE hProcess);
extern BOOL TerminateProcessViaJob(HANDLE hProcess);

std::string WideCharToMultiByte(const std::wstring& wstr) {
    int bufferSize = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(bufferSize, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], bufferSize, NULL, NULL);
    return str;
}

// 辅助函数：检查进程是否是否具有管理员权限
bool CheckProcessAdminRights(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE) return false;

    HANDLE tokenHandle = NULL;
    if (!OpenProcessToken(handle, TOKEN_QUERY, &tokenHandle)) {
        return false;
    }

    TOKEN_ELEVATION elevation;
    DWORD tokenSize = sizeof(TOKEN_ELEVATION);
    bool isElevated = false;

    if (GetTokenInformation(tokenHandle, TokenElevation, &elevation, tokenSize, &tokenSize)) {
        isElevated = (elevation.TokenIsElevated != 0);
    }

    CloseHandle(tokenHandle);
    return isElevated;
}

// 辅助函数：获取进程所属用户名称
std::string GetProcessUserName(HANDLE handle ) {
    if (handle == INVALID_HANDLE_VALUE) return "";

    HANDLE tokenHandle = NULL;
    if (!OpenProcessToken(handle, TOKEN_QUERY, &tokenHandle)) {
        return "";
    }

    // 获取令牌用户信息
    DWORD tokenInfoSize = 0;
    GetTokenInformation(tokenHandle, TokenUser, NULL, 0, &tokenInfoSize);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(tokenHandle);
        return "";
    }

    std::vector<BYTE> tokenInfoBuffer(tokenInfoSize);
    PTOKEN_USER tokenUser = reinterpret_cast<PTOKEN_USER>(tokenInfoBuffer.data());

    if (!GetTokenInformation(tokenHandle, TokenUser, tokenUser, tokenInfoSize, &tokenInfoSize)) {
        CloseHandle(tokenHandle);
        return "";
    }

    // 转换SID到账户名
    WCHAR userName[256] = { 0 };
    WCHAR domainName[256] = { 0 };
    DWORD userNameSize = 256;
    DWORD domainNameSize = 256;
    SID_NAME_USE sidType;

    if (LookupAccountSidW(NULL, tokenUser->User.Sid, userName, &userNameSize,
        domainName, &domainNameSize, &sidType)) {
        CloseHandle(tokenHandle);
        return WideCharToMultiByte(std::wstring(userName));
    }

    CloseHandle(tokenHandle);
    return "";
}


void KillProcessByTaskkill(int pid) {
	int Tmppid;
	Tmppid=kItem.AddProcess(
		/*C*/(("使用task kill终止pid为" + std::to_string(pid) + "的进程").c_str()),
		/*C*/("执行cmd命令"),
		NULL
	);
	std::string tmp = "taskkill /pid " + std::to_string(pid);
	std::string tmp1 = GetCmdResultWithUTF8(tmp);
	kLog.Add(Info, C(tmp1.c_str()));
	return;
}

kProcess::kProcess(DWORD pid) : PID(pid), handle(INVALID_HANDLE_VALUE), isAdmin(false) {
    handle = GetProcessHandleByPID(pid);
    if (handle != INVALID_HANDLE_VALUE) {
        // 获取进程名称并赋值给name
        name = GetProcessName();

        // 获取进程可执行文件路径
        WCHAR pathBuf[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(handle, NULL, pathBuf, MAX_PATH)) {
            exePath = std::string(WideCharToMultiByte(pathBuf));
        }

        // 检查进程是否拥有管理员权限
        isAdmin = CheckProcessAdminRights(handle);

        // 获取进程所属用户名称
        AuthName = GetProcessUserName(handle);
    }
}

// 构造函数：通过进程句柄初始化
kProcess::kProcess(HANDLE hProc) : handle(hProc), isAdmin(false), PID(0) {
    if (handle != INVALID_HANDLE_VALUE && handle != NULL) {
        PID = GetProcessId(hProc);
        name = GetProcessName();

        // 获取进程可执行文件路径
        WCHAR pathBuf[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(handle, NULL, pathBuf, MAX_PATH)) {
            exePath = std::string(WideCharToMultiByte(pathBuf));
        }

        // 检查进程是否拥有管理员权限
        isAdmin = CheckProcessAdminRights(handle);

        // 获取进程所属用户名称
        AuthName = GetProcessUserName(handle);
    }
}
bool kProcess::Terminate(kProcKillMethod method, UINT uExitCode)
{
	if (method == kTaskkill) {
		// 使用taskkill命令结束进程
		std::string cmd = "taskkill /pid " + std::to_string(PID);
		RunCmdNow(cmd);
		return true;
	}
    else if (method == kTaskkillF) {
        // 使用taskkill命令结束进程
        std::string cmd = "taskkill /pid " + std::to_string(PID)+"/f";
        RunCmdNow(cmd);
        return true;
    }
	else if (method == kTerminate) {
		return TerminateProcess(handle, uExitCode);
	}
	else if (method == kTerminateThread) {
		// 调用TerminateThread函数结束所有线程
        return TerminateAllThreads(this->handle);
	}
	else if (method == kNTTerminate) {
		// 调用NT API结束进程
        return NtTerminate(handle);
	}
	else if (method == kJobTer) {
		// 使用作业对象结束进程
        return TerminateProcessViaJob(handle);
    }
	else {
		
		return false;
	}
}

// 挂起进程
bool kProcess::Suspend() {
    return SuspendProcess(this->PID);
}

// 恢复进程
bool kProcess::Resume() {
    return UnSuspendProcess(this->PID);
}

bool kProcess::SetKeyProc() {
    return SetKeyProcess(handle, true);
}
bool kProcess::CancelKeyProc() {
    return SetKeyProcess(handle, false);
}

std::string kProcess::GetProcessName() {
    return GetProcessNameByPID(PID); 
}

HANDLE kProcess::GetProcessHandleByPID(DWORD pid) {
    if (pid == 0) {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    return hProc;
}

// 静态方法：通过进程名获取PID
DWORD kProcess::GetProcessIdByName(const std::wstring& procName) {
    HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hProcessSnap, &pe32)) {
        do {
            if (procName == pe32.szExeFile) {
                CloseHandle(hProcessSnap);
                return pe32.th32ProcessID;
            }
        } while (Process32NextW(hProcessSnap, &pe32));
    }

    CloseHandle(hProcessSnap);
    return 0;
}

// 移动构造函数：转移句柄所有权
kProcess::kProcess(kProcess&& other) noexcept
    : PID(other.PID),
    AuthName(std::move(other.AuthName)),
    exePath(std::move(other.exePath)),
    isAdmin(other.isAdmin),
    handle(other.handle),  // 转移句柄
    name(std::move(other.name)) {
    other.handle = INVALID_HANDLE_VALUE;  // 原对象句柄置为无效，避免析构时释放
    other.PID = 0;
}


// 移动赋值运算符：转移句柄所有权
kProcess& kProcess::operator=(kProcess&& other) noexcept {
    if (this != &other) {
        // 释放当前对象的句柄
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }

        // 转移资源
        PID = other.PID;
        AuthName = std::move(other.AuthName);
        exePath = std::move(other.exePath);
        isAdmin = other.isAdmin;
        handle = other.handle;  // 接管句柄
        name = std::move(other.name);

        // 原对象句柄置为无效
        other.handle = INVALID_HANDLE_VALUE;
        other.PID = 0;
    }
    return *this;
}

kProcess::~kProcess() {
    if (handle != INVALID_HANDLE_VALUE && handle != NULL) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

DWORD kProcess::pid() const
{
    return PID;
}

std::string kProcess::Name() const
{
    return name;
}

std::string kProcess::User() const
{
    return AuthName;
}

std::string kProcess::ExePath() const
{
    return exePath;
}

HANDLE kProcess::Handle() const 
{
    return handle;
}

bool kProcess::IsAdmin() const
{
    return isAdmin;
}
