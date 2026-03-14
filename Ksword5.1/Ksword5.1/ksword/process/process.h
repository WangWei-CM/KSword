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
        std::string signatureState;        // 数字签名状态（Signed/Unsigned/Unknown）。
        std::string startTimeText;         // 启动时间文本（YYYY-MM-DD HH:MM:SS）。
        std::string architectureText;      // 架构文本（x64/x86/ARM/Unknown）。
        std::string priorityText;          // 优先级文本（Normal/High/...）。
        bool isAdmin = false;              // 进程令牌是否已提升（管理员权限）。

        // ======== 原始性能计数器（用于相邻两轮差值计算） ========
        std::uint64_t rawCpuTime100ns = 0;      // Kernel + User 总 CPU 时间（100ns）。
        std::uint64_t rawWorkingSetBytes = 0;   // 工作集内存字节数。
        std::uint64_t rawIoBytes = 0;           // Read/Write/Other 传输累计字节。

        // ======== UI 直接显示的衍生性能数据 ========
        double cpuPercent = 0.0;           // CPU 百分比（0~100 * 逻辑核折算）。
        double ramMB = 0.0;                // RAM（MB）。
        double diskMBps = 0.0;             // 磁盘吞吐（MB/s）。
        double gpuPercent = 0.0;           // GPU（当前预留，默认 0）。
        double netKBps = 0.0;              // 网络（当前预留，默认 0）。

        bool staticDetailsReady = false;   // true 表示详情字段已完整填充。
        bool dynamicCountersReady = false; // true 表示性能计数器可用。
    };

    // ProcessModuleRecord：进程模块列表中的单行数据。
    struct ProcessModuleRecord
    {
        std::string modulePath;                // 模块完整路径。
        std::string moduleName;                // 模块名（文件名）。
        std::uint64_t moduleBaseAddress = 0;   // 模块加载基址。
        std::uint32_t moduleSizeBytes = 0;     // 模块映像大小（字节）。
        std::string signatureState;            // 模块签名状态（Signed/Unsigned/Unknown）。
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

    // ProcessModuleSnapshot：模块页签所需快照数据（模块 + 线程）。
    struct ProcessModuleSnapshot
    {
        std::vector<ProcessModuleRecord> modules; // 模块列表。
        std::vector<ProcessThreadRecord> threads; // 线程列表。
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

    // UpdateDerivedCounters 作用：
    // - 根据“上一轮样本 + 当前原始计数器”计算 CPU%、DiskMB/s；
    // - GPU/Net 当前默认保留为 0（后续可扩展）。
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

    // TerminateAllThreadsByPid 作用：枚举并 TerminateThread 结束该进程全部线程。
    bool TerminateAllThreadsByPid(std::uint32_t pid, std::string* errorMessage);

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

    // OpenProcessFolder 作用：在资源管理器定位该进程路径所在文件。
    bool OpenProcessFolder(std::uint32_t pid, std::string* errorMessage);

    // QueryProcessStaticDetailByPid 作用：
    // - 以 PID 为入口，查询单个进程的静态详情快照；
    // - 失败时返回 false，outRecord 保留已获得字段。
    bool QueryProcessStaticDetailByPid(std::uint32_t pid, ProcessRecord& outRecord);

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
}
