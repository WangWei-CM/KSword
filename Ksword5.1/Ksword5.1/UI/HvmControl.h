#pragma once

// HvmControl：KSwordVM（R-1 / hypervisor 层）能力门面。
//
// 存在的理由：
// - 标题栏权限按钮排里的 HVM 按钮、KernelDock 的 HVM 页、以及后续的 EPT 内存
//   保护 / 隐蔽 Hook / 内存隐藏都要读同一份状态、走同一套确认与写权限门。
//   把它们收在一个门面里，避免每个调用点各自拼 IOCTL 参数和各自判断可用性。
// - 所有查询都是同步阻塞调用（IOCTL），调用方必须放到后台线程；
//   SOAK 更是会占用驱动侧状态锁数十秒，绝不能在 UI 线程调用。

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

#include "../ArkDriverClient/ArkDriverClient.h"

namespace ksword::hvm
{
    // HvmAvailability：HVM 为什么不可用，决定按钮 tooltip 与点击行为。
    enum class HvmAvailability
    {
        Available,          // 能力齐备，可以启动常驻。
        DriverNotRunning,   // KswordARK 驱动服务未运行（先点 R0）。
        UnsupportedCpu,     // 缺少后端对应的 SVM/NPT 或 VMX/EPT 硬门。
        FirmwareDisabled,   // 固件里关闭了虚拟化。
        HypervisorConflict, // Hyper-V/VBS 已占用 VMX root。
        NotPrepared,        // 资源尚未准备（PREPARE 未执行或已 TEARDOWN）。
        Faulted,            // 存在故障或需要回滚，必须先重置。
        // 硬件支持虚拟化，但本版本没有对应后端。
        // 与 UnsupportedCpu 分开：前者要换机器，后者要等软件。
        BackendNotImplemented,
        // 外层已有 hypervisor（虚拟机内，或裸机开着 VBS/HVCI），但嵌套模式没开。
        // 这一态是可以由用户自己解决的，所以必须与"不支持"分开报。
        NestedNotAllowed,
        PowerTransition,
        Busy
    };

    // HvmState：一次状态快照。UI 只读这个结构，不直接解析 featureFlags。
    struct HvmState
    {
        HvmAvailability availability = HvmAvailability::DriverNotRunning;
        // backend：驱动当前选定的虚拟化后端，取 KSWORD_ARK_HVM_BACKEND_*。
        //
        // 抬到这一层是因为界面上有一整组入口只对 Intel 成立（EPT 分离视图、
        // 执行域、MSR / CR 策略、隐蔽 Hook），而"这台机器是不是 AMD"以前只能
        // 由各调用点各自去翻 QUERY 响应。不抬上来的代价不是麻烦，是不一致：
        // 标题栏菜单、HVM 页、硬件虚拟化证据页三处会各判各的。
        //
        // 注意它与 availability 不是一回事：AMD 机器上后端为 SVM 且完全可用，
        // availability 仍然是 Available——BackendNotImplemented 说的是"驱动
        // 没有这个处理器的后端"，那是另一件事。
        unsigned long backend = KSWORD_ARK_HVM_BACKEND_NONE;
        bool nestedSupported = false;
        bool nestedPrepared = false;
        bool nestedArmed = false;
        bool identityHidden = false;
        bool nestedRequested = false;
        bool selfTestPassed = false;
        bool slatReady = false;
        bool residentAdmission = false;
        QString admissionReason;
        QString configurationReason;
        bool residentActive = false;   // 至少一个逻辑处理器仍持有常驻状态。
        bool residentComplete = false; // 全部逻辑处理器都在 non-root。
        bool sustainedProven = false;  // 通过过 SOAK，证明常驻能长期存活。
        bool msrBitmapReady = false;   // 有 MSR bitmap，常驻才可能存活。
        bool exitEmulationReady = false; // 分发器能完成全部无条件 exit。
        bool eptRulesReady = false;    // EPT 规则后端可用。
        bool faulted = false;          // FAULTED 或 ROLLBACK_REQUIRED。
        // hypervisorPresent：外层已有 hypervisor（虚拟机内，或裸机开着 VBS/HVCI）。
        bool hypervisorPresent = false;
        // nestedResident：当前常驻是作为 L1 跑在别人之下，属于降级模式。
        bool nestedResident = false;
        // nestedL2LaunchRefusedCount：我们拒绝过多少次「别人想在我们之下起 VM」。
        //
        // 方向与 nestedResident 相反：那个说的是我们跑在谁之下，这个说的是**谁想跑在
        // 我们之下**。常驻期间机器上的 VMware / VirtualBox / WSL2 / Docker 一旦
        // VMLAUNCH，驱动会拒绝（vmcs02 合并故意没做完），而驱动侧那个状态位在紧接着
        // 的 VMXOFF 就被清掉——两秒一轮的轮询几乎必然错过。所以这里是只增不减的计数，
        // 非零就说明我们正在挡着别人的虚拟机，而用户那边看到的现象是「VM 突然起不来」。
        unsigned long nestedL2LaunchRefusedCount = 0;
        // veArmed：本次常驻武装了 EPT-violation #VE 控制位。
        // 注意语义：这只说明控制位是开的，不说明 #VE 能被投递——驱动侧
        // 的两道保险（全叶项 suppress-#VE、信息区锁 busy）与本位无关。
        bool veArmed = false;
        // veSuppressedByDefault：驱动确认它装的每个 EPT 叶项都带 suppress-#VE。
        // 这一位为假时不该武装 #VE，那意味着地基没铺好。
        bool veSuppressedByDefault = false;
        // vmFuncArmed：本次常驻武装了 VMFUNC，guest 可以自行切换 EPT 视图。
        bool vmFuncArmed = false;
        // eptpSwitchingAvailable：处理器提供 VM function 0，域才有意义。
        bool eptpSwitchingAvailable = false;
        // eptpSwitchArmed：本次运行时真的用上了 EPTP 切换后端。
        // 这一位是唯一可信的后端判据：请求位只说明调用方想要什么，能力不够
        // 时驱动保持默认后端，两种情形的请求位一模一样。
        bool eptpSwitchArmed = false;
        // localEptArmed：本次运行时真的拿到了每处理器私有 EPT 层次。
        //
        // 与 eptpSwitchArmed 同理，这一位是唯一可信的判据：菜单上的勾只表示
        // **请求**，而能力不够时驱动保持共享层次，两种情形的请求位一模一样。
        // 以前 UI 只显示那个勾，于是「打开了私有 EPT」是一句用户看得到、
        // 却可能与事实相反的话 —— 唯一的反馈是启动常驻时的一个裸状态码 21。
        bool localEptArmed = false;
        // 下面四位是驱动安装 EPT 分离视图（CLOAK/HOOK）时逐条检查的前置条件。
        // 它们本来就在 QUERY 响应里，只是从没被抬到这一层，于是安装失败时 UI
        // 只能转述一个协议状态码，说不出"缺的是哪一条"。UI 的职责是把这些约束
        // 解释清楚——它们由驱动判定，客户端既不能放宽也不该假装能绕过去。
        //
        // resourcesReady：PREPARE 已执行且未 TEARDOWN。
        bool resourcesReady = false;
        // eptReady：EPT 层次已建好，是安装视图的第一道硬门。
        bool eptReady = false;
        // inveptSingleReady：处理器支持单上下文 INVEPT。两套后端都要它——
        // 无论是写回叶项还是切 EPTP，都得把按旧值建出来的翻译丢掉。
        bool inveptSingleReady = false;
        // monitorTrapFlagReady：处理器支持 Monitor Trap Flag。
        // 只有默认后端（写叶 + 单步一条指令 + 写回）需要它；EPTP 切换后端不需要。
        // 所以判断"这台机器能不能装视图"必须连着 eptpSwitchArmed 一起看，
        // 单看这一位会把嵌套 Hyper-V 客户机误判成无解。
        bool monitorTrapFlagReady = false;
        unsigned long generation = 0;  // 用于 compare-before 控制请求。
        unsigned long processorCount = 0;
        unsigned long residentProcessorCount = 0;
        unsigned long eptRuleCount = 0;
        unsigned long long vmExitCount = 0;
        // eptPointer：当前生效的 EPT 指针（含 EPTP 的类型与层数编码位）。
        // 它是判断"驱动到底在用哪一份 EPT 层次"的唯一可观测值：私有 EPT 与
        // 执行域都会让不同的处理器/域挂在不同的指针上，而共享根被换掉又不
        // 失效正是最难查的一类故障。抬上来是为了让 UI 能把它摆出来核对。
        unsigned long long eptPointer = 0;
        unsigned long soakElapsedMilliseconds = 0;
        unsigned long soakUnexpectedDevirtualizations = 0;
        QString shortStatus; // 按钮 tooltip 首行。
        QString detail;      // 按钮 tooltip 详情。
    };

    // HvmCommandResult：一次控制命令的结果，供 UI 直接展示。
    struct HvmCommandResult
    {
        bool ok = false;
        unsigned long protocolStatus = 0; // KSWORD_ARK_HVM_CONTROL_STATUS_*。
        long ntStatus = 0;
        QString message; // 已本地化的失败原因或成功摘要。
    };

    // queryState：读取一次完整状态快照。阻塞，必须在后台线程调用。
    HvmState queryState();
    // Both query entrypoints decode the same admission and actual-mode evidence.
    HvmState stateFromStatus(const ksword::ark::HvmStatusResult& result);
    // All GUI lifecycle callers share backend selection and retained-preference filtering.
    ksword::ark::HvmControlResult controlWithPreferences(
        const ksword::ark::DriverClient& client,
        const KSWORD_ARK_QUERY_HVM_RESPONSE& status,
        unsigned long command, bool force,
        bool enableEptEvents = false, bool enableNestedVmx = false,
        bool enableEvmcs = false, unsigned long soakMilliseconds = 0);

    // ensurePrepared：按需执行 PREPARE + SELF_TEST，使常驻具备启动条件。
    // 已经准备好时直接返回成功，不重复分配资源。
    HvmCommandResult ensurePrepared();

    // startResident/stopResident：进入或离开全核 VMX non-root。
    // startResident 会在必要时先调用 ensurePrepared。
    HvmCommandResult startResident(unsigned long expectedGeneration);
    HvmCommandResult stopResident(unsigned long expectedGeneration);

    // runSoak：启动常驻、保持指定毫秒数、再停止，用于证明常驻能长期存活。
    // 驱动会把时长夹到协议上下界；调用期间驱动侧状态锁被独占。
    HvmCommandResult runSoak(
        unsigned long expectedGeneration,
        unsigned long milliseconds);

    // resetFault：清除可恢复的故障与回滚标记。常驻中会被拒绝。
    HvmCommandResult resetFault(unsigned long expectedGeneration);

    // releaseResources：TEARDOWN，释放全部可逆资源，回到"未准备"。
    //
    // 加进这一层是因为 HVM 菜单里原本**没有**它，而没有它就存在一条必然踩中的
    // 死路：视图/MSR/CR/域四个面板都要求资源已准备，可用户在这个菜单里能做的
    // 只有「启动常驻」——而它会在同一次调用里准备完资源紧接着进入常驻，
    // 于是四个面板立刻从"尚未准备"变成"常驻期间不能改"。
    // 中间那个唯一可用的窗口期，在这个菜单里按不出来。
    //
    // 它同时是「改了后端开关之后让它生效」的唯一途径：后端在 PREPARE 时选定，
    // 而 ensurePrepared 在资源已就绪时不会重发 PREPARE。
    HvmCommandResult releaseResources(unsigned long expectedGeneration);

    // 嵌套模式开关：
    // - 默认关闭，裸机独占 VT-x 仍是预期的运行方式；
    // - 打开后 PREPARE/SELF_TEST/常驻 都会带上 ALLOW_NESTED，从而可以在虚拟机里
    //   或在开着 VBS 的机器上运行。代价是每条 VMX 操作都由外层 hypervisor 模拟，
    //   性能显著下降，可用能力也只剩外层愿意暴露的那部分；
    // - 这不是危险开关（不改写任何系统状态），所以与写权限门分开。
    bool isNestedAllowed();
    void setNestedAllowed(bool allowed);

    // 嵌套派发开关（ENABLE_NESTED_VMX）：
    // - 【和上面那个是反方向的两件事，别混】isNestedAllowed 说的是「允许我们
    //   跑在别人底下」，我们是来宾；这一个说的是「允许别人跑在我们底下」，我们
    //   是宿主。共用一个开关会让打开前者的人在毫不知情的情况下打开后者；
    // - 打开后，来宾里的 ring 0 代码可以真的 VMXON、维护自己的 vmcs12、把 L2
    //   跑起来。退出先落到我们手上，按所有权决定自己处理还是投递给 L1；L1 要
    //   EPT 时由影子层次（EPT01 ∘ EPT12）按需合成，MSR 与 I/O 拦截按 L1 自己的
    //   位图合并后路由。这**不是**失败桩；
    // - 关闭时 VMX 指令被注 #UD。在 CPUID 不报 VMX 的前提下那是架构正确的行为，
    //   但对已经在跑的 VMware / VirtualBox / WSL2 来说就是"虚拟机打不开了"，
    //   而且没有任何提示指向我们——所以状态面板要报那个单调的拒绝计数；
    // - 与 ENABLE_LOCAL_EPT 互斥：嵌套要从来宾的层次和我们的层次合成出一个 EPT
    //   指针，每处理器私有根会让这个合成变成处理器相关的，驱动直接判
    //   STATUS_INVALID_PARAMETER。两个都开的请求会整条被拒，而理由只说"请求
    //   不合法"，不指是哪一位——所以要在发出去之前就挡住；
    // - 【不持久化】：与 #VE / VMFUNC 同类，武装的是一项能力而不是描述环境的
    //   事实，每次启动客户端都必须重新打开。
    bool isNestedDispatchEnabled();
    bool isHypervisorHidden();
    void setHypervisorHidden(bool enabled);
    void setNestedDispatchEnabled(bool enabled);

    // #VE 开关（把 EPT violation 反射成 guest 的 #VE，向量 20）：
    // - 这里的 guest 就是正在跑的这台 Windows。它的 IDT[20] 没有 #VE 处理程序，
    //   真投递一次就是 #GP -> #DF -> triple fault，机器当场断电式重启；
    // - 驱动侧有两道与本开关无关的保险：每一个 EPT 叶项（含未映射区域的空槽）
    //   都带 suppress-#VE，每 CPU 的信息区在分配时就把 busy 锁死。两道都在时，
    //   哪怕控制位开着也投递不出 #VE，EPT violation 会退回成常规 VM-exit；
    // - 所以打开它得到的是「控制位已武装」，不是「#VE 已生效」。要真的收到
    //   #VE，还得先在 guest 里装好处理程序、清 busy、再把目标页显式设为可转换；
    // - 硬件不支持时驱动直接拒绝启动常驻，而不是静默降级——否则调用方会以为
    //   自己在测 #VE，其实测的是别的东西；
    // - 【不持久化】：每次启动客户端都必须重新打开。这是刻意的。
    bool isVeEnabled();
    void setVeEnabled(bool enabled);

    // VMFUNC 开关：
    // - 武装后 guest 用一条 VMFUNC 就能在 EPTP list 的域之间切换，不产生
    //   VM exit，驱动也收不到通知。VMFUNC 不做 CPL 检查，所以「guest」在这里
    //   包括任意进程的任意 ring 3 线程；
    // - 这不是提权路径：域只能被拿掉权限，切进去最坏是自己吃 EPT violation。
    //   但它确实是一个驱动观测不到的状态切换，值得单独一道门；
    // - 与 #VE 一样【不持久化】，重启客户端即回到关闭。
    bool isVmFuncEnabled();
    void setVmFuncEnabled(bool enabled);

    // 私有 EPT 开关（每处理器一份 EPT 层次）：
    // - 与 #VE / VMFUNC 相反，这个开关不放开任何新能力，它让【已有】的 EPT
    //   视图与 allow-once 授权在多核上也安全：翻转只落在取到 exit 的那个
    //   处理器上，其余处理器看不到那个窗口；
    // - 打开后才能在多核机器上安装 EPT 视图。关着时视图仍然只能在单核拓扑
    //   安装，那是这个开关出现之前的行为；
    // - 与 VMFUNC、嵌套 VMX 互斥：前者要求所有处理器共享一份 EPTP list，
    //   后者要复合出一个与处理器无关的 EPT 指针；
    // - 【持久化】：它不是危险开关，是更安全的那个方向。
    bool isLocalEptEnabled();
    void setLocalEptEnabled(bool enabled);

    // EPTP 切换后端开关（分离视图用哪套机器装）：
    // - 关着时行为与今天逐字节相同：默认后端是「写 EPT 叶 + 用 Monitor Trap
    //   Flag 单步一条指令 + 把叶写回去」；
    // - 打开后改用「切 EPTP」：两套后端回答同一个问题，差别不是性能而是所需
    //   能力——默认后端要 Monitor Trap Flag，这套只要 execute-only EPT 叶；
    // - 存在的理由就在这里：嵌套 Hyper-V 客户机拿不到 MTF，所以在那种机器上
    //   只有这套后端能装上 CLOAK/HOOK 视图；
    // - 与 VMFUNC、私有 EPT 互斥，驱动在任何分配之前就拒绝同时请求；
    // - 这一位只随 PREPARE 发出，驱动在准备资源时决定武装与否。因此资源已经
    //   准备好之后再改这个开关，要到下一次重新准备才生效；
    // - 【持久化】：它不放开任何新能力，只是换机器，和私有 EPT 一样。
    bool isEptpSwitchEnabled();
    void setEptpSwitchEnabled(bool enabled);

    // 写权限门：
    // - 默认关闭。关闭时 HVM 只做观测，任何会改变系统状态的 R-1 操作都被拒绝；
    // - 由标题栏 HVM 菜单显式切换，并持久化到 QSettings；
    // - 这是进程内的第二道门，驱动侧仍然各自要求确认令牌与 FILE_WRITE_ACCESS。
    bool isWriteAccessEnabled();
    void setWriteAccessEnabled(bool enabled);

    // describeAvailability：把不可用原因翻译成可直接显示的一句话。
    QString describeAvailability(HvmAvailability availability);

    // HvmMemoryResult：一次 R-1 内存操作的结果。
    struct HvmMemoryResult
    {
        bool ok = false;
        // usedDirectWindow：真正走了私有页表窗口（绕开 Mm* 导出）。
        // 为 false 表示退化到 MmCopyMemory，仍能读，但不再规避内核层 Hook。
        bool usedDirectWindow = false;
        // windowReady：本机是否成功建立过私有窗口。
        bool windowReady = false;
        unsigned long long physicalAddress = 0;
        QByteArray data;
        QString message;
    };

    // isMemoryWindowReady：查询私有窗口是否可用，不触碰任何内存。
    HvmMemoryResult queryMemoryWindow();

    // readPhysical/writePhysical：物理内存读写。
    // - 单次上限由驱动协议决定（KSWORD_ARK_HVM_MEMORY_MAX_BYTES）；
    // - writePhysical 受写权限门约束，关闭时直接失败且不发起 IOCTL。
    HvmMemoryResult readPhysical(
        unsigned long long physicalAddress,
        unsigned long length);
    HvmMemoryResult writePhysical(
        unsigned long long physicalAddress,
        const QByteArray& payload);

    // readVirtual/writeVirtual：先自动解析目标 PID 的 CR3，或用自定义 CR3 翻译。
    // 非零 directoryBase 为显式覆盖，processId 不再参与；二者为 0 保留当前线程页表语义。
    HvmMemoryResult readVirtual(
        unsigned long long directoryBase,
        unsigned long long virtualAddress,
        unsigned long length,
        unsigned long processId = 0UL);
    HvmMemoryResult writeVirtual(
        unsigned long long directoryBase,
        unsigned long long virtualAddress,
        const QByteArray& payload,
        unsigned long processId = 0UL);

    // translate：只做虚拟到物理翻译，不访问目标内存。
    HvmMemoryResult translate(
        unsigned long long directoryBase,
        unsigned long long virtualAddress,
        unsigned long processId = 0UL);

    // HvmViewEntry：一条已安装的 EPT 分离视图。
    struct HvmViewEntry
    {
        unsigned long viewId = 0;
        unsigned long kind = 0;
        unsigned long flags = 0;
        unsigned long long physicalAddress = 0;
        unsigned long long shadowPhysicalAddress = 0;
        unsigned long long flipCount = 0;
    };

    // HvmViewShadowSeed：影子页初始内容的来源。
    enum class HvmViewShadowSeed
    {
        Zero,       // 全零：读取者看到一片空白。
        FromTarget, // 冻结目标页当前内容：读取者看到安装那一刻的样子。
        Explicit    // 使用调用方提供的整页内容。
    };

    // HvmViewResult：一次视图操作的结果。
    struct HvmViewResult
    {
        bool ok = false;
        unsigned long viewId = 0;
        unsigned long viewCount = 0;
        // protocolStatus/lastStatus：原样上传的两级失败码。
        //
        // message 是给人读的一句话，一旦翻译过就丢掉了"失败发生在哪一步"。
        // 而同一个 protocolStatus 会由多个不同的分支产生（MULTIPROCESSOR_UNSAFE
        // 既可能是"正在常驻"也可能是"拓扑或能力不满足"），只有配上 lastStatus
        // 才分得开。调用方要靠这两个值把失败精确退回到产生它的那一步，
        // 而不是拿字符串去猜。
        // protocolStatus 取 KSWORD_ARK_HVM_VIEW_STATUS_*，lastStatus 是 NTSTATUS。
        //
        // 只在 ok 为假时才去读它们，而且要先看 ok：写权限门与影子页长度这两条
        // 在客户端就被拒的路径根本没发过 IOCTL，两个字段保持零——而零恰好就是
        // VIEW_STATUS_OK。不给它们编一个协议里没有的哨兵值，是因为那等于凭空
        // 发明一个驱动永远不会返回的状态码。
        unsigned long protocolStatus = 0;
        long lastStatus = 0;
        QVector<HvmViewEntry> views;
        QString message;
    };

    // listViews：读取已安装视图，不需要写权限。
    HvmViewResult listViews();

    // addView：安装一条视图。受写权限门约束。
    // shadow 只在 seed 为 Explicit 时使用，必须恰好是一页（4096 字节）。
    HvmViewResult addView(
        unsigned long kind,
        unsigned long long physicalAddress,
        HvmViewShadowSeed seed,
        const QByteArray& shadow);

    // removeView/clearViews：移除一条或全部视图。受写权限门约束。
    HvmViewResult removeView(unsigned long viewId);
    HvmViewResult clearViews();

    // HvmDomainEntry：EPTP list 里的一个槽位。
    struct HvmDomainEntry
    {
        unsigned long domainIndex = 0;
        bool active = false;
        // 该域从共享层次里分叉出去的页表数：0 表示与默认视图完全一致。
        unsigned long privateTableCount = 0;
        unsigned long long eptPointer = 0;
    };

    // HvmDomainResult：一次执行域操作的结果。
    struct HvmDomainResult
    {
        bool ok = false;
        unsigned long domainIndex = 0;
        unsigned long domainCount = 0;
        QVector<HvmDomainEntry> domains;
        QString message;
    };

    // 执行域（EPT execution domain）：
    // - 域发布在 EPTP list 里，guest 用一条 VMFUNC 就能切过去，而 VMFUNC 不做
    //   CPL 检查——任何 ring 3 线程都能切，不产生 VM exit，驱动也不会被通知；
    // - 所以接口只有「减权限」一个方向：域出生时与默认视图完全一致，之后只能
    //   被拿掉权限。切进域的线程结构性地拿不到它原本没有的访问权；
    // - 建好域本身不改变任何行为。要让 VMFUNC 真的可用，还得用带 ENABLE_VMFUNC
    //   的常驻启动，而那是另一个独立的开关。
    HvmDomainResult listDomains();
    HvmDomainResult createDomain();
    // restrictDomain：从一个域里拿掉某段物理范围的权限。
    // deniedAccess 用 KSWORD_ARK_HVM_EPT_ACCESS_* 位。拿掉读权限需要处理器
    // 支持 execute-only 翻译，否则驱动拒绝（那样的叶项会让 VM entry 失败）。
    HvmDomainResult restrictDomain(
        unsigned long domainIndex,
        unsigned long long physicalAddress,
        unsigned long long byteCount,
        unsigned long deniedAccess);
    HvmDomainResult resetDomains();

    // HvmMsrPolicyEntry：一条已安装的 MSR 策略。
    struct HvmMsrPolicyEntry
    {
        unsigned long policyId = 0;
        unsigned long msrIndex = 0;
        unsigned long access = 0;
        unsigned long action = 0;
        unsigned long long fakeValue = 0;
        unsigned long long hitCount = 0;
    };

    // HvmMsrPolicyResult：一次 MSR 策略操作的结果。
    struct HvmMsrPolicyResult
    {
        bool ok = false;
        unsigned long policyId = 0;
        unsigned long policyCount = 0;
        QVector<HvmMsrPolicyEntry> policies;
        QString message;
    };

    // listMsrPolicies：读取已安装策略，不需要写权限。
    HvmMsrPolicyResult listMsrPolicies();

    // addMsrPolicy：安装一条策略。受写权限门约束。
    // action 为 LOG 时不能带写方向：在 VMX root 里重放 WRMSR 没有安全退路。
    HvmMsrPolicyResult addMsrPolicy(
        unsigned long msrIndex,
        unsigned long access,
        unsigned long action,
        unsigned long long fakeValue);

    // removeMsrPolicy/clearMsrPolicies：移除一条或全部策略。受写权限门约束。
    HvmMsrPolicyResult removeMsrPolicy(unsigned long policyId);
    HvmMsrPolicyResult clearMsrPolicies();

    // HvmEventEntry：一条 HVM 事件。sequence 单调递增，用作消费游标。
    struct HvmEventEntry
    {
        unsigned long long sequence = 0;
        unsigned long long timestamp = 0;
        unsigned long long guestPhysicalAddress = 0;
        unsigned long long guestLinearAddress = 0;
        unsigned long long guestRip = 0;
        unsigned long long qualification = 0;
        unsigned short processorGroup = 0;
        unsigned char processorNumber = 0;
        unsigned long type = 0;
        unsigned long exitReason = 0;
        unsigned long access = 0;
        // ruleId 对 EPT 视图翻转承载的是 viewId：两者共用这一列。
        unsigned long ruleId = 0;
        long status = 0;
        // 命中那一刻的栈指针与地址空间。只能在 VM-exit 现场取：一旦 VMRESUME
        // 回去，它们描述的就是另一个线程了。零表示驱动没能读到（外层
        // hypervisor 会拒绝某些客户状态编码），不是"值就是 0"。
        unsigned long long guestRsp = 0;
        unsigned long long guestCr3 = 0;
        // 见 KSWORD_ARK_HVM_EPT_WATCH_STATE_*，只对 watch 命中行有意义。
        unsigned long watchState = 0;
        // 见 KSWORD_ARK_HVM_EVENT_FLAG_*。
        unsigned long eventFlags = 0;
    };

    // HvmEventResult：一次事件读取的结果。
    struct HvmEventResult
    {
        bool ok = false;
        // droppedRows：本次快照中被覆盖或不可用的行数，非零说明消费跟不上。
        unsigned long droppedRows = 0;
        unsigned long availableRows = 0;
        unsigned long long newestSequence = 0;
        QVector<HvmEventEntry> events;
        QString message;
    };

    // readEvents：读取 afterSequence 之后的事件。只读，不需要写权限。
    // clear 为 true 时同时清空环，用于开始一次干净的观察。
    HvmEventResult readEvents(
        unsigned long long afterSequence,
        bool clear);

    // HvmCrPolicyResult：控制寄存器策略的当前配置与计数。
    struct HvmCrPolicyResult
    {
        bool ok = false;
        unsigned long long cr0PinnedMask = 0;
        unsigned long long cr4PinnedMask = 0;
        unsigned long long cr0PinnedValue = 0;
        unsigned long long cr4PinnedValue = 0;
        unsigned long long refusedWriteCount = 0;
        unsigned long long cr3SwitchCount = 0;
        unsigned long long debugAccessCount = 0;
        bool trackCr3 = false;
        bool interceptDr = false;
        bool log = false;
        QString message;
    };

    // readCrPolicy：读取当前配置与计数。只读。
    HvmCrPolicyResult readCrPolicy();

    // applyCrPolicy：设置钉住掩码与拦截开关。受写权限门约束。
    // 掩码与开关在建 VMCS 时消费，常驻期间驱动会拒绝改动。
    HvmCrPolicyResult applyCrPolicy(
        unsigned long long cr0PinnedMask,
        unsigned long long cr4PinnedMask,
        bool trackCr3,
        bool interceptDr,
        bool log);

    // clearCrPolicy：清除全部配置与计数。受写权限门约束。
    HvmCrPolicyResult clearCrPolicy();

    // ——— R-1 进程处置与注入 ———
    //
    // 这两组此前在 GUI 里唯一的入口是「完整命令面板」，而那条路是拿主程序当
    // hvm_ctl 子进程拉起来的——探针的调用方式，不是产品的。探针从主程序摘掉
    // 之后，这两组能力本身并不跟着走：它们各自有专属 IOCTL，驱动侧是实现完整
    // 的生产路径。所以在这里补上直连门面，与视图 / MSR / CR 三组同一个形状：
    // 同一道写权限门、同一套状态码翻译、同样返回整张表。

    // HvmProcessDispositionEntry：一条已安装的 R-1 进程处置。
    struct HvmProcessDispositionEntry
    {
        unsigned long processId = 0;
        // disposition 取 KSWORD_ARK_HVM_PROCESS_OP_FREEZE / _TERMINATE，
        // 以及 _DISPOSITION_RELEASED（常驻期间被撤销、层次尚未回收）。
        unsigned long disposition = 0;
        unsigned long hierarchyIndex = 0;
        // directoryBase 才是这条记录的判据：PID 会被回收，地址空间不会。
        unsigned long long directoryBase = 0;
        unsigned long long guestPhysicalAddress = 0;
        unsigned long long guestLinearAddress = 0;
        // interceptCount 在冻结下持续增长，那正是目标线程还在自旋的证据。
        unsigned long long interceptCount = 0;
    };

    // HvmProcessResult：一次进程处置操作的结果。每种操作都回填整张表。
    struct HvmProcessResult
    {
        bool ok = false;
        // protocolStatus 取 KSWORD_ARK_HVM_PROCESS_STATUS_*。只在 ok 为假时读，
        // 与 HvmViewResult 上那条注释同理：客户端就地拒绝的路径没发过 IOCTL。
        unsigned long protocolStatus = 0;
        long lastStatus = 0;
        unsigned long rowCount = 0;
        QVector<HvmProcessDispositionEntry> dispositions;
        QString message;
    };

    // listProcessDispositions：读取处置表。只读，不需要写权限。
    HvmProcessResult listProcessDispositions();

    // freezeProcess/terminateProcess：安装一条处置。受写权限门约束。
    //
    // guestLinearAddress 给 0 表示由驱动取主映像入口页。允许指定是因为"哪一页
    // 代表这个进程"没有普适答案：入口页对刚起来的进程有效，对已经跑进消息循环
    // 的进程未必会再被执行到，而没被执行到的拒绝等于什么都没做。
    HvmProcessResult freezeProcess(
        unsigned long processId,
        unsigned long long guestLinearAddress);
    HvmProcessResult terminateProcess(
        unsigned long processId,
        unsigned long long guestLinearAddress);

    // releaseProcessDisposition/releaseAllProcessDispositions：撤销。
    // 常驻期间是「解除」而不是完整撤销，驱动侧语义见协议头注释。
    HvmProcessResult releaseProcessDisposition(unsigned long processId);
    HvmProcessResult releaseAllProcessDispositions();

    // HvmInjectionEntry：一条已安装的 R-1 注入。
    struct HvmInjectionEntry
    {
        unsigned long processId = 0;
        unsigned long payloadBytes = 0;
        unsigned long long directoryBase = 0;
        unsigned long long guestLinearAddress = 0;
        unsigned long long guestPhysicalAddress = 0;
        // caveOffset：外壳在页内的偏移，也就是 RIP 会被指向的位置。
        unsigned long caveOffset = 0;
        unsigned long caveBytes = 0;
        // executionCount：载荷被执行的次数。一次性注入完成后应为 1。
        unsigned long long executionCount = 0;
        unsigned long viewId = 0;
        // caveFiller：空隙原本的填充字节（0x00 / 0xCC / 0x90），排查时用来归因。
        unsigned long caveFiller = 0;
    };

    // HvmInjectResult：一次注入操作的结果。
    struct HvmInjectResult
    {
        bool ok = false;
        unsigned long protocolStatus = 0; // KSWORD_ARK_HVM_INJECT_STATUS_*。
        long lastStatus = 0;
        unsigned long rowCount = 0;
        QVector<HvmInjectionEntry> injections;
        QString message;
    };

    // listInjections：读取注入表。只读，不需要写权限。
    HvmInjectResult listInjections();

    // injectDll：在目标进程里通过分离视图 + 线程劫持加载一个 DLL。
    //
    // guestLinearAddress **必填**：要劫持的那一页里的任意地址。驱动不猜这一页，
    // 猜错的表现是载荷装上了却永远不执行——从外面看和成功完全一样。取目标某个
    // 线程此刻正在执行的位置，那一页按定义会被执行到。
    //
    // loadLibraryAddress 给 0 表示由客户端就地解析：kernel32 在同一次启动内对
    // 所有进程是同一个基址，所以本进程解析出来的 LoadLibraryW 对靶子同样成立。
    HvmInjectResult injectDll(
        unsigned long processId,
        unsigned long long guestLinearAddress,
        unsigned long long loadLibraryAddress,
        const QString& dllPath);

    // releaseInjection/releaseAllInjections：撤销注入。受写权限门约束。
    HvmInjectResult releaseInjection(unsigned long processId);
    HvmInjectResult releaseAllInjections();

    // ——— R-1 内存监视（首次访问归因） ———
    //
    // 它回答的是别的机制答不了的一个问题：某个内核对象被改过之后，**下一次**是
    // 谁动的、从哪条指令动的。快照式检测只能给出"改之前是 A，改之后是 B"，中间
    // 那个动作没有任何证据。
    //
    // 边界必须说清楚，因为它很容易被当成不是的东西：这是观察与归因，不是保护。
    // 它不阻止访问（命中后原访问照常完成）、不是安全边界（EPT 是页粒度，DMA 不
    // 经过 CPU EPT，目标换掉自己那一页的物理页就不在被监视的页上了）、也不是
    // 持续监视（第一版只保证"下一次访问产生一次可靠事件"）。

    // HvmWatchEntry：一条 watch 的完整快照。
    struct HvmWatchEntry
    {
        unsigned long watchId = 0;
        // 见 KSWORD_ARK_HVM_EPT_WATCH_STATE_*。
        unsigned long state = 0;
        // 用户勾的 / 实际装上的访问类型。两个都要：EPT 不允许 W=1 而 R=0，
        // 所以"只监视读"在硬件上一定连写也监视了。只显示其中一个，要么替用户
        // 改了他的请求，要么谎称监视得比实际更细。
        unsigned long requestedAccess = 0;
        unsigned long effectiveAccess = 0;
        unsigned long addressKind = 0;
        unsigned long hitCount = 0;
        unsigned long long lastHitSequence = 0;
        // 见 KSWORD_ARK_HVM_EPT_WATCH_HIT_*。EVENT_LOST 与"从未命中"必须分开。
        unsigned long lastHitStatus = 0;
        unsigned long armedGeneration = 0;
        // 用户请求的那一段，与硬件真正监视的那一页。前者可能是 8 字节，
        // 后者恒为 4096 —— 界面必须两套都摆出来。
        unsigned long long requestedAddress = 0;
        unsigned long long requestedLength = 0;
        unsigned long long physicalPage = 0;
        unsigned long long pageCount = 0;
        unsigned long long lastHitRip = 0;
        unsigned long long lastHitGuestLinearAddress = 0;
        unsigned long long lastHitGuestPhysicalAddress = 0;
        unsigned long long lastHitCr3 = 0;
        unsigned long long lastHitRsp = 0;
        unsigned long long lastHitTimestamp = 0;
        unsigned short lastHitProcessorGroup = 0;
        unsigned char lastHitProcessorNumber = 0;
        bool lastHitGuestLinearValid = false;
        // 命中的 GLA 落在 requestedAddress/Length 之内，而不只是同一页上。
        bool lastHitRangeMatch = false;
    };

    // HvmWatchResult：一次 watch 操作的结果。
    struct HvmWatchResult
    {
        bool ok = false;
        unsigned long protocolStatus = 0; // KSWORD_ARK_HVM_EPT_RULE_STATUS_*。
        long lastStatus = 0;
        // 冲突时占着这一页的是谁，见 KSWORD_ARK_HVM_WATCH_CONFLICT_*。
        unsigned long conflictOwnerId = 0;
        unsigned long conflictOwnerKind = 0;
        unsigned long watchCount = 0;
        QVector<HvmWatchEntry> watches;
        QString message;
    };

    // listWatches：读取整张 watch 表。只读，不需要写权限。
    HvmWatchResult listWatches();

    // HvmWatchTarget：一次 watch 安装请求。
    struct HvmWatchTarget
    {
        // 为真表示 address 是内核虚拟地址，安装前先翻译成物理页。
        bool virtualAddress = true;
        unsigned long long address = 0;
        // 用户真正关心的字节数。给 0 表示整页。
        unsigned long long length = 0;
        // KSWORD_ARK_HVM_EPT_ACCESS_* 的组合。
        unsigned long access = 0;
    };

    // addWatch：安装一条首次访问 watch。受写权限门约束。
    //
    // 虚拟地址会在这里翻译一次并记下结果；**不跟踪后续的重映射**。安装之后
    // guest 页表把同一个 VA 指到别的物理页，这条 watch 仍然监视原来那一页，
    // 界面要能检测出这个分歧并说出来，而不是继续声称"正在监视该 VA"。
    HvmWatchResult addWatch(const HvmWatchTarget& target);

    // rearmWatch：把一条已命中或已失效的 watch 重新武装，保留标识与历史。
    HvmWatchResult rearmWatch(unsigned long watchId);

    // removeWatch：按编号撤销一条 watch 并恢复该页权限。受写权限门约束。
    HvmWatchResult removeWatch(unsigned long watchId);

    // clearWatches：清空整张表。受写权限门约束。
    //
    // 它复用 EPT 规则的 CLEAR 操作，因此**会连同普通 EPT 规则一起清掉**——
    // watch 与规则本来就是同一张表里的东西，协议里没有"只清 watch"这个操作。
    // 调用点必须把这一点原样说给用户听：把它描述成"清空监视"，用户就会在
    // 不知情的情况下丢掉自己装的 tripwire，而那件事没有任何提示。
    HvmWatchResult clearWatches();

    // HvmWatchHitEventKind：一次"去事件环里找这条 watch 的命中事件"的结果分类。
    //
    // 四态而不是"找到/没找到"：后三种都会表现为"看不到事件"，而它们要人做的
    // 事完全不同——从未命中是结论，被挤出环是证据丢失（目标**确实**被动过），
    // 环读不出来是这次查询自己没跑起来。压成一态就等于把"目标没被访问"这句
    // 没有根据的话说给了用户。
    enum class HvmWatchHitEventKind
    {
        // 这条 watch 从来没有命中过，所以没有事件可找。
        NeverHit = 0,
        // 事件还在环里，已经取回来了。
        Found,
        // watch 自己记着命中过，但那一行已经被后来的事件挤出了环。
        Evicted,
        // 环没读出来：驱动没在、常驻没跑、查询失败。
        Unavailable,
    };

    // HvmWatchHitEvent：上面那次查询的完整结果。
    struct HvmWatchHitEvent
    {
        HvmWatchHitEventKind kind = HvmWatchHitEventKind::NeverHit;
        // kind 为 Found 时有效，是命中那一刻写进环里的整行。
        HvmEventEntry event;
        // 查询当时环里最新的序号，用来解释 Evicted：它比命中序号大多少，
        // 就是这条证据被推走之前还剩多少行。
        unsigned long long newestSequence = 0;
        // 本次快照里被覆盖或不可用的行数，非零说明消费跟不上。
        unsigned long droppedRows = 0;
        // 面向用户的一句话说明，四态各自不同。
        QString message;
    };

    // findWatchHitEvent：去事件环里找某条 watch 最近一次命中的那一行。
    //
    // watch 表自己已经存了 RIP / RSP / CR3 / GLA / GPA / 时间 / CPU，所以这一步
    // **不是**为了拿那些字段；它要的是只存在于事件行里的东西——尤其是
    // qualification（退出限定符，watch 行里没有这个字段）与事件序号的上下文。
    //
    // 入参 entry 用 watch 表快照里那一行：函数按它的 lastHitStatus 与
    // lastHitSequence 决定要不要去读环，以及读不到时该判 Evicted 还是 NeverHit。
    HvmWatchHitEvent findWatchHitEvent(const HvmWatchEntry& entry);

    // ——— 目标标签 ———
    //
    // 协议里没有标签字段：watch 行是驱动存的，而"\Driver\Foo 的
    // MajorFunction[IRP_MJ_DEVICE_CONTROL]"是一句只对人有意义的话，让内核背着
    // 它没有任何收益。所以标签存在 R3 这一侧，按 watchId 索引。
    //
    // 但 watchId 是驱动分配的 ruleId，**会在驱动重载后从头开始**。只按编号存，
    // 下一次装的另一条 watch 就会顶着上一条的描述显示出来——那不是缺了信息，
    // 是显示了一句错的。所以每条标签同时存一份身份指纹（地址种类、请求地址、
    // 请求长度、监视页），取回时逐项比对，对不上就当没有标签。

    // rememberWatchLabel：记下一条 watch 的人话标签。
    // entry 必须是安装成功后驱动回填的那一行（watchId 与监视页都已确定）。
    // label 为空时等价于 forgetWatchLabel。
    void rememberWatchLabel(const HvmWatchEntry& entry, const QString& label);

    // watchLabel：取回标签。没存过、或身份指纹对不上时返回空串。
    QString watchLabel(const HvmWatchEntry& entry);

    // forgetWatchLabel：撤销 watch 时一并丢掉它的标签。
    void forgetWatchLabel(unsigned long watchId);

    // forgetAllWatchLabels：清空整张表时一并丢掉全部标签。
    void forgetAllWatchLabels();

    // HvmWatchAttribution：把一个客户 RIP 归到某个已加载内核模块上。
    //
    // 只做"地址落在哪个模块的映像范围里"这一步。它在 R3 普通上下文做，不在
    // VMX root 做：那里解析 Windows 对象是拿整台机器冒险，而这一步晚几毫秒
    // 做完全不影响结论。
    // HvmWatchAttributionKind：模块归因的四态。
    //
    // 与进程归因同一套分法，理由也一样：三种"没归出来"必须分开。把
    // "这个地址确实不在任何已加载模块里"（一条可疑读数，值得继续查）和
    // "这次查询自己没跑起来"（什么都没问出来）显示成同一句话，会让后者被
    // 当成前者去读——那是把一次失败伪装成结论。
    enum class HvmWatchAttributionKind
    {
        // 没有地址可归（传进来的是 0）。
        Unavailable = 0,
        // 地址落在某个已加载模块的映像范围内。
        Resolved,
        // 模块表读到了，逐个比过，这个地址不在任何模块里。这是结论。
        NotFound,
        // 模块表根本没读出来：符号解析不到、长度问不出、两次查询都失败。
        Failed,
    };

    struct HvmWatchAttribution
    {
        HvmWatchAttributionKind kind = HvmWatchAttributionKind::Unavailable;
        // resolved：kind == Resolved 的别名，保留给只关心"有没有模块名"的调用点。
        bool resolved = false;
        QString moduleName;
        QString modulePath;
        unsigned long long moduleBase = 0;
        unsigned long long moduleSize = 0;
        // 相对模块基址的偏移，resolved 为真时有效。
        unsigned long long relativeAddress = 0;
        // 最近的**导出**符号，以及相对它的偏移。
        //
        // 只解析导出表，不解析 PDB：没有符号服务器也没有本地 PDB 时，导出表是
        // 唯一一份随模块自带、离线可读、且不需要联网的符号来源。它答不出静态
        // 函数，但能答出绝大多数值得怀疑的目标（dispatch、回调、SSDT 例程都是
        // 导出的或紧邻导出的）。
        //
        // 空串表示这个地址前面没有任何导出符号，那时调用方应当只显示
        // `module.sys+0xRVA`，而不是编一个最近的名字出来 —— 一个错误的函数名
        // 比没有名字更难纠正。
        QString symbolName;
        unsigned long long symbolOffset = 0;
    };

    // attributeKernelAddress：把一个内核地址归到模块。
    //
    // 返回四态，调用方必须按态分开措辞：
    //   Resolved     给出模块名与模块内偏移（有导出符号时还给符号）。
    //   NotFound     模块表读到了、比过了，这个地址不在任何已加载模块里。这是
    //                一条结论（"未知可执行区域"），本身就值得怀疑，所以要提供
    //                打开内存 / 打开反汇编 / 查看所在页的入口，而不是只写 Unknown。
    //   Failed       模块表没读出来。这时**不能**说"不属于任何模块"——那句话
    //                没有根据。要说的是"这次归因没跑起来"。
    //   Unavailable  没有地址可归。
    HvmWatchAttribution attributeKernelAddress(unsigned long long address);
    // One immutable loaded-module snapshot for the complete bounded watch batch.
    QVector<HvmWatchAttribution> attributeKernelAddresses(const QVector<quint64>& addresses);

    // HvmProcessAttribution：把命中现场的 CR3 归到一个进程上。
    //
    // 四态，对应 issue #195 第十一节要求的四种措辞。它们不是同一件事的四个
    // 程度，而是四种**不同的答案**，混起来就会让"这个地址空间已经不在了"和
    // "这次归因根本没跑起来"显示成同一句话。
    enum class HvmProcessAttributionKind
    {
        // 没有 CR3 可归（命中现场没记下来，或者根本没命中过）。
        Unavailable = 0,
        // 扫过了，某个进程的 CR3 与它逐位相等。
        Resolved,
        // 扫过了，没有一个对得上。地址空间多半已经拆掉了。
        NotFound,
        // 一个进程都没问成：驱动没在、快照拿不到、权限不够。
        Failed,
    };

    struct HvmProcessAttribution
    {
        HvmProcessAttributionKind kind = HvmProcessAttributionKind::Unavailable;
        unsigned long processId = 0;
        // 界面自己的进程快照解析出来的映像名。可能是空的，也可能因为 PID 被
        // 回收而指向另一个进程——所以它永远只作为补充显示，判据始终是 PID。
        QString imageName;
        // 实际问过 CR3 的进程数。区分"扫过都不是它"与"一个都没扫成"。
        unsigned long scannedProcesses = 0;
    };

    // attributeProcessByCr3：走驱动把 CR3 归到 PID，再用界面自己的进程快照补
    // 映像名。阻塞 IOCTL，且会遍历全部进程，必须在后台线程调用。
    HvmProcessAttribution attributeProcessByCr3(unsigned long long directoryBase);

    // describeProcessAttribution：把四态翻译成一句可直接显示、且不夸大确定性
    // 的话。永远不会返回"未知"这种把四种答案压成一种的措辞。
    QString describeProcessAttribution(const HvmProcessAttribution& attribution);

    // toWin32ModulePath：把内核视角的模块路径转成资源管理器认得的路径。
    // 认不出来返回空串——调用方必须安静放弃，而不是拿原串去试：资源管理器
    // 会拿一个不存在的路径开一个默认目录，看起来完全像成功了。
    QString toWin32ModulePath(const QString& ntPath);

    // describeWatchState/describeWatchAccess：把协议值翻译成可直接显示的文字。
    QString describeWatchState(unsigned long state);
    QString describeWatchAccess(unsigned long access);
    // describeWatchHitTime：把命中时刻的性能计数值说成一句人能读的话。
    //
    // 驱动在 VM-exit 现场记的是 KeQueryPerformanceCounter 的计数，**不是**墙钟
    // 时间——VMX root 里没有可以安全调用的时间转换例程。这个函数用用户态的
    // QueryPerformanceCounter（读的是同一个系统级计数源）把它倒推成墙钟时间，
    // 并且**在返回的文字里标明它是换算来的**：计数源在睡眠前后可能被重置，
    // 而且换算发生在查看的那一刻而不是命中的那一刻。把它显示成一个干净的
    // 时间戳，等于声称记下了一件没有记过的事。
    //
    // 入参 ticks：watch 行里的 lastHitTimestamp，零表示没采集。
    // 返回：一行说明文字；换算不成立时只回报原始计数并说明原因。
    QString describeWatchHitTime(unsigned long long ticks);

    // describeWatchConflict：冲突时说清楚是谁占着这一页。
    QString describeWatchConflict(unsigned long ownerKind, unsigned long ownerId);
}

// 表格行要把整条快照存进 Qt::UserRole：从已本地化的单元格文字反推回数值，
// 会在第一个被翻译的词上出错。
Q_DECLARE_METATYPE(ksword::hvm::HvmWatchEntry)
