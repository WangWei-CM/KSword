# Ksword ARK Development Plan - System Informer DynData First

本文档是 Ksword ARK 后续开发计划的新版本。

1. 先接入 System Informer 的动态偏移数据源和精确匹配模型。
2. 在 Ksword 内实现轻量 DynData loader，不搬整套 KPH 框架。
3. 所有依赖未公开字段的功能统一走能力门控。
4. 对主流 Windows 10/11，优先使用 System Informer 已覆盖的 offset profile。
5. 匹配失败时禁用对应功能，不猜偏移，不按相近 build 冒险运行。

在执行 DynData 和后续 ARK 功能前，必须先完成一次协作性重构。当前架构能继续单人推进，但不适合多人并行开发：驱动 IOCTL 分发、用户态 R0 调用、Dock 大文件、工程文件和协作规范都会成为合并冲突热点。这个问题优先级高于任何新功能。

参考仓库：

- Ksword：`/ark/KSword`
- Ksword 驱动：`/ark/KSword/KswordARKDriver`
- Ksword R0/R3 共享协议：`/ark/KSword/shared/driver`
- System Informer 驱动：`/ark/SystemInformer/KSystemInformer`
- System Informer DynData：`/ark/SystemInformer/kphlib/kphdyn.xml`、`/ark/SystemInformer/kphlib/kphdyn.c`、`/ark/SystemInformer/kphlib/include/kphdyn.h`
- System Informer DynData loader 参考：`/ark/SystemInformer/KSystemInformer/dyndata.c`
- SKT64：`/ark/SKT64`，只作为功能和特征码思路参考，不作为正式偏移数据源

## Phase -1：多人协作前置重构

这是所有后续开发的前置阶段。目标不是改业务行为，而是把 Ksword 拆成适合多人和多 AI 并行工作的结构。完成这个阶段后，协作者可以按模块分工：一个人做 DynData，一个人做句柄表，一个人做 ALPC，一个人做 UI 能力页，而不是所有人同时改 `ioctl_dispatch.c`、`ProcessDock.cpp`、`MainWindow.cpp`。

### -1.1 当前协作风险

驱动侧风险：

- `/ark/KSword/KswordARKDriver/src/dispatch/ioctl_dispatch.c` 是中心大 switch，所有新 IOCTL 都会修改同一个文件。
- `/ark/KSword/KswordARKDriver/src/features/process/process_actions.c` 已经承载终止、挂起、PPL、枚举等多类逻辑，继续加功能会超过合理维护边界。
- `AGENTS.md` 仍提到旧的 `Queue.c` 落点，与当前 `src/dispatch/ioctl_dispatch.c` 实际结构不一致，会误导协作者。
- 新功能需要频繁同时改 `.vcxproj` 和 `.vcxproj.filters`，多人合并时容易冲突。
- 缺少统一 IOCTL handler 模式，参数校验、日志、完成字节数、错误处理会在每个 case 中重复。

用户态风险：

- `ProcessDock.cpp`、`FileDock.cpp`、`MonitorDock.cpp`、`MainWindow.cpp` 都是超大文件，是天然合并冲突热点。
- R0 调用散落在 UI 代码中，多个 Dock 直接 `CreateFileW + DeviceIoControl`。
- 缺少统一 `ArkDriverClient`，导致打开驱动、错误转换、capability 查询、缓冲区重试、NTSTATUS 展示都分散实现。
- UI 与 R0 协议调用耦合过紧，后续改协议会牵动多个 Dock。
- `agents.md` 中部分路径仍是历史路径或 Windows 绝对路径，不适合 `/ark/KSword` 当前工作区。

项目协作风险：

- 没有模块 ownership 约定，协作者容易同时改同一批文件。
- 缺少“协议头先行”的分支规则。
- 缺少“一个功能只允许改哪些目录”的边界。
- 缺少统一能力查询，UI 不知道哪些 R0 功能可用。

### -1.2 交付目标

本阶段完成后必须达到：

- 驱动中心 dispatch 不再写大量业务 case。
- 每个 IOCTL 都有独立 handler 文件或独立 feature handler。
- 用户态所有 R0 调用都通过 `ArkDriverClient`。
- Dock 代码只负责 UI，不直接调用 `DeviceIoControl`。
- `AGENTS.md` 和 `agents.md` 路径、规则、落点全部更新。
- 工程文件按模块分组，新增功能时只产生最小项目文件改动。
- 每个后续 Phase 都能指定清晰 owner 和写入范围。

### -1.3 驱动侧拆分方案

新增或调整目录：

- `/ark/KSword/KswordARKDriver/src/dispatch/ioctl_dispatch.c`
- `/ark/KSword/KswordARKDriver/src/dispatch/ioctl_registry.c`
- `/ark/KSword/KswordARKDriver/src/dispatch/ioctl_registry.h`
- `/ark/KSword/KswordARKDriver/src/dispatch/ioctl_validation.c`
- `/ark/KSword/KswordARKDriver/src/dispatch/ioctl_validation.h`
- `/ark/KSword/KswordARKDriver/src/features/process/process_ioctl.c`
- `/ark/KSword/KswordARKDriver/src/features/file/file_ioctl.c`
- `/ark/KSword/KswordARKDriver/src/features/kernel/kernel_ioctl.c`
- `/ark/KSword/KswordARKDriver/src/features/callback/callback_ioctl.c`
- `/ark/KSword/KswordARKDriver/src/features/log/log_ioctl.c`

核心接口：

```c
typedef NTSTATUS
(*KSWORD_ARK_IOCTL_HANDLER)(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

typedef struct _KSWORD_ARK_IOCTL_ENTRY
{
    ULONG IoControlCode;
    KSWORD_ARK_IOCTL_HANDLER Handler;
    const char* Name;
    ULONG RequiredCapability;
    ULONG Flags;
} KSWORD_ARK_IOCTL_ENTRY;
```

dispatch 规则：

- `KswordARKDriverEvtIoDeviceControl` 只负责查找 handler、调用 handler、完成请求。
- handler 负责取输入输出 buffer、校验协议、调用 feature、返回 bytes。
- 统一日志函数记录 IOCTL 名称、状态、输入长度、输出长度、返回长度。
- 未注册 IOCTL 返回 `STATUS_INVALID_DEVICE_REQUEST`。
- 任何新功能只新增自己的 `xxx_ioctl.c`，尽量不改中心 dispatch。

拆分要求：

- 现有 `IOCTL_KSWORD_ARK_TERMINATE_PROCESS` 移入 `process_ioctl.c`。
- 现有 `IOCTL_KSWORD_ARK_SUSPEND_PROCESS` 移入 `process_ioctl.c`。
- 现有 `IOCTL_KSWORD_ARK_SET_PPL_LEVEL` 移入 `process_ioctl.c`。
- 现有 `IOCTL_KSWORD_ARK_ENUM_PROCESS` 移入 `process_ioctl.c`。
- 现有 `IOCTL_KSWORD_ARK_DELETE_PATH` 移入 `file_ioctl.c`。
- 现有 `IOCTL_KSWORD_ARK_ENUM_SSDT` 移入 `kernel_ioctl.c`。
- 现有 callback rule/wait/answer/remove IOCTL 移入 `callback_ioctl.c`。
- log channel 相关 IOCTL 或设备逻辑保留独立模块。

验证：

- 重构前后 IOCTL 号不变。
- 重构前后所有现有 R0 功能行为不变。
- 未知 IOCTL 返回明确错误。
- 每个 handler 文件不超过 1000 行。
- `ioctl_dispatch.c` 目标小于 250 行。

### -1.4 用户态 R0 Client 拆分方案

新增目录：

- `/ark/KSword/Ksword5.1/Ksword5.1/ArkDriverClient`

新增文件：

- `ArkDriverClient/ArkDriverClient.h`
- `ArkDriverClient/ArkDriverClient.cpp`
- `ArkDriverClient/ArkDriverTypes.h`
- `ArkDriverClient/ArkDriverError.h`
- `ArkDriverClient/ArkDriverError.cpp`
- `ArkDriverClient/ArkDriverCapabilities.h`
- `ArkDriverClient/ArkDriverCapabilities.cpp`
- `ArkDriverClient/ArkDriverProcess.cpp`
- `ArkDriverClient/ArkDriverFile.cpp`
- `ArkDriverClient/ArkDriverKernel.cpp`
- `ArkDriverClient/ArkDriverCallback.cpp`

职责边界：

- `ArkDriverClient` 负责打开和关闭驱动设备。
- `ArkDriverClient` 负责 `DeviceIoControl`。
- `ArkDriverClient` 负责 Win32 error 和 NTSTATUS 转换。
- `ArkDriverClient` 负责 capability 缓存。
- Dock 不直接调用 `CreateFileW` 打开 KswordARK 设备。
- Dock 不直接调用 `DeviceIoControl`。
- Dock 只拿到结构化结果，例如 `ArkProcessEnumResult`、`ArkIoResult`。

示例接口：

```cpp
namespace ksword::ark
{
    struct IoResult
    {
        bool ok;
        unsigned long win32Error;
        long ntStatus;
        std::string message;
        unsigned long bytesReturned;
    };

    class DriverClient
    {
    public:
        IoResult terminateProcess(unsigned long pid, long exitStatus);
        IoResult suspendProcess(unsigned long pid);
        IoResult setProcessProtection(unsigned long pid, unsigned char level);
        ProcessEnumResult enumerateProcesses(unsigned long flags);
        FileDeleteResult deletePath(const std::wstring& path, bool directory);
        SsdtEnumResult enumerateSsdt();
    };
}
```

迁移要求：

- `ProcessDock.cpp` 中的 R0 终止、挂起、PPL、R0 枚举全部迁移到 `ArkDriverClient`。
- `FileDock.cpp` 中的 R0 删除和 R0 终止占用进程迁移到 `ArkDriverClient`。
- `KernelDock` 中 SSDT、callback rule、callback wait/answer/remove 迁移到 `ArkDriverClient`。
- `MainWindow.cpp` 中服务安装/启动/停止可以暂时保留，但日志设备连接建议后续拆到 `ArkDriverServiceManager`。

验证：

- 全局搜索 `DeviceIoControl(`，除 `ArkDriverClient` 和少数非 KswordARK 设备调用外，不应出现在 Dock UI 文件。
- 全局搜索 `CreateFileW(KSWORD_ARK` 或 `\\\\.\\KswordARK`，不应出现在 Dock UI 文件。
- ProcessDock、FileDock、KernelDock 的现有 R0 功能行为不变。

### -1.5 大文件拆分规则

立即拆分目标：

- `ProcessDock.cpp`
- `FileDock.cpp`
- `MainWindow.cpp`
- `MonitorDock.cpp`
- `process_actions.c`
- `ioctl_dispatch.c`

拆分方式：

- 按 Tab 拆 UI 文件。
- 按功能拆 action 文件。
- 按 worker 拆后台任务。
- 按 IOCTL 类别拆驱动 handler。
- 不使用 `.inc` 堆叠。
- 不把新功能继续塞进已有超大文件。

建议目标：

- 新增业务文件小于 800 行。
- 允许复杂 UI 文件暂时超过 1000 行，但新增代码必须进入新文件。
- 中心调度文件只保留调度逻辑。
- 共享协议头按功能拆分，不做单一巨大协议头。

### -1.6 协作 ownership 规则

节假日对齐进度前，建议按以下 owner 分组：

- Owner A：驱动 dispatch registry 和 IOCTL validation。
- Owner B：用户态 `ArkDriverClient`。
- Owner C：DynData third_party 接入和 loader。
- Owner D：KernelDock 能力页和动态偏移 UI。
- Owner E：ProcessDock 清理和进程 R0 调用迁移。
- Owner F：FileDock/HandleDock 清理和文件占用调用迁移。

写入范围：

- Owner A 主要写 `/ark/KSword/KswordARKDriver/src/dispatch`。
- Owner B 主要写 `/ark/KSword/Ksword5.1/Ksword5.1/ArkDriverClient`。
- Owner C 主要写 `/ark/KSword/third_party/systeminformer_dyn` 和 `/ark/KSword/KswordARKDriver/src/features/dyndata`。
- Owner D 主要写 `/ark/KSword/Ksword5.1/Ksword5.1/KernelDock`。
- Owner E 主要写 `/ark/KSword/Ksword5.1/Ksword5.1/ProcessDock`。
- Owner F 主要写 `/ark/KSword/Ksword5.1/Ksword5.1/FileDock` 和 `/ark/KSword/Ksword5.1/Ksword5.1/句柄`。

冲突控制：

- 协议头变更必须先合并。
- `.vcxproj` 变更集中由一个 owner 合并。
- 不同 owner 不同时改同一个 Dock 大文件。
- 驱动新 IOCTL 必须先注册 handler，再实现 feature。
- UI 新功能必须先走 `ArkDriverClient`，不能临时直连驱动。

### -1.7 文档和规范修正

必须更新：

- `/ark/KSword/KswordARKDriver/AGENTS.md`
- `/ark/KSword/Ksword5.1/agents.md`
- `/ark/KSword/README.md`
- 可选新增 `/ark/KSword/CONTRIBUTING.md`

修正内容：

- 把旧的 `Queue.c` 落点改成当前 `src/dispatch` 和 handler registry。
- 把旧 Windows 绝对路径改成 `/ark/KSword` 相对说明。
- 明确 R0/R3 协议只在 `/ark/KSword/shared/driver/`。
- 明确 UI 不直接调用 KswordARK `DeviceIoControl`。
- 明确新增文件必须进 `.vcxproj` 和 `.filters`。
- 明确第三方代码接入必须有 LICENSE 和 NOTICE。

### -1.8 AI 实现提示词

```text
你在 /ark/KSword 中工作。请执行“多人协作前置重构”，目标是降低后续多人并行开发冲突，不改变现有功能行为。

任务：
1. 驱动侧新增 IOCTL handler registry：src/dispatch/ioctl_registry.c/.h、ioctl_validation.c/.h。
2. 将 ioctl_dispatch.c 中现有大 switch 拆到 process_ioctl.c、file_ioctl.c、kernel_ioctl.c、callback_ioctl.c。
3. ioctl_dispatch.c 只负责查表、调用 handler、完成请求和统一日志。
4. 用户态新增 ArkDriverClient 目录，封装 KswordARK 设备打开、DeviceIoControl、错误转换和现有 R0 操作。
5. 将 ProcessDock、FileDock、KernelDock 中直接调用 KswordARK DeviceIoControl 的代码迁移到 ArkDriverClient。
6. 更新 AGENTS.md、agents.md、README 或 CONTRIBUTING，修正 Queue.c 等旧路径。
7. 不改变任何 IOCTL 号、协议结构和现有 UI 行为。
8. 更新 vcxproj 和 filters。
9. 保持 Warning as Error 无警告。

验证：
- 全局搜索 Dock UI 文件中不再直接调用 KswordARK DeviceIoControl。
- 现有进程终止、进程挂起、PPL 修改、R0 进程枚举、文件强删、SSDT、callback 功能仍可用。
- ioctl_dispatch.c 明显变薄，业务逻辑在各 feature ioctl 文件。
```

## 当前结论

System Informer 的动态偏移表是当前最高收益的数据源。原因：

- System Informer 使用 MIT License，可以集成，但必须保留版权和许可证。
- 它的 `kphdyn.xml` 覆盖大量 Windows 10/11 `ntoskrnl.exe` 和部分 `lxcore.sys` 版本，比短期自建 PDB 下载和解析现实得多。
- 它按模块 class、machine、timestamp、size 匹配，比 SKT64 那种“Windows 10 一组、Windows 11 一组”的硬编码可靠。
- 它已经包含 Ksword 后续需要的关键字段，例如 `EPROCESS.ObjectTable`、`KTHREAD` 栈字段、`HANDLE_TABLE_ENTRY` 解码位、`OBJECT_TYPE.Name/Index`、ALPC 字段、Section/ControlArea 字段、部分 WSL/lxcore 字段。

不采用的路线：

- 不全量搬 KSystemInformer 驱动框架。
- 不搬 KPH 通信层、对象系统、session token、trace 框架。
- 不把 SKT64 的硬编码偏移作为正式数据源。
- 不按 build number 粗略匹配偏移。
- 不在内核里下载符号或解析 PDB。
- 不对未匹配内核使用“相邻版本 fallback”。

## Phase 0：System Informer DynData 接入

这是所有基于未公开字段功能的前置阶段。没有 Phase 0，就不要继续做句柄表直接枚举、ALPC、Section、KTHREAD 栈、ControlArea、对象头解码等功能。

### 0.1 交付目标

Phase 0 完成后，Ksword 应具备以下能力：

- 驱动启动时识别当前 `ntoskrnl.exe` 的 machine、timestamp、SizeOfImage。
- 可选识别 `lxcore.sys`，用于后续 WSL 字段。
- 从 vendored System Informer `KphDynConfig` 中匹配当前内核的字段集。
- 将匹配结果转换为 Ksword 自己的 `KSW_DYN_OFFSETS`。
- 对每个字段记录可用状态。
- 对每个依赖偏移的功能生成 capability flag。
- UI 能显示当前 offset profile 是否匹配、匹配来源、字段列表和不可用原因。
- 匹配失败时驱动仍可加载，低风险功能仍可用，高风险功能自动禁用。

### 0.2 第三方代码接入方式

新增目录：

- `/ark/KSword/third_party/systeminformer_dyn/LICENSE.txt`
- `/ark/KSword/third_party/systeminformer_dyn/NOTICE.md`
- `/ark/KSword/third_party/systeminformer_dyn/kphdyn.xml`
- `/ark/KSword/third_party/systeminformer_dyn/kphdyn.c`
- `/ark/KSword/third_party/systeminformer_dyn/kphdyn.h`
- `/ark/KSword/third_party/systeminformer_dyn/README_KSWORD.md`

接入规则：

- 从 `/ark/SystemInformer/LICENSE.txt` 复制 MIT License。
- `NOTICE.md` 写明数据来源、原项目地址、原版权、导入日期、原 commit hash。
- `kphdyn.xml` 保留原始文件，作为可审计数据源。
- `kphdyn.c` 和 `kphdyn.h` 可以做最小改名或包装，但不要混入 Ksword 业务逻辑。
- 如果直接包含 `kphdyn.c`，必须剥离或替换它对 `kphlibbase.h` 的依赖。
- 推荐做法是生成 Ksword 专用包装头 `ksw_si_dynconfig.h`，只暴露 `KphDynConfig`、`KphDynConfigLength` 和必要结构定义。

不允许：

- 不允许直接把 KPH 的全局对象、引用计数对象系统、session token 一起搬进 Ksword。
- 不允许把 System Informer 的 IOCTL 协议混入 Ksword。
- 不允许在 Ksword 驱动内部重复定义 R0/R3 共享协议。
- 不允许为了编译方便删除原版权说明。

### 0.3 Ksword 目标文件布局

共享协议：

- `/ark/KSword/shared/driver/KswordArkDynDataIoctl.h`

驱动侧：

- `/ark/KSword/KswordARKDriver/include/ark/ark_dyndata.h`
- `/ark/KSword/KswordARKDriver/include/ark/ark_dyndata_fields.h`
- `/ark/KSword/KswordARKDriver/src/features/dyndata/dyndata_loader.c`
- `/ark/KSword/KswordARKDriver/src/features/dyndata/dyndata_query.c`
- `/ark/KSword/KswordARKDriver/src/features/dyndata/dyndata_validate.c`
- `/ark/KSword/KswordARKDriver/src/platform/kernel_module_identity.c`

用户态：

- `/ark/KSword/Ksword5.1/Ksword5.1/KernelDock`
- `/ark/KSword/Ksword5.1/Ksword5.1/ArkDriverClient`
- 新增或扩展“动态偏移”诊断页。

工程文件：

- 更新 `KswordARKDriver.vcxproj`
- 更新 `KswordARKDriver.vcxproj.filters`
- 更新 `Ksword5.1.vcxproj`

### 0.4 内核模块身份识别

优先使用 `AuxKlibQueryModuleInformation` 获取内核模块列表。若当前工程不方便引入 AuxKlib，则封装一个平台层，后续可替换为 `ZwQuerySystemInformation(SystemModuleInformation)`。

必须获取：

- 模块名：`ntoskrnl.exe`、可选 `ntkrla57.exe`、可选 `lxcore.sys`
- 模块基址
- SizeOfImage
- PE FileHeader.Machine
- PE FileHeader.TimeDateStamp

匹配键：

- `Class`
- `Machine`
- `TimeDateStamp`
- `SizeOfImage`

匹配策略：

- `ntoskrnl.exe` 必须精确匹配。
- `lxcore.sys` 可选匹配，未加载时 WSL 字段全部不可用。
- 不用 build number 做最终匹配，build number 只用于 UI 展示和日志。
- 不做 hash 时也可以先交付，但后续应在用户态诊断中显示文件 hash。
- 若多个条目匹配，视为数据异常，拒绝激活。

### 0.5 DynData 解析和激活

System Informer 的基础结构来自 `kphdyn.h`：

- `KPH_DYN_CONFIG`
- `KPH_DYN_DATA`
- `KPH_DYN_FIELDS`
- `KPH_DYN_KERNEL_FIELDS`
- `KPH_DYN_LXCORE_FIELDS`

Ksword 内部不要直接暴露 KPH 命名，转换为：

- `KSW_DYN_STATE`
- `KSW_DYN_OFFSETS`
- `KSW_DYN_KERNEL_OFFSETS`
- `KSW_DYN_LXCORE_OFFSETS`
- `KSW_DYN_FIELD_STATUS`

字段转换规则：

- `USHORT` 偏移转换为 `ULONG`。
- `0xffff` 或无字段统一转换为 `KSW_DYN_FIELD_UNAVAILABLE`。
- 每个字段有独立 `Present` bit。
- 每个功能有独立 `Capability` bit。
- 激活后不再修改当前 profile，除非驱动卸载重载。

必须支持的 System Informer 字段：

- `EpObjectTable`
- `EpSectionObject`
- `HtHandleContentionEvent`
- `OtName`
- `OtIndex`
- `ObDecodeShift`
- `ObAttributesShift`
- `KtInitialStack`
- `KtStackLimit`
- `KtStackBase`
- `KtKernelStack`
- `KtReadOperationCount`
- `KtWriteOperationCount`
- `KtOtherOperationCount`
- `KtReadTransferCount`
- `KtWriteTransferCount`
- `KtOtherTransferCount`
- `MmSectionControlArea`
- `MmControlAreaListHead`
- `MmControlAreaLock`
- `AlpcCommunicationInfo`
- `AlpcOwnerProcess`
- `AlpcConnectionPort`
- `AlpcServerCommunicationPort`
- `AlpcClientCommunicationPort`
- `AlpcHandleTable`
- `AlpcHandleTableLock`
- `AlpcAttributes`
- `AlpcAttributesFlags`
- `AlpcPortContext`
- `AlpcPortObjectLock`
- `AlpcSequenceNo`
- `AlpcState`
- `LxPicoProc`
- `LxPicoProcInfo`
- `LxPicoProcInfoPID`
- `LxPicoThrdInfo`
- `LxPicoThrdInfoTID`

### 0.6 Ksword 额外偏移

System Informer 的表不覆盖所有 ARK 常用字段。Ksword 需要单独定义扩展表，但扩展表必须复用同一套精确匹配和 capability gating。

第一批额外字段：

- `EPROCESS.Protection`
- `EPROCESS.SignatureLevel`
- `EPROCESS.SectionSignatureLevel`

来源：

- 优先沿用 Ksword 现有 `PsIsProtectedProcess` / `PsIsProtectedProcessLight` 指令解析逻辑，但把结果缓存进 `KSW_DYN_OFFSETS`，并在 UI 中标记来源为 `runtime pattern`。
- 如果后续从 PDB 或人工表获得稳定数据，再加入 `KswordExtraOffsets`。

第二批额外字段，只有在有可靠数据源后启用：

- `EPROCESS.UniqueProcessId`
- `EPROCESS.ActiveProcessLinks`
- `EPROCESS.Token`
- `EPROCESS.Flags`
- `EPROCESS.VadRoot`
- `EPROCESS.VadCount`
- `ETHREAD.StartAddress`
- `ETHREAD.Win32StartAddress`
- `ETHREAD.ThreadListEntry`
- `ETHREAD.Cid`

这些字段不能从 SKT64 硬编码表直接照搬到正式版。可以用 SKT64 对照验证，但正式表必须带匹配键和来源记录。

### 0.7 能力门控

新增 capability flags：

- `KSW_CAP_DYN_NTOS_ACTIVE`
- `KSW_CAP_DYN_LXCORE_ACTIVE`
- `KSW_CAP_OBJECT_TYPE_FIELDS`
- `KSW_CAP_HANDLE_TABLE_DECODE`
- `KSW_CAP_PROCESS_OBJECT_TABLE`
- `KSW_CAP_THREAD_STACK_FIELDS`
- `KSW_CAP_THREAD_IO_COUNTERS`
- `KSW_CAP_ALPC_FIELDS`
- `KSW_CAP_SECTION_CONTROL_AREA`
- `KSW_CAP_PROCESS_PROTECTION_PATCH`
- `KSW_CAP_WSL_LXCORE_FIELDS`

功能依赖：

- 进程句柄表直接枚举依赖 `EpObjectTable`、`ObDecodeShift`、`ObAttributesShift`、`OtName`、`OtIndex`。
- 线程栈依赖 `KtInitialStack`、`KtStackLimit`、`KtStackBase`、`KtKernelStack`。
- 线程 I/O 计数依赖 KTHREAD I/O counter 字段。
- ALPC 查询依赖 ALPC 字段全组。
- Section 查询依赖 `EpSectionObject`、`MmSectionControlArea`、`MmControlAreaListHead`、`MmControlAreaLock`。
- WSL 查询依赖 lxcore 字段。
- PPL 修改依赖 `EPROCESS.Protection`、`SignatureLevel`、`SectionSignatureLevel`。

门控规则：

- 缺少必需字段则对应功能完全禁用。
- 缺少可选字段则功能降级，UI 必须显示 degraded。
- 任何字段越界、profile 长度异常、重复匹配都拒绝激活。
- 不允许功能代码绕过 DynData 自己读取硬编码偏移。

### 0.8 IOCTL 和 UI 交付

新增 IOCTL：

- `IOCTL_KSWORD_ARK_QUERY_DYN_STATUS`
- `IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS`
- `IOCTL_KSWORD_ARK_QUERY_CAPABILITIES`

`QUERY_DYN_STATUS` 输出：

- 协议版本
- DynData 是否激活
- System Informer data version
- ntoskrnl module identity
- lxcore module identity
- matched profile class
- matched profile offset
- matched fields id
- active capability mask
- unavailable reason

`QUERY_DYN_FIELDS` 输出：

- 字段名或字段 ID
- 偏移值
- Present/Unavailable
- 来源：System Informer、Ksword runtime pattern、Ksword extra table
- 所属功能
- 是否必需

UI 要求：

- KernelDock 增加“动态偏移”页。
- 显示当前内核模块 identity。
- 显示是否命中 System Informer profile。
- 显示字段列表。
- 显示功能依赖关系。
- 对禁用功能给出具体缺失字段。
- 支持复制诊断报告。

### 0.9 验证方法

基础验证：

- 加载驱动后 UI 能看到 DynData 激活状态。
- `ntoskrnl` timestamp/size 与 WinDbg 或工具读取结果一致。
- 字段表中的 `EpObjectTable`、`KtInitialStack`、ALPC 字段与 System Informer `kphdyn.xml` 匹配。
- 修改一份错误的 `KphDynConfigLength` 或构造异常条目时，驱动拒绝激活且不崩溃。

兼容性验证：

- Windows 10 22H2。
- Windows 11 22H2。
- Windows 11 23H2。
- Windows 11 24H2。
- Server 2019 或 Server 2022，如果测试环境可得。
- 未匹配 build 上驱动可加载，但依赖偏移功能禁用。

压力验证：

- 反复打开关闭 UI 动态偏移页。
- 并发调用 `QUERY_DYN_STATUS` 和依赖功能 IOCTL。
- Driver Verifier 下加载、查询、卸载。

交付要求：

- x64 Debug/Release 无警告。
- 所有新增共享结构只在 `/ark/KSword/shared/driver/`。
- `git diff --check` 无空白错误。
- 新增第三方文件带 LICENSE 和 NOTICE。
- UI 对未匹配系统显示清楚，不误导用户以为功能可用。

### 0.10 AI 实现提示词

```text
你在 /ark/KSword 中工作。请为 KswordARKDriver 实现 System Informer DynData 接入的第一阶段。

目标：
1. 将 /ark/SystemInformer/kphlib/kphdyn.xml、kphdyn.c、include/kphdyn.h 以 third_party/systeminformer_dyn 形式引入 Ksword，保留 MIT License 和 NOTICE。
2. 不引入 KPH 通信层、对象系统、session token、trace 框架。
3. 在 /ark/KSword/shared/driver 新增 KswordArkDynDataIoctl.h，定义 QUERY_DYN_STATUS、QUERY_DYN_FIELDS、capability flags 和输出结构。
4. 在驱动中新增 ark_dyndata.h、ark_dyndata_fields.h、dyndata_loader.c、dyndata_query.c、dyndata_validate.c。
5. 启动时识别 ntoskrnl.exe 的 Machine、TimeDateStamp、SizeOfImage，并用 System Informer 的 KphDynConfig 精确匹配字段集。
6. 将 KPH_DYN_KERNEL_FIELDS 和 KPH_DYN_LXCORE_FIELDS 转换为 Ksword 自己的 KSW_DYN_OFFSETS。
7. 每个字段有 Present 状态；每个依赖功能有 capability bit。
8. 匹配失败时驱动仍能加载，但依赖偏移功能 capability 必须为 false。
9. UI 增加动态偏移诊断页，展示内核 identity、profile 命中状态、字段列表、功能可用性和缺失原因。
10. 更新 vcxproj 和 filters，保持 Warning as Error 无警告。

完成后给出文件清单、匹配逻辑说明和手工验证步骤。
```

## Phase 1：驱动能力查询和统一状态页

Phase 1 不再只是低风险能力查询，而是所有后续 UI 和功能的状态中枢。它必须同时展示驱动加载状态、协议版本、安全策略、DynData 状态和功能 capability。

目标功能：

- 查询驱动协议版本。
- 查询安全策略状态。
- 查询 DynData 激活状态。
- 查询所有功能 capability。
- 查询最近一次驱动错误摘要。

UI 呈现：

- 状态栏显示 `Driver Loaded`、`Driver Missing`、`Protocol Mismatch`、`DynData Missing`、`Limited`。
- KernelDock 显示能力矩阵。
- 每个功能项显示 `Available`、`Unavailable`、`Degraded`、`Denied by policy`。
- 点击功能项显示依赖字段，例如 `Phase 4 requires EpObjectTable + ObDecodeShift + OtName`。
- 提供“复制诊断报告”按钮。

验证：

- 驱动未加载时 UI 不崩溃。
- DynData 未命中时能力矩阵正确禁用高风险功能。
- 协议不兼容时不调用危险 IOCTL。

AI 提示词：

```text
实现 Ksword 驱动统一能力查询。能力查询必须合并协议版本、安全策略、DynData 状态和功能 capability。UI 在 KernelDock 显示状态卡片和能力矩阵。任何依赖未公开字段的功能必须通过 capability 判断是否启用。
```

## Phase 2：进程扩展信息和 EPROCESS 字段能力

目标功能：

- 查询 PID、父 PID、Session、短名称、完整镜像路径。
- 查询保护状态和 PPL 信息。
- 查询 `EPROCESS.ObjectTable` 是否存在，用于后续句柄页。
- 查询 `EPROCESS.SectionObject` 是否存在，用于后续 Section 页。
- 对现有 PPL 修改逻辑改造为 DynData/Runtime resolver 统一来源。

实现要求：

- 基础字段仍优先使用公开 API。
- PPL 字段允许使用现有 `PsIsProtectedProcess` 指令解析，但结果要进入 `KSW_DYN_OFFSETS`。
- 如果 `EpObjectTable` 来自 System Informer profile，则进程详情页显示“HandleTable available”。
- 如果 `EpSectionObject` 可用，则进程详情页显示“SectionObject available”。
- 不允许在 process feature 中散落硬编码偏移。

UI 呈现：

- ProcessDock 增加列：保护状态、PPL、HandleTable 可用、SectionObject 可用、R0 状态。
- ProcessDetail 增加“内核对象”页。
- 显示 EPROCESS 相关字段来源：Public API、System Informer DynData、Runtime pattern、Unavailable。
- PPL 修改确认框显示当前 Protection、目标 Protection、SignatureLevel 影响和回滚风险。

验证：

- 普通进程、PPL 进程、系统进程均可打开详情。
- DynData 未命中时不显示句柄表直接枚举入口。
- PPL 修改功能必须显示字段来源和二次确认。

AI 提示词：

```text
改造 Ksword 进程扩展信息。基础信息继续使用公开 API，但 EPROCESS.ObjectTable、SectionObject 和 Protection 相关字段必须从统一 DynData/Runtime resolver 获取。UI 展示字段来源和 capability。禁止在进程功能代码中新增散落硬编码偏移。
```

## Phase 3：线程扩展信息、KTHREAD 栈字段和 I/O 计数

目标功能：

- 查询线程所属进程、状态、优先级、TEB、StartAddress。
- 使用 System Informer KTHREAD 字段读取栈边界和 KernelStack。
- 使用 KTHREAD I/O counter 字段读取线程 I/O 统计。
- 为后续线程栈回溯提供前置能力。

依赖字段：

- `KtInitialStack`
- `KtStackLimit`
- `KtStackBase`
- `KtKernelStack`
- `KtReadOperationCount`
- `KtWriteOperationCount`
- `KtOtherOperationCount`
- `KtReadTransferCount`
- `KtWriteTransferCount`
- `KtOtherTransferCount`

UI 呈现：

- Thread tab 增加列：KernelStack、StackBase、StackLimit、ReadOps、WriteOps、OtherOps、ReadBytes、WriteBytes、OtherBytes。
- 字段不可用时列显示 `Unavailable`，不隐藏整页。
- 点击线程显示字段来源和原始偏移值。
- 地址字段支持复制。

验证：

- DynData 命中时显示 KTHREAD 字段。
- DynData 未命中时线程基础页仍可用，但栈字段和 I/O counter 禁用。
- 快速退出线程不导致蓝屏或 UI 崩溃。

AI 提示词：

```text
为 Ksword 增加线程扩展信息。KTHREAD 栈和 I/O counter 字段必须从 KSW_DYN_OFFSETS 读取，并受 capability 控制。UI 显示线程栈边界、KernelStack 和 I/O counter，不可用字段显示 Unavailable。
```

## Phase 4：进程句柄表直接枚举

这是 DynData 接入后的第一个高收益 ARK 功能。目标是绕过用户态 `NtQuerySystemInformation + DuplicateHandle` 的限制，直接读取目标进程 `EPROCESS.ObjectTable`。

依赖字段：

- `EpObjectTable`
- `HtHandleContentionEvent`
- `ObDecodeShift`
- `ObAttributesShift`
- `OtName`
- `OtIndex`

实现要求：

- 只在 capability 全部满足时启用。
- 枚举过程中必须引用目标进程对象。
- 不返回可被用户态当作凭据使用的内核指针。
- Object 地址可用于展示和差异检测，但不得作为后续危险 IOCTL 的输入凭据。
- 对 handle table entry 解码失败的条目保留错误状态。

UI 呈现：

- HandleDock 增加枚举模式：`User Snapshot`、`DuplicateHandle`、`Kernel HandleTable`。
- 列表显示 Handle、Object、Type、Name、GrantedAccess、Attributes、来源、解码状态。
- 差异视图显示“仅内核可见”、“仅用户态可见”、“两者均可见”。
- 支持按类型、名称、访问掩码、差异状态过滤。
- 动态偏移页显示本功能依赖字段全部状态。

验证：

- 对普通进程和大量句柄进程枚举。
- 与用户态快照对比差异。
- 目标进程退出时安全失败。
- Driver Verifier 下反复刷新。

AI 提示词：

```text
实现进程句柄表直接枚举。必须使用 KSW_DYN_OFFSETS 中的 EpObjectTable、ObDecodeShift、ObAttributesShift、OtName、OtIndex 等字段，并通过 capability gate 控制。UI 增加 Kernel HandleTable 模式和差异视图。禁止使用 SKT64 硬编码偏移。
```

## Phase 5：对象类型、对象名和受限句柄代理

目标功能：

- 查询对象类型名。
- 查询对象名。
- 支持受限内核句柄代理，用于打开被用户态权限挡住的对象。
- 将句柄页、文件占用扫描、进程对象详情统一到同一对象查询层。

依赖字段：

- `OtName`
- `OtIndex`
- `ObDecodeShift`
- `ObAttributesShift`
- Phase 4 的 handle table 解码能力。

安全要求：

- 返回用户态 handle 必须降权。
- 不允许用户传任意内核地址要求打开。
- 所有代理打开操作必须经过安全策略。
- UI 必须显示请求访问掩码和实际授予访问掩码。

UI 呈现：

- HandleDock 中对象类型和对象名优先由 R0 查询。
- 对无名称对象显示 `(unnamed)`。
- 详情页展示 GrantedAccess 解码、ObjectType、ObjectName、查询来源。
- 代理打开失败时显示 NTSTATUS 和策略原因。

验证：

- 对 File、Process、Thread、Key、ALPC Port 等对象查询类型名。
- 对高权限进程句柄做失败和降权测试。
- 确认 handle 不泄漏。

AI 提示词：

```text
实现对象类型、对象名查询和受限内核句柄代理。对象字段来源必须走 DynData，代理打开必须降权并受安全策略控制。UI 显示请求权限、实际权限、对象类型名、对象名和失败原因。
```

## Phase 6：ALPC 查询

目标功能：

- 从句柄页识别 ALPC Port。
- 查询 ALPC Port owner、communication info、server/client/connection port。
- 展示 ALPC 连接关系。

依赖字段：

- `AlpcCommunicationInfo`
- `AlpcOwnerProcess`
- `AlpcConnectionPort`
- `AlpcServerCommunicationPort`
- `AlpcClientCommunicationPort`
- `AlpcHandleTable`
- `AlpcHandleTableLock`
- `AlpcAttributes`
- `AlpcAttributesFlags`
- `AlpcPortContext`
- `AlpcPortObjectLock`
- `AlpcSequenceNo`
- `AlpcState`

UI 呈现：

- 新增 ALPCDock 或在句柄详情中增加 ALPC 专用详情页。
- 显示 Port 名称、Owner PID、Owner 进程、State、Flags、SequenceNo。
- 连接关系用 Server/Client/Connection 三栏展示。
- 点击 PID 跳转 ProcessDetail。
- 字段不可用时显示缺失字段，不显示空白。

验证：

- 查询系统服务进程中的 ALPC Port。
- 从 HandleDock 跳转 ALPC 详情。
- DynData 未命中时 ALPC 页禁用。

AI 提示词：

```text
实现 ALPC 查询。所有 ALPC_PORT 和 ALPC_COMMUNICATION_INFO 字段必须来自 KSW_DYN_OFFSETS。UI 从句柄页识别 ALPC Port，并展示 owner、state、flags、server/client/connection 关系。
```

## Phase 7：Section、ControlArea 和映射关系

目标功能：

- 查询进程 `EPROCESS.SectionObject`。
- 查询 Section 的 ControlArea。
- 展示文件映射、映像映射和进程映射关系。
- 为“文件被哪些进程映射”提供内核辅助。

依赖字段：

- `EpSectionObject`
- `MmSectionControlArea`
- `MmControlAreaListHead`
- `MmControlAreaLock`

UI 呈现：

- ProcessDetail 内存页增加 Section 子页。
- FileDock 增加“映射进程”按钮。
- 显示 SectionObject、ControlArea、文件路径、映射类型、进程 PID、基址、大小。
- ControlArea 地址标注“仅诊断展示”。

验证：

- 查询 DLL 映像映射。
- 查询内存映射文件。
- 对文件反查映射进程。
- DynData 未命中时只显示用户态可得的内存映射信息。

AI 提示词：

```text
实现 Section 和 ControlArea 查询。必须使用 EpSectionObject、MmSectionControlArea、MmControlAreaListHead、MmControlAreaLock capability。UI 支持从进程内存区域跳转 Section，从文件反查映射进程。
```

## Phase 8：线程栈回溯

目标功能：

- 捕获用户栈。
- 捕获或辅助展示内核栈边界。
- 对每个栈帧解析模块和符号。

依赖字段：

- Phase 3 的 KTHREAD 栈字段。
- 用户态符号解析模块。

UI 呈现：

- ThreadDetail 增加“调用栈”页。
- 列表显示帧序号、地址、模块、符号、偏移。
- 无符号时显示 `module+offset`。
- 支持复制完整调用栈。
- 显示捕获模式：User、Kernel boundary、Mixed、Unavailable。

验证：

- 对等待线程、忙线程、GUI 线程捕获栈。
- 关闭符号路径时仍显示地址和模块。
- 线程退出时安全失败。

AI 提示词：

```text
实现线程栈回溯 UI 和内核辅助。KTHREAD 栈边界来自 KSW_DYN_OFFSETS。UI 显示帧、模块、符号和字段来源，符号不可用时降级为 module+offset。
```

## Phase 9：驱动对象和设备对象查询

目标功能：

- 查询 DriverObject。
- 查询 MajorFunction 表。
- 查询 DeviceObject 链。
- 查询 AttachedDevice 栈。

这部分不强依赖 DynData，但必须纳入统一 capability 和状态页。

UI 呈现：

- DriverDock 增加对象信息页。
- MajorFunction 表显示 IRP 名称、函数地址、所属模块、是否落在自身镜像内。
- DeviceObject 页显示设备名、类型、Flags、Characteristics、StackSize、Attached chain。
- 外部 dispatch 地址用文本标记，不只用颜色。

验证：

- 查询普通驱动、文件系统驱动、无卸载例程驱动。
- 查询有过滤栈的设备。
- 驱动卸载后刷新安全失败。

AI 提示词：

```text
实现 DriverObject 和 DeviceObject 查询。虽然不强依赖 DynData，但必须接入统一 capability 状态和 UI 错误展示。MajorFunction 地址需要识别所属模块。
```

## Phase 10：文件对象信息、文件占用和哈希

目标功能：

- R0 查询文件基础信息。
- R0 或用户态流式计算 SHA256。
- 结合对象查询和句柄表枚举做文件占用扫描。

UI 呈现：

- FileDock 显示 NT 路径、Win32 路径、大小、时间戳、属性、查询来源。
- 哈希页显示 SHA256、耗时、读取速度、是否取消。
- 文件占用页显示占用进程、PID、Handle、GrantedAccess、枚举来源。
- 对大文件计算提供取消按钮。

验证：

- 普通文件、目录、系统文件、被占用文件。
- 哈希结果与 PowerShell `Get-FileHash` 对比。
- 使用 Phase 4 kernel handle table 模式提升占用扫描能力。

AI 提示词：

```text
实现文件基础信息、文件哈希和文件占用扫描整合。文件占用扫描优先使用 Kernel HandleTable capability，失败时回退用户态 DuplicateHandle。UI 显示来源和失败原因。
```

## Phase 11：进程内存读取和内存区域查询

目标功能：

- R0 读取进程内存。
- 查询内存区域。
- 与 Section 查询联动。

UI 呈现：

- MemoryDock 显示 Hex、ASCII、UTF-16。
- 显示地址、长度、实际读取字节数、保护属性、区域类型、来源。
- 支持复制选中字节。
- 部分读取显示 `Partial copy`。

安全要求：

- 第一版只读，不做写入、分配、保护修改。
- 对系统关键进程需要二次确认。
- 读取失败不弹窗刷屏，在视图内显示错误。

验证：

- 读取普通进程可读页、不可读页、模块映像区域。
- 目标进程退出时安全失败。
- 与用户态 ReadProcessMemory 对比。

AI 提示词：

```text
实现只读进程内存读取。UI 提供 Hex 视图和区域信息。第一版不做写入。读取失败在视图内显示 NTSTATUS，并支持用户态 fallback。
```

## Phase 12：文件系统 minifilter 实时事件

目标功能：

- 注册 minifilter。
- 捕获 Create、Read、Write、SetInfo、Rename、Delete、Cleanup、Close。
- 支持按进程、路径、扩展名、操作类型过滤。

UI 呈现：

- 新增 FileMonitorDock。
- 事件列：时间、进程、PID、操作、路径、结果、访问掩码、来源。
- 支持开始、暂停、清空、导出 CSV/JSON。
- 高频事件列表必须虚拟化。
- 状态栏显示 dropped count。

验证：

- 文件创建、写入、重命名、删除。
- 大量事件压测 UI 不冻结。
- 驱动缓冲区满时 UI 提示缩小过滤范围。

AI 提示词：

```text
实现文件系统 minifilter 实时事件。UI 必须虚拟化列表，支持过滤和导出。驱动事件缓冲区满时返回 dropped count，UI 明确展示。
```

## Phase 13：WSL 和 Silo 信息

目标功能：

- 检测 lxcore 是否加载。
- 如果 System Informer lxcore profile 命中，读取 Pico process/thread 信息。
- 展示 Windows PID/TID 与 Linux PID/TID 映射。

依赖字段：

- `LxPicoProc`
- `LxPicoProcInfo`
- `LxPicoProcInfoPID`
- `LxPicoThrdInfo`
- `LxPicoThrdInfoTID`

UI 呈现：

- ProcessDock 增加 WSL/Silo 列。
- ProcessDetail 增加“容器/Silo”页。
- 显示是否 Pico process、Linux PID、Linux TID、字段来源。
- WSL 未启用时显示 `WSL components not loaded`。

验证：

- 未启用 WSL 的系统显示空状态。
- 启动 WSL 后显示映射。
- lxcore profile 未命中时字段禁用。

AI 提示词：

```text
实现 WSL/Pico 信息展示。只有 lxcore DynData 命中时启用 Linux PID/TID 字段。UI 对未启用 WSL 和未命中 profile 分别显示不同原因。
```

## Phase 14：镜像信任、签名和 CI 信息

目标功能：

- 文件 Authenticode 验证。
- 驱动和模块签名状态展示。
- 可选探索 Code Integrity 内核信息。

UI 呈现：

- DriverDock 和模块页增加签名状态列。
- FileDock 哈希页增加签名区域。
- 显示 SHA256、签名主体、颁发者、证书有效期、Catalog 状态、验证来源。
- 区分“当前文件签名状态”和“内核加载时信任状态”。

验证：

- 微软签名驱动、第三方签名驱动、未签名测试文件。
- 文件被删除或替换时显示状态差异。
- 离线环境吊销检查显示 Unknown，不误判 Invalid。

AI 提示词：

```text
实现镜像签名和信任信息展示。优先用户态 WinVerifyTrust/AuthentiCode，后续再考虑 CI 内核信息。UI 必须区分文件当前签名状态和内核加载时信任状态。
```

## Phase 15：危险操作统一治理

目标功能：

- 统一治理进程终止、挂起、恢复、PPL 修改、线程终止、句柄关闭、驱动卸载等危险操作。
- 所有危险操作必须走安全策略、二次确认、日志记录。

UI 呈现：

- 高级模式开关。
- 危险操作确认框显示目标、PID/TID、操作、风险、是否可逆、字段来源。
- 需要输入目标名称确认高风险操作。
- 操作结果写入日志面板。

驱动要求：

- 安全策略集中实现。
- 禁止每个 feature 自己临时判断权限。
- 对系统关键进程默认拒绝或要求更高级确认。

验证：

- 对普通测试进程执行终止、挂起、恢复。
- 对 PPL 修改显示字段来源和回滚风险。
- 对关键系统进程验证 UI 和驱动双层拦截。

AI 提示词：

```text
为 Ksword 所有危险操作实现统一治理。UI 增加高级模式和确认框；驱动集中执行安全策略。所有危险操作必须记录日志并显示 NTSTATUS。
```

## Phase 16：发布前交付门槛

功能完成不等于可以上架。Ksword ARK 至少满足以下条件才进入公开发布候选：

驱动发布要求：

- x64 Debug/Release 无警告。
- Driver Verifier 基础场景通过。
- 加载、卸载、重复打开关闭 UI 不崩溃。
- 未匹配 DynData profile 时驱动仍可加载。
- 所有内核对象引用都有释放。
- 所有 IOCTL 校验 size、version、输入输出长度。
- 所有危险操作有安全策略。

兼容性要求：

- Windows 10 22H2。
- Windows 11 22H2。
- Windows 11 23H2。
- Windows 11 24H2。
- 至少一个 Server 版本，如果目标用户包括服务器。
- HVCI 开启/关闭各测试一次。

UI 发布要求：

- 所有依赖 R0 的页面都有 unavailable/degraded 状态。
- 不弹窗轰炸。
- 所有表格关键字段可复制。
- 所有长耗时操作可取消或后台执行。
- 诊断报告可复制。

分发要求：

- 明确驱动签名方案。
- 明确安装、启动、停止、卸载流程。
- 明确崩溃回滚方法。
- 明确第三方许可证 NOTICE。
- 明确隐私和风险说明。

## 删除或废弃的旧计划内容

以下旧路线不再保留：

- “第一阶段尽量不碰私有结构”的策略。
- “进程扩展信息先不做 EPROCESS 字段”的限制。
- “句柄表、ALPC、Section 等放到很后面再考虑”的保守排序。
- “动态偏移框架作为 Phase 13 才做”的旧安排。
- “SKT64 偏移表可作为候选数据源”的表述。
- “按 Windows 10/11 大版本硬编码偏移”的路线。
- “无法匹配时猜测相近 build”的任何 fallback。

## 总体执行顺序

推荐顺序：

1. Phase -1：多人协作前置重构。
2. Phase 0：System Informer DynData 接入。
3. Phase 1：驱动能力查询和统一状态页。
4. Phase 2：进程扩展信息和 EPROCESS 字段能力。
5. Phase 4：进程句柄表直接枚举。
6. Phase 5：对象类型、对象名和受限句柄代理。
7. Phase 3：线程扩展信息、KTHREAD 栈字段和 I/O 计数。
8. Phase 8：线程栈回溯。
9. Phase 7：Section、ControlArea 和映射关系。
10. Phase 6：ALPC 查询。
11. Phase 9：驱动对象和设备对象查询。
12. Phase 10：文件对象信息、文件占用和哈希。
13. Phase 11：进程内存读取和内存区域查询。
14. Phase 12：文件系统 minifilter 实时事件。
15. Phase 13：WSL 和 Silo 信息。
16. Phase 14：镜像信任、签名和 CI 信息。
17. Phase 15：危险操作统一治理。
18. Phase 16：发布前交付门槛。

前六项是新的最小高价值里程碑。Phase -1 完成后，协作者才能安全并行；Phase 0 到 Phase 5 完成后，Ksword ARK 会具备真正区别于普通用户态工具的核心能力。

## 通用验收清单

代码层：

- 新增功能必须有明确 owner 和写入范围。
- Dock UI 文件不直接调用 KswordARK `DeviceIoControl`。
- KswordARK R0 调用统一通过 `ArkDriverClient`。
- 驱动新 IOCTL 统一通过 handler registry 注册。
- R0/R3 协议只在 `/ark/KSword/shared/driver/`。
- 驱动 include 不重复定义共享结构。
- 所有 IOCTL 校验 `size`、`version`、输入输出长度。
- 所有内核对象引用都有释放。
- 所有字符串输出保证 NUL 结尾。
- 输出缓冲不足时返回明确错误或所需长度。
- 不吞掉 NTSTATUS。
- 不使用散落硬编码偏移。
- 不把内核地址作为用户态操作凭据。

DynData 层：

- `ntoskrnl` profile 必须精确匹配。
- 字段缺失必须反映到 capability。
- 未命中 profile 时驱动可加载。
- 未命中 profile 时依赖私有字段功能禁用。
- 所有字段来源可在 UI 查询。
- 第三方 DynData 更新有 NOTICE 和版本记录。

UI 层：

- 每个功能显示来源：Kernel、User-mode fallback、System Informer DynData、Runtime pattern、Unavailable。
- Unknown、Unavailable、Degraded、Denied 都有明确文本。
- 表格关键字段可复制。
- 危险操作二次确认。
- 长耗时操作不阻塞主 UI。
- 错误显示用户可读信息和原始 NTSTATUS。

测试层：

- 普通路径测试。
- 权限不足测试。
- 目标退出/对象释放并发测试。
- 缓冲区过小测试。
- DynData 未命中测试。
- Driver Verifier 基础路径测试。
