#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../../shared/KswordArkLogProtocol.h"
#include "../../../shared/driver/KswordArkCallbackIoctl.h"
#include "../../../shared/driver/KswordArkCapabilityIoctl.h"
#include "../../../shared/driver/KswordArkDynDataIoctl.h"
#include "../../../shared/driver/KswordArkFileIoctl.h"
#include "../../../shared/driver/KswordArkKernelIoctl.h"
#include "../../../shared/driver/KswordArkProcessIoctl.h"

namespace ksword::ark
{
    // IoResult is the common outcome for every KswordARK driver operation.
    // ok mirrors the Win32 DeviceIoControl success bit, win32Error preserves
    // GetLastError(), ntStatus is filled only when a response packet carries it.
    struct IoResult
    {
        bool ok = false;
        unsigned long win32Error = ERROR_SUCCESS;
        long ntStatus = 0;
        std::string message;
        unsigned long bytesReturned = 0;
    };

    // DriverHandle owns one KswordARK control-device handle. It is move-only so
    // UI code can cache handles without duplicating close responsibility.
    class DriverHandle
    {
    public:
        DriverHandle() noexcept = default;
        explicit DriverHandle(HANDLE handleValue) noexcept;
        ~DriverHandle();

        DriverHandle(const DriverHandle&) = delete;
        DriverHandle& operator=(const DriverHandle&) = delete;
        DriverHandle(DriverHandle&& other) noexcept;
        DriverHandle& operator=(DriverHandle&& other) noexcept;

        bool isValid() const noexcept;
        HANDLE native() const noexcept;
        HANDLE release() noexcept;
        void reset(HANDLE newHandle = INVALID_HANDLE_VALUE) noexcept;

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

    // ProcessEntry is a normalized, UI-friendly view of one R0 process row.
    struct ProcessEntry
    {
        std::uint32_t processId = 0;
        std::uint32_t parentProcessId = 0;
        std::uint32_t flags = 0;
        std::uint32_t sessionId = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t r0Status = KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE;
        std::uint32_t sessionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint8_t protection = 0;
        std::uint8_t signatureLevel = 0;
        std::uint8_t sectionSignatureLevel = 0;
        std::uint32_t protectionSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t signatureLevelSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t sectionSignatureLevelSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t objectTableSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t sectionObjectSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t imagePathSource = KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t protectionOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t signatureLevelOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t sectionSignatureLevelOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t objectTableOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t sectionObjectOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint64_t objectTableAddress = 0;
        std::uint64_t sectionObjectAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::string imageName;
        std::string imagePath;
    };

    // ProcessEnumResult carries both the parsed rows and protocol metadata.
    struct ProcessEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::vector<ProcessEntry> entries;
    };

    // SsdtEntry is the R3 model of one kernel SSDT response row.
    struct SsdtEntry
    {
        std::uint32_t serviceIndex = 0;
        std::uint32_t flags = 0;
        std::uint64_t zwRoutineAddress = 0;
        std::uint64_t serviceRoutineAddress = 0;
        std::string serviceName;
        std::string moduleName;
    };

    // SsdtEnumResult carries parsed SSDT rows and response header metadata.
    struct SsdtEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint64_t serviceTableBase = 0;
        std::uint32_t serviceCountFromTable = 0;
        std::vector<SsdtEntry> entries;
    };

    // CallbackRuntimeResult wraps the runtime-state response packet.
    struct CallbackRuntimeResult
    {
        IoResult io;
        KSWORD_ARK_CALLBACK_RUNTIME_STATE state{};
    };

    // CallbackRemoveResult wraps the external-callback removal response packet.
    struct CallbackRemoveResult
    {
        IoResult io;
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE response{};
    };

    // ArkDynModuleIdentity 是 R3 侧模块身份展示结构。
    struct ArkDynModuleIdentity
    {
        bool present = false;
        std::uint32_t classId = 0;
        std::uint32_t machine = 0;
        std::uint32_t timeDateStamp = 0;
        std::uint32_t sizeOfImage = 0;
        std::uint64_t imageBase = 0;
        std::wstring moduleName;
    };

    // DynDataStatusResult 承载 R0 DynData 状态与匹配诊断。
    struct DynDataStatusResult
    {
        IoResult io;
        std::uint32_t statusFlags = 0;
        std::uint32_t systemInformerDataVersion = 0;
        std::uint32_t systemInformerDataLength = 0;
        long lastStatus = 0;
        std::uint32_t matchedProfileClass = 0;
        std::uint32_t matchedProfileOffset = 0;
        std::uint32_t matchedFieldsId = 0;
        std::uint32_t fieldCount = 0;
        std::uint64_t capabilityMask = 0;
        ArkDynModuleIdentity ntoskrnl;
        ArkDynModuleIdentity lxcore;
        std::wstring unavailableReason;
    };

    // DynDataFieldEntry 是 R3 侧字段行模型。
    struct DynDataFieldEntry
    {
        std::uint32_t fieldId = 0;
        std::uint32_t flags = 0;
        std::uint32_t source = 0;
        std::uint32_t offset = 0;
        std::uint64_t capabilityMask = 0;
        std::string fieldName;
        std::string sourceName;
        std::string featureName;
    };

    // DynDataFieldsResult 承载字段列表响应。
    struct DynDataFieldsResult
    {
        IoResult io;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::vector<DynDataFieldEntry> entries;
    };

    // DynDataCapabilitiesResult 承载轻量 capability 查询响应。
    struct DynDataCapabilitiesResult
    {
        IoResult io;
        std::uint32_t statusFlags = 0;
        std::uint64_t capabilityMask = 0;
    };

    // DriverFeatureCapabilityEntry 是统一能力矩阵的一行 R3 模型。
    struct DriverFeatureCapabilityEntry
    {
        std::uint32_t featureId = 0;
        std::uint32_t state = 0;
        std::uint32_t flags = 0;
        std::uint32_t requiredPolicyFlags = 0;
        std::uint32_t deniedPolicyFlags = 0;
        std::uint64_t requiredDynDataMask = 0;
        std::uint64_t presentDynDataMask = 0;
        std::string featureName;
        std::string stateName;
        std::string dependencyText;
        std::string reasonText;
    };

    // DriverCapabilitiesQueryResult 承载 Phase 1 统一能力查询响应。
    struct DriverCapabilitiesQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t driverProtocolVersion = 0;
        std::uint32_t statusFlags = 0;
        std::uint32_t securityPolicyFlags = 0;
        std::uint32_t dynDataStatusFlags = 0;
        long lastErrorStatus = 0;
        std::uint32_t totalFeatureCount = 0;
        std::uint32_t returnedFeatureCount = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::string lastErrorSource;
        std::string lastErrorSummary;
        std::vector<DriverFeatureCapabilityEntry> entries;
    };

    // AsyncIoResult reports an overlapped DeviceIoControl issue attempt. A false
    // issued value with win32Error==ERROR_IO_PENDING means the request is queued.
    struct AsyncIoResult
    {
        bool issued = false;
        unsigned long win32Error = ERROR_SUCCESS;
        unsigned long bytesReturned = 0;
    };
}
