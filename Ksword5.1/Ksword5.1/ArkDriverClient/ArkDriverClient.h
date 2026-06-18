#pragma once

#include "ArkDriverCapabilities.h"
#include "ArkDriverTypes.h"

namespace ksword::ark
{
    // DriverClient centralizes all KswordARK control-device access. Docks should
    // call this class instead of opening \\.\KswordARKLog or invoking
    // DeviceIoControl directly.
    class DriverClient
    {
    public:
        DriverClient() = default;

        // Open one synchronous control handle. The returned handle owns CloseHandle.
        DriverHandle open(unsigned long desiredAccess = GENERIC_READ | GENERIC_WRITE) const;

        // Open one overlapped control handle for wait-style callback receivers.
        DriverHandle openOverlapped(unsigned long desiredAccess = GENERIC_READ | GENERIC_WRITE) const;

        // Low-level synchronous IOCTL helper used by narrow advanced UI paths.
        IoResult deviceIoControl(
            unsigned long ioControlCode,
            void* inputBuffer,
            unsigned long inputBytes,
            void* outputBuffer,
            unsigned long outputBytes,
            DriverHandle* existingHandle = nullptr) const;

        // Low-level overlapped IOCTL helper for callback event waiting.
        AsyncIoResult deviceIoControlAsync(
            DriverHandle& handle,
            unsigned long ioControlCode,
            void* inputBuffer,
            unsigned long inputBytes,
            void* outputBuffer,
            unsigned long outputBytes,
            OVERLAPPED* overlapped) const;

        IoResult terminateProcess(std::uint32_t processId, long exitStatus) const;
        IoResult terminateProcess(DriverHandle& handle, std::uint32_t processId, long exitStatus) const;
        IoResult suspendProcess(std::uint32_t processId) const;
        IoResult setProcessProtection(std::uint32_t processId, std::uint8_t protectionLevel) const;
        ProcessVisibilityResult setProcessVisibility(std::uint32_t processId, unsigned long action, unsigned long flags = 0UL) const;
        ProcessSpecialFlagsResult setProcessSpecialFlags(std::uint32_t processId, unsigned long action, unsigned long flags = 0UL) const;
        ProcessDkomResult dkomProcess(std::uint32_t processId, unsigned long action, unsigned long flags = 0UL) const;

        ProcessEnumResult enumerateProcesses(unsigned long flags) const;
        ThreadEnumResult enumerateThreads(unsigned long flags, std::uint32_t processId = 0) const;
        HandleEnumResult enumerateProcessHandles(std::uint32_t processId, unsigned long flags = KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL) const;
        HandleObjectQueryResult queryHandleObject(std::uint32_t processId, std::uint64_t handleValue, unsigned long flags = KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL, unsigned long requestedAccess = 0) const;
        AlpcPortQueryResult queryAlpcPort(std::uint32_t processId, std::uint64_t handleValue, unsigned long flags = KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_ALL) const;
        ProcessSectionQueryResult queryProcessSection(std::uint32_t processId, unsigned long flags = KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_ALL, unsigned long maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT) const;
        FileSectionMappingsQueryResult queryFileSectionMappings(const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL, unsigned long maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT) const;
        VirtualMemoryReadResult readVirtualMemory(std::uint32_t processId, std::uint64_t baseAddress, std::uint32_t bytesToRead, unsigned long flags = KSWORD_ARK_MEMORY_READ_FLAG_ZERO_FILL_UNREADABLE) const;
        VirtualMemoryWriteResult writeVirtualMemory(std::uint32_t processId, std::uint64_t baseAddress, const std::vector<std::uint8_t>& bytes, unsigned long flags = 0UL) const;
        // queryKernelMemoryEvidence：
        // - 输入：只读采集 flags、行数/字节预算和可选地址半开区间。
        // - 处理：封装 IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE，解析变长 evidence rows。
        // - 返回：KernelMemoryEvidenceResult；旧驱动/能力缺失时 unsupported=true，调用方显示 graceful message。
        KernelMemoryEvidenceResult queryKernelMemoryEvidence(
            unsigned long flags = KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_ALL,
            unsigned long maxRows = KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_ROWS,
            std::uint64_t startAddress = 0,
            std::uint64_t endAddress = 0,
            std::uint64_t maxBytes = KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_BYTES,
            unsigned long maxBigPoolRows = KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_BIGPOOL_ROWS,
            unsigned long sampleBytes = KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_SAMPLE_BYTES) const;
        FileInfoQueryResult queryFileInfo(const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL) const;
        FileInfoQueryResult queryFileInfo(DriverHandle& handle, const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL) const;
        IoResult controlFileMonitor(unsigned long action, unsigned long operationMask = KSWORD_ARK_FILE_MONITOR_OPERATION_ALL, unsigned long processId = 0UL, unsigned long flags = 0UL) const;
        FileMonitorStatusResult queryFileMonitorStatus() const;
        FileMonitorDrainResult drainFileMonitor(unsigned long maxEvents = 128UL, unsigned long flags = 0UL) const;
        RegistryReadResult readRegistryValue(const std::wstring& kernelKeyPath, const std::wstring& valueName, unsigned long maxDataBytes = KSWORD_ARK_REGISTRY_DATA_MAX_BYTES) const;
        RegistryEnumResult enumerateRegistryKey(const std::wstring& kernelKeyPath, unsigned long flags = KSWORD_ARK_REGISTRY_ENUM_FLAG_INCLUDE_SUBKEYS | KSWORD_ARK_REGISTRY_ENUM_FLAG_INCLUDE_VALUES) const;
        RegistryOperationResult setRegistryValue(const std::wstring& kernelKeyPath, const std::wstring& valueName, std::uint32_t valueType, const std::vector<std::uint8_t>& data) const;
        RegistryOperationResult deleteRegistryValue(const std::wstring& kernelKeyPath, const std::wstring& valueName) const;
        RegistryOperationResult createRegistryKey(const std::wstring& kernelKeyPath) const;
        RegistryOperationResult deleteRegistryKey(const std::wstring& kernelKeyPath) const;
        RegistryOperationResult renameRegistryValue(const std::wstring& kernelKeyPath, const std::wstring& oldValueName, const std::wstring& newValueName) const;
        RegistryOperationResult renameRegistryKey(const std::wstring& kernelKeyPath, const std::wstring& newKeyName) const;
        IoResult deletePath(const std::wstring& ntPath, bool isDirectory) const;
        IoResult deletePath(DriverHandle& handle, const std::wstring& ntPath, bool isDirectory) const;

        SsdtEnumResult enumerateSsdt(unsigned long flags) const;
        SsdtEnumResult enumerateShadowSsdt(unsigned long flags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED) const;
        // queryProcessCrossView：
        // - 输入：进程 cross-view 采集 flags、PID 半开/闭合过滤和节点预算。
        // - 处理：只通过 ArkDriverClient 调用 R0，不让 Dock 直接 DeviceIoControl。
        // - 返回：ProcessCrossViewResult，包含 source matrix、anomaly flags 和 DynData 缺口。
        ProcessCrossViewResult queryProcessCrossView(
            unsigned long flags = KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ALL,
            std::uint32_t startPid = 0,
            std::uint32_t endPid = 0,
            unsigned long maxNodes = KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES) const;
        // queryThreadCrossView：
        // - 输入：线程 cross-view 采集 flags、可选 PID/TID 过滤和节点预算。
        // - 处理：解析 ETHREAD/KTHREAD 来源矩阵，只读展示线程 DKOM 证据。
        // - 返回：ThreadCrossViewResult；不返回可用于写操作的对象凭据。
        ThreadCrossViewResult queryThreadCrossView(
            unsigned long flags = KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_ALL,
            std::uint32_t processId = 0,
            std::uint32_t startTid = 0,
            std::uint32_t endTid = 0,
            unsigned long maxNodes = KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES) const;
        KernelInlineHookScanResult scanInlineHooks(unsigned long flags = 0UL, unsigned long maxEntries = KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES, const std::wstring& moduleName = std::wstring()) const;
        // scanKernelExecutableMemory：
        // - 作用：调用 Prompt 1 定义的内核可执行页扫描 IOCTL，并解析变长响应为 R3 模型。
        // - 参数 flags：扫描开关位，通常由 UI 传入 INCLUDE_ALL。
        // - 参数 maxEntries：单次最大返回条数，0 表示使用默认值。
        // - 参数 modulePathFilter：R3 预留筛选参数，当前由 UI 在本地完成过滤。
        // - 返回：KernelExecutableMemoryScanResult，io.ok 表示传输和协议解析成功。
        KernelExecutableMemoryScanResult scanKernelExecutableMemory(unsigned long flags = 0UL, unsigned long maxEntries = 4096UL, const std::wstring& modulePathFilter = std::wstring()) const;
        KernelInlinePatchResult patchInlineHook(std::uint64_t functionAddress, unsigned long mode, unsigned long patchBytes, const std::vector<std::uint8_t>& expectedCurrentBytes, const std::vector<std::uint8_t>& restoreBytes = std::vector<std::uint8_t>(), unsigned long flags = 0UL) const;
        KernelIatEatHookScanResult enumerateIatEatHooks(unsigned long flags = KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS, unsigned long maxEntries = KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES, const std::wstring& moduleName = std::wstring()) const;
        DriverObjectQueryResult queryDriverObject(const std::wstring& driverName, unsigned long flags = KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL, unsigned long maxDevices = KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT, unsigned long maxAttachedDevices = KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT) const;
        // queryDriverIntegrity：
        // - 输入：可选 DriverObject 名称、模块基址和采集预算。
        // - 处理：调用统一驱动完整性 IOCTL，聚合 DriverObject/LDR/FastIo/CPU/IDT 证据。
        // - 返回：DriverIntegrityResult；unsupported=true 表示 R0 尚未集成或驱动过旧。
        DriverIntegrityResult queryDriverIntegrity(
            const std::wstring& driverName = std::wstring(),
            std::uint64_t targetModuleBase = 0,
            unsigned long flags = KSWORD_ARK_DRIVER_INTEGRITY_FLAG_DEFAULT,
            unsigned long maxRows = KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS,
            unsigned long maxIdtVectorsPerCpu = KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS) const;
        // queryKernelCpuIntegrity：
        // - 输入：CPU/IDT 采集 flags 与预算。
        // - 处理：复用 queryDriverIntegrity 的协议，只请求 CPU entry evidence。
        // - 返回：DriverIntegrityResult；不执行任何 MSR/IDT/GDT 写操作。
        DriverIntegrityResult queryKernelCpuIntegrity(
            unsigned long flags = KSWORD_ARK_DRIVER_INTEGRITY_FLAG_CPU | KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES,
            unsigned long maxRows = KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS,
            unsigned long maxIdtVectorsPerCpu = KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS) const;
        // queryCpuHardwareSnapshot：
        // - 输入：无；R0 只执行 CPUID 与处理器数量查询。
        // - 处理：封装 IOCTL_KSWORD_ARK_QUERY_CPU_HARDWARE，解析 vendor/brand/family/model/feature mask。
        // - 返回：CpuHardwareSnapshotResult；旧驱动未注册 IOCTL 时 unsupported=true。
        CpuHardwareSnapshotResult queryCpuHardwareSnapshot() const;
        // queryPhysicalMemoryLayout：
        // - 输入：无；R0 只读取 MmGetPhysicalMemoryRanges 聚合结果。
        // - 处理：封装 IOCTL_KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT，解析物理内存范围统计。
        // - 返回：PhysicalMemoryLayoutResult；不返回任何内存内容。
        PhysicalMemoryLayoutResult queryPhysicalMemoryLayout() const;
        DriverForceUnloadResult forceUnloadDriver(const std::wstring& driverName, unsigned long flags = 0UL, unsigned long timeoutMilliseconds = 3000UL) const;
        DriverForceUnloadResult forceUnloadDriverByModuleBase(std::uint64_t moduleBase, const std::wstring& fallbackDriverName = std::wstring(), unsigned long flags = 0UL, unsigned long timeoutMilliseconds = 3000UL) const;
        // prepareMutation / commitMutation / rollbackMutation / queryMutationAudit：
        // - 输入：受控 transaction 参数或只读 audit 查询参数。
        // - 处理：仅在 ArkDriverClient 内封装 mutation IOCTL；Dock UI 不直接调用 DeviceIoControl。
        // - 返回：固定响应或 audit rows；UI 只能展示 dry-run/audit/rollback，不暴露任意写按钮。
        MutationResponseResult prepareMutation(const MutationPrepareInput& input) const;
        MutationResponseResult commitMutation(std::uint64_t transactionId, unsigned long flags = KSWORD_ARK_MUTATION_FLAG_DRY_RUN) const;
        MutationResponseResult rollbackMutation(std::uint64_t transactionId, unsigned long flags = KSWORD_ARK_MUTATION_FLAG_DRY_RUN) const;
        MutationAuditResult queryMutationAudit(unsigned long flags = 0, unsigned long maxEntries = KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY, std::uint64_t startSequence = 0) const;
        IoResult setCallbackRules(const void* blobBytes, unsigned long blobSize) const;
        AsyncIoResult waitCallbackEventAsync(
            DriverHandle& handle,
            KSWORD_ARK_CALLBACK_WAIT_REQUEST& request,
            KSWORD_ARK_CALLBACK_EVENT_PACKET& eventPacket,
            OVERLAPPED* overlapped) const;
        CallbackRuntimeResult queryCallbackRuntimeState() const;
        IoResult answerCallbackEvent(const KSWORD_ARK_CALLBACK_ANSWER_REQUEST& request) const;
        IoResult cancelAllPendingCallbackDecisions() const;
        CallbackRemoveResult removeExternalCallback(const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST& request) const;
        CallbackRemoveExResult removeExternalCallbackEx(const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST& request) const;
        bool supportsExternalCallbackExperimentalUnlink() const;
        CallbackEnumResult enumerateCallbacks(unsigned long flags = KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL) const;
        KeyboardHotkeyEnumResult enumerateKeyboardHotkeys(std::uint32_t processId = 0, unsigned long flags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM | KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS, unsigned long maxEntries = 2048UL) const;
        KeyboardHookEnumResult enumerateKeyboardHooks(std::uint32_t processId = 0, unsigned long flags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS | KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS | KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS, unsigned long maxEntries = 2048UL) const;
        DriverCapabilitiesQueryResult queryDriverCapabilities() const;
        DynDataStatusResult queryDynDataStatus() const;
        DynDataFieldsResult queryDynDataFields() const;
        DynDataCapabilitiesResult queryDynDataCapabilities() const;
        DynDataProfileApplyResult applyDynDataProfile(const DynDataProfileApplyInput& profile) const;
        DynDataProfileApplyExResult applyDynDataProfileEx(const DynDataProfileApplyExInput& profile) const;
    };
}
