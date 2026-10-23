#pragma once

// ============================================================
// RxpfBackend.h
// 作用：
// - RXPF（RX Page Fault）模拟执行实验的后端接口；
// - 驱动侧通过影子 IDT 接管 #PF 向量，对被标记为 NX
//   的内核映像页进行指令模拟，从而在 HVCI 环境下
//   执行被修改的代码页——无需恢复 RX 权限。
// - 本文件只定义数据结构与函数声明，不含 QWidget 依赖；
//   UI 由 RxpfPage 负责。
// - IOCTL 通信通过 ArkDriverClient 完成，
//   协议结构定义在 shared/driver/KswordArkRxPfIoctl.h。
// ============================================================

#include <QString>
#include <QByteArray>

#include <cstdint>
#include <vector>

namespace ks::misc::rxpf
{
    // ----------------------------------------------------------
    // 确认令牌，与 R0 协议的 KSWORD_ARK_RXPF_CONFIRMATION_TOKEN 一致。
    // ----------------------------------------------------------
    inline constexpr unsigned long kConfirmationToken = 0x46505852UL;

    // ----------------------------------------------------------
    // 目标类型，与 R0 协议的 KSWORD_ARK_RXPF_TARGET_* 一致。
    // ----------------------------------------------------------

    // kTargetAllocatedTest：驱动分配的测试页（最安全的自检目标）。
    inline constexpr unsigned long kTargetAllocatedTest = 1UL;

    // kTargetSelfImageTest：驱动自身映像中的只读页。
    inline constexpr unsigned long kTargetSelfImageTest = 2UL;

    // kTargetExternalImage：外部内核映像中的页（如 CI.dll .text）。
    inline constexpr unsigned long kTargetExternalImage = 3UL;

    // ----------------------------------------------------------
    // 请求标志位，与 R0 协议的 KSWORD_ARK_RXPF_FLAG_* 一致。
    // ----------------------------------------------------------

    // kFlagUiConfirmed：表明操作已经过用户确认。
    inline constexpr unsigned long kFlagUiConfirmed = 0x00000001UL;

    // kFlagCaptureBackup：请求 R0 在变更前备份原始字节。
    inline constexpr unsigned long kFlagCaptureBackup = 0x00000002UL;

    // kFlagSkipBackupRestore：Unregister 专用；令 R0 跳过 ReleaseRecord
    // 自动把 Backup 恢复到物理页。R3 写入的字节保留到 PTE 回 RX 之后。
    // 调用方必须自己负责在 PatchGuard 扫描前把字节改回去（否则必蓝屏）。
    inline constexpr unsigned long kFlagSkipBackupRestore = 0x00000004UL;

    // ----------------------------------------------------------
    // SupportInfo：RXPF 子系统支持状态查询结果。
    // ----------------------------------------------------------
    struct SupportInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        unsigned long supportFlags = 0;     // 位掩码：各子系统就绪状态。
        unsigned long buildStatus = 0;      // 构建/ABI 匹配状态枚举。
        unsigned long ntBuildNumber = 0;    // 当前 NT 内核构建号。
        unsigned long imageTimeDateStamp = 0; // ntoskrnl 映像时间戳。
        unsigned long imageSize = 0;        // ntoskrnl 映像大小。
        unsigned long imageCheckSum = 0;    // ntoskrnl 映像校验和。
        unsigned long functionRva = 0;      // MmChangeImageProtection 的 RVA。
        unsigned long functionParameterCount = 0; // 函数参数个数。
        unsigned long processorCount = 0;   // 活动处理器数量。
        unsigned long pdbAge = 0;           // PDB 年龄。
        long lastStatus = 0;               // 最近一次操作的 NTSTATUS。
    };

    // ----------------------------------------------------------
    // PageInfo：注册/查询/变更/写入/模拟/注销操作的统一响应。
    // ----------------------------------------------------------
    struct PageInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        unsigned long state = 0;            // 页面当前状态枚举。
        unsigned long targetKind = 0;       // 目标类型。
        unsigned long flags = 0;            // 页面标志位。
        unsigned long generation = 0;       // 页面记录代次。
        unsigned long referenceCount = 0;   // 引用计数。
        unsigned long emulationEnabled = 0; // 模拟是否启用。
        std::uint64_t recordId = 0;         // 驱动分配的记录 ID。
        std::uint64_t pageBase = 0;         // 页面基址。
        std::uint64_t writableAlias = 0;    // 可写别名映射地址。
        std::uint64_t pfn = 0;             // 物理页号。
        std::uint64_t ownerImageBase = 0;   // 所属映像基址。
        std::uint64_t faultCount = 0;       // 累计 #PF 次数。
        std::uint64_t emulatedCount = 0;    // 成功模拟的指令次数。
        std::uint64_t unsupportedCount = 0; // 不支持的指令次数。
        long lastStatus = 0;               // 最近一次操作的 NTSTATUS。
        unsigned long lastFailureReason = 0; // 最近一次失败的原因枚举。
        unsigned long originalProtection = 0;  // 原始保护属性。
        unsigned long currentProtection = 0;   // 当前保护属性。
        unsigned long writableAliasProtection = 0; // 别名实际保护（PAGE_READWRITE=可写）。
        std::uint64_t requestedWriteLength = 0; // R3 调用者请求的长度。
        unsigned long writtenLength = 0;    // R0 本次确认写入的长度；失败时为零。
    };

    // ----------------------------------------------------------
    // StatsInfo：RXPF 全局统计信息。
    // ----------------------------------------------------------
    struct StatsInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        unsigned long generation = 0;       // 全局代次。
        unsigned long registeredPages = 0;  // 已注册页面数。
        unsigned long enabledPages = 0;     // 已启用模拟的页面数。
        unsigned long idtInstalled = 0;     // 影子 IDT 是否已安装。
        unsigned long processorCount = 0;   // 处理器数量。
        unsigned long activeHandlers = 0;   // 当前活跃的 #PF 处理器数。
        std::uint64_t totalFaults = 0;      // 累计 #PF 总次数。
        std::uint64_t managedFaults = 0;    // 被管理（命中注册页）的次数。
        std::uint64_t emulatedInstructions = 0; // 成功模拟的指令总次数。
        std::uint64_t chainedFaults = 0;    // 转发给原始处理器的次数。
        std::uint64_t recursiveFaults = 0;  // 递归 #PF 次数。
        std::uint64_t unsupportedInstructions = 0; // 不支持指令总次数。
        std::uint64_t droppedEvents = 0;    // 丢弃的事件数。
        long lastStatus = 0;               // 最近一次操作的 NTSTATUS。
    };

    // ----------------------------------------------------------
    // EventRow：一条 #PF 模拟事件记录。
    // ----------------------------------------------------------
    struct EventRow
    {
        std::uint64_t sequence = 0;         // 全局序列号。
        std::uint64_t timestamp = 0;        // 时间戳（KeQueryPerformanceCounter）。
        std::uint64_t cr2 = 0;             // 触发 #PF 的线性地址。
        std::uint64_t rip = 0;             // 触发时的 RIP。
        std::uint64_t errorCode = 0;        // #PF 错误码。
        std::uint64_t recordId = 0;         // 所属的页面记录 ID。
        std::uint64_t newRip = 0;           // 模拟后的新 RIP。
        unsigned long processorIndex = 0;   // 处理器编号。
        unsigned long decodedInstruction = 0; // 解码出的指令类型枚举。
        unsigned long emulationResult = 0;  // 模拟结果枚举。
        long status = 0;                   // 操作 NTSTATUS。
    };

    // ----------------------------------------------------------
    // EventsInfo：事件拉取结果。
    // ----------------------------------------------------------
    struct EventsInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        unsigned long returnedRows = 0;     // 本次返回的行数。
        unsigned long availableRows = 0;    // 驱动中仍可用的行数。
        std::uint64_t newestSequence = 0;   // 最新的序列号。
        std::uint64_t droppedRows = 0;      // 已丢弃的行数。
        std::vector<EventRow> rows;         // 事件行数组。
    };

    // ----------------------------------------------------------
    // SelfTestInfo：端到端自检结果。
    // ----------------------------------------------------------
    struct SelfTestInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        unsigned long result = 0;           // 自检结果（0=失败，1=通过）。
        unsigned long instructionCount = 0; // 模拟的指令数。
        std::uint64_t recordId = 0;         // 自检使用的记录 ID。
        std::uint64_t returnedValue = 0;    // 自检函数的返回值。
        std::uint64_t expectedValue = 0;    // 期望的返回值。
        std::uint64_t faultsObserved = 0;   // 观察到的 #PF 次数。
        long lastStatus = 0;               // 操作 NTSTATUS。
    };

    // ----------------------------------------------------------
    // 钩点动作，与 R0 协议的 KSWORD_ARK_RXPF_HOOK_ACTION_* 一致。
    // ----------------------------------------------------------

    // kHookActionNone：清除钩点。
    inline constexpr unsigned long kHookActionNone = 0UL;

    // kHookActionRedirect：执行到钩点地址时重定向 RIP。
    inline constexpr unsigned long kHookActionRedirect = 1UL;

    // kHookActionLog：执行到钩点地址时仅记录事件，然后正常模拟。
    inline constexpr unsigned long kHookActionLog = 2UL;

    // ----------------------------------------------------------
    // BackupInfo：Phase G 读 Record->Backup 子段的响应。
    // ----------------------------------------------------------
    struct BackupInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        std::uint64_t recordId = 0;         // 回显请求的记录 ID。
        unsigned long offset = 0;           // 回显请求的页内偏移。
        unsigned long length = 0;           // 回显请求的字节数。
        bool hasBackup = false;             // Record->Backup 是否存在（Register 带 CAPTURE_BACKUP）。
        long lastStatus = 0;                // 最近一次操作的 NTSTATUS。
        QByteArray bytes;                   // 从 Backup 读到的字节；length==0 或 hasBackup==false 时为空。
    };

    // ReadbackInfo：经现有只读内核内存接口读取恢复范围，不改变页面保护。
    struct ReadbackInfo
    {
        bool ok = false;                    // 所请求字节均已读取。
        QString errorText;                  // 传输或读取失败原因。
        QByteArray bytes;                   // 完整回读数据，不接受补零/部分读取。
    };

    // ----------------------------------------------------------
    // HookPointInfo：钩点设置操作的响应。
    // ----------------------------------------------------------
    struct HookPointInfo
    {
        bool ioSuccess = false;             // IOCTL 是否成功返回。
        QString ioErrorText;                // IOCTL 失败时的错误描述。
        std::uint64_t recordId = 0;         // 页面记录 ID。
        std::uint64_t hookAddress = 0;      // 钩点拦截地址。
        unsigned long hookAction = 0;       // 当前钩点动作。
        unsigned long state = 0;            // 页面状态。
        std::uint64_t redirectTarget = 0;   // 重定向目标 RIP。
        std::uint64_t redirectCount = 0;    // 累计重定向次数。
        long lastStatus = 0;               // 最近一次操作的 NTSTATUS。
    };

    // ----------------------------------------------------------
    // IOCTL 包装函数
    // ----------------------------------------------------------

    // querySupport：查询 RXPF 子系统的支持状态与能力。
    SupportInfo querySupport();

    // registerPage：注册一个内核页面用于 #PF 模拟。
    // - targetKind：目标类型（kTargetAllocatedTest 等）。
    // - targetAddress：目标虚拟地址（外部映像时需要指定）。
    PageInfo registerPage(unsigned long targetKind, std::uint64_t targetAddress);

    // changePage：将已注册的页面保护从 RX 改为 RW/NX。
    // - recordId：注册时返回的记录 ID。
    PageInfo changePage(std::uint64_t recordId);

    // queryPage：查询已注册页面的当前状态与计数器。
    // - recordId：注册时返回的记录 ID。
    PageInfo queryPage(std::uint64_t recordId);

    // writePage：向已注册页面的可写别名写入数据。
    // - recordId：注册时返回的记录 ID。
    // - offset：页内偏移（0 ~ PAGE_SIZE-1）。
    // - bytes：要写入的字节数组（最多 64 字节）。
    PageInfo writePage(std::uint64_t recordId, unsigned long offset,
        const QByteArray& bytes);

    // setEmulation：启用或禁用已注册页面的指令模拟。
    // - recordId：注册时返回的记录 ID。
    // - enable：true=启用，false=禁用。
    PageInfo setEmulation(std::uint64_t recordId, bool enable);

    // queryStats：查询 RXPF 全局统计信息。
    StatsInfo queryStats();

    // drainEvents：拉取模拟事件环形缓冲区中的记录。
    // - afterSequence：只返回序列号大于此值的事件。
    // - maxRows：最多返回多少行（上限 64）。
    EventsInfo drainEvents(std::uint64_t afterSequence, unsigned long maxRows);

    // unregisterPage：注销已注册的页面并恢复原始保护。
    // - recordId：注册时返回的记录 ID。
    // - skipBackupRestore：true 时附带 kFlagSkipBackupRestore，令 R0 不自动
    //   把 Backup 还原到物理页；R3 写入字节保留到 PTE 回 RX（Phase G 路径，
    //   有 BSOD 风险，调用方必须尽快改回原字节）。默认 false 走安全路径。
    PageInfo unregisterPage(std::uint64_t recordId,
        bool skipBackupRestore = false);

    // runSelfTest：运行驱动内置的端到端自检。
    // - recordId：必须是已注册且处于 RW_NX 状态的 SelfImageTest/AllocatedTest 记录。
    SelfTestInfo runSelfTest(std::uint64_t recordId);

    // queryBackup：读 Record->Backup 的子段。供 KernelHookWizard 在
    // Unregister(SKIP_RESTORE) 之前把原字节缓存到 R3 作恢复用。
    // - recordId：注册时返回的记录 ID；必须是带 kFlagCaptureBackup 注册的记录。
    // - offset：页内偏移（0 ~ PAGE_SIZE-1）。
    // - length：读取字节数（1 ~ 64）。
    // 返回值：hasBackup=false 表示 Record 未带 Backup（没传 CAPTURE_BACKUP），
    //         或 recordId 已失效；bytes 空；lastStatus=STATUS_NOT_FOUND。
    BackupInfo queryBackup(std::uint64_t recordId,
        unsigned long offset, unsigned long length);

    // readPageBytes：只读校验已恢复的页内范围，length 必须在 1..64 内。
    ReadbackInfo readPageBytes(std::uint64_t targetAddress,
        unsigned long offset, unsigned long length);

    // atomicCommit：原子提交——单次 IOCTL 完成 Register+Change(mode=2)+Write+
    // Unregister(SKIP_BACKUP_RESTORE)。减少往返不提供全核隔离或无执行故障保证。
    // 仅支持 EXTERNAL_IMAGE 目标。返回完整 PageInfo（包含 mode=1 恢复后的状态）。
    // - targetAddress：目标页面虚拟地址。
    // - offset：页内偏移（0 ~ PAGE_SIZE-1）。
    // - bytes：要写入的字节数组（最多 64 字节）。
    PageInfo atomicCommit(std::uint64_t targetAddress,
        unsigned long offset, const QByteArray& bytes);

    // setHookPoint：设置或清除已注册页面上的钩点。
    // - recordId：注册时返回的记录 ID。
    // - hookAddress：页内拦截地址（清除时传 0）。
    // - hookAction：动作类型（kHookActionNone/Redirect/Log）。
    // - redirectTarget：重定向目标地址（仅 REDIRECT 时需要）。
    // - confirmed：调用方必须在用户明确确认后传 true。
    HookPointInfo setHookPoint(std::uint64_t recordId,
        std::uint64_t hookAddress, unsigned long hookAction,
        std::uint64_t redirectTarget, bool confirmed);

    // ----------------------------------------------------------
    // 文本格式化辅助函数
    // ----------------------------------------------------------

    // supportFlagsText：将支持标志位掩码转为可读文本。
    QString supportFlagsText(unsigned long flags);

    // buildStatusText：将构建匹配状态枚举转为可读文本。
    QString buildStatusText(unsigned long status);

    // pageStateText：将页面状态枚举转为可读文本。
    QString pageStateText(unsigned long state);

    // decodedInstructionText：将解码指令类型枚举转为可读文本。
    QString decodedInstructionText(unsigned long instruction);

    // emulationResultText：将模拟结果枚举转为可读文本。
    QString emulationResultText(unsigned long result);

    // hookActionText：将钩点动作枚举转为可读文本。
    QString hookActionText(unsigned long action);

} // namespace ks::misc::rxpf
