#include "../../KswordTotalHead.h"
#include "Process.h"
#define CURRENT_MODULE "进程核心管理"
extern LPDIRECT3DTEXTURE9 GetCachedProcessIcon(DWORD pid);


extern BOOL TerminateAllThreads(HANDLE hProcess);
extern BOOL NtTerminate(HANDLE hProcess);
extern BOOL TerminateProcessViaJob(HANDLE hProcess);

bool GetServiceInfoByPID(DWORD pid, ServiceInfo& info) {
    // 打开服务控制管理器
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hSCM) return false;

    DWORD bytesNeeded = 0;
    DWORD servicesReturned = 0;
    // 第一次调用获取缓冲区大小
    EnumServicesStatusExW(
        hSCM,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32,
        SERVICE_STATE_ALL,
        nullptr,
        0,
        &bytesNeeded,
        &servicesReturned,
        nullptr,
        nullptr
    );
    if (GetLastError() != ERROR_MORE_DATA) {
        CloseServiceHandle(hSCM);
        return false;
    }

    // 分配缓冲区并枚举服务
    std::vector<BYTE> buffer(bytesNeeded);
    auto pServices = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buffer.data());
    if (!EnumServicesStatusExW(
        hSCM,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32,
        SERVICE_STATE_ALL,
        buffer.data(),
        bytesNeeded,
        &bytesNeeded,
        &servicesReturned,
        nullptr,
        nullptr
    )) {
        CloseServiceHandle(hSCM);
        return false;
    }

    // 匹配PID对应的服务
    for (DWORD i = 0; i < servicesReturned; ++i) {
        if (pServices[i].ServiceStatusProcess.dwProcessId == pid) {
            info.serviceName = pServices[i].lpServiceName;
            info.displayName = pServices[i].lpDisplayName;

            // 服务默认运行账户（可根据实际场景扩展）
            info.userName = L"NT AUTHORITY\\SYSTEM";
            CloseServiceHandle(hSCM);
            return true;
        }
    }

    CloseServiceHandle(hSCM);
    return false;
}

std::string WideCharToMultiByte(const std::wstring& wstr) {
    int bufferSize = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(bufferSize, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], bufferSize, NULL, NULL);
    return str;
}

// GUIprocessCore.cpp 新增函数
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

void kProcess::UpdateMemoryInfo() {

}
// GUIprocessCore.cpp 新增函数
// 文件顶部添加常量定义（或在类声明中添加）
const double FILETIME_SECOND_FACTOR = 10000000.0;  // 100ns = 10^-7秒的转换因子


    static uint64_t FileTimeToUInt64(const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }
// 在构造函数中初始化核心数

    static uint64_t convert_time_format(const FILETIME* ftime)
    {
        LARGE_INTEGER li;

        li.LowPart = ftime->dwLowDateTime;
        li.HighPart = ftime->dwHighDateTime;
        return li.QuadPart;
    }

double kProcess::UpdateCPUUsage() {
    ULONGLONG lastKernel_ = 0, lastUser_ = 0;
    ULONGLONG lastTick_ = 0;
    FILETIME createTime, exitTime, kernelTime, userTime;
    if (!GetProcessTimes(handle, &createTime, &exitTime, &kernelTime, &userTime))
        return -1;

    ULONGLONG currentKernel = *(ULONGLONG*)&kernelTime;
    ULONGLONG currentUser = *(ULONGLONG*)&userTime;
    ULONGLONG delta = (currentKernel + currentUser) - (lastKernel_ + lastUser_);

    // 计算时间差（单位：100纳秒）
    ULONGLONG timePassed = GetTickCount64() - lastTick_;
    lastTick_ = GetTickCount64();
    lastKernel_ = currentKernel;
    lastUser_ = currentUser;

    if (timePassed == 0) return 0.0f;
    return (delta / 10000.0f) / timePassed * 100.0f; // 转换为百分比
}
kThread::kThread(DWORD tid) : hThread(NULL), TID(tid), priority(0), statusCode(0) {
    // 通过线程ID打开线程句柄（需要THREAD_QUERY_INFORMATION权限）
    hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    if (hThread == NULL) {
        statusCode = GetLastError();
        return;
    }
    InitThreadInfo();
}

// 通过进程句柄初始化（获取主线程）
kThread::kThread(HANDLE hProcess) : hThread(NULL), TID(0), priority(0), statusCode(0) {
    if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE) {
        statusCode = ERROR_INVALID_HANDLE;
        return;
    }

    // 创建线程快照查找主线程
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        statusCode = GetLastError();
        return;
    }

    THREADENTRY32 te32;
    te32.dwSize = sizeof(THREADENTRY32);
    if (Thread32First(hSnapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID == GetProcessId(hProcess)) {
                // 找到进程的第一个线程作为主线程
                TID = te32.th32ThreadID;
                hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, TID);
                if (hThread != NULL) {
                    break;
                }
            }
        } while (Thread32Next(hSnapshot, &te32));
    }

    CloseHandle(hSnapshot);

    if (hThread == NULL) {
        statusCode = GetLastError();
        return;
    }

    InitThreadInfo();
}

// 析构函数：释放线程句柄
kThread::~kThread() {
    if (hThread != NULL && hThread != INVALID_HANDLE_VALUE) {
        CloseHandle(hThread);
        hThread = NULL;
    }
}

// 初始化线程信息（优先级、模块路径等）
void kThread::InitThreadInfo() {
    // 获取线程优先级
    int prio = GetThreadPriority(hThread);
    if (prio != THREAD_PRIORITY_ERROR_RETURN) {
        priority = prio;
    }
    else {
        statusCode = GetLastError();
    }

    // 获取线程所属模块路径
    HMODULE hModule;
    DWORD bytesNeeded;
    if (EnumProcessModulesEx(GetCurrentProcess(), &hModule, sizeof(hModule), &bytesNeeded, LIST_MODULES_ALL)) {
        char path[MAX_PATH];
        if (GetModuleFileNameExA(GetCurrentProcess(), hModule, path, MAX_PATH)) {
            modulePath = path;
        }
    }
}

// 终止线程
int kThread::Terminate(UINT exitCode) {
    if (hThread == NULL) {
        return statusCode = ERROR_INVALID_HANDLE;
    }

    if (::TerminateThread(hThread, exitCode)) {
        statusCode = 0;
        return 0;
    }
    else {
        statusCode = GetLastError();
        return statusCode;
    }
}

// 挂起线程
int kThread::Suspend() {
    if (hThread == NULL) {
        return statusCode = ERROR_INVALID_HANDLE;
    }

    if (SuspendThread(hThread) != (DWORD)-1) {
        statusCode = 0;
        return 0;
    }
    else {
        statusCode = GetLastError();
        return statusCode;
    }
}

// 恢复线程
int kThread::UnSuspend() {
    if (hThread == NULL) {
        return statusCode = ERROR_INVALID_HANDLE;
    }

    if (ResumeThread(hThread) != (DWORD)-1) {
        statusCode = 0;
        return 0;
    }
    else {
        statusCode = GetLastError();
        return statusCode;
    }
}

// 获取成员变量的实现
HANDLE kThread::GetHandle() const { return hThread; }
DWORD kThread::GetTID() const { return TID; }
int kThread::GetPriority() const { return priority; }
int kThread::GetStatusCode() const { return statusCode; }
std::string kThread::GetModulePath() const { return modulePath; }


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
	kLog.Add(Info, C(tmp1.c_str()),C("进程管理"));
	return;
}

kProcess::kProcess(DWORD pid) : PID(pid), handle(INVALID_HANDLE_VALUE), isAdmin(false)
, cpuUsage(0.0)                  // CPU使用率初始化为0
{
    handle = GetProcessHandleByPID(pid);
    if (handle != INVALID_HANDLE_VALUE) {
        // 获取进程名称并赋值给name
        name = C(GetProcessName());

        // 获取进程可执行文件路径
        WCHAR pathBuf[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(handle, NULL, pathBuf, MAX_PATH)) {
            exePath = std::string(WideCharToMultiByte(pathBuf));
        }

        // 检查进程是否拥有管理员权限
        isAdmin = CheckProcessAdminRights(handle);

        // 获取进程所属用户名称
        AuthName = GetProcessUserName(handle);
        UpdateMemoryInfo();    // 新增：初始化内存信息
        cpuUsage=UpdateCPUUsage();      // 新增：初始化CPU信息

        //GetCachedProcessIcon(PID);
    }
}
void kProcess::UpdateStats() {
    UpdateMemoryInfo();
    cpuUsage=UpdateCPUUsage();
}


// 构造函数：通过进程句柄初始化
kProcess::kProcess(HANDLE hProc) : handle(hProc), isAdmin(false), PID(0)
, cpuUsage(0.0)                  // CPU使用率初始化为0
{
    bool isService = false;
    ServiceInfo serviceInfo;

    if (1/*handle != INVALID_HANDLE_VALUE && handle != NULL*/) {
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
        UpdateMemoryInfo();    // 新增：初始化内存信息
        cpuUsage=UpdateCPUUsage();      // 新增：初始化CPU信息
        //GetCachedProcessIcon(PID);
    }
    if (GetServiceInfoByPID(PID, serviceInfo)) {
        isService = true;
        // 常规方法获取失败时，用服务信息填充
        if (name.empty()) {
            name = WideCharToMultiByte(serviceInfo.displayName); // 显示名称作为进程名
        }
        if (exePath.empty()) {
            exePath = "Service: " + WideCharToMultiByte(serviceInfo.serviceName); // 服务名称作为路径
        }
        if (AuthName.empty()) {
            AuthName = WideCharToMultiByte(serviceInfo.userName); // 服务运行账户
        }
    }

    // 特殊处理：如果是服务进程但仍有信息缺失，强制填充默认值
    if (isService) {
        if (name.empty()) name = "Service Process";
        if (AuthName.empty()) AuthName = "NT AUTHORITY\\SYSTEM";
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



#undef CURRENT_MODULE   