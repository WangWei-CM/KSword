#include "../../KswordTotalHead.h"
#include "Process.h"
#define CURRENT_MODULE "���̺��Ĺ���"
extern LPDIRECT3DTEXTURE9 GetCachedProcessIcon(DWORD pid);


extern BOOL TerminateAllThreads(HANDLE hProcess);
extern BOOL NtTerminate(HANDLE hProcess);
extern BOOL TerminateProcessViaJob(HANDLE hProcess);

bool GetServiceInfoByPID(DWORD pid, ServiceInfo& info) {
    // �򿪷�����ƹ�����
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hSCM) return false;

    DWORD bytesNeeded = 0;
    DWORD servicesReturned = 0;
    // ��һ�ε��û�ȡ��������С
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

    // ���仺������ö�ٷ���
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

    // ƥ��PID��Ӧ�ķ���
    for (DWORD i = 0; i < servicesReturned; ++i) {
        if (pServices[i].ServiceStatusProcess.dwProcessId == pid) {
            info.serviceName = pServices[i].lpServiceName;
            info.displayName = pServices[i].lpDisplayName;

            // ����Ĭ�������˻����ɸ���ʵ�ʳ�����չ��
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

// GUIprocessCore.cpp ��������
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

void kProcess::UpdateMemoryInfo() {

}
// GUIprocessCore.cpp ��������
// �ļ�������ӳ������壨��������������ӣ�
const double FILETIME_SECOND_FACTOR = 10000000.0;  // 100ns = 10^-7���ת������


    static uint64_t FileTimeToUInt64(const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }
// �ڹ��캯���г�ʼ��������

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

    // ����ʱ����λ��100���룩
    ULONGLONG timePassed = GetTickCount64() - lastTick_;
    lastTick_ = GetTickCount64();
    lastKernel_ = currentKernel;
    lastUser_ = currentUser;

    if (timePassed == 0) return 0.0f;
    return (delta / 10000.0f) / timePassed * 100.0f; // ת��Ϊ�ٷֱ�
}
kThread::kThread(DWORD tid) : hThread(NULL), TID(tid), priority(0), statusCode(0) {
    // ͨ���߳�ID���߳̾������ҪTHREAD_QUERY_INFORMATIONȨ�ޣ�
    hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    if (hThread == NULL) {
        statusCode = GetLastError();
        return;
    }
    InitThreadInfo();
}

// ͨ�����̾����ʼ������ȡ���̣߳�
kThread::kThread(HANDLE hProcess) : hThread(NULL), TID(0), priority(0), statusCode(0) {
    if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE) {
        statusCode = ERROR_INVALID_HANDLE;
        return;
    }

    // �����߳̿��ղ������߳�
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
                // �ҵ����̵ĵ�һ���߳���Ϊ���߳�
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

// �����������ͷ��߳̾��
kThread::~kThread() {
    if (hThread != NULL && hThread != INVALID_HANDLE_VALUE) {
        CloseHandle(hThread);
        hThread = NULL;
    }
}

// ��ʼ���߳���Ϣ�����ȼ���ģ��·���ȣ�
void kThread::InitThreadInfo() {
    // ��ȡ�߳����ȼ�
    int prio = GetThreadPriority(hThread);
    if (prio != THREAD_PRIORITY_ERROR_RETURN) {
        priority = prio;
    }
    else {
        statusCode = GetLastError();
    }

    // ��ȡ�߳�����ģ��·��
    HMODULE hModule;
    DWORD bytesNeeded;
    if (EnumProcessModulesEx(GetCurrentProcess(), &hModule, sizeof(hModule), &bytesNeeded, LIST_MODULES_ALL)) {
        char path[MAX_PATH];
        if (GetModuleFileNameExA(GetCurrentProcess(), hModule, path, MAX_PATH)) {
            modulePath = path;
        }
    }
}

// ��ֹ�߳�
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

// �����߳�
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

// �ָ��߳�
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

// ��ȡ��Ա������ʵ��
HANDLE kThread::GetHandle() const { return hThread; }
DWORD kThread::GetTID() const { return TID; }
int kThread::GetPriority() const { return priority; }
int kThread::GetStatusCode() const { return statusCode; }
std::string kThread::GetModulePath() const { return modulePath; }


// �����������������Ƿ��Ƿ���й���ԱȨ��
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

// ������������ȡ���������û�����
std::string GetProcessUserName(HANDLE handle ) {
    if (handle == INVALID_HANDLE_VALUE) return "";

    HANDLE tokenHandle = NULL;
    if (!OpenProcessToken(handle, TOKEN_QUERY, &tokenHandle)) {
        return "";
    }

    // ��ȡ�����û���Ϣ
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

    // ת��SID���˻���
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
		/*C*/(("ʹ��task kill��ֹpidΪ" + std::to_string(pid) + "�Ľ���").c_str()),
		/*C*/("ִ��cmd����"),
		NULL
	);
	std::string tmp = "taskkill /pid " + std::to_string(pid);
	std::string tmp1 = GetCmdResultWithUTF8(tmp);
	kLog.Add(Info, C(tmp1.c_str()),C("���̹���"));
	return;
}

kProcess::kProcess(DWORD pid) : PID(pid), handle(INVALID_HANDLE_VALUE), isAdmin(false)
, cpuUsage(0.0)                  // CPUʹ���ʳ�ʼ��Ϊ0
{
    handle = GetProcessHandleByPID(pid);
    if (handle != INVALID_HANDLE_VALUE) {
        // ��ȡ�������Ʋ���ֵ��name
        name = C(GetProcessName());

        // ��ȡ���̿�ִ���ļ�·��
        WCHAR pathBuf[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(handle, NULL, pathBuf, MAX_PATH)) {
            exePath = std::string(WideCharToMultiByte(pathBuf));
        }

        // �������Ƿ�ӵ�й���ԱȨ��
        isAdmin = CheckProcessAdminRights(handle);

        // ��ȡ���������û�����
        AuthName = GetProcessUserName(handle);
        UpdateMemoryInfo();    // ��������ʼ���ڴ���Ϣ
        cpuUsage=UpdateCPUUsage();      // ��������ʼ��CPU��Ϣ

        //GetCachedProcessIcon(PID);
    }
}
void kProcess::UpdateStats() {
    UpdateMemoryInfo();
    cpuUsage=UpdateCPUUsage();
}


// ���캯����ͨ�����̾����ʼ��
kProcess::kProcess(HANDLE hProc) : handle(hProc), isAdmin(false), PID(0)
, cpuUsage(0.0)                  // CPUʹ���ʳ�ʼ��Ϊ0
{
    bool isService = false;
    ServiceInfo serviceInfo;

    if (1/*handle != INVALID_HANDLE_VALUE && handle != NULL*/) {
        PID = GetProcessId(hProc);
        name = GetProcessName();

        // ��ȡ���̿�ִ���ļ�·��
        WCHAR pathBuf[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(handle, NULL, pathBuf, MAX_PATH)) {
            exePath = std::string(WideCharToMultiByte(pathBuf));
        }

        // �������Ƿ�ӵ�й���ԱȨ��
        isAdmin = CheckProcessAdminRights(handle);

        // ��ȡ���������û�����
        AuthName = GetProcessUserName(handle);
        UpdateMemoryInfo();    // ��������ʼ���ڴ���Ϣ
        cpuUsage=UpdateCPUUsage();      // ��������ʼ��CPU��Ϣ
        //GetCachedProcessIcon(PID);
    }
    if (GetServiceInfoByPID(PID, serviceInfo)) {
        isService = true;
        // ���淽����ȡʧ��ʱ���÷�����Ϣ���
        if (name.empty()) {
            name = WideCharToMultiByte(serviceInfo.displayName); // ��ʾ������Ϊ������
        }
        if (exePath.empty()) {
            exePath = "Service: " + WideCharToMultiByte(serviceInfo.serviceName); // ����������Ϊ·��
        }
        if (AuthName.empty()) {
            AuthName = WideCharToMultiByte(serviceInfo.userName); // ���������˻�
        }
    }

    // ���⴦������Ƿ�����̵�������Ϣȱʧ��ǿ�����Ĭ��ֵ
    if (isService) {
        if (name.empty()) name = "Service Process";
        if (AuthName.empty()) AuthName = "NT AUTHORITY\\SYSTEM";
    }

}
bool kProcess::Terminate(kProcKillMethod method, UINT uExitCode)
{
	if (method == kTaskkill) {
		// ʹ��taskkill�����������
		std::string cmd = "taskkill /pid " + std::to_string(PID);
		RunCmdNow(cmd);
		return true;
	}
    else if (method == kTaskkillF) {
        // ʹ��taskkill�����������
        std::string cmd = "taskkill /pid " + std::to_string(PID)+"/f";
        RunCmdNow(cmd);
        return true;
    }
	else if (method == kTerminate) {
		return TerminateProcess(handle, uExitCode);
	}
	else if (method == kTerminateThread) {
		// ����TerminateThread�������������߳�
        return TerminateAllThreads(this->handle);
	}
	else if (method == kNTTerminate) {
		// ����NT API��������
        return NtTerminate(handle);
	}
	else if (method == kJobTer) {
		// ʹ����ҵ�����������
        return TerminateProcessViaJob(handle);
    }
	else {
		
		return false;
	}
}

// �������
bool kProcess::Suspend() {
    return SuspendProcess(this->PID);
}

// �ָ�����
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

// ��̬������ͨ����������ȡPID
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

// �ƶ����캯����ת�ƾ������Ȩ
kProcess::kProcess(kProcess&& other) noexcept
    : PID(other.PID),
    AuthName(std::move(other.AuthName)),
    exePath(std::move(other.exePath)),
    isAdmin(other.isAdmin),
    handle(other.handle),  // ת�ƾ��
    name(std::move(other.name)) {
    other.handle = INVALID_HANDLE_VALUE;  // ԭ��������Ϊ��Ч����������ʱ�ͷ�
    other.PID = 0;
}


// �ƶ���ֵ�������ת�ƾ������Ȩ
kProcess& kProcess::operator=(kProcess&& other) noexcept {
    if (this != &other) {
        // �ͷŵ�ǰ����ľ��
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }

        // ת����Դ
        PID = other.PID;
        AuthName = std::move(other.AuthName);
        exePath = std::move(other.exePath);
        isAdmin = other.isAdmin;
        handle = other.handle;  // �ӹܾ��
        name = std::move(other.name);

        // ԭ��������Ϊ��Ч
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