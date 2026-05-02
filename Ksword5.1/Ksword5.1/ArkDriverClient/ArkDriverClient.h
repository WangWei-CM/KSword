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
        FileInfoQueryResult queryFileInfo(const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL) const;
        FileInfoQueryResult queryFileInfo(DriverHandle& handle, const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL) const;
        IoResult controlFileMonitor(unsigned long action, unsigned long operationMask = KSWORD_ARK_FILE_MONITOR_OPERATION_ALL, unsigned long processId = 0UL, unsigned long flags = 0UL) const;
        FileMonitorStatusResult queryFileMonitorStatus() const;
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
        KernelInlineHookScanResult scanInlineHooks(unsigned long flags = 0UL, unsigned long maxEntries = KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES, const std::wstring& moduleName = std::wstring()) const;
        KernelInlinePatchResult patchInlineHook(std::uint64_t functionAddress, unsigned long mode, unsigned long patchBytes, const std::vector<std::uint8_t>& expectedCurrentBytes, const std::vector<std::uint8_t>& restoreBytes = std::vector<std::uint8_t>(), unsigned long flags = 0UL) const;
        KernelIatEatHookScanResult enumerateIatEatHooks(unsigned long flags = KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS, unsigned long maxEntries = KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES, const std::wstring& moduleName = std::wstring()) const;
        DriverObjectQueryResult queryDriverObject(const std::wstring& driverName, unsigned long flags = KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL, unsigned long maxDevices = KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT, unsigned long maxAttachedDevices = KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT) const;
        DriverForceUnloadResult forceUnloadDriver(const std::wstring& driverName, unsigned long flags = 0UL, unsigned long timeoutMilliseconds = 3000UL) const;
        DriverForceUnloadResult forceUnloadDriverByModuleBase(std::uint64_t moduleBase, const std::wstring& fallbackDriverName = std::wstring(), unsigned long flags = KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP, unsigned long timeoutMilliseconds = 3000UL) const;
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
        CallbackEnumResult enumerateCallbacks(unsigned long flags = KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL) const;
        DriverCapabilitiesQueryResult queryDriverCapabilities() const;
        DynDataStatusResult queryDynDataStatus() const;
        DynDataFieldsResult queryDynDataFields() const;
        DynDataCapabilitiesResult queryDynDataCapabilities() const;
    };
}
