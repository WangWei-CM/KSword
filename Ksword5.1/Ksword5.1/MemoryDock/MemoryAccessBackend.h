#pragma once

// ============================================================
// MemoryAccessBackend.h
// 作用：
// - 为"内存"页的所有读写入口提供统一的后端选择层；
// - 标准后端走驱动既有的 MmCopyMemory / MmMapIoSpaceEx 通道；
// - DDMA 后端走磁盘控制器的总线主控 DMA，绕开 CPU 页表与 SLAT/EPT。
//
// 为什么集中在这一个文件：
// - 内存搜索、内存查看器、驱动内存读写、系统内存审计四个页面都要能切后端。
//   如果每个页各判一遍"DDMA 现在能不能用"，判据一定会走散——只要有一处漏判
//   暂存扇区确认或内核调试状态，用户就会在那一处撞上蓝屏或脏扇区。
//   因此可用性判据、切片规则、错误文案只在这里有一份实现。
//
// 虚拟地址通道是怎么做的：
// - DDMA 本身只能按物理地址读写，所以虚拟地址请求会逐页调用 R0 既有的
//   VA → PA 翻译（IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS），拿到物理页
//   之后再做 DMA。翻译后端是复用的，本文件不实现任何页表解析。
// ============================================================

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <string>
#include <functional>
#include <mutex>
#include <vector>

namespace ksword::memory_backend
{
    // MemoryAccessBackend：
    // - 标识一次内存访问走哪条通道；
    // - 枚举值顺序必须与各页面"访问后端"下拉框的条目顺序一致，界面按索引转换。
    enum class MemoryAccessBackend : int
    {
        UserMode = 0,       // R3：ReadProcessMemory / WriteProcessMemory，不经驱动。
        StandardDriver,     // R0：驱动通道 MmCopyVirtualMemory / MmMapIoSpaceEx。
        Hvm,                // R0 私有页表窗口：改写自有页表项指向目标帧，不调内存管理器。
        Ddma                // 磁盘直接内存访问：ATA / SCSI PASS_THROUGH_DIRECT + DMA。
    };

    // HVM 这一条到底独立在哪：**不是**"绕过 EPT"。
    //
    // 名字叫 ring -1，但实现（KswordARKDriver/src/features/hvm/hvm_memory.c）在
    // PASSIVE_LEVEL 的驱动上下文里跑，不进 VMX root。它预留一个私有页、把该页的
    // 页表项改写成指向目标帧、访问完再还原——所以它**照样受 SLAT / EPT 约束**，
    // 与 R3 / R0 同组。
    //
    // 它的独立性是另一回事：整条路径不调用任何文档化的内存管理器例程
    // （MmCopyMemory / MmCopyVirtualMemory），因此**别的驱动挂钩那些例程挂不到
    // 它头上**。R0 与 HVM 对同一地址给出不同答案，说明内存管理器被挂了钩；
    // 这跟 DDMA 与 CPU 侧分歧说明的 SLAT 重定向是两件事，不能混为一谈。
    //
    // 一个必须传递给调用方的状态：自映射基址发现失败时（窗口落进大页映射、
    // 布局不认识），驱动会**回退到 MmCopyMemory** 并把 usedDirectWindow 置 0。
    // 那一次读走的正是我们想避开的那条路，拿它去跟 R0 比对什么都证明不了。

    // 为什么 R3 与 R0 必须是两个并列的选项，而不是"标准通道"一个条目：
    // - 两者的失败面完全不同。R3 受句柄权限、进程保护、VAD 可读性约束；R0 走
    //   MmCopyVirtualMemory，绕开前两项但仍受页表约束。同一个地址一条读得到
    //   另一条读不到，本身就是判据——合成一个条目就把这个判据抹掉了。
    // - 原先查看器那条"标准驱动通道"名义上是 R0，实现却是 ReadProcessMemory，
    //   而同一个枚举值在另外三个页面确实走驱动。名字与行为不一致时，用户读到
    //   的失败原因就指不到真因。

    // DdmaSession：
    // - DDMA 的一次配置，由"DDMA"子页产出，四个页面共用同一份；
    // - 只有全部字段都就绪才允许发起 DDMA 访问，判据见 isDdmaUsable。
    struct DdmaSession
    {
        bool configured = false;            // 已经完成过一次成功的能力探测。
        std::uint32_t diskIndex = 0;        // 目标磁盘在 \Driver\Disk 设备列表中的下标。
        std::wstring deviceName;            // 探测时记下的设备名，用于事后核对用的是同一块盘。
        std::uint64_t scratchLba = 0;       // 暂存扇区起始 LBA。
        bool scratchLbaValid = false;       // 用户显式填写过 LBA。注意 LBA 0 合法，不能用 0 当哨兵。
        bool scratchAcknowledged = false;   // 用户确认这几个扇区可以被临时覆盖。
        bool kernelDebuggerEnabled = false; // 本机开着内核调试：这种机器上 DDMA 会蓝屏。
        std::uint32_t transferBytes = 0;    // R0 自报的一次 DMA 传输长度，正常为一页。
        std::uint32_t scratchSectorCount = 0; // R0 自报的暂存区占用扇区数。
    };

    // AccessOutcome：
    // - 一次读或写的统一结果模型；
    // - ok 为真才代表数据可用或写入完成，其余字段用于状态栏与告警展示。
    struct AccessOutcome
    {
        bool ok = false;                    // 整体成功。
        QString failureText;                // 失败原因，已经是可直接展示给用户的文案。
        QByteArray data;                    // 读取结果；写入路径为空。
        std::uint64_t bytesDone = 0;        // 实际完成的字节数，失败时表示失败前已完成的量。
        bool forceRequired = false;         // R0 要求附加强制标志，调用方应征求用户同意后重试。

        // scratchDirty：暂存扇区没能还原，磁盘上留下了脏扇区。
        // 这是唯一一个"操作成功也必须告警"的状态，不能只在失败分支里看。
        bool scratchDirty = false;
        // lostUpdateWindow：写入走了 read-modify-write，同页其它字节存在
        // 4KB 粒度的覆盖窗口。
        bool lostUpdateWindow = false;
        // partial：只完成了一部分；读取时 data 长度会小于请求长度。
        bool partial = false;
    };

    // ========================================================
    // 进程级 DDMA 会话（"常驻虚扇区"）
    // ========================================================
    //
    // 为什么是进程级而不是挂在 DDMA 页上：
    // - "常驻"的含义就是这份配置在整个程序里有效，而不是某个页面的私有状态；
    // - 右上角的权限指示灯由 MainWindow 拥有，内存页的四个后端下拉由 MemoryDock
    //   拥有，两边都要读同一份事实。让它们各自去"问对方那个控件"，就会在 Dock
    //   布局恢复顺序、页面尚未构建等时机上拿到不一致的答案。
    //
    // 写入者只有一个：DDMA 子页。其余各处一律只读。

    // currentDdmaSession：
    // - 作用：读取当前进程级 DDMA 会话；
    // - 返回：常量引用；从未配置过时返回一个"未配置"的空会话。
    const DdmaSession& currentDdmaSession();

    // setCurrentDdmaSession：
    // - 作用：由 DDMA 子页写入会话配置；
    // - 参数 session：新的会话；
    // - 说明：这是唯一的写入入口，其它页面不得调用。
    void setCurrentDdmaSession(const DdmaSession& session);

    // ddmaSessionGeneration：
    // - 作用：返回会话代次，每次写入自增；
    // - 说明：轮询方（右上角指示灯）用它判断"这一轮要不要重画"，省掉逐字段比较。
    std::uint64_t ddmaSessionGeneration();

    // Thread-safe frozen session for background consumers. The legacy reference
    // accessor remains GUI-thread only; no worker may retain that reference.
    struct DdmaSessionSnapshot
    {
        DdmaSession session;
        std::uint64_t generation = 0;
    };
    DdmaSessionSnapshot captureDdmaSession();

    // Failure history belongs to the process, independent of any requesting
    // page. This is an audit receipt, not a new channel-lock/reset policy.
    struct DdmaScratchFailure
    {
        DdmaSession session;
        std::uint64_t physicalAddress = 0;
        std::uint64_t bytesDone = 0;
        bool write = false;
        QString failureText;
    };
    std::vector<DdmaScratchFailure> captureDdmaScratchFailures();


    // One process-wide gate covers complete multi-call transactions and all
    // synchronous facade callers. Recursive only for facade calls inside a task.
    std::recursive_mutex& ddmaTransactionMutex();
    // Accepted tasks are owned by the process queue and finish even if their
    // requesting page closes. A full queue rejects before accepting any work.
    bool submitDdmaTransaction(std::function<void()> transaction);

    // backendDisplayName：
    // - 输入：后端枚举；
    // - 返回：下拉框与状态栏使用的短名称。
    QString backendDisplayName(MemoryAccessBackend backend);

    // ddmaTransferBytes：
    // - 作用：返回 DDMA 一次 DMA 传输的字节数，也就是切片与复核的粒度；
    // - 说明：调用方按这个值对齐地址和长度即可，不必为了一个常量把驱动协议
    //   头文件拉进自己的编译单元。
    std::uint32_t ddmaTransferBytes();

    // isDdmaUsable：
    // - 输入：当前 DDMA 会话配置；
    // - 处理：按"已探测 → 未开内核调试 → 已填 LBA → 已确认覆盖"的顺序逐条检查，
    //   每一条都给出可直接展示的中文原因；
    // - 返回：true 表示可以发起 DDMA 访问；false 时 reasonOut 说明缺哪一步。
    bool isDdmaUsable(const DdmaSession& session, QString* reasonOut);

    // isHvmMemoryUsable：
    // - 处理：发一次 QUERY_WINDOW，它不碰任何内存，只回答私有窗口在不在；
    // - 返回：true 表示可以走 HVM 通道；false 时 reasonOut 说明卡在哪一步。
    // - 注意返回 true 只代表"这条通道能用"，不代表每一次访问都会走私有窗口——
    //   窗口没标定出来时驱动会回退，那一次的 usedDirectWindow 为 0。
    bool isHvmMemoryUsable(QString* reasonOut);

    // readPhysical：
    // - 输入：后端、DDMA 会话、物理起始地址与长度、是否要求 HVM 私有窗口；
    // - 处理：标准后端一次最多 64KB 直接调驱动；DDMA 后端按页切片逐页 DMA；
    //   requireHvmDirectWindow 仅影响 HVM：逐片要求并核验 usedDirectWindow，
    //   拒绝回退分片且不改走 R0，仅保留此前已确认的直接读取前缀。
    //   默认 false 保留旧调用允许回退并报告告警的行为。
    // - 返回：AccessOutcome，data 为读回字节。
    AccessOutcome readPhysical(
        MemoryAccessBackend backend,
        const DdmaSession& session,
        std::uint64_t physicalAddress,
        std::uint64_t lengthBytes,
        bool requireHvmDirectWindow = false);

    // writePhysical：
    // - 输入：后端、DDMA 会话、物理起始地址、待写字节、是否已获得强制写入同意；
    // - 处理：两个后端都按各自的单次上限切片；DDMA 非整页写会触发 R0 的
    //   read-modify-write，结果里的 lostUpdateWindow 会如实上报；
    // - 返回：AccessOutcome；forceRequired 为真时调用方应弹确认后带 force 重试。
    AccessOutcome writePhysical(
        MemoryAccessBackend backend,
        const DdmaSession& session,
        std::uint64_t physicalAddress,
        const QByteArray& bytes,
        bool forceApproved);

    // readVirtual：
    // - 输入：后端、DDMA 会话、目标 PID（0 表示内核地址空间）、虚拟地址与长度、
    //   是否要求 HVM 私有窗口（仅影响 HVM，默认 false）；
    // - 处理：标准后端直接调 R0 虚拟读；DDMA 后端逐页走 VA → PA 翻译再 DMA，
    //   翻译不出物理页时停止并返回失败，不用零值伪造不可读内容；
    //   HVM 严格读取逐片要求并核验 usedDirectWindow；回退分片即使报告成功也
    //   不采纳其数据，且不自动改走 R0。
    // - 返回：AccessOutcome，DDMA 或 HVM 严格读取失败时 data 仅保留真实读取的前缀，
    //   bytesDone 表示完成数量，partial 表示已完成部分。
    AccessOutcome readVirtual(
        MemoryAccessBackend backend,
        const DdmaSession& session,
        std::uint32_t processId,
        std::uint64_t virtualAddress,
        std::uint64_t lengthBytes,
        bool requireHvmDirectWindow = false);

    // writeVirtual：
    // - 输入：后端、DDMA 会话、目标 PID（0 表示内核地址空间）、虚拟地址、待写
    //   字节、是否已获得强制写入同意；
    // - 处理：DDMA 后端逐页翻译后 DMA 写；任何一页翻译不出物理地址都直接失败，
    //   不做"跳过这一页继续写下一页"的静默降级；
    // - 返回：AccessOutcome。
    AccessOutcome writeVirtual(
        MemoryAccessBackend backend,
        const DdmaSession& session,
        std::uint32_t processId,
        std::uint64_t virtualAddress,
        const QByteArray& bytes,
        bool forceApproved);

    // isKernelVirtualAddress：
    // - 输入：虚拟地址；
    // - 返回：是否落在 x64 的内核高半区。两个后端都用这一个判据决定是否给
    //   R0 请求附加 KERNEL_ADDRESS 标志，避免各页各写一遍阈值。
    bool isKernelVirtualAddress(std::uint64_t virtualAddress);
}
