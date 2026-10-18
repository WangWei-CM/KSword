#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ks::ui
{
    // MemoryDebugThreadIdentity：进程和线程均以创建时间绑定，禁止把旧 TID 当作新线程。
    struct MemoryDebugThreadIdentity
    {
        std::uint32_t pid = 0; // 目标进程编号。
        std::uint64_t processCreateTime100ns = 0; // 目标进程创建时间。
        std::uint32_t tid = 0; // 目标线程编号。
        std::uint64_t threadCreateTime100ns = 0; // 目标线程创建时间。
    };

    // MemoryDebugThreadRecord：可跨工作线程传递的线程枚举值，不向界面暴露句柄。
    struct MemoryDebugThreadRecord
    {
        MemoryDebugThreadIdentity identity; // 已经用真实句柄验证的强身份。
        std::int32_t basePriority = 0; // Toolhelp 报告的基础优先级。
        bool ownedPause = false; // 本会话是否持有该线程的一次暂停。
    };

    // MemoryDebugThreadSnapshot：只读 AMD64 通用、控制和调试寄存器快照。
    struct MemoryDebugThreadSnapshot
    {
        MemoryDebugThreadIdentity identity; // 读取时验证的进程和线程身份。
        std::uint64_t rax = 0; // RAX 通用寄存器。
        std::uint64_t rbx = 0; // RBX 通用寄存器。
        std::uint64_t rcx = 0; // RCX 通用寄存器。
        std::uint64_t rdx = 0; // RDX 通用寄存器。
        std::uint64_t rsi = 0; // RSI 源索引寄存器。
        std::uint64_t rdi = 0; // RDI 目标索引寄存器。
        std::uint64_t rbp = 0; // RBP 基址寄存器。
        std::uint64_t rsp = 0; // RSP 栈指针。
        std::uint64_t r8 = 0; // R8 扩展通用寄存器。
        std::uint64_t r9 = 0; // R9 扩展通用寄存器。
        std::uint64_t r10 = 0; // R10 扩展通用寄存器。
        std::uint64_t r11 = 0; // R11 扩展通用寄存器。
        std::uint64_t r12 = 0; // R12 扩展通用寄存器。
        std::uint64_t r13 = 0; // R13 扩展通用寄存器。
        std::uint64_t r14 = 0; // R14 扩展通用寄存器。
        std::uint64_t r15 = 0; // R15 扩展通用寄存器。
        std::uint64_t rip = 0; // RIP 指令指针；不是调试停止事件。
        std::uint32_t rflags = 0; // Windows AMD64 CONTEXT 提供的 EFlags 位域。
        std::uint64_t dr0 = 0; // 只读 DR0 地址寄存器。
        std::uint64_t dr1 = 0; // 只读 DR1 地址寄存器。
        std::uint64_t dr2 = 0; // 只读 DR2 地址寄存器。
        std::uint64_t dr3 = 0; // 只读 DR3 地址寄存器。
        std::uint64_t dr6 = 0; // 只读 DR6 状态寄存器。
        std::uint64_t dr7 = 0; // 只读 DR7 控制寄存器。
        std::uint64_t capturedAt100ns = 0; // 系统时间表示的读取时刻。
        bool transientPause = false; // 为快照创建临时暂停；是否已恢复还须检查结果的 ownedSuspend。
    };

    // MemoryDebugThreadState：描述真实会话状态，恢复失败时不能显示成正在运行。
    enum class MemoryDebugThreadState
    {
        Closed,
        Ready,
        Paused,
        RecoveryRequired,
        TargetExited
    };

    // MemoryDebugThreadError：供 UI 翻译的稳定错误分类；Win32 原因另行保留。
    enum class MemoryDebugThreadError
    {
        None,
        InvalidIdentity,
        SelfTarget,
        UnsupportedArchitecture,
        OpenProcessFailed,
        ProcessIdentityMismatch,
        TargetExited,
        NotOpen,
        EnumerateFailed,
        OpenThreadFailed,
        ThreadIdentityMismatch,
        ThreadExited,
        AlreadyPaused,
        DifferentThreadPaused,
        ThreadAlreadySuspended,
        SuspendFailed,
        ContextFailed,
        ResumeFailed,
        NotPaused
    };

    // MemoryDebugThreadResult：每次调用独立返回值快照，success 不替代 ownedSuspend 检查。
    struct MemoryDebugThreadResult
    {
        bool success = false; // 该次请求是否完整完成。
        MemoryDebugThreadError error = MemoryDebugThreadError::None; // 稳定错误分类。
        std::uint32_t errorWin32 = 0; // 原始 Win32 错误；成功时为零。
        std::string detail; // 便于诊断的 API/身份原因，不包含目标敏感内容。
        MemoryDebugThreadState status = MemoryDebugThreadState::Closed; // 当前真实会话状态。
        std::uint32_t pid = 0; // 当前保留句柄对应的目标 PID。
        std::uint64_t processCreateTime100ns = 0; // 当前保留进程对象的创建时间。
        bool ownedSuspend = false; // 是否仍有一次必须配平的本会话暂停。
        MemoryDebugThreadIdentity selectedThread; // 自有暂停绑定的线程；未持有时为零。
        bool hasSnapshot = false; // 本次结果是否包含成功获取的上下文。
        MemoryDebugThreadSnapshot snapshot; // 只有 hasSnapshot 时有效。
        std::vector<MemoryDebugThreadRecord> threads; // 只有枚举调用填充。
        std::uint32_t inaccessibleThreads = 0; // 存在但无法取得可靠身份的线程数量。
    };

    // MemoryDebugThreadSession：无 Windows debug attach、注入、断点或上下文写入的线程控制。
    // 所有接口可在 worker 调用；实例内部串行化。一次只暂停一个线程，其余线程继续运行。
    // close 恢复失败时保留句柄以便重试；析构仅对自己的暂停尽力配平，不能保证进程崩溃恢复。
    class MemoryDebugThreadSession final
    {
    public:
        MemoryDebugThreadSession(); // 创建关闭状态的会话，不访问任何目标。
        ~MemoryDebugThreadSession(); // 尽力恢复自有暂停并释放句柄。
        MemoryDebugThreadSession(const MemoryDebugThreadSession&) = delete;
        MemoryDebugThreadSession& operator=(const MemoryDebugThreadSession&) = delete;

        // open：绑定 PID+创建时间；拒绝自身与非原生 AMD64，不改变已有会话。
        MemoryDebugThreadResult open(std::uint32_t pid, std::uint64_t processCreateTime100ns);
        // enumerateThreads：枚举并复核每个线程的 owner 与创建时间，返回可控强身份。
        MemoryDebugThreadResult enumerateThreads();
        // pause：仅增加一次线程暂停；已有外部暂停时立即配平并拒绝持有。
        MemoryDebugThreadResult pause(const MemoryDebugThreadIdentity& identity);
        // querySnapshot：运行线程临时暂停后读取再恢复；已由本会话暂停者保持暂停。
        MemoryDebugThreadResult querySnapshot(const MemoryDebugThreadIdentity& identity);
        // resume：只配平自己的一次暂停；失败后保留身份、句柄与自有暂停状态。
        MemoryDebugThreadResult resume();
        // close：先配平自有暂停再关闭；恢复失败时保持 RecoveryRequired 以便重试。
        MemoryDebugThreadResult close();
        // status：不暂停目标，只核对已持有进程和线程是否已经退出并返回状态。
        MemoryDebugThreadResult status();

    private:
        struct Impl; // Win32 句柄、互斥锁和内部恢复状态。
        std::unique_ptr<Impl> impl_; // 独占实现，防止 UI 接触原生句柄。
    };
}
