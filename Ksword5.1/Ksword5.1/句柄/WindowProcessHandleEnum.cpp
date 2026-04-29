// Main.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "KswordFrameCore/Ksword.h"
//包含万能头，万能与不万能的都在里面了
void KswordFrameShow();//演示Ksword相关函数调用。建议保存以备不时之需。

#define UNICODE
#define _UNICODE
typedef LONG    KPRIORITY;
#define SystemProcessInformation    5 // 功能号
std::wstring GetProcessNameByPid(DWORD pid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return L"";
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return L"";
    }

    do {
        if (pe32.th32ProcessID == pid) {
            CloseHandle(hSnapshot);
            return pe32.szExeFile; // 返回进程名（带扩展名）
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return L"";
}
typedef NTSTATUS(NTAPI* _NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
    );
// SYSTEM_HANDLE_TABLE_ENTRY_INFO定义（来自NtQuerySystemInformation返回的数据结构）
typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO {
    USHORT UniqueProcessId;     // 进程ID（USHORT）
    USHORT CreatorBackTraceIndex;
    UCHAR ObjectTypeIndex;      // 对象类型编号
    UCHAR HandleAttributes;     // 句柄属性
    USHORT HandleValue;         // 句柄值（USHORT）
    PVOID Object;               // 对象地址
    ULONG GrantedAccess;        // 访问权限
} SYSTEM_HANDLE_TABLE_ENTRY_INFO;

// 自定义兼容的SYSTEM_HANDLE结构体
typedef struct _SYSTEM_HANDLE {
    ULONG ProcessId;            // 进程ID（ULONG）
    UCHAR ObjectTypeNumber;     // 对象类型编号
    BYTE Flags;                // 标志位
    USHORT Handle;             // 句柄值（USHORT）
    PVOID Object;              // 对象地址
    ACCESS_MASK GrantedAccess; // 访问权限
} SYSTEM_HANDLE, * PSYSTEM_HANDLE;

// 句柄信息表结构体
typedef struct _SYSTEM_HANDLE_INFORMATION {
    ULONG HandleCount;          // 正确字段名
    SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];   // 动态数组
} SYSTEM_HANDLE_INFORMATION, * PSYSTEM_HANDLE_INFORMATION;
void GetSystemHandles(std::string startString) {
    _NtQuerySystemInformation NtQuerySystemInformation =
        (_NtQuerySystemInformation)GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtQuerySystemInformation");

    ULONG bufferSize = 0x1000;
    PSYSTEM_HANDLE_INFORMATION handleInfo = (PSYSTEM_HANDLE_INFORMATION)malloc(bufferSize);
    NTSTATUS status;
    // 动态调整缓冲区（参考网页5）
    while ((status = NtQuerySystemInformation(16, handleInfo, bufferSize, NULL)) == 0xC0000004L) {
        free(handleInfo);
        bufferSize *= 2;
        handleInfo = (PSYSTEM_HANDLE_INFORMATION)malloc(bufferSize);
    }
    if (status == 0) {
        std::cout << "Query succeeded with buffer size: " << bufferSize<<std::endl;
        std::cout <<"一共有句柄数量：" << std::endl << handleInfo->HandleCount << std::endl;
        for (ULONG i = 0; i < handleInfo->HandleCount; i++) { // 使用HandleCount
            
            SYSTEM_HANDLE_TABLE_ENTRY_INFO sourceEntry = handleInfo->Handles[i]; // 原始数据
            SYSTEM_HANDLE targetHandle;
            // 显式映射字段
            targetHandle.ProcessId = static_cast<ULONG>(sourceEntry.UniqueProcessId);
            targetHandle.Handle = sourceEntry.HandleValue;
            targetHandle.ObjectTypeNumber = sourceEntry.ObjectTypeIndex;
            targetHandle.GrantedAccess = sourceEntry.GrantedAccess;
            // 输出信息
            if (sourceEntry.ObjectTypeIndex == 37) {
                // 这是文件句柄
                HANDLE hProcess = OpenProcess(
                    PROCESS_DUP_HANDLE,
                    FALSE,
                    sourceEntry.UniqueProcessId
                ); 
                if (hProcess) {
                    HANDLE hDupHandle;
                    if (DuplicateHandle(
                        hProcess,
                        (HANDLE)sourceEntry.HandleValue,
                        GetCurrentProcess(),
                        &hDupHandle,
                        0,
                        FALSE,
                        DUPLICATE_SAME_ACCESS
                    )) {
                        wchar_t typeName[64];
                        DWORD retSize;
                        if (NT_SUCCESS(NtQueryObject(
                            hDupHandle,
                            ObjectTypeInformation,
                            typeName,
                            sizeof(typeName),
                            &retSize
                        ))) {
                            //std::cout << "成功获取文件句柄: 0x"
                            //    << std::hex << hDupHandle << std::endl;
                            // 此处可存储句柄或进行后续操作
                            //std::cout << "3";
                                wchar_t devicePath[MAX_PATH + 1] = { 0 };
                                // 使用GetFinalPathNameByHandleW获取设备路径（网页3）
                                DWORD pathLen = GetFinalPathNameByHandleW(
                                    hDupHandle,
                                    devicePath,
                                    MAX_PATH,
                                    VOLUME_NAME_DOS  // 返回DOS格式路径（网页8）
                                ); 
                                if (pathLen == 0) {
                                    //DWORD err = GetLastError();
                                    //std::cerr << "路径获取失败 (错误码: 0x" << std::hex << err << ")" << std::endl;
                                    // 失败时必须先关闭复制句柄，避免循环中持续泄漏句柄资源。
                                    CloseHandle(hDupHandle);
                                    continue;
                                }
                                char ansiPath[MAX_PATH * 2] = { 0 };
                                WideCharToMultiByte(
                                    CP_ACP,
                                    0,
                                    devicePath,
                                    -1,
                                    ansiPath,
                                    sizeof(ansiPath),
                                    NULL,
                                    NULL
                                );
                                //std::cout << "5";
                                // 处理路径前缀（网页3）
                                std::string finalPath(ansiPath);
                                size_t pos = finalPath.find("\\\\?\\");
                                if (pos != std::string::npos) {
                                    finalPath.erase(0, 4); // 移除长路径前缀
                                }
                                if (finalPath.rfind(startString, 0) == 0) {
                                    std::cout << "Handle[" << i << "]: "
                                        << "PID=" << targetHandle.ProcessId
                                        << ", Value=0x" << std::hex << targetHandle.Handle
                                        << ", Type=0x" << (int)targetHandle.ObjectTypeNumber
                                        << ", Name=" << WstringToString(GetProcessNameByPid(targetHandle.ProcessId))
                                        << std::dec << std::endl;
                                    
                                std::cout << "目标文件路径: " << finalPath << std::endl;
                                }
                                

                        }
                        CloseHandle(hDupHandle);
                    }
                    else {
                        //std::cerr << "复制令牌 (错误码: 0x" << std::hex << GetLastError() << ")" << std::endl;
                    }
                    CloseHandle(hProcess);
                }
                else {
                }
            }
        }
    }
    // 函数退出前统一释放句柄快照缓冲，避免重复调用时产生稳定内存泄漏。
    if (handleInfo != nullptr) {
        free(handleInfo);
        handleInfo = nullptr;
    }
}
//进程结构体，从官网copy
//从NTDLL里定义原型
typedef DWORD(WINAPI* PNtQuerySystemInformation) (UINT systemInformation, PVOID SystemInformation, ULONG SystemInformationLength,
    PULONG ReturnLength);
BOOL NtQueryAllProcess() {
    BOOL ret = FALSE;
    PNtQuerySystemInformation NtQuerySystemInformation = NULL;
    // ntdllHandle 用途：保存 LoadLibrary 返回值，以便函数结束时对应 FreeLibrary，避免模块引用泄漏。
    HMODULE ntdllHandle = LoadLibrary(L"ntdll.dll");
    NtQuerySystemInformation = (PNtQuerySystemInformation)GetProcAddress(ntdllHandle, "NtQuerySystemInformation");
    PSYSTEM_PROCESS_INFORMATION sysProInfo = NULL, old = NULL;
    if (NtQuerySystemInformation != NULL) {
        ULONG cbSize = sizeof(SYSTEM_PROCESS_INFORMATION);
        //查询
        LONG status = 0;
        do {
            old = sysProInfo = (PSYSTEM_PROCESS_INFORMATION)malloc(cbSize);
            status = NtQuerySystemInformation(SystemProcessInformation, sysProInfo, cbSize, &cbSize);
            if (status)
                free(sysProInfo);
        } while (status);
        ret = TRUE;
        //遍历进程
        do {
            if (sysProInfo->ImageName.Buffer != NULL)
            {
                _tprintf(L"进程名:\t%s \t进程ID:%u \t句柄总数:%u \t线程总数:%u \n", sysProInfo->ImageName.Buffer, sysProInfo->UniqueProcessId,
                sysProInfo->HandleCount, sysProInfo->NumberOfThreads);
                //打印线程信息
                PSYSTEM_THREAD_INFORMATION threadInfo = NULL;
                threadInfo = (PSYSTEM_THREAD_INFORMATION)((ULONG64)sysProInfo + sizeof(SYSTEM_PROCESS_INFORMATION));
                DWORD curThreadIndex = 1;
                do {
                    _tprintf(L"\t线程ID:%u\t起始地址:%x \t线程的状态码:%u\n", threadInfo->ClientId.UniqueThread, threadInfo->StartAddress, threadInfo->ThreadState);
                    threadInfo += 1;
                } while (curThreadIndex++ < sysProInfo->NumberOfThreads);
                _tprintf(L"\n");
            }
            //指针的加减运算的单位是根据所指向数据类型大小的。字节指针就是1，所以加减运算没问题。这里是结构体指针，所以必须转成数字类型再运算。
            sysProInfo = (PSYSTEM_PROCESS_INFORMATION)((ULONG64)sysProInfo + sysProInfo->NextEntryOffset);
        } while (sysProInfo->NextEntryOffset != 0);
        free(old);
    }

    if (ntdllHandle != nullptr) {
        FreeLibrary(ntdllHandle);
        ntdllHandle = nullptr;
    }
    return ret;
}


int main() {
    if (!HasDebugPrivilege()) {
        if (!EnableDebugPrivilege(TRUE)) {
            KMesErr("出现错误，提权失败");
        }
        else {
            KMesInfo("提权成功");
        }
    }
    std::cout << "请输入路径前缀：";
    GetSystemHandles(Kgetline());
    Kpause();
    return 0;
}



// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
void
KswordFrameShow() {
    KMesInfo("在一切之前，请看向我们的生成配置。AVB是AntiVirusBypass的缩写，这是经过尝试误报率最低的编译方式。没有配置x86的。");
    KMesInfo("这个生成的文件在最外层目录下的AntiVirusBypassRelease下。");
    KMesInfo("下面我们开始介绍消息提示。");
    KMesInfo("我们有三种提示类型：KMesInfo，");
    KMesWarn("KMesWarn，和");
    KMesErr("KMesErr。");
    std::cout << "调用Kgetline()函数，使用钩子获取输入。Kgetline的实现参见Ksword的readme。如果嫌麻烦就用cin也可以。\n";
    std::cout << "请输入内容："; std::string a = Kgetline();
    KMesInfo("你输入的是" + a);

    KMesInfo("下面演示一段经典程序初始化");
    KMesInfo("我们使用以下代码来申请管理员权限");
    Kpause();
    //////////////////////////////////////////////////////////////
    if (!IsAdmin()) {
        if (RequestAdmin(StringToWString(GetSelfPath())) == KSWORD_ERROR_EXIT) {
            KMesErr("出现错误，无法启动");
        }
        else {
            exit(0);
        }
    }
    else {
        KMesInfo("你已经是管理员了！");
    }
    //////////////////////////////////////////////////////////////
    KMesInfo("我们使用以下代码来申请Debug权限");
    Kpause();
    //////////////////////////////////////////////////////////////
    if (!HasDebugPrivilege()) {
        if (!EnableDebugPrivilege(TRUE)) {
            KMesErr("出现错误，提权失败");
        }
        else {
            KMesInfo("提权成功");
        }
    }
    else {
        KMesInfo("你已经是Debug权限！");
    }
    //////////////////////////////////////////////////////////////
    KMesInfo("我们像这样获取用户信息（需要KEnviProb();）");
    Kpause();
    //////////////////////////////////////////////////////////////
    KMesInfo("当前进程权限账号：" + AuthName);
    KMesInfo("当前计算机名称：" + HostName);
    KMesInfo("当前Exe路径：" + ExePath);
    //////////////////////////////////////////////////////////////
    KMesInfo("这样可以实时执行cmd命令并显示结果。请输入你想要执行的一段命令，直接敲回车代表ping www.baidu.com");
    std::string testCmd = Kgetline();
    //////////////////////////////////////////////////////////////
    if (testCmd == "") {
        RunCmdNow("ping www.baidu.com");
    }
    else {
        RunCmdNow(testCmd);
    }
    //////////////////////////////////////////////////////////////
    KMesInfo("RunCmdAsyn可以异步执行cmd命令，当然也就没有回显和返回值");
    KMesInfo("还有一些众多小玩意，详情请参见ksword.h头文件。");
    KMesInfo("当你想要字符串转换的时候，直接输入chartow，自动补全会找到你需要的函数的。有几个函数用完了要delete一下，都有备注。没delete其实也没什么鸟关系。");
    KMesInfo("cprint(文字，前景色代号，背景色代号)可以彩色打印，需要的时候可以玩玩。");
    KMesWarn("向导正在退出，按回车键继续");
    Kpause();

}
