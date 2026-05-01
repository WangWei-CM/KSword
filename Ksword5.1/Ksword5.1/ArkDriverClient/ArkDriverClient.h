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

        ProcessEnumResult enumerateProcesses(unsigned long flags) const;
        ThreadEnumResult enumerateThreads(unsigned long flags, std::uint32_t processId = 0) const;
        HandleEnumResult enumerateProcessHandles(std::uint32_t processId, unsigned long flags = KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL) const;
        HandleObjectQueryResult queryHandleObject(std::uint32_t processId, std::uint64_t handleValue, unsigned long flags = KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL, unsigned long requestedAccess = 0) const;
        AlpcPortQueryResult queryAlpcPort(std::uint32_t processId, std::uint64_t handleValue, unsigned long flags = KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_ALL) const;
        ProcessSectionQueryResult queryProcessSection(std::uint32_t processId, unsigned long flags = KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_ALL, unsigned long maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT) const;
        FileSectionMappingsQueryResult queryFileSectionMappings(const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL, unsigned long maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT) const;
        FileInfoQueryResult queryFileInfo(const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL) const;
        FileInfoQueryResult queryFileInfo(DriverHandle& handle, const std::wstring& ntPath, unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL) const;
        IoResult deletePath(const std::wstring& ntPath, bool isDirectory) const;
        IoResult deletePath(DriverHandle& handle, const std::wstring& ntPath, bool isDirectory) const;

        SsdtEnumResult enumerateSsdt(unsigned long flags) const;
        DriverObjectQueryResult queryDriverObject(const std::wstring& driverName, unsigned long flags = KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL, unsigned long maxDevices = KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT, unsigned long maxAttachedDevices = KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT) const;
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
        DriverCapabilitiesQueryResult queryDriverCapabilities() const;
        DynDataStatusResult queryDynDataStatus() const;
        DynDataFieldsResult queryDynDataFields() const;
        DynDataCapabilitiesResult queryDynDataCapabilities() const;
    };
}
