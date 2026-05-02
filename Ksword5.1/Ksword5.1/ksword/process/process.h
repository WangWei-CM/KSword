#pragma once

// ============================================================
// ksword/process/process.h
// 命名空间：ks::process
// 作用：
// - 封装进程枚举、进程详情、进程控制等 Win32 API；
// - 供 ProcessDock 等 UI 层直接调用；
// - 与项目解耦后可作为独立工具库复用。
// ============================================================

#include <cstdint> // std::uint32_t/std::uint64_t：PID、计数器、时间戳。
#include <string>  // std::string：跨层统一文本类型（UTF-8）。
#include <vector>  // std::vector：进程列表容器。

#include "../../../../shared/driver/KswordArkProcessIoctl.h" // R0 进程扩展字段常量。
#include "../../../../shared/driver/KswordArkThreadIoctl.h"  // R0 线程扩展字段常量。

namespace ks::process
{
    // ProcessEnumStrategy：进程遍历策略。
    enum class ProcessEnumStrategy
    {
        SnapshotProcess32 = 0,   // CreateToolhelp32Snapshot + Process32First/Next。
        NtQuerySystemInfo = 1,   // NtQuerySystemInformation(SystemProcessInformation)。
        Auto = 2                 // 自动：优先 NtQuery，失败时回退 Snapshot。
    };

    // ProcessPriorityLevel：进程优先级枚举（映射 Win32 PriorityClass）。
    enum class ProcessPriorityLevel
    {
        Idle = 0,
        BelowNormal,
        Normal,
        AboveNormal,
        High,
        Realtime
    };

    // TokenPrivilegeAction：令牌特权调整动作（用于 AdjustTokenPrivileges）。
    enum class TokenPrivilegeAction
    {
        Keep = 0,   // 保持当前状态，不调整。
        Enable,     // 启用该特权。
        Disable,    // 禁用该特权。
        Remove      // 从令牌中移除该特权（高风险）。
    };

    // TokenPrivilegeEdit：单个特权的调整请求。
    struct TokenPrivilegeEdit
    {
        std::string privilegeName;               // 例如 "SeDebugPrivilege"。
        TokenPrivilegeAction action = TokenPrivilegeAction::Keep;
    };

    // SecurityAttributesInput：CreateProcessW 两个 SECURITY_ATTRIBUTES 参数的输入镜像。
    struct SecurityAttributesInput
    {
        bool useValue = false;                   // false -> 传 nullptr。
        std::uint32_t nLength = 0;               // 0 表示实现层使用 sizeof(SECURITY_ATTRIBUTES)。
        std::uint64_t securityDescriptor = 0;    // 指针地址（0 表示 nullptr）。
        bool inheritHandle = false;              // bInheritHandle。
    };

    // StartupInfoInput：STARTUPINFOW 输入镜像（全部字段可由 UI 自定义）。
    struct StartupInfoInput
    {
        bool useValue = false;                   // false -> lpStartupInfo 传 nullptr。
        std::uint32_t cb = 0;                    // 0 表示使用 sizeof(STARTUPINFOW)。
        std::string lpReserved;
        std::string lpDesktop;
        std::string lpTitle;
        std::uint32_t dwX = 0;
        std::uint32_t dwY = 0;
        std::uint32_t dwXSize = 0;
        std::uint32_t dwYSize = 0;
        std::uint32_t dwXCountChars = 0;
        std::uint32_t dwYCountChars = 0;
        std::uint32_t dwFillAttribute = 0;
        std::uint32_t dwFlags = 0;
        std::uint16_t wShowWindow = 0;
        std::uint16_t cbReserved2 = 0;
        std::uint64_t lpReserved2 = 0;
        std::uint64_t hStdInput = 0;
        std::uint64_t hStdOutput = 0;
        std::uint64_t hStdError = 0;
    };

    // ProcessInformationInput：PROCESS_INFORMATION 输入镜像。
    // 说明：
    // - 正常场景该结构应由 API 输出；
    // - 此处保留“预填充字段”能力，满足 UI 100% 可编辑需求。
    struct ProcessInformationInput
    {
        bool useValue = false;                   // false -> lpProcessInformation 传 nullptr。
        std::uint64_t hProcess = 0;
        std::uint64_t hThread = 0;
        std::uint32_t dwProcessId = 0;
        std::uint32_t dwThreadId = 0;
    };

    // CreateProcessRequest：创建进程参数对象（覆盖 CreateProcessW 全部参数 + Token 扩展）。
    struct CreateProcessRequest
    {
        bool useApplicationName = false;         // false -> lpApplicationName 传 nullptr。
        std::string applicationName;
        bool useCommandLine = false;             // false -> lpCommandLine 传 nullptr。
        std::string commandLine;

        SecurityAttributesInput processAttributes;   // lpProcessAttributes。
        SecurityAttributesInput threadAttributes;    // lpThreadAttributes。

        bool inheritHandles = false;             // bInheritHandles。
        std::uint32_t creationFlags = 0;         // dwCreationFlags。

        bool useEnvironment = false;             // false -> lpEnvironment 传 nullptr。
        bool environmentUnicode = true;          // true 时自动附加 CREATE_UNICODE_ENVIRONMENT。
        std::vector<std::string> environmentEntries; // 每行一个 "KEY=VALUE"。

        bool useCurrentDirectory = false;        // false -> lpCurrentDirectory 传 nullptr。
        std::string currentDirectory;

        StartupInfoInput startupInfo;            // lpStartupInfo。
        ProcessInformationInput processInfo;     // lpProcessInformation。

        // tokenModeEnabled=false 时调用 CreateProcessW；
        // true 时执行 “打开 PID 令牌 + 可选调权 + CreateProcessAsUserW”。
        bool tokenModeEnabled = false;
        std::uint32_t tokenSourcePid = 0;
        std::uint32_t tokenDesiredAccess = 0;    // 0 表示实现层使用默认访问掩码。
        bool duplicatePrimaryToken = true;       // true 时先 DuplicateTokenEx(TokenPrimary)。
        std::vector<TokenPrivilegeEdit> tokenPrivilegeEdits;
    };

    // CreateProcessResult：创建进程结果快照（供 UI 反馈和日志输出）。
    struct CreateProcessResult
    {
        bool success = false;
        std::uint32_t win32Error = 0;
        std::string detailText;
        bool usedTokenPath = false;
        bool usedCreateProcessWithTokenFallback = false;
        bool processInfoAvailable = false;
        std::uint64_t hProcess = 0;
        std::uint64_t hThread = 0;
        std::uint32_t dwProcessId = 0;
        std::uint32_t dwThreadId = 0;
    };

    // ProcessRecord：单进程快照数据结构。
    struct ProcessRecord
    {
        std::uint32_t pid = 0;             // 进程 ID。
        std::uint32_t parentPid = 0;       // 父进程 ID。
        std::uint32_t threadCount = 0;     // 线程数量（快照时刻统计）。
        std::uint32_t handleCount = 0;     // 句柄数量（快照时刻统计，部分策略可得）。
        std::uint32_t sessionId = 0;       // 会话 ID（用于区分登录会话）。

        // creationTime100ns：
        // - 进程创建时间（FILETIME 100ns 基准）；
        // - 与 PID 共同构成稳定 identity（用于“复用旧数据”规则）。
        std::uint64_t creationTime100ns = 0;

        std::string processName;           // 进程名（显示名）。
        std::string imagePath;             // 可执行文件完整路径。
        std::string commandLine;           // 启动参数（命令行）。
        std::string userName;              // 进程令牌用户（DOMAIN\\User）。
        // signatureState：用于 UI 直接显示的签名文本。
        // 典型值：
        // - "Microsoft Corporation (Trusted)"
        // - "Unknown Publisher (Untrusted)"
        // - "Unsigned"
        // - "Pending"
        std::string signatureState;
        std::string signaturePublisher;    // 签名发布者（厂家）名称。
        bool signatureTrusted = false;     // 是否被 Windows 信任链验证通过。
        std::string startTimeText;         // 启动时间文本（YYYY-MM-DD HH:MM:SS）。
        std::string architectureText;      // 架构文本（x64/x86/ARM/Unknown）。
        std::string priorityText;          // 优先级文本（Normal/High/...）。
        bool efficiencyModeSupported = false; // 是否成功查询效率模式状态。
        bool efficiencyModeEnabled = false;   // 是否启用效率模式（PowerThrottling ExecutionSpeed）。
        bool isAdmin = false;              // 进程令牌是否已提升（管理员权限）。
        std::uint32_t protectionLevel = 0;  // PPL 保护级别枚举值，来自手动刷新快照。
        bool protectionLevelKnown = false;  // protectionLevelKnown：true 表示本轮已手动查询 PPL。
        std::string protectionLevelText;    // protectionLevelText：PPL 枚举文本，未刷新时保持空。

        // ======== 原始性能计数器（用于相邻两轮差值计算） ========
        std::uint64_t rawCpuTime100ns = 0;      // Kernel + User 总 CPU 时间（100ns）。
        std::uint64_t rawWorkingSetBytes = 0;   // 工作集内存字节数。
        std::uint64_t rawPrivateBytes = 0;      // 私有提交/申请内存字节数。
        std::uint64_t rawIoBytes = 0;           // Read/Write/Other 传输累计字节。

        // ======== UI 直接显示的衍生性能数据 ========
        double cpuPercent = 0.0;           // CPU 百分比（0~100 * 逻辑核折算）。
        double ramMB = 0.0;                // RAM 申请内存（MB，优先 PrivateUsage）。
        double workingSetMB = 0.0;         // RAM 实际使用工作集（MB）。
        double diskMBps = 0.0;             // 磁盘吞吐（MB/s）。
        double gpuPercent = 0.0;           // GPU 百分比（R3 通过 PDH GPU Engine 按 PID 聚合）。
        double netKBps = 0.0;              // 网络（当前预留，默认 0）。

        bool staticDetailsReady = false;   // true 表示详情字段已完整填充。
        bool dynamicCountersReady = false; // true 表示性能计数器可用。

        // ======== R0 / EPROCESS 扩展信息（Phase 2） ========
        std::uint32_t r0Flags = 0;          // R0 枚举行原始 flags，例如隐藏进程标记。
        std::uint32_t r0FieldFlags = 0;     // KSWORD_ARK_PROCESS_FIELD_* 可用性位图。
        std::uint32_t r0Status = KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE; // R0 扩展读取状态。
        std::uint64_t r0DynDataCapabilityMask = 0; // 驱动 DynData capability 位图快照。
        std::string r0ImagePath;            // R0 通过公开 API 返回的镜像路径（通常是 NT 路径）。

        std::uint8_t r0Protection = 0;      // EPROCESS.Protection 原始字节。
        std::uint8_t r0SignatureLevel = 0;  // EPROCESS.SignatureLevel 原始字节。
        std::uint8_t r0SectionSignatureLevel = 0; // EPROCESS.SectionSignatureLevel 原始字节。

        std::uint32_t r0SessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // Session 字段来源。
        std::uint32_t r0ImagePathSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // 镜像路径来源。
        std::uint32_t r0ProtectionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // Protection 来源。
        std::uint32_t r0SignatureLevelSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // SignatureLevel 来源。
        std::uint32_t r0SectionSignatureLevelSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // SectionSignatureLevel 来源。
        std::uint32_t r0ObjectTableSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // ObjectTable 来源。
        std::uint32_t r0SectionObjectSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE; // SectionObject 来源。

        std::uint32_t r0ProtectionOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // EPROCESS.Protection 偏移。
        std::uint32_t r0SignatureLevelOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // EPROCESS.SignatureLevel 偏移。
        std::uint32_t r0SectionSignatureLevelOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // EPROCESS.SectionSignatureLevel 偏移。
        std::uint32_t r0ObjectTableOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // EPROCESS.ObjectTable 偏移。
        std::uint32_t r0SectionObjectOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // EPROCESS.SectionObject 偏移。
        std::uint64_t r0ObjectTableAddress = 0; // EPROCESS.ObjectTable 当前指针值。
        std::uint64_t r0SectionObjectAddress = 0; // EPROCESS.SectionObject 当前指针值。
    };

    // ProcessModuleRecord：进程模块列表中的单行数据。
    struct ProcessModuleRecord
    {
        std::string modulePath;                // 模块完整路径。
        std::string moduleName;                // 模块名（文件名）。
        std::uint64_t moduleBaseAddress = 0;   // 模块加载基址。
        std::uint32_t moduleSizeBytes = 0;     // 模块映像大小（字节）。
        // 模块签名显示文本与发布者/可信标记，语义与 ProcessRecord 对齐。
        std::string signatureState;            // 模块签名显示文本。
        std::string signaturePublisher;        // 模块签名发布者（厂家）。
        bool signatureTrusted = false;         // 模块签名是否受 Windows 信任。
        std::uint32_t entryPointRva = 0;       // 入口点 RVA（相对偏移）。
        std::string runningState;              // 运行状态文本（Loaded/Unknown）。
        std::uint32_t representativeThreadId = 0; // 代表线程 ID（用于线程操作快捷入口）。
        std::string threadIdText;              // 线程 ID 汇总文本（逗号分隔）。
    };

    // ProcessThreadRecord：进程线程列表中的单线程数据。
    struct ProcessThreadRecord
    {
        std::uint32_t threadId = 0;        // 线程 ID。
        std::uint32_t ownerPid = 0;        // 所属进程 PID。
        int basePriority = 0;              // 线程基础优先级。
        std::string stateText;             // 线程状态文本（Running/Unknown）。
    };

    // SystemThreadRecord：系统全量线程快照中的单线程数据。
    struct SystemThreadRecord
    {
        std::uint32_t threadId = 0;            // 线程 ID（TID）。
        std::uint32_t ownerPid = 0;            // 所属进程 PID。
        std::string ownerProcessName;          // 所属进程名称（用于列表展示）。
        std::uint64_t startAddress = 0;        // 线程启动地址（内核返回的指针值）。
        int priority = 0;                      // 当前动态优先级。
        int basePriority = 0;                  // 基础优先级。
        std::uint32_t threadState = 0;         // 线程状态码（KTHREAD_STATE 数值）。
        std::uint32_t waitReason = 0;          // 等待原因码（KWAIT_REASON 数值）。
        std::uint64_t kernelTime100ns = 0;     // 内核态累计时间（100ns）。
        std::uint64_t userTime100ns = 0;       // 用户态累计时间（100ns）。
        std::uint64_t createTime100ns = 0;     // 创建时间（FILETIME 100ns）。
        std::uint32_t waitTimeTick = 0;        // 等待时长计数（Nt 原始字段）。
        std::uint32_t contextSwitchCount = 0;  // 上下文切换次数（Nt 原始字段）。
        std::uint64_t stackBase = 0;           // R3 SystemExtendedProcessInformation 返回的用户栈基址。
        std::uint64_t stackLimit = 0;          // R3 SystemExtendedProcessInformation 返回的用户栈边界。
        std::uint64_t win32StartAddress = 0;   // R3 SystemExtendedProcessInformation 返回的 Win32StartAddress。
        std::uint64_t tebBaseAddress = 0;      // R3 SystemExtendedProcessInformation 返回的 TEB 基址。
        std::uint32_t r0ThreadFieldFlags = 0;  // KSWORD_ARK_THREAD_FIELD_* 可用性位图。
        std::uint32_t r0ThreadStatus = KSWORD_ARK_THREAD_R0_STATUS_UNAVAILABLE; // R0 KTHREAD 扩展状态。
        std::uint32_t r0StackFieldSource = KSW_DYN_FIELD_SOURCE_UNAVAILABLE; // KTHREAD 栈字段来源。
        std::uint32_t r0IoFieldSource = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;    // KTHREAD I/O counter 来源。
        std::uint64_t r0InitialStack = 0;      // KTHREAD.InitialStack。
        std::uint64_t r0StackLimit = 0;        // KTHREAD.StackLimit。
        std::uint64_t r0StackBase = 0;         // KTHREAD.StackBase。
        std::uint64_t r0KernelStack = 0;       // KTHREAD.KernelStack。
        std::uint64_t r0ReadOperationCount = 0; // KTHREAD ReadOperationCount。
        std::uint64_t r0WriteOperationCount = 0; // KTHREAD WriteOperationCount。
        std::uint64_t r0OtherOperationCount = 0; // KTHREAD OtherOperationCount。
        std::uint64_t r0ReadTransferCount = 0; // KTHREAD ReadTransferCount。
        std::uint64_t r0WriteTransferCount = 0; // KTHREAD WriteTransferCount。
        std::uint64_t r0OtherTransferCount = 0; // KTHREAD OtherTransferCount。
        std::uint32_t r0KtInitialStackOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // DynData KtInitialStack 偏移。
        std::uint32_t r0KtStackLimitOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;   // DynData KtStackLimit 偏移。
        std::uint32_t r0KtStackBaseOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;    // DynData KtStackBase 偏移。
        std::uint32_t r0KtKernelStackOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;  // DynData KtKernelStack 偏移。
        std::uint32_t r0KtReadOperationCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // DynData KtReadOperationCount 偏移。
        std::uint32_t r0KtWriteOperationCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // DynData KtWriteOperationCount 偏移。
        std::uint32_t r0KtOtherOperationCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE; // DynData KtOtherOperationCount 偏移。
        std::uint32_t r0KtReadTransferCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;   // DynData KtReadTransferCount 偏移。
        std::uint32_t r0KtWriteTransferCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;  // DynData KtWriteTransferCount 偏移。
        std::uint32_t r0KtOtherTransferCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;  // DynData KtOtherTransferCount 偏移。
        std::uint64_t r0ThreadDynDataCapabilityMask = 0; // 驱动 DynData capability 位图快照。
    };

    // ProcessModuleSnapshot：模块页签所需快照数据（模块 + 线程）。
    struct ProcessModuleSnapshot
    {
        std::vector<ProcessModuleRecord> modules; // 模块列表。
        std::vector<ProcessThreadRecord> threads; // 线程列表。
        std::string diagnosticText;              // 刷新诊断文本（失败原因/回退路径）。
    };

    // CounterSample：用于跨刷新轮次计算差值的历史样本。
    struct CounterSample
    {
        std::uint64_t cpuTime100ns = 0;    // 上一轮 CPU 累计值。
        std::uint64_t ioBytes = 0;         // 上一轮 IO 累计值。
        std::uint64_t sampleTick100ns = 0; // 采样时刻（steady_clock 转 100ns）。
    };

    // BuildProcessIdentityKey 作用：
    // - 使用 “PID + 创建时间” 生成稳定 identity 字符串；
    // - 用于判断“是否同一个进程实例”。
    std::string BuildProcessIdentityKey(std::uint32_t pid, std::uint64_t creationTime100ns);

    // EnumerateProcesses 作用：
    // - 按策略返回当前系统进程列表（含基础性能计数器）；
    // - 不保证每条记录都含完整静态详情。
    // 参数 strategy：枚举策略。
    // 参数 actualStrategyOut：
    // - 可选输出参数；
    // - 返回实际执行的枚举策略（例如 Auto 可能回退到 Snapshot）。
    std::vector<ProcessRecord> EnumerateProcesses(
        ProcessEnumStrategy strategy,
        ProcessEnumStrategy* actualStrategyOut = nullptr);

    // EnumerateSystemThreads 作用：
    // - 枚举当前系统全部线程（优先 NtQuerySystemInformation）；
    // - 当 Nt 路径不可用时自动回退 Toolhelp 快照。
    // 参数 usedNtQueryOut：
    // - 可选输出参数；
    // - true 表示使用了 NtQuerySystemInformation，false 表示回退 Toolhelp。
    // 参数 diagnosticTextOut：
    // - 可选输出参数；
    // - 返回本轮枚举的路径说明或失败原因，便于 UI 诊断展示。
    std::vector<SystemThreadRecord> EnumerateSystemThreads(
        bool* usedNtQueryOut = nullptr,
        std::string* diagnosticTextOut = nullptr);

    // FillProcessStaticDetails 作用：
    // - 填充路径、命令行、用户、签名、管理员等“相对静态”字段；
    // - 通常只对“新出现进程”调用一次。
    // 参数 processRecord：目标记录（按引用修改）。
    // 参数 includeSignatureCheck：
    // - true：执行 WinVerifyTrust 做签名校验（较慢）；
    // - false：跳过签名校验，仅填充基础静态信息（快速模式）。
    // 返回值：true 成功；false 表示部分字段无法获取。
    bool FillProcessStaticDetails(ProcessRecord& processRecord, bool includeSignatureCheck = true);

    // RefreshProcessDynamicCounters 作用：
    // - 刷新 CPU 原始时间、RAM 工作集、IO 累计值等动态字段；
    // - 支持 Snapshot 路径下补齐性能计数器。
    // 参数 processRecord：目标记录（按引用修改）。
    // 返回值：true 成功；false 表示无法获取动态计数器。
    bool RefreshProcessDynamicCounters(ProcessRecord& processRecord);

    // QueryProcessProtectionLevelByPid 作用：
    // - 通过用户态 ProcessProtectionLevelInfo 查询目标进程 PPL 枚举；
    // - 该值为手动刷新字段，ProcessDock 不把它写入跨轮静态缓存。
    // 参数 pid：目标进程 PID。
    // 参数 levelOut：输出 PROTECTION_LEVEL_* 原始枚举值，可为空。
    // 参数 displayTextOut：输出可读文本，可为空。
    // 参数 errorMessageOut：失败原因，可为空。
    // 返回值：查询成功返回 true，失败返回 false。
    bool QueryProcessProtectionLevelByPid(
        std::uint32_t pid,
        std::uint32_t* levelOut,
        std::string* displayTextOut,
        std::string* errorMessageOut = nullptr);

    // UpdateDerivedCounters 作用：
    // - 根据“上一轮样本 + 当前原始计数器”计算 CPU%、DiskMB/s；
    // - GPU 由枚举阶段的 PDH GPU Engine 采样写入，本函数只保留该值；
    // - Net 当前默认保留为 0（后续可扩展）。
    // 参数 processRecord：目标记录（按引用写入衍生值）。
    // 参数 previousSample：上一轮样本（可空）。
    // 参数 nextSampleOut：输出下一轮样本。
    // 参数 logicalCpuCount：逻辑处理器数量（CPU 百分比折算用）。
    // 参数 currentTick100ns：当前采样时刻（100ns）。
    void UpdateDerivedCounters(
        ProcessRecord& processRecord,
        const CounterSample* previousSample,
        CounterSample& nextSampleOut,
        std::uint32_t logicalCpuCount,
        std::uint64_t currentTick100ns);

    // GetProcessNameByPID 作用：按 PID 读取进程名（失败返回空串）。
    std::string GetProcessNameByPID(std::uint32_t pid);

    // QueryProcessPathByPid 作用：按 PID 读取可执行路径（失败返回空串）。
    std::string QueryProcessPathByPid(std::uint32_t pid);

    // ExecuteTaskKill 作用：执行 taskkill 结束进程（可选 /F）。
    bool ExecuteTaskKill(std::uint32_t pid, bool forceKill, std::string* errorMessage);

    // TerminateProcessByWin32 作用：调用 TerminateProcess 结束进程。
    bool TerminateProcessByWin32(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByNtNative 作用：
    // - 调用 NtTerminateProcess / ZwTerminateProcess 结束进程。
    bool TerminateProcessByNtNative(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByWtsApi 作用：
    // - 调用 WTSTerminateProcess（WTS API）结束进程。
    bool TerminateProcessByWtsApi(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByWinStationApi 作用：
    // - 调用 WinStationTerminateProcess（winsta 接口）结束进程。
    bool TerminateProcessByWinStationApi(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByJobObject 作用：
    // - 创建临时 Job，把目标进程加入后调用 TerminateJobObject 结束。
    bool TerminateProcessByJobObject(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByNtJobObject 作用：
    // - 创建临时 Job，把目标进程加入后调用 NtTerminateJobObject 结束。
    bool TerminateProcessByNtJobObject(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByRestartManager 作用：
    // - 通过 Restart Manager 注册目标进程并调用 RmShutdown；
    // - forceShutdown=true 时使用强制关闭选项。
    bool TerminateProcessByRestartManager(
        std::uint32_t pid,
        bool forceShutdown,
        std::string* errorMessage);

    // TerminateProcessByDuplicateHandlePseudo 作用：
    // - 以 PROCESS_DUP_HANDLE 打开目标进程；
    // - 复制目标进程伪句柄(-1)到当前进程后调用 TerminateProcess。
    bool TerminateProcessByDuplicateHandlePseudo(std::uint32_t pid, std::string* errorMessage);

    // TerminateAllThreadsByPid 作用：枚举并 TerminateThread 结束该进程全部线程。
    bool TerminateAllThreadsByPid(std::uint32_t pid, std::string* errorMessage);

    // TerminateAllThreadsByPidNtNative 作用：
    // - 枚举目标进程全部线程并调用 NtTerminateThread / ZwTerminateThread。
    bool TerminateAllThreadsByPidNtNative(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByNtUnmapNtdll 作用：
    // - 以 PROCESS_VM_OPERATION 打开目标进程；
    // - 定位并调用 NtUnmapViewOfSection 卸载其 ntdll.dll 映射（高风险）。
    bool TerminateProcessByNtUnmapNtdll(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByDebugAttach 作用：
    // - 通过 DebugActiveProcess 附加调试目标；
    // - 可作为调试器攻击链路的一环（不保证立即终止）。
    bool TerminateProcessByDebugAttach(std::uint32_t pid, std::string* errorMessage);

    // TerminateProcessByNtsdCommand 作用：
    // - 调用 ntsd 命令行附加并立即退出（`ntsd -c q -p <pid>`）；
    // - 作为调试器攻击链路的外部工具方式。
    bool TerminateProcessByNtsdCommand(std::uint32_t pid, std::string* errorMessage);

    // InjectInvalidShellcode 作用：远程分配并执行无效 shellcode（实验性高风险操作）。
    bool InjectInvalidShellcode(std::uint32_t pid, std::string* errorMessage);

    // SuspendProcess 作用：暂停进程（NtSuspendProcess）。
    bool SuspendProcess(std::uint32_t pid, std::string* errorMessage);

    // ResumeProcess 作用：恢复进程（NtResumeProcess）。
    bool ResumeProcess(std::uint32_t pid, std::string* errorMessage);

    // SetProcessCriticalFlag 作用：设置/取消关键进程标记（NtSetInformationProcess）。
    bool SetProcessCriticalFlag(std::uint32_t pid, bool enableCritical, std::string* errorMessage);

    // SetProcessPriority 作用：设置进程优先级（SetPriorityClass）。
    bool SetProcessPriority(std::uint32_t pid, ProcessPriorityLevel priorityLevel, std::string* errorMessage);

    // SetProcessEfficiencyMode 作用：启用/关闭 Windows 进程效率模式（PowerThrottling）。
    bool SetProcessEfficiencyMode(std::uint32_t pid, bool enableEfficiencyMode, std::string* errorMessage);

    // OpenProcessFolder 作用：在资源管理器定位该进程路径所在文件。
    bool OpenProcessFolder(std::uint32_t pid, std::string* errorMessage);

    // QueryProcessStaticDetailByPid 作用：
    // - 以 PID 为入口，查询单个进程的静态详情快照；
    // - includeSignatureCheck=true 时执行签名校验，可能较慢；
    // - UI 开窗路径应传 false 或改用后台任务，避免阻塞事件循环。
    // 参数 pid：目标进程 PID。
    // 参数 outRecord：输出记录，失败时保留已获得字段。
    // 参数 includeSignatureCheck：是否同步执行 WinVerifyTrust 签名校验。
    // 返回值：基础静态详情读取成功返回 true，失败返回 false。
    bool QueryProcessStaticDetailByPid(
        std::uint32_t pid,
        ProcessRecord& outRecord,
        bool includeSignatureCheck = true);

    // EnumerateProcessModulesAndThreads 作用：
    // - 枚举目标进程模块 + 线程信息（供“模块”Tab 刷新）；
    // - includeSignatureCheck=true 时会额外做模块签名校验（较慢）。
    ProcessModuleSnapshot EnumerateProcessModulesAndThreads(
        std::uint32_t pid,
        bool includeSignatureCheck);

    // UnloadModuleByBaseAddress 作用：
    // - 对远程进程调用 FreeLibrary 卸载指定基址模块。
    bool UnloadModuleByBaseAddress(
        std::uint32_t pid,
        std::uint64_t moduleBaseAddress,
        std::string* errorMessage);

    // SuspendThreadById 作用：挂起指定线程。
    bool SuspendThreadById(std::uint32_t threadId, std::string* errorMessage);

    // ResumeThreadById 作用：恢复指定线程。
    bool ResumeThreadById(std::uint32_t threadId, std::string* errorMessage);

    // TerminateThreadById 作用：结束指定线程。
    bool TerminateThreadById(std::uint32_t threadId, std::string* errorMessage);

    // InjectDllByPath 作用：
    // - 把指定 DLL 路径注入到目标进程（LoadLibraryW 远程线程方案）。
    bool InjectDllByPath(std::uint32_t pid, const std::string& dllPath, std::string* errorMessage);

    // InjectShellcodeBuffer 作用：
    // - 把原始 shellcode 字节写入远程进程并创建远程线程执行。
    bool InjectShellcodeBuffer(
        std::uint32_t pid,
        const std::vector<std::uint8_t>& shellcodeBuffer,
        std::string* errorMessage);

    // OpenFolderByPath 作用：
    // - 在资源管理器中定位到目标路径（文件或目录）。
    bool OpenFolderByPath(const std::string& targetPath, std::string* errorMessage);

    // ApplyTokenPrivilegeEditsByPid 作用：
    // - 打开指定 PID 的进程令牌（可选 DuplicateTokenEx）；
    // - 按 edits 调整特权（AdjustTokenPrivileges）。
    bool ApplyTokenPrivilegeEditsByPid(
        std::uint32_t sourcePid,
        std::uint32_t tokenDesiredAccess,
        bool duplicatePrimaryToken,
        const std::vector<TokenPrivilegeEdit>& edits,
        std::string* errorMessage);

    // LaunchProcess 作用：
    // - 按 request 调用 CreateProcessW 或 Token 路径创建进程；
    // - 对外统一返回 CreateProcessResult。
    bool LaunchProcess(
        const CreateProcessRequest& request,
        CreateProcessResult* resultOut);
    std::wstring GetCurrentProcessPath();

}
