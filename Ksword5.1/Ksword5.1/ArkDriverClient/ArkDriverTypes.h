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
#include "../../../shared/driver/KswordArkFileMonitorIoctl.h"
#include "../../../shared/driver/KswordArkHandleIoctl.h"
#include "../../../shared/driver/KswordArkKernelIoctl.h"
#include "../../../shared/driver/KswordArkKeyboardIoctl.h"
#include "../../../shared/driver/KswordArkMemoryIoctl.h"
#include "../../../shared/driver/KswordArkMutationIoctl.h"
#include "../../../shared/driver/KswordArkProcessIoctl.h"
#include "../../../shared/driver/KswordArkThreadIoctl.h"
#include "../../../shared/driver/KswordArkAlpcIoctl.h"
#include "../../../shared/driver/KswordArkSectionIoctl.h"
#include "../../../shared/driver/KswordArkRegistryIoctl.h"
#include "../../../shared/driver/KswordArkNetworkIoctl.h"
#include "../../../shared/driver/KswordArkStorageIoctl.h"
#include "../../../shared/driver/KswordArkSecurityAuditIoctl.h"
#include "../../../shared/driver/KswordArkTrustIoctl.h"
#include "../../../shared/driver/KswordArkWin32kIoctl.h"
#include "../../../shared/driver/KswordArkDeviceAuditIoctl.h"
#include "../../../shared/driver/KswordArkFilterIoctl.h"
#include "../../../shared/driver/KswordArkKernelObjectIoctl.h"
#include "../../../shared/driver/KswordArkHwidIoctl.h"
#include "../../../shared/driver/KswordArkDebugOutputIoctl.h"
#include "../../../shared/driver/KswordArkBugcheckIoctl.h"

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

    // HwidDispatchResult：
    // - 输入：由 DriverClient 的 HWID Dispatch IOCTL wrapper 填充；
    // - 处理：保留 R0 原始响应，UI 负责解释目标驱动状态和风险提示；
    // - 返回行为：结构体无成员函数，io.ok 表示 DeviceIoControl 是否成功。
    struct HwidDispatchResult
    {
        IoResult io;                                  // io：底层 DeviceIoControl 状态。
        bool unsupported = false;                     // unsupported：旧驱动未注册新 IOCTL 时为 true。
        KSWORD_ARK_HWID_DISPATCH_RESPONSE response{}; // response：R0 固定响应包。
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

    // ProcessVisibilityResult 承载 R0 可恢复隐藏标记的更新结果。
    struct ProcessVisibilityResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t processId = 0;
        std::uint32_t status = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_UNKNOWN;
        std::uint32_t hiddenCount = 0;
        long lastStatus = 0;
    };

    // ProcessIntegrityResult 承载 R0 进程完整性写入响应。
    // 输入：由 DriverClient::setProcessIntegrity 填充，processId/integrityRid 回显目标。
    // 处理：io.ok 只代表驱动通信和固定响应解析成功，status/lastStatus 表示 R0 内核 API 执行结果。
    // 返回行为：unsupported=true 表示旧驱动缺少 IOCTL，调用方可以按策略回退到 R3。
    struct ProcessIntegrityResult
    {
        IoResult io;                         // io：底层 DeviceIoControl 状态和响应 NTSTATUS。
        bool unsupported = false;            // unsupported：旧驱动未注册 IOCTL 或返回不支持。
        std::uint32_t version = 0;           // version：协议版本。
        std::uint32_t processId = 0;         // processId：目标 PID。
        std::uint32_t integrityRid = 0;      // integrityRid：S-1-16-* mandatory label RID。
        std::uint32_t status = KSWORD_ARK_PROCESS_INTEGRITY_STATUS_UNKNOWN; // status：R0 聚合状态。
        long lastStatus = 0;                 // lastStatus：Zw* token API 或 R0 DynData Token 兜底路径 NTSTATUS。
    };

    // ProcessSpecialFlagsResult 承载 BreakOnTermination/APC 插入控制响应。
    struct ProcessSpecialFlagsResult
    {
        IoResult io;                         // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;           // version：协议版本。
        std::uint32_t processId = 0;         // processId：目标 PID。
        std::uint32_t action = 0;            // action：请求动作。
        std::uint32_t status = KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNKNOWN; // status：R0 聚合状态。
        std::uint32_t appliedFlags = 0;      // appliedFlags：已应用标志。
        std::uint32_t touchedThreadCount = 0;// touchedThreadCount：禁 APC 时改变的线程数。
        long lastStatus = 0;                 // lastStatus：底层 NTSTATUS。
    };

    // ProcessDkomResult 承载 PspCidTable DKOM 删除响应。
    struct ProcessDkomResult
    {
        IoResult io;                         // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;           // version：协议版本。
        std::uint32_t processId = 0;         // processId：目标 PID。
        std::uint32_t action = 0;            // action：请求动作。
        std::uint32_t status = KSWORD_ARK_PROCESS_DKOM_STATUS_UNKNOWN; // status：R0 聚合状态。
        std::uint32_t removedEntries = 0;    // removedEntries：清零的 CID 表项数。
        long lastStatus = 0;                 // lastStatus：底层 NTSTATUS。
        std::uint64_t pspCidTableAddress = 0;// pspCidTableAddress：诊断地址。
        std::uint64_t processObjectAddress = 0; // processObjectAddress：诊断地址。
    };

    // ProcessInjectResult 承载 R0 DLL / Shellcode 注入响应。
    struct ProcessInjectResult
    {
        IoResult io;                         // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;           // version：协议版本。
        std::uint32_t processId = 0;         // processId：目标 PID。
        std::uint32_t injectType = 0;        // injectType：DLL 路径或 shellcode。
        std::uint32_t status = KSWORD_ARK_PROCESS_INJECT_STATUS_UNKNOWN; // status：R0 聚合状态。
        std::uint32_t flags = 0;             // flags：请求标志回显。
        std::uint32_t bytesWritten = 0;      // bytesWritten：写入目标进程的 payload 字节数。
        long lastStatus = 0;                 // lastStatus：底层 NTSTATUS。
        long waitStatus = 0;                 // waitStatus：可选等待远端线程的状态。
        std::uint64_t entryPointAddress = 0; // entryPointAddress：远端线程入口。
        std::uint64_t parameterAddress = 0;  // parameterAddress：远端线程参数。
        std::uint64_t remoteBaseAddress = 0; // remoteBaseAddress：远端 payload 区域。
        std::uint64_t remoteRegionSize = 0;  // remoteRegionSize：远端分配区域大小。
    };

    // ThreadEntry 是 R0 KTHREAD 扩展字段的 R3 侧模型。
    // 输入：ArkDriverProcess.cpp 从 KSWORD_ARK_THREAD_ENTRY 逐字段复制。
    // 处理：flags 保留 KSWORD_ARK_THREAD_FLAG_* cross-view 结果，fieldFlags 保留字段可用性。
    // 返回：纯数据结构，无成员函数返回值。
    struct ThreadEntry
    {
        std::uint32_t threadId = 0;
        std::uint32_t processId = 0;
        std::uint32_t flags = 0;      // KSWORD_ARK_THREAD_FLAG_*：R0 active walk / CID scan 交叉视图标记。
        std::uint32_t fieldFlags = 0;
        std::uint32_t r0Status = KSWORD_ARK_THREAD_R0_STATUS_UNAVAILABLE;
        std::uint32_t stackFieldSource = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
        std::uint32_t ioFieldSource = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
        std::uint64_t initialStack = 0;
        std::uint64_t stackLimit = 0;
        std::uint64_t stackBase = 0;
        std::uint64_t kernelStack = 0;
        std::uint64_t readOperationCount = 0;
        std::uint64_t writeOperationCount = 0;
        std::uint64_t otherOperationCount = 0;
        std::uint64_t readTransferCount = 0;
        std::uint64_t writeTransferCount = 0;
        std::uint64_t otherTransferCount = 0;
        std::uint32_t ktInitialStackOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktStackLimitOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktStackBaseOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktKernelStackOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktReadOperationCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktWriteOperationCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktOtherOperationCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktReadTransferCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktWriteTransferCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktOtherTransferCountOffset = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint64_t dynDataCapabilityMask = 0;
    };

    // ThreadEnumResult 承载 R0 线程扩展枚举响应。
    struct ThreadEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::vector<ThreadEntry> entries;
    };

    // HandleEntry 是 R0 HandleTable 直接枚举的 R3 侧模型。
    struct HandleEntry
    {
        std::uint32_t processId = 0;
        std::uint32_t handleValue = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE;
        std::uint32_t grantedAccess = 0;
        std::uint32_t attributes = 0;
        std::uint32_t objectTypeIndex = 0;
        std::uint64_t objectAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint32_t epObjectTableOffset = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::uint32_t htHandleContentionEventOffset = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::uint32_t obDecodeShift = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::uint32_t obAttributesShift = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::uint32_t otNameOffset = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::uint32_t otIndexOffset = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
    };

    // HandleEnumResult 承载 R0 进程 HandleTable 枚举响应。
    struct HandleEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t processId = 0;
        std::uint32_t overallStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE;
        long lastStatus = 0;
        std::vector<HandleEntry> entries;
    };

    // HandleObjectQueryResult 承载 R0 对象类型/对象名查询结果。
    struct HandleObjectQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t processId = 0;
        std::uint32_t fieldFlags = 0;
        std::uint64_t handleValue = 0;
        std::uint64_t objectAddress = 0;
        std::uint32_t objectTypeIndex = 0;
        std::uint32_t queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE;
        long objectReferenceStatus = 0;
        long typeStatus = 0;
        long nameStatus = 0;
        std::uint32_t proxyStatus = KSWORD_ARK_OBJECT_PROXY_STATUS_NOT_REQUESTED;
        long proxyNtStatus = 0;
        std::uint32_t proxyPolicyFlags = 0;
        std::uint32_t requestedAccess = 0;
        std::uint32_t actualGrantedAccess = 0;
        std::uint64_t proxyHandle = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint32_t otNameOffset = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::uint32_t otIndexOffset = KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
        std::wstring typeName;
        std::wstring objectName;
    };

    // AlpcPortInfo 是 R0 ALPC Port 查询中单个端口节点的 R3 展示模型。
    struct AlpcPortInfo
    {
        std::uint32_t relation = KSWORD_ARK_ALPC_PORT_RELATION_QUERY;
        std::uint32_t fieldFlags = 0;
        std::uint32_t ownerProcessId = 0;
        std::uint32_t flags = 0;
        std::uint32_t state = 0;
        std::uint32_t sequenceNo = 0;
        long basicStatus = 0;
        long nameStatus = 0;
        std::uint64_t objectAddress = 0;
        std::uint64_t portContext = 0;
        std::wstring portName;
    };

    // AlpcPortQueryResult 承载 Phase-6 R0 ALPC 查询响应。
    struct AlpcPortQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t processId = 0;
        std::uint32_t fieldFlags = 0;
        std::uint64_t handleValue = 0;
        std::uint32_t queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE;
        long objectReferenceStatus = 0;
        long typeStatus = 0;
        long basicStatus = 0;
        long communicationStatus = 0;
        long nameStatus = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint32_t alpcCommunicationInfoOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcOwnerProcessOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcConnectionPortOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcServerCommunicationPortOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcClientCommunicationPortOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcHandleTableOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcHandleTableLockOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcAttributesOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcAttributesFlagsOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcPortContextOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcPortObjectLockOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcSequenceNoOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::uint32_t alpcStateOffset = KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
        std::wstring typeName;
        AlpcPortInfo queryPort;
        AlpcPortInfo connectionPort;
        AlpcPortInfo serverPort;
        AlpcPortInfo clientPort;
    };

    // SectionMappingEntry 是 R0 ControlArea 映射关系的一行 R3 模型。
    struct SectionMappingEntry
    {
        std::uint32_t viewMapType = KSWORD_ARK_SECTION_MAP_TYPE_UNKNOWN;
        std::uint32_t processId = 0;
        std::uint64_t startVa = 0;
        std::uint64_t endVa = 0;
    };

    // ProcessSectionQueryResult 承载 Phase-7 进程 SectionObject / ControlArea 查询响应。
    struct ProcessSectionQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t processId = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_UNAVAILABLE;
        long lastStatus = 0;
        std::uint64_t sectionObjectAddress = 0;
        std::uint64_t controlAreaAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint32_t epSectionObjectOffset = KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE;
        std::uint32_t mmSectionControlAreaOffset = KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE;
        std::uint32_t mmControlAreaListHeadOffset = KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE;
        std::uint32_t mmControlAreaLockOffset = KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE;
        std::vector<SectionMappingEntry> mappings;
    };

    // FileSectionMappingEntry 是 R0 文件 Data/Image ControlArea 映射关系的一行 R3 模型。
    struct FileSectionMappingEntry
    {
        std::uint32_t sectionKind = KSWORD_ARK_FILE_SECTION_KIND_UNKNOWN; // Data 或 Image。
        std::uint32_t viewMapType = KSWORD_ARK_SECTION_MAP_TYPE_UNKNOWN;  // Process/Session/SystemCache。
        std::uint32_t processId = 0;                                      // 命中映射进程 PID。
        std::uint64_t controlAreaAddress = 0;                             // 仅诊断展示。
        std::uint64_t startVa = 0;                                        // 映射起始 VA。
        std::uint64_t endVa = 0;                                          // 映射结束 VA。
    };

    // FileSectionMappingsQueryResult 承载 Phase-7 文件反查映射进程响应。
    struct FileSectionMappingsQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_UNAVAILABLE;
        long lastStatus = 0;
        std::uint64_t fileObjectAddress = 0;
        std::uint64_t sectionObjectPointersAddress = 0;
        std::uint64_t dataControlAreaAddress = 0;
        std::uint64_t imageControlAreaAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint32_t mmControlAreaListHeadOffset = KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE;
        std::uint32_t mmControlAreaLockOffset = KSWORD_ARK_SECTION_OFFSET_UNAVAILABLE;
        std::vector<FileSectionMappingEntry> mappings;
    };

    // FileInfoQueryResult 是 Phase-10 R0 文件基础信息查询的 R3 模型。
    struct FileInfoQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;        // 协议版本。
        std::uint32_t fieldFlags = 0;     // KSWORD_ARK_FILE_INFO_FIELD_*。
        std::uint32_t queryStatus = KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE; // 查询聚合状态。
        long openStatus = 0;              // ZwCreateFile 状态。
        long basicStatus = 0;             // FileBasicInformation 查询状态。
        long standardStatus = 0;          // FileStandardInformation 查询状态。
        long objectStatus = 0;            // ObReferenceObjectByHandle/SectionPointer 状态。
        long nameStatus = 0;              // ObQueryNameString 状态。
        std::uint32_t fileAttributes = 0; // FILE_ATTRIBUTE_*。
        std::int64_t allocationSize = 0;  // 分配大小。
        std::int64_t endOfFile = 0;       // 文件逻辑大小。
        std::int64_t creationTime = 0;    // FILETIME 兼容时间戳。
        std::int64_t lastAccessTime = 0;  // FILETIME 兼容时间戳。
        std::int64_t lastWriteTime = 0;   // FILETIME 兼容时间戳。
        std::int64_t changeTime = 0;      // NTFS change time。
        std::uint64_t fileObjectAddress = 0; // 诊断展示，不作为凭据。
        std::uint64_t sectionObjectPointersAddress = 0; // 诊断展示。
        std::uint64_t dataSectionObjectAddress = 0;     // 诊断展示。
        std::uint64_t imageSectionObjectAddress = 0;    // 诊断展示。
        std::wstring ntPath;            // 请求 NT 路径回显。
        std::wstring objectName;        // ObQueryNameString 文件对象名。
    };

    // ImageSignatureQueryResult preserves the complete fixed R0 response so
    // callers can distinguish PE certificate-table structure from the
    // independent Code Integrity cached-signing-level result.
    struct ImageSignatureQueryResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE response{};
    };

    // FileIntegrityResult 承载 R0 文件 Mandatory Label 写入响应。
    // 输入：由 DriverClient::setFileIntegrity 填充，flags/integrityRid/pathLengthChars 回显请求。
    // 处理：io.ok 只代表驱动通信和响应解析成功，status/lastStatus 表示 ZwCreateFile/ZwSetSecurityObject 结果。
    // 返回行为：unsupported=true 表示旧驱动缺少 IOCTL，调用方可以按策略回退到 R3。
    struct FileIntegrityResult
    {
        IoResult io;                         // io：底层 DeviceIoControl 状态和响应 NTSTATUS。
        bool unsupported = false;            // unsupported：旧驱动未注册 IOCTL 或返回不支持。
        std::uint32_t version = 0;           // version：协议版本。
        std::uint32_t flags = 0;             // flags：KSWORD_ARK_FILE_INTEGRITY_FLAG_* 回显。
        std::uint32_t integrityRid = 0;      // integrityRid：S-1-16-* mandatory label RID。
        std::uint32_t status = KSWORD_ARK_FILE_INTEGRITY_STATUS_UNKNOWN; // status：R0 聚合状态。
        long lastStatus = 0;                 // lastStatus：ZwCreateFile/ZwSetSecurityObject 等 NTSTATUS。
        std::uint32_t pathLengthChars = 0;   // pathLengthChars：驱动接收的 NT 路径字符数。
    };

    // FileMonitorStatusResult 是 R0 文件系统 minifilter 运行状态的 R3 模型。
    struct FileMonitorStatusResult
    {
        IoResult io;                    // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;      // version：文件监控协议版本。
        std::uint32_t size = 0;         // size：R0 返回结构大小。
        std::uint32_t runtimeFlags = 0; // runtimeFlags：REGISTERED/STARTED/DROPPED 等标志。
        std::uint32_t operationMask = 0;// operationMask：当前文件事件操作过滤位。
        std::uint32_t processIdFilter = 0; // processIdFilter：0 表示不过滤 PID。
        std::uint32_t ringCapacity = 0; // ringCapacity：R0 环形队列容量。
        std::uint32_t queuedCount = 0;  // queuedCount：当前待取事件数。
        std::uint32_t droppedCount = 0; // droppedCount：累计覆盖丢弃事件数。
        std::uint64_t sequence = 0;     // sequence：R0 文件事件序列号。
        long registerStatus = 0;        // registerStatus：FltRegisterFilter 状态。
        long startStatus = 0;           // startStatus：FltStartFiltering 状态。
        long lastErrorStatus = 0;       // lastErrorStatus：最近一次文件监控错误。
    };

    // FileMonitorEventRow 是 R0 file-monitor ring buffer 的 R3 展示模型。
    struct FileMonitorEventRow
    {
        std::uint32_t version = 0;       // version：事件协议版本。
        std::uint32_t size = 0;          // size：R0 事件结构大小。
        std::uint32_t operationType = 0; // operationType：KSWORD_ARK_FILE_MONITOR_OPERATION_*。
        std::uint32_t majorFunction = 0; // majorFunction：IRP_MJ_*。
        std::uint32_t minorFunction = 0; // minorFunction：IRP_MN_*。
        std::uint32_t processId = 0;     // processId：请求发起进程 PID。
        std::uint32_t threadId = 0;      // threadId：请求发起线程 ID。
        std::uint32_t fieldFlags = 0;    // fieldFlags：有效字段位图。
        std::uint32_t desiredAccess = 0; // desiredAccess：Create/Open 访问掩码。
        std::uint32_t shareAccess = 0;   // shareAccess：Create/Open 共享掩码。
        std::uint32_t createOptions = 0; // createOptions：Create/Open options。
        std::uint32_t fileInformationClass = 0; // fileInformationClass：SetInformation class。
        long resultStatus = 0;           // resultStatus：post-operation NTSTATUS。
        std::uint32_t pathLengthChars = 0; // pathLengthChars：R0 返回路径字符数。
        std::uint64_t sequence = 0;      // sequence：R0 事件序号。
        std::int64_t timeUtc100ns = 0;   // timeUtc100ns：UTC FILETIME。
        std::uint64_t fileObjectAddress = 0; // fileObjectAddress：FileObject 地址，仅诊断展示。
        std::uint32_t fsControlCode = 0; // fsControlCode：IRP_MJ_FILE_SYSTEM_CONTROL 控制码。
        std::uint32_t fsInputBufferLength = 0; // fsInputBufferLength：输入缓冲区长度。
        std::uint32_t fsOutputBufferLength = 0; // fsOutputBufferLength：输出缓冲区长度。
        std::wstring path;               // path：R0 解析出的 normalized/opened file name。
    };

    // FileMonitorDrainResult 是文件监控 drain IOCTL 的解析结果。
    struct FileMonitorDrainResult
    {
        IoResult io;                     // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;       // version：响应协议版本。
        std::uint32_t totalQueuedBeforeDrain = 0; // totalQueuedBeforeDrain：取出前队列深度。
        std::uint32_t returnedCount = 0; // returnedCount：本次返回事件数。
        std::uint32_t entrySize = 0;     // entrySize：R0 单个事件字节数。
        std::uint32_t droppedCount = 0;  // droppedCount：累计丢弃事件数。
        std::uint32_t runtimeFlags = 0;  // runtimeFlags：REGISTERED/STARTED/DROPPED。
        std::uint32_t ringCapacity = 0;  // ringCapacity：R0 ring 容量。
        std::vector<FileMonitorEventRow> events; // events：已解析事件列表。
    };

    // DebugOutputControlResult 保存 R0 调试输出回调的注册、捕获与丢弃状态。
    struct DebugOutputControlResult
    {
        IoResult io;                         // io：DeviceIoControl 与固定响应解析状态。
        bool unsupported = false;            // unsupported：当前驱动尚未注册调试输出 IOCTL。
        std::uint32_t version = 0;           // version：共享协议版本。
        std::uint32_t runtimeFlags = 0;      // runtimeFlags：REGISTERED/CAPTURING/DROPPED。
        std::uint32_t ringCapacity = 0;      // ringCapacity：R0 固定环形缓冲区容量。
        std::uint32_t queuedCount = 0;       // queuedCount：当前仍可读取的记录数量。
        std::uint64_t latestSequence = 0;    // latestSequence：最近提交的单调序号。
        std::uint64_t droppedCount = 0;      // droppedCount：高 IRQL 并发写入时累计丢弃数。
        long registrationStatus = 0;         // registrationStatus：DbgSetDebugPrintCallback 状态。
        long lastStatus = 0;                 // lastStatus：最近控制动作的 NTSTATUS。
    };

    // DebugOutputRecord 是一条已经稳定复制到 R3 的内核调试消息。
    struct DebugOutputRecord
    {
        std::uint64_t sequence = 0;          // sequence：R0 单调序号。
        std::uint64_t interruptTime100ns = 0;// interruptTime100ns：KeQueryInterruptTime 时间戳。
        std::uint32_t componentId = 0;       // componentId：DbgPrintEx 组件 ID。
        std::uint32_t level = 0;             // level：DbgPrintEx 级别。
        std::uint32_t flags = 0;             // flags：TEXT_TRUNCATED 等记录标志。
        std::string text;                    // text：按协议长度复制的 UTF-8/ANSI 调试文本。
    };

    // DebugOutputDrainResult 是按游标增量读取调试输出环形缓冲区的结果。
    struct DebugOutputDrainResult
    {
        IoResult io;                         // io：DeviceIoControl 与变长响应解析状态。
        bool unsupported = false;            // unsupported：当前驱动不支持该 IOCTL。
        std::uint32_t runtimeFlags = 0;      // runtimeFlags：当前回调运行时标志。
        std::uint32_t responseFlags = 0;     // responseFlags：OVERFLOW/MORE/SNAPSHOT_RACE。
        std::uint32_t ringCapacity = 0;      // ringCapacity：R0 环形容量。
        std::uint64_t firstAvailableSequence = 0; // firstAvailableSequence：最早未覆盖序号。
        std::uint64_t latestSequence = 0;    // latestSequence：读取快照时的最新序号。
        std::uint64_t nextSequence = 0;      // nextSequence：下次请求应携带的游标。
        std::uint64_t droppedCount = 0;      // droppedCount：回调 try-lock 累计丢弃数。
        std::uint64_t lostBeforeFirst = 0;   // lostBeforeFirst：调用方游标落后导致的覆盖数。
        std::vector<DebugOutputRecord> records; // records：本次成功解析的升序记录。
    };

    // RegistryReadResult 是 R0 注册表值读取响应的 R3 模型。
    struct RegistryReadResult
    {
        IoResult io;                    // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;      // version：协议版本。
        std::uint32_t status = KSWORD_ARK_REGISTRY_READ_STATUS_UNKNOWN; // status：R0 聚合状态。
        std::uint32_t valueType = 0;    // valueType：REG_* 类型。
        std::uint32_t dataBytes = 0;    // dataBytes：返回数据长度。
        std::uint32_t requiredBytes = 0; // requiredBytes：完整值数据长度。
        long lastStatus = 0;            // lastStatus：底层 Zw* 状态。
        std::vector<std::uint8_t> data; // data：原始注册表值数据。
    };

    // RegistrySubKeyEntry 是 R0 枚举出的一个子键。
    struct RegistrySubKeyEntry
    {
        std::wstring name;              // name：子键名称，不含父路径。
    };

    // RegistryValueEntry 是 R0 枚举出的一个注册表值。
    struct RegistryValueEntry
    {
        std::wstring name;              // name：值名，空字符串表示默认值。
        std::uint32_t valueType = 0;    // valueType：REG_* 类型。
        std::uint32_t dataBytes = 0;    // dataBytes：返回预览数据长度。
        std::uint32_t requiredBytes = 0; // requiredBytes：完整数据长度。
        std::vector<std::uint8_t> data; // data：预览数据，可能被 R0 截断。
    };

    // RegistryEnumResult 是 R0 枚举键响应的 R3 模型。
    struct RegistryEnumResult
    {
        IoResult io;                    // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;      // version：协议版本。
        std::uint32_t status = KSWORD_ARK_REGISTRY_ENUM_STATUS_UNKNOWN; // status：R0 聚合状态。
        std::uint32_t subKeyCount = 0;  // subKeyCount：R0 观察到的子键数。
        std::uint32_t returnedSubKeyCount = 0; // returnedSubKeyCount：已返回子键数。
        std::uint32_t valueCount = 0;   // valueCount：R0 观察到的值数。
        std::uint32_t returnedValueCount = 0; // returnedValueCount：已返回值数。
        long lastStatus = 0;            // lastStatus：底层 Zw* 状态。
        std::vector<RegistrySubKeyEntry> subKeys; // subKeys：子键列表。
        std::vector<RegistryValueEntry> values;   // values：值列表。
    };

    // RegistryOperationResult 是 R0 注册表写操作通用响应模型。
    struct RegistryOperationResult
    {
        IoResult io;                    // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;      // version：协议版本。
        std::uint32_t status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_UNKNOWN; // status：操作聚合状态。
        long lastStatus = 0;            // lastStatus：底层 Zw* 状态。
    };

    // VirtualMemoryReadResult 是 R0 读目标进程虚拟内存的 R3 模型。
    struct VirtualMemoryReadResult
    {
        IoResult io;                    // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;      // version：协议版本。
        std::uint32_t processId = 0;    // processId：目标 PID。
        std::uint32_t fieldFlags = 0;   // fieldFlags：KSWORD_ARK_MEMORY_FIELD_*。
        std::uint32_t readStatus = KSWORD_ARK_MEMORY_READ_STATUS_UNAVAILABLE; // readStatus：R0 读聚合状态。
        long lookupStatus = 0;          // lookupStatus：PsLookupProcessByProcessId 状态。
        long copyStatus = 0;            // copyStatus：MmCopyVirtualMemory 状态。
        std::uint32_t source = 0;       // source：数据来源。
        std::uint64_t requestedBaseAddress = 0; // requestedBaseAddress：请求基址。
        std::uint32_t requestedBytes = 0;       // requestedBytes：请求长度。
        std::uint32_t bytesRead = 0;            // bytesRead：R0 返回有效长度。
        std::uint32_t maxBytesPerRequest = 0;   // maxBytesPerRequest：驱动限制。
        std::vector<std::uint8_t> data;         // data：读回数据，失败区域按 R0 策略可为 00。
    };

    // VirtualMemoryWriteResult 是 R0 写目标进程虚拟内存的 R3 模型。
    struct VirtualMemoryWriteResult
    {
        IoResult io;                    // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;      // version：协议版本。
        std::uint32_t processId = 0;    // processId：目标 PID。
        std::uint32_t fieldFlags = 0;   // fieldFlags：KSWORD_ARK_MEMORY_FIELD_*。
        std::uint32_t writeStatus = KSWORD_ARK_MEMORY_WRITE_STATUS_UNAVAILABLE; // writeStatus：R0 写聚合状态。
        long lookupStatus = 0;          // lookupStatus：PsLookupProcessByProcessId 状态。
        long copyStatus = 0;            // copyStatus：MmCopyVirtualMemory 状态。
        std::uint32_t source = 0;       // source：写入来源。
        std::uint64_t requestedBaseAddress = 0; // requestedBaseAddress：请求基址。
        std::uint32_t requestedBytes = 0;       // requestedBytes：请求写入长度。
        std::uint32_t bytesWritten = 0;         // bytesWritten：实际写入长度。
        std::uint32_t maxBytesPerRequest = 0;   // maxBytesPerRequest：驱动限制。
    };

    // Kernel executable-memory permission bits used by the R3 display model.
    // Input: values parsed from the kernel executable page scan response.
    // Processing: MemoryDock maps these bits to readable R/W/X/NX/Large labels.
    // Return behavior: constants are consumed directly and do not return data.
    inline constexpr std::uint32_t KernelExecutableMemoryPermissionPresent = KSWORD_ARK_PAGE_TABLE_FLAG_PRESENT;
    inline constexpr std::uint32_t KernelExecutableMemoryPermissionWritable = KSWORD_ARK_PAGE_TABLE_FLAG_WRITABLE;
    inline constexpr std::uint32_t KernelExecutableMemoryPermissionUser = KSWORD_ARK_PAGE_TABLE_FLAG_USER;
    inline constexpr std::uint32_t KernelExecutableMemoryPermissionNoExecute = KSWORD_ARK_PAGE_TABLE_FLAG_NX;
    inline constexpr std::uint32_t KernelExecutableMemoryPermissionLargePage = KSWORD_ARK_PAGE_TABLE_FLAG_LARGE_PAGE;
    inline constexpr std::uint32_t KernelExecutableMemoryPermissionGlobal = KSWORD_ARK_PAGE_TABLE_FLAG_GLOBAL;

    // Kernel executable-memory risk bits used by the R3 display model.
    // Input: values parsed from R0 scan entries.
    // Processing: UI uses these stable bits for filtering and risk text.
    // Return behavior: constants are values only; no function return is involved.
    inline constexpr std::uint32_t KernelExecutableMemoryRiskWritableExecutable = KSWORD_ARK_KERNEL_EXEC_RISK_WRITABLE_EXECUTABLE;
    inline constexpr std::uint32_t KernelExecutableMemoryRiskModuleNonTextExecutable = KSWORD_ARK_KERNEL_EXEC_RISK_MODULE_NON_TEXT_EXECUTABLE;
    inline constexpr std::uint32_t KernelExecutableMemoryRiskSectionWritable = KSWORD_ARK_KERNEL_EXEC_RISK_SECTION_WRITABLE;
    inline constexpr std::uint32_t KernelExecutableMemoryRiskLargePage = KSWORD_ARK_KERNEL_EXEC_RISK_LARGE_PAGE;

    // KernelExecutableMemoryPageEntry is the R3 model for one executable kernel
    // memory range. Input fields are copied from the Prompt-1 scan response.
    // Processing keeps kernel addresses diagnostic-only and stores owner/path
    // strings for filtering and details. Return behavior: plain data object.
    struct KernelExecutableMemoryPageEntry
    {
        std::uint32_t status = 0;              // status：R0 row status.
        std::uint32_t riskFlags = 0;           // riskFlags：KernelExecutableMemoryRisk* bits.
        std::uint32_t permissionFlags = 0;     // permissionFlags：KernelExecutableMemoryPermission* bits.
        std::uint32_t ownerKind = 0;           // ownerKind：R0 owner classifier, shown diagnostically.
        std::uint32_t pageCount = 0;           // pageCount：contiguous executable pages.
        std::uint32_t pageSize = 0;            // pageSize：4KB/2MB/1GB or R0 effective size.
        long lastStatus = 0;                   // lastStatus：row-level backend status.
        std::uint64_t virtualAddress = 0;      // virtualAddress：range start VA, display only.
        std::uint64_t ownerAddress = 0;        // ownerAddress：diagnostic owner object/address.
        std::uint64_t moduleBase = 0;          // moduleBase：matched module base when available.
        std::uint32_t moduleSize = 0;          // moduleSize：matched module image size when available.
        std::uint64_t regionSize = 0;          // regionSize：pageCount * pageSize or R0 range size.
        std::wstring owner;                    // owner：R0 owner text.
        std::wstring modulePath;               // modulePath：matched module image path.
        std::wstring detail;                   // detail：R0 diagnostic detail for CodeEditorWidget.
    };

    // KernelExecutableMemoryScanResult carries the parsed Prompt-1 response.
    // Input: returned by DriverClient::scanKernelExecutableMemory.
    // Processing: io.ok indicates transport/protocol success; unsupported tells
    // UI to show "not supported / driver too old" instead of crashing.
    // Return behavior: returned by value from DriverClient.
    struct KernelExecutableMemoryScanResult
    {
        IoResult io;                           // io：DeviceIoControl and parse status.
        bool unsupported = false;              // unsupported：true when IOCTL is absent/old.
        std::uint32_t version = 0;             // version：scan protocol version.
        std::uint32_t status = 0;              // status：R0 aggregate status.
        std::uint32_t totalCount = 0;          // totalCount：R0 observed ranges.
        std::uint32_t returnedCount = 0;       // returnedCount：R0 returned ranges.
        std::uint32_t moduleCount = 0;         // moduleCount：R0 module owner set size.
        long lastStatus = 0;                   // lastStatus：R0 aggregate backend status.
        std::vector<KernelExecutableMemoryPageEntry> entries; // entries：parsed scan rows.
    };


    // KernelMemoryEvidenceEntry is the unified R3 model for memory evidence rows.
    // Input: fields are copied from KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW.
    // Processing: keeps addresses and samples diagnostic-only; UI scoring uses
    // riskFlags/permissionFlags without issuing write or repair actions.
    // Return behavior: plain data carrier returned inside KernelMemoryEvidenceResult.
    struct KernelMemoryEvidenceEntry
    {
        std::uint32_t evidenceKind = KSWORD_ARK_MEMORY_EVIDENCE_KIND_UNKNOWN;
        std::uint32_t pageSize = 0;
        std::uint32_t permissionFlags = 0;
        std::uint32_t ownerKind = KSWORD_ARK_MEMORY_EVIDENCE_OWNER_UNKNOWN;
        std::uint32_t riskFlags = 0;
        std::uint32_t moduleSize = 0;
        std::uint32_t confidence = 0;
        std::uint32_t bigPoolTag = 0;
        std::uint32_t bigPoolFlags = 0;
        std::uint32_t sectionRva = 0;
        std::uint32_t sectionSize = 0;
        std::uint32_t hashAlgorithm = KSWORD_ARK_MEMORY_EVIDENCE_HASH_NONE;
        std::uint32_t sampleSize = 0;
        long lastStatus = 0;
        std::uint64_t virtualAddress = 0;
        std::uint64_t regionSize = 0;
        std::uint64_t moduleBase = 0;
        std::uint64_t ownerAddress = 0;
        std::uint64_t contentHash = 0;
        std::string sectionName;
        std::vector<std::uint8_t> sample;
        std::wstring ownerName;
        std::wstring detail;
    };

    // KernelMemoryEvidenceResult carries the variable-length memory evidence response.
    // Input: produced by DriverClient::queryKernelMemoryEvidence.
    // Processing: unsupported distinguishes old drivers from parse failures so UI can
    // render a graceful capability message.
    // Return behavior: returned by value; io.ok reports transport/protocol success.
    struct KernelMemoryEvidenceResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_MEMORY_EVIDENCE_STATUS_UNAVAILABLE;
        std::uint32_t responseFlags = 0;
        std::uint32_t sourceFlags = 0;
        std::uint32_t totalRows = 0;
        std::uint32_t returnedRows = 0;
        std::uint32_t maxRows = 0;
        std::uint64_t maxBytes = 0;
        std::uint64_t bytesScanned = 0;
        std::uint32_t moduleCount = 0;
        std::uint32_t bigPoolRowsSeen = 0;
        long lastStatus = 0;
        std::vector<KernelMemoryEvidenceEntry> entries;
    };

    // CrossViewFieldOffsets mirrors the shared R0 offset packet.
    // Input: copied from process/thread cross-view response headers or rows.
    // Processing: UI uses it only for diagnostics and capability explanations.
    // Return behavior: plain data object with no member function return.
    struct CrossViewFieldOffsets
    {
        std::uint32_t epUniqueProcessId = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t epActiveProcessLinks = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t epThreadListHead = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t epImageFileName = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t etCid = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t etThreadListEntry = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t etStartAddress = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t etWin32StartAddress = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t ktProcess = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t htTableCode = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t hteLowValue = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint32_t pspCidTableRva = KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
        std::uint64_t pspCidTableAddress = 0;
        std::uint32_t epUniqueProcessIdSource = 0;
        std::uint32_t epActiveProcessLinksSource = 0;
        std::uint32_t epThreadListHeadSource = 0;
        std::uint32_t epImageFileNameSource = 0;
        std::uint32_t etCidSource = 0;
        std::uint32_t etThreadListEntrySource = 0;
        std::uint32_t etStartAddressSource = 0;
        std::uint32_t etWin32StartAddressSource = 0;
        std::uint32_t ktProcessSource = 0;
        std::uint32_t htTableCodeSource = 0;
        std::uint32_t hteLowValueSource = 0;
        std::uint32_t pspCidTableSource = 0;
    };

    // ProcessCrossViewEntry is one EPROCESS cross-view evidence row.
    // Input: copied from KSWORD_ARK_PROCESS_CROSSVIEW_ROW.
    // Processing: sourceMask and anomalyFlags remain raw protocol bits so multiple
    // Dock pages can render consistent DKOM diagnostics.
    // Return behavior: data only; no return value.
    struct ProcessCrossViewEntry
    {
        std::uint64_t objectAddress = 0;
        std::uint64_t startAddress = 0;
        std::uint32_t processId = 0;
        std::uint32_t parentProcessId = 0;
        std::uint32_t sourceMask = 0;
        std::uint32_t anomalyFlags = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        CrossViewFieldOffsets fieldOffsets;
        long lastStatus = 0;
        std::uint32_t confidence = 0;
        std::uint32_t publicProcessId = 0;
        std::uint32_t activeListProcessId = 0;
        std::uint32_t cidTableProcessId = 0;
        long publicWalkStatus = 0;
        long activeListStatus = 0;
        long cidTableStatus = 0;
        std::uint32_t detailStatus = 0;
        std::uint32_t denoiseFlags = 0;
        std::string imageName;
        std::string detail;
    };

    // ProcessCrossViewResult carries a complete process cross-view query.
    // Input: produced by DriverClient::queryProcessCrossView.
    // Processing: missingCapabilityMask explains DynData gaps without hiding rows.
    // Return behavior: returned by value; unsupported flags old drivers.
    struct ProcessCrossViewResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_CROSSVIEW_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        long lastStatus = 0;
        CrossViewFieldOffsets fieldOffsets;
        std::vector<ProcessCrossViewEntry> entries;
    };

    // ProcessRuntimeDetailResult 承载单进程 PDB/DynData 运行时详情。
    // 输入：queryProcessRuntimeDetail 返回。
    // 处理：response 直接保存 shared\driver 固定响应，避免 UI 重新定义偏移字段。
    // 返回行为：只读展示 EPROCESS 字段，不修改进程对象。
    struct ProcessRuntimeDetailResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_PROCESS_DETAIL_RESPONSE response{};
    };

    // RuntimeFieldSampleRequestItem 是 deep PDB runtime catalog 到 R0 sampler 的一项请求。
    // 输入：runtimeItemId/offset/size 来自 profiles\pdb_deep_offsets JSON。
    // 处理：R0 会按对象基址 + offset 安全读取最多 16 字节。
    // 返回行为：该结构只作为 R3 请求模型，不保存对象地址。
    struct RuntimeFieldSampleRequestItem
    {
        std::uint32_t runtimeItemId = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t flags = 0;
        std::string name;
        std::string type;
    };

    // RuntimeFieldSampleEntry 是 R0 返回的一项小字段采样结果。
    // 输入：queryProcessRuntimeFieldSamples/queryThreadRuntimeFieldSamples 返回。
    // 处理：sampleBytes 保留原始字节，valueU64 仅用于 <=8 字节字段的摘要展示。
    // 返回行为：只读证据行，不可作为写入或 patch 凭据。
    struct RuntimeFieldSampleEntry
    {
        std::uint32_t runtimeItemId = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_UNKNOWN;
        std::uint32_t bytesRead = 0;
        std::uint32_t flags = 0;
        long lastStatus = 0;
        std::uint64_t valueU64 = 0;
        std::vector<std::uint8_t> sampleBytes;
        std::string name;
        std::string type;
    };

    // RuntimeFieldSampleResult 承载 process/thread 通用 deep PDB 字段采样响应。
    // 输入：ArkDriverClient 对 0x83E/0x83F 只读 IOCTL 的解析结果。
    // 处理：objectAddress 只用于显示 R0 实际 lookup 到的对象，不回喂任何写操作。
    // 返回行为：unsupported=true 表示旧驱动缺少 sampler IOCTL。
    struct RuntimeFieldSampleResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t entrySize = 0;
        std::uint32_t flags = 0;
        long lastStatus = 0;
        std::uint64_t objectAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::vector<RuntimeFieldSampleEntry> entries;
    };

    // ThreadCrossViewEntry is one ETHREAD/KTHREAD cross-view evidence row.
    // Input: copied from KSWORD_ARK_THREAD_CROSSVIEW_ROW.
    // Processing: target addresses are diagnostic-only and never used as operation credentials.
    // Return behavior: data-only row.
    struct ThreadCrossViewEntry
    {
        std::uint64_t objectAddress = 0;
        std::uint64_t processObjectAddress = 0;
        std::uint64_t startAddress = 0;
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        std::uint32_t sourceMask = 0;
        std::uint32_t anomalyFlags = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        CrossViewFieldOffsets fieldOffsets;
        long lastStatus = 0;
        std::uint32_t confidence = 0;
        std::uint32_t publicThreadId = 0;
        std::uint32_t threadListThreadId = 0;
        std::uint32_t cidTableThreadId = 0;
        std::uint32_t publicProcessId = 0;
        std::uint32_t threadListProcessId = 0;
        std::uint32_t cidTableProcessId = 0;
        long publicWalkStatus = 0;
        long threadListStatus = 0;
        long cidTableStatus = 0;
        long startAddressStatus = 0;
        std::uint32_t detailStatus = 0;
        std::uint32_t denoiseFlags = 0;
        std::string imageName;
        std::string detail;
    };

    // ThreadCrossViewResult carries a complete thread cross-view query.
    // Input: produced by DriverClient::queryThreadCrossView.
    // Processing: rows may include orphan/CID-only evidence and remain read-only in UI.
    // Return behavior: returned by value; io.ok indicates parseable response.
    struct ThreadCrossViewResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_CROSSVIEW_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        long lastStatus = 0;
        CrossViewFieldOffsets fieldOffsets;
        std::vector<ThreadCrossViewEntry> entries;
    };

    // ThreadRuntimeDetailResult 承载单线程 PDB/DynData 运行时详情。
    // 输入：queryThreadRuntimeDetail 返回。
    // 处理：response 保存 ETHREAD/KTHREAD Cid、链表、栈和 I/O counter 字段。
    // 返回行为：只读展示线程对象，不挂起、不终止、不改链表。
    struct ThreadRuntimeDetailResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_THREAD_DETAIL_RESPONSE response{};
    };

    // DriverIntegrityEvidenceEntry is one driver/kernel integrity evidence row.
    // Input: copied from KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE.
    // Processing: evidenceClass groups DriverObject, LDR, FastIo, MajorFunction, CPU,
    // descriptor-table and MSR rows while riskFlags keeps raw R0 findings.
    // Return behavior: plain data object.
    struct DriverIntegrityEvidenceEntry
    {
        std::uint32_t evidenceClass = 0;
        std::uint32_t riskFlags = 0;
        std::uint32_t sourceMask = 0;
        std::uint32_t confidence = 0;
        std::uint32_t processorGroup = 0;
        std::uint32_t processorNumber = 0;
        std::uint32_t vector = 0;
        std::uint32_t ownerModuleSize = 0;
        std::uint64_t objectAddress = 0;
        std::uint64_t targetAddress = 0;
        std::uint64_t ownerModuleBase = 0;
        std::wstring ownerModule;
        std::wstring detail;
        std::uint32_t entryStatus = 0;              // entryStatus：v2 单行状态，老驱动返回 0。
        std::uint32_t statusFlags = 0;              // statusFlags：partial/unsupported/PDB required 等位。
        std::uint32_t fieldMask = 0;                // fieldMask：本行实际填充的 typed 字段位。
        std::uint32_t riskScore = 0;                // riskScore：R0 汇总风险分，0-100。
        std::uint32_t rangeState = 0;               // rangeState：目标地址相对所属驱动镜像的位置。
        std::uint32_t ordinal = 0;                  // ordinal：IRP major/FastIo/attached depth 等序号。
        std::uint32_t deviceType = 0;               // deviceType：DeviceObject 类型字段。
        std::uint32_t deviceFlags = 0;              // deviceFlags：DeviceObject flags。
        std::uint64_t driverObjectAddress = 0;      // driverObjectAddress：关联 DriverObject。
        std::uint64_t driverStart = 0;              // driverStart：DriverObject.DriverStart。
        std::uint64_t driverSize = 0;               // driverSize：DriverObject.DriverSize。
        std::uint64_t driverSection = 0;            // driverSection：DriverObject.DriverSection。
        std::uint64_t driverUnload = 0;             // driverUnload：DriverObject.DriverUnload。
        std::uint64_t deviceObjectAddress = 0;      // deviceObjectAddress：DeviceObject 地址。
        std::uint64_t nextDeviceObjectAddress = 0;  // nextDeviceObjectAddress：NextDevice。
        std::uint64_t attachedDeviceObjectAddress = 0; // attachedDeviceObjectAddress：AttachedDevice。
        std::uint64_t deviceDriverObjectAddress = 0; // deviceDriverObjectAddress：DeviceObject.DriverObject。
        std::uint64_t kldrEntryAddress = 0;         // kldrEntryAddress：KLDR_DATA_TABLE_ENTRY 地址。
        std::uint64_t kldrListHeadAddress = 0;      // kldrListHeadAddress：PsLoadedModuleList 链表头。
        std::uint64_t kldrDllBase = 0;              // kldrDllBase：KLDR.DllBase。
        std::uint32_t kldrSizeOfImage = 0;          // kldrSizeOfImage：KLDR.SizeOfImage。
    };

    // DriverIntegrityResult carries DriverObject/LDR/CPU integrity evidence.
    // Input: produced by queryDriverIntegrity or queryKernelCpuIntegrity.
    // Processing: unsupported provides graceful UI fallback for older R0 drivers.
    // Return behavior: returned by value with parsed evidence entries.
    struct DriverIntegrityResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t queryStatus = KSWORD_ARK_DRIVER_INTEGRITY_STATUS_UNAVAILABLE;
        std::uint32_t flags = 0;
        std::uint32_t sourceMask = 0;
        std::uint32_t fieldFlags = 0;        // fieldFlags：R0 汇总本次响应实际填充的 evidence 字段位。
        std::uint32_t statusFlags = 0;       // statusFlags：R0 汇总 partial/unsupported/truncated/PDB-required 状态位。
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t cpuCount = 0;
        std::uint32_t moduleCount = 0;
        long lastStatus = 0;
        std::vector<DriverIntegrityEvidenceEntry> entries;
    };

    // CpuHardwareSnapshotResult carries the read-only R0 CPUID hardware packet.
    // Input: produced by DriverClient::queryCpuHardwareSnapshot.
    // Processing: featureMask is a stable KSWORD_ARK_CPU_FEATURE_* projection while
    // raw CPUID leaves remain available for diagnostics and future UI expansion.
    // Return behavior: returned by value; unsupported=true means the loaded driver
    // predates IOCTL_KSWORD_ARK_QUERY_CPU_HARDWARE.
    struct CpuHardwareSnapshotResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t logicalProcessorCount = 0;
        std::uint32_t activeProcessorCount = 0;
        std::uint32_t packageCount = 0;
        std::uint32_t family = 0;
        std::uint32_t model = 0;
        std::uint32_t stepping = 0;
        std::uint32_t processorType = 0;
        std::uint32_t brandIndex = 0;
        std::uint32_t clflushLineSize = 0;
        std::uint32_t initialApicId = 0;
        std::uint32_t maxBasicLeaf = 0;
        std::uint32_t maxExtendedLeaf = 0;
        long lastStatus = 0;
        std::uint64_t featureMask = 0;
        std::uint64_t leaf1Ecx = 0;
        std::uint64_t leaf1Edx = 0;
        std::uint64_t leaf7Ebx = 0;
        std::uint64_t leaf7Ecx = 0;
        std::uint64_t leaf7Edx = 0;
        std::uint64_t leaf80000001Ecx = 0;
        std::uint64_t leaf80000001Edx = 0;
        std::string vendor;
        std::string brand;
    };

    // PhysicalMemoryLayoutResult is the R3 view of the R0 physical memory map summary.
    // Input: produced by DriverClient::queryPhysicalMemoryLayout.
    // Processing: stores aggregate ranges only; no physical memory bytes or per-page
    // content are returned to the UI.
    // Return behavior: returned by value; unsupported=true means the loaded driver is old.
    struct PhysicalMemoryLayoutResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t rangeCount = 0;
        std::uint32_t zeroLengthRangeCount = 0;
        std::uint32_t truncated = 0;
        long lastStatus = 0;
        std::uint64_t totalPhysicalBytes = 0;
        std::uint64_t highestPhysicalAddress = 0;
        std::uint64_t largestRangeBytes = 0;
        std::uint64_t smallestRangeBytes = 0;
        std::uint64_t firstBaseAddress = 0;
        std::uint64_t lastEndAddress = 0;
        std::uint64_t estimatedAddressSpaceGapBytes = 0;
    };

    // MutationPrepareInput is the safe R3-side representation of a mutation prepare request.
    // Input: UI/future repair paths populate target kind, address, bytes and expected-before bytes.
    // Processing: DriverClient packs the fields into KSWORD_ARK_MUTATION_PREPARE_REQUEST.
    // Return behavior: used as input to prepareMutation; no member function return.
    struct MutationPrepareInput
    {
        std::uint32_t flags = KSWORD_ARK_MUTATION_FLAG_DRY_RUN;
        std::uint32_t targetKind = KSWORD_ARK_MUTATION_TARGET_UNKNOWN;
        std::uint32_t processId = 0;
        std::uint32_t bytes = 0;
        std::uint64_t targetAddress = 0;
        std::uint64_t targetContext = 0;
        std::vector<std::uint8_t> afterBytes;
        std::vector<std::uint8_t> expectedBeforeBytes;
    };

    // MutationResponseResult carries PREPARE/COMMIT/ROLLBACK response metadata.
    // Input: returned by mutation DriverClient methods.
    // Processing: before/after byte arrays are bounded by the shared protocol max.
    // Return behavior: io.ok reports transport and fixed-response parse success.
    struct MutationResponseResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_MUTATION_STATUS_UNKNOWN;
        std::uint32_t targetKind = KSWORD_ARK_MUTATION_TARGET_UNKNOWN;
        std::uint32_t processId = 0;
        std::uint32_t bytes = 0;
        std::uint32_t riskFlags = 0;
        long lastStatus = 0;
        std::uint64_t transactionId = 0;
        std::uint64_t targetAddress = 0;
        std::uint64_t targetContext = 0;
        std::uint64_t beforeHash = 0;
        std::uint64_t afterHash = 0;
        std::uint64_t timestampTick = 0;
        std::vector<std::uint8_t> beforeBytes;
        std::vector<std::uint8_t> afterBytes;
    };

    // MutationAuditEntry is one read-only transaction audit row.
    // Input: copied from KSWORD_ARK_MUTATION_AUDIT_ENTRY.
    // Processing: UI displays audit/dry-run/rollback status only; no arbitrary-write button is exposed.
    // Return behavior: data-only row.
    struct MutationAuditEntry
    {
        std::uint32_t operation = KSWORD_ARK_MUTATION_OPERATION_UNKNOWN;
        std::uint32_t status = KSWORD_ARK_MUTATION_STATUS_UNKNOWN;
        long lastStatus = 0;
        std::uint32_t targetKind = KSWORD_ARK_MUTATION_TARGET_UNKNOWN;
        std::uint32_t riskFlags = 0;
        std::uint32_t flags = 0;
        std::uint32_t processId = 0;
        std::uint32_t bytes = 0;
        std::uint64_t transactionId = 0;
        std::uint64_t sequence = 0;
        std::uint64_t targetAddress = 0;
        std::uint64_t targetContext = 0;
        std::uint64_t beforeHash = 0;
        std::uint64_t afterHash = 0;
        std::uint64_t timestampTick = 0;
        std::vector<std::uint8_t> byteData;
    };

    // MutationAuditResult carries the bounded R0 audit ring snapshot.
    // Input: produced by DriverClient::queryMutationAudit.
    // Processing: unsupported distinguishes missing transaction IOCTL from empty audit rings.
    // Return behavior: returned by value; entries contains parsed audit rows.
    struct MutationAuditResult
    {
        IoResult io;
        bool unsupported = false;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t lostCount = 0;
        std::uint64_t oldestSequence = 0;
        std::uint64_t nextSequence = 0;
        std::vector<MutationAuditEntry> entries;
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

    // KernelInlineHookEntry 是 R0 Inline Hook 扫描返回的一行 R3 模型。
    struct KernelInlineHookEntry
    {
        std::uint32_t status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN; // 行状态。
        std::uint32_t hookType = KSWORD_ARK_INLINE_HOOK_TYPE_NONE;    // 命中的补丁形态。
        std::uint32_t flags = 0;                                      // R0 诊断标志。
        std::uint32_t originalByteCount = 0;                          // R0 观察基线字节长度；不是磁盘原始字节长度。
        std::uint32_t currentByteCount = 0;                           // 当前字节长度。
        std::uint64_t functionAddress = 0;                            // 函数入口地址。
        std::uint64_t targetAddress = 0;                              // 跳转/补丁目标。
        std::uint64_t moduleBase = 0;                                 // 所属模块基址。
        std::uint64_t targetModuleBase = 0;                           // 目标模块基址。
        std::string functionName;                                     // 导出函数名。
        std::wstring moduleName;                                      // 所属模块名。
        std::wstring targetModuleName;                                // 目标模块名。
        std::vector<std::uint8_t> currentBytes;                       // 当前函数头字节。
        std::vector<std::uint8_t> expectedBytes;                      // 协议兼容字段：R0 观察基线，不代表磁盘原始字节。
    };

    // KernelInlineHookScanResult 承载 R0 Inline Hook 扫描响应。
    struct KernelInlineHookScanResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t moduleCount = 0;
        long lastStatus = 0;
        std::vector<KernelInlineHookEntry> entries;
    };

    // KernelInlinePatchResult 承载 R0 Inline Hook 摘除/修复响应。
    struct KernelInlinePatchResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN;
        std::uint32_t bytesPatched = 0;
        std::uint32_t fieldFlags = 0;
        long lastStatus = 0;
        std::uint64_t functionAddress = 0;
        std::vector<std::uint8_t> beforeBytes;
        std::vector<std::uint8_t> afterBytes;
    };

    // KernelIatEatHookEntry 是内核模块 IAT/EAT 指针检查的一行 R3 模型。
    struct KernelIatEatHookEntry
    {
        std::uint32_t hookClass = KSWORD_ARK_IAT_EAT_HOOK_CLASS_IAT; // IAT 或 EAT。
        std::uint32_t status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN; // 行状态。
        std::uint32_t flags = 0;                                      // R0 诊断标志。
        std::uint32_t ordinal = 0;                                    // 导出序号或 thunk 序号。
        std::uint64_t moduleBase = 0;                                 // 所属模块基址。
        std::uint64_t thunkAddress = 0;                               // IAT thunk 或 EAT 项地址。
        std::uint64_t currentTarget = 0;                              // 当前目标地址。
        std::uint64_t expectedTarget = 0;                             // 期望目标地址。
        std::uint64_t targetModuleBase = 0;                           // 目标模块基址。
        std::string functionName;                                     // 函数名或占位符。
        std::wstring moduleName;                                      // 所属模块名。
        std::wstring importModuleName;                                // IAT 声明导入模块名。
        std::wstring targetModuleName;                                // 当前目标模块名。
    };

    // KernelIatEatHookScanResult 承载 R0 IAT/EAT 扫描响应。
    struct KernelIatEatHookScanResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t moduleCount = 0;
        long lastStatus = 0;
        std::vector<KernelIatEatHookEntry> entries;
    };

    // DriverMajorFunctionEntry 是 Phase-9 DriverObject.MajorFunction 单行模型。
    struct DriverMajorFunctionEntry
    {
        std::uint32_t majorFunction = 0;       // IRP_MJ_* 编号。
        std::uint32_t flags = 0;               // R0 诊断 flags。
        std::uint64_t dispatchAddress = 0;     // dispatch 入口地址，仅展示。
        std::uint64_t moduleBase = 0;          // 所属模块基址，仅展示。
        std::wstring moduleName;               // 所属模块名。
    };

    // DriverDeviceEntry 是 Phase-9 DeviceObject/AttachedDevice 单行模型。
    struct DriverDeviceEntry
    {
        std::uint32_t relationDepth = 0;       // 0=DriverObject->DeviceObject 链，>0=AttachedDevice 深度。
        std::uint32_t deviceType = 0;          // DEVICE_TYPE。
        std::uint32_t flags = 0;               // DO_* flags。
        std::uint32_t characteristics = 0;     // FILE_DEVICE_* characteristics。
        std::uint32_t stackSize = 0;           // DeviceObject.StackSize。
        std::uint32_t alignmentRequirement = 0;// DeviceObject.AlignmentRequirement。
        long nameStatus = 0;                   // ObQueryNameString 状态。
        std::uint64_t rootDeviceObjectAddress = 0; // 根 DeviceObject。
        std::uint64_t deviceObjectAddress = 0;     // 当前 DeviceObject。
        std::uint64_t nextDeviceObjectAddress = 0; // NextDevice。
        std::uint64_t attachedDeviceObjectAddress = 0; // AttachedDevice。
        std::uint64_t driverObjectAddress = 0;    // DeviceObject.DriverObject。
        std::wstring deviceName;              // 设备对象名，可能为空。
    };

    // DriverObjectQueryResult 承载 Phase-9 DriverObject/DeviceObject 查询响应。
    struct DriverObjectQueryResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t queryStatus = KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_UNAVAILABLE;
        std::uint32_t fieldFlags = 0;
        std::uint32_t majorFunctionCount = 0;
        std::uint32_t totalDeviceCount = 0;
        std::uint32_t returnedDeviceCount = 0;
        long lastStatus = 0;
        std::uint32_t driverFlags = 0;
        std::uint32_t driverSize = 0;
        std::uint64_t driverObjectAddress = 0;
        std::uint64_t driverStart = 0;
        std::uint64_t driverSection = 0;
        std::uint64_t driverUnload = 0;
        std::wstring driverName;
        std::wstring serviceKeyName;
        std::wstring imagePath;
        std::vector<DriverMajorFunctionEntry> majorFunctions;
        std::vector<DriverDeviceEntry> devices;
    };

    // IoctlRegistryEntry 承载 KswordARK dispatch registry 的一条只读诊断行。
    struct IoctlRegistryEntry
    {
        std::uint32_t ioControlCode = 0;       // ioControlCode：完整 CTL_CODE。
        std::uint32_t functionNumber = 0;      // functionNumber：CTL_CODE 的 function 部分。
        std::uint32_t method = 0;              // method：METHOD_BUFFERED 等传输方式。
        std::uint32_t access = 0;              // access：FILE_ANY_ACCESS/READ/WRITE。
        std::uint32_t flags = 0;                // flags：dispatch registry flags。
        std::uint64_t requiredCapability = 0;  // requiredCapability：DynData capability 门槛。
        std::uint64_t handlerAddress = 0;       // handlerAddress：可选 handler 诊断地址。
        std::string name;                       // name：注册表中的固定名称。
    };

    // IoctlRegistryQueryResult 承载 KswordARK 自身 IOCTL registry 查询响应。
    struct IoctlRegistryQueryResult
    {
        IoResult io;                            // io：DeviceIoControl 传输状态。
        bool unsupported = false;               // unsupported：旧驱动未注册查询 IOCTL。
        std::uint32_t version = 0;              // version：协议版本。
        std::uint32_t status = 0;               // status：完整/截断状态。
        std::uint32_t totalCount = 0;           // totalCount：R0 registry 总行数。
        std::uint32_t returnedCount = 0;        // returnedCount：本次返回行数。
        std::uint32_t duplicateCount = 0;       // duplicateCount：重复控制码数量。
        long lastStatus = 0;                    // lastStatus：R0 查询状态。
        std::vector<IoctlRegistryEntry> entries; // entries：按 dispatch 顺序排列的行。
    };

    // DriverForceUnloadResult 承载 R0 DriverObject 强制卸载响应。
    struct DriverForceUnloadResult
    {
        IoResult io;                         // io：DeviceIoControl 调用状态。
        std::uint32_t version = 0;           // version：协议版本。
        std::uint32_t status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNKNOWN; // status：卸载聚合状态。
        std::uint32_t flags = 0;             // flags：请求 flags 回显。
        long lastStatus = 0;                 // lastStatus：卸载线程/后端状态。
        long waitStatus = 0;                 // waitStatus：KeWaitForSingleObject 状态。
        std::uint32_t cleanupFlagsApplied = 0; // cleanupFlagsApplied：R0 实际执行的持久清理 flags。
        std::uint32_t deletedDeviceCount = 0;  // deletedDeviceCount：R0 实际删除的 DeviceObject 数量。
        std::uint64_t driverObjectAddress = 0; // driverObjectAddress：诊断地址。
        std::uint64_t driverUnloadAddress = 0; // driverUnloadAddress：DriverUnload 入口。
        std::uint32_t callbackCandidates = 0;  // callbackCandidates：按模块基址命中的回调候选数。
        std::uint32_t callbacksRemoved = 0;    // callbacksRemoved：R0 成功移除的回调数。
        std::uint32_t callbackFailures = 0;    // callbackFailures：R0 移除失败或不支持的回调数。
        long callbackLastStatus = 0;           // callbackLastStatus：最后一个回调移除失败状态。
        std::wstring driverName;             // driverName：R0 规范化对象名。
    };

    // CallbackRuntimeResult wraps the runtime-state response packet.
    struct CallbackRuntimeResult
    {
        IoResult io;
        KSWORD_ARK_CALLBACK_RUNTIME_STATE state{};
    };

    // MinifilterBypassPidResult wraps the fixed PID whitelist response.
    // Input: none; DriverClient::queryMinifilterBypassPids fills this struct.
    // Processing: io reports transport/protocol success and response carries
    // the full R0 whitelist snapshot.
    // Return behavior: the struct itself has no methods; callers inspect io.ok.
    struct MinifilterBypassPidResult
    {
        IoResult io;
        KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE response{};
    };

    // CallbackRemoveResult wraps the legacy external-callback removal response packet.
    struct CallbackRemoveResult
    {
        IoResult io;
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE response{};
    };

    // CallbackRemoveExResult wraps the extended removal response.
    // Input: none; it is returned by DriverClient::removeExternalCallbackEx.
    // Processing: keeps public-API and experimental-unlink diagnostics together.
    // Return behavior: io.ok reports transport/protocol success; response.ntstatus
    // reports the kernel operation result.
    struct CallbackRemoveExResult
    {
        IoResult io;
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE response{};
    };

    // CallbackEnumEntry 是 R0 回调遍历的一行 R3 模型。
    struct CallbackEnumEntry
    {
        std::uint32_t callbackClass = 0;
        std::uint32_t source = 0;
        std::uint32_t status = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t trustFlags = 0;        // trustFlags：PDB/public/fallback/revalidated 可信位。
        std::uint32_t removeBehavior = 0;    // removeBehavior：R0 推荐的公开 API/实验 unlink 行为。
        std::uint32_t removeFlags = 0;       // removeFlags：兼容旧 UI 命名，始终镜像 removeBehavior。
        std::uint32_t operationMask = 0;
        std::uint32_t objectTypeMask = 0;
        std::uint64_t generation = 0;        // generation：R0 枚举代次，用于 EX 移除前重验证。
        long lastStatus = 0;
        std::uint64_t callbackAddress = 0;
        std::uint64_t contextAddress = 0;
        std::uint64_t registrationAddress = 0;
        std::uint64_t identityHash = 0;      // Reserved identity hash; v1 protocol leaves it empty/zero.
        std::uint64_t rawStorageValue = 0;   // Reserved raw storage value; v1 protocol leaves it zero.
        std::uint64_t moduleBase = 0;
        std::uint32_t moduleSize = 0;
        std::wstring name;
        std::wstring altitude;
        std::wstring modulePath;
        std::wstring detail;
    };

    // CallbackEnumResult 承载 R0 回调遍历响应。
    struct CallbackEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t flags = 0;
        long lastStatus = 0;
        std::vector<CallbackEnumEntry> entries;
    };

    // KeyboardHotkeyEntry 是 R0 win32k RegisterHotKey 内部表的一行 R3 模型。
    struct KeyboardHotkeyEntry
    {
        std::uint32_t source = 0;
        std::uint32_t status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
        std::uint32_t flags = 0;
        std::uint32_t bucketIndex = 0;
        std::uint32_t depth = 0;
        std::uint32_t modifiers = 0;
        std::uint32_t modifierFlags2 = 0;
        std::uint32_t virtualKey = 0;
        std::uint32_t hotkeyId = 0;
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        long lastStatus = 0;
        std::uint64_t hotkeyObject = 0;
        std::uint64_t nextHotkeyObject = 0;
        std::uint64_t sessionGlobals = 0;
        std::uint64_t threadInfo = 0;
        std::uint64_t threadObject = 0;
        std::uint64_t windowObject = 0;
        std::wstring detail;
    };

    // KeyboardHotkeyEnumResult 承载 R0 键盘热键枚举响应。
    struct KeyboardHotkeyEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t flags = 0;
        long lastStatus = 0;
        std::uint64_t win32kBase = 0;
        std::uint64_t sessionGlobals = 0;
        std::uint32_t tableOffset = 0;
        std::uint32_t hotkeyNextOffset = 0;
        std::uint32_t hotkeyModifiersOffset = 0;
        std::uint32_t hotkeyVkOffset = 0;
        std::uint32_t hotkeyIdOffset = 0;
        std::vector<KeyboardHotkeyEntry> entries;
    };

    // KeyboardHookEntry 是 R0 win32k WH_KEYBOARD/WH_KEYBOARD_LL 链的一行 R3 模型。
    struct KeyboardHookEntry
    {
        std::uint32_t source = 0;
        std::uint32_t status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
        std::uint32_t flags = 0;
        std::uint32_t hookType = 0;
        std::uint32_t hookScope = KSWORD_ARK_KEYBOARD_HOOK_SCOPE_UNKNOWN;
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        std::uint32_t moduleId = 0;
        long lastStatus = 0;
        std::uint64_t hookObject = 0;
        std::uint64_t chainHead = 0;
        std::uint64_t nextHookObject = 0;
        std::uint64_t threadInfo = 0;
        std::uint64_t targetThreadInfo = 0;
        std::uint64_t desktopInfo = 0;
        std::uint64_t procedureAddress = 0;
        std::uint64_t procedureOffset = 0;
        std::uint64_t moduleBase = 0;
        std::wstring detail;
    };

    // KeyboardHookEnumResult 承载 R0 键盘钩子枚举响应。
    struct KeyboardHookEnumResult
    {
        IoResult io;
        std::uint32_t version = 0;
        std::uint32_t status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
        std::uint32_t totalCount = 0;
        std::uint32_t returnedCount = 0;
        std::uint32_t flags = 0;
        long lastStatus = 0;
        std::uint64_t win32kBase = 0;
        std::uint32_t threadHookArrayOffset = 0;
        std::uint32_t desktopInfoOffset = 0;
        std::uint32_t desktopHookArrayOffset = 0;
        std::uint32_t hookNextOffset = 0;
        std::uint32_t hookTypeOffset = 0;
        std::uint32_t hookProcedureOffset = 0;
        std::uint32_t hookFlagsOffset = 0;
        std::uint32_t hookModuleIdOffset = 0;
        std::uint32_t hookTargetThreadInfoOffset = 0;
        std::vector<KeyboardHookEntry> entries;
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

    // DynDataProfileField 是 R3 JSON profile 解析后的单个字段包。
    // 输入：fieldId/offset 来自 profiles/ark_dyndata JSON。
    // 处理：ArkDriverClient 将该数组打包为 KSW_APPLY_DYN_PROFILE_REQUEST。
    // 返回行为：结构本身无返回值，只作为 applyDynDataProfile 的输入。
    struct DynDataProfileField
    {
        std::uint32_t fieldId = 0;
        std::uint32_t offset = 0;
    };

    // DynDataProfileExItem 是 v2 profile pack 展开后的 typed item。
    // 输入：itemId/itemKind/value/flags 来自 R3 JSON pack 校验结果。
    // 处理：ArkDriverClient 只做 packed IOCTL 传输；语义校验由 R3/R0 双层完成。
    // 返回行为：结构本身无返回值，只作为 applyDynDataProfileEx 的输入元素。
    struct DynDataProfileExItem
    {
        std::uint32_t itemId = 0;
        std::uint32_t itemKind = 0;
        std::uint32_t value = 0;
        std::uint32_t flags = 0;
    };

    // DynDataProfileApplyInput 是驱动 apply IOCTL 的 R3 输入模型。
    // 输入：profile 元数据、当前 ntoskrnl identity 和字段列表。
    // 处理：客户端只负责协议打包，不解析 JSON 语义。
    // 返回行为：传入 applyDynDataProfile 后得到 DynDataProfileApplyResult。
    struct DynDataProfileApplyInput
    {
        std::string profileName;
        std::string pdbName;
        std::string pdbGuid;
        std::uint32_t pdbAge = 0;
        ArkDynModuleIdentity ntoskrnl;
        std::vector<DynDataProfileField> fields;
    };

    // DynDataProfileApplyResult 承载 R0 合并 PDB profile 后的固定响应。
    // 输入：无，由 DriverClient::applyDynDataProfile 返回。
    // 处理：保存 R0 校验结果、应用字段数、状态位和消息。
    // 返回行为：io.ok 表示 IOCTL 调用和协议响应可用；status 表示 R0 语义结果。
    struct DynDataProfileApplyResult
    {
        IoResult io;
        long status = 0;
        std::uint32_t appliedFieldCount = 0;
        std::uint32_t rejectedFieldCount = 0;
        std::uint32_t unknownFieldCount = 0;
        std::uint32_t statusFlags = 0;
        std::uint64_t capabilityMask = 0;
        std::wstring message;
    };

    // DynDataProfileApplyExInput 是 v2 typed item apply 的 R3 输入模型。
    // 输入：profile 元数据、当前 ntoskrnl identity 和 v2 items。
    // 处理：客户端打包成 KSW_APPLY_DYN_PROFILE_EX_REQUEST。
    // 返回行为：传入 applyDynDataProfileEx 后得到 DynDataProfileApplyExResult。
    struct DynDataProfileApplyExInput
    {
        std::string profileName;
        std::string pdbName;
        std::string pdbGuid;
        std::uint32_t pdbAge = 0;
        ArkDynModuleIdentity ntoskrnl;
        std::vector<DynDataProfileExItem> items;
    };

    // DynDataProfileApplyExResult 承载 R0 合并 v2 typed item 后的响应。
    // 输入：无，由 DriverClient::applyDynDataProfileEx 返回。
    // 处理：保存 item 级应用/拒绝/未知计数、状态位和 capability。
    // 返回行为：io.ok 表示 IOCTL 调用和协议响应可用；status 表示 R0 语义结果。
    struct DynDataProfileApplyExResult
    {
        IoResult io;
        long status = 0;
        std::uint32_t appliedItemCount = 0;
        std::uint32_t rejectedItemCount = 0;
        std::uint32_t unknownItemCount = 0;
        std::uint32_t statusFlags = 0;
        std::uint64_t capabilityMask = 0;
        std::wstring message;
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


    // VariableAuditResultBase 保存所有新增只读审计 wrapper 共用的 IO 状态。
    // 输入：由 ArkDriverAudit.cpp 在解析 METHOD_BUFFERED 响应时填充。
    // 处理：unsupported 用于区分旧驱动未注册 IOCTL 与协议解析失败。
    // 返回行为：结构体本身无函数返回；调用方读取字段展示 R0 审计状态。
    struct VariableAuditResultBase
    {
        IoResult io;                         // io：底层 DeviceIoControl 与协议解析结果。
        bool unsupported = false;            // unsupported：旧驱动缺少该 IOCTL 或明确返回不支持。
        std::uint32_t version = 0;           // version：共享协议版本。
        std::uint32_t status = 0;            // status：协议定义的总体状态。
        std::uint32_t flags = 0;             // flags：响应级标志或查询标志。
        std::uint32_t totalCount = 0;        // totalCount：R0 观察到的总行数。
        std::uint32_t returnedCount = 0;     // returnedCount：R0 写入输出缓冲的行数。
        std::uint32_t entrySize = 0;         // entrySize：单行协议结构大小。
        long lastStatus = 0;                 // lastStatus：R0 最近一次 NTSTATUS。
    };

    // NetworkEndpointAuditResult 承载 TCP/UDP endpoint 的 PDB/R0 只读审计结果。
    // 输入：queryNetworkTcpEndpoints/queryNetworkUdpEndpoints 返回。
    // 处理：entries 直接保存 shared/driver 协议行，避免 UI 重新定义字段。
    // 返回行为：io.ok 表示传输和协议解析成功，unsupported 表示旧驱动缺入口。
    struct NetworkEndpointAuditResult : VariableAuditResultBase
    {
        std::uint32_t sourceFlags = 0;        // sourceFlags：tcpip/netio/runtime 等证据来源。
        std::uint32_t budgetRows = 0;         // budgetRows：R0 实际接受的行预算。
        std::uint32_t generation = 0;         // generation：R0 快照代数。
        std::vector<KSWORD_ARK_NETWORK_ENDPOINT_ROW> entries;
    };

    // NetworkWfpInventoryResult 承载 WFP provider/sublayer/filter/callout 只读审计结果。
    // 输入：queryNetworkWfpInventory 返回。
    // 处理：每行包含对象类型、GUID、函数地址和 owner module 提示。
    // 返回行为：不包含任何禁用、detach 或删除动作。
    struct NetworkWfpInventoryResult : VariableAuditResultBase
    {
        std::uint32_t sourceFlags = 0;
        std::uint32_t budgetRows = 0;
        std::uint32_t generation = 0;
        std::vector<KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW> entries;
    };

    // NetworkNdisChainResult 承载 NDIS miniport/filter/protocol/binding 链只读审计结果。
    // 输入：queryNetworkNdisChain 返回。
    // 处理：每行保留对象地址、父对象、驱动对象和 owner module 诊断字段。
    // 返回行为：不执行 NDIS detach、pause、restart 或 filter 操作。
    struct NetworkNdisChainResult : VariableAuditResultBase
    {
        std::uint32_t sourceFlags = 0;
        std::uint32_t budgetRows = 0;
        std::uint32_t generation = 0;
        std::vector<KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW> entries;
    };

    // MinifilterInventoryResult 承载 fltMgr 过滤器与实例绑定清单。
    // 输入：queryMinifilterInventory 返回。
    // 处理：entries 保存 filter/altitude/volume/callback-owner 状态。
    // 返回行为：仅用于展示，不卸载、不 detach、不改 callback。
    struct MinifilterInventoryResult : VariableAuditResultBase
    {
        std::uint32_t responseFlags = 0;
        std::vector<KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY> entries;
    };

    // StorageVolumeStackAuditResult 承载卷设备栈和 fvevol 位置审计结果。
    // 输入：queryVolumeStackAudit 返回。
    // 处理：rows 保存设备对象、驱动对象、栈深度、风险和置信度。
    // 返回行为：不返回 BitLocker 密钥材料，也不改变存储栈。
    struct StorageVolumeStackAuditResult : VariableAuditResultBase
    {
        std::uint32_t responseFlags = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t maxRows = 0;
        std::uint32_t fvevolPresent = 0;
        std::uint32_t fvevolPosition = 0xFFFFFFFFUL;
        std::vector<KSWORD_ARK_VOLUME_STACK_ROW> rows;
    };

    // StorageBitlockerFveAuditResult 承载 BitLocker/FVE 安全状态摘要。
    // 输入：queryBitlockerFveAudit 返回。
    // 处理：rows 只包含保护状态、转换状态、锁定状态和 protector 类型计数。
    // 返回行为：协议不承载密钥、恢复密码或元数据 payload。
    struct StorageBitlockerFveAuditResult : VariableAuditResultBase
    {
        std::uint32_t responseFlags = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t maxRows = 0;
        std::vector<KSWORD_ARK_BITLOCKER_FVE_ROW> rows;
    };

    // StorageMountMgrMappingAuditResult 承载 MountMgr 盘符/GUID/NT 路径映射审计结果。
    // 输入：queryMountMgrMappingAudit 返回。
    // 处理：rows 保存符号名和风险标志，不解析卷内数据。
    // 返回行为：仅展示映射关系，不修改挂载点。
    struct StorageMountMgrMappingAuditResult : VariableAuditResultBase
    {
        std::uint32_t responseFlags = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t maxRows = 0;
        std::vector<KSWORD_ARK_MOUNTMGR_MAPPING_ROW> rows;
    };

    // StorageFilesystemIntegrityAuditResult 承载文件系统 DriverObject/FastIo/dispatch 完整性行。
    // 输入：queryFilesystemIntegrityAudit 返回。
    // 处理：rows 保存 slot owner、target address 和风险，不写任何函数指针。
    // 返回行为：只读审计结果。
    struct StorageFilesystemIntegrityAuditResult : VariableAuditResultBase
    {
        std::uint32_t responseFlags = 0;
        std::uint32_t fieldFlags = 0;
        std::uint32_t maxRows = 0;
        std::vector<KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW> rows;
    };

    // SecurityStatusAuditResult 承载 CI/SecureBoot/VBS/SKCI/调试态固定响应。
    // 输入：querySecurityStatus 返回。
    // 处理：response 直接保留共享协议固定结构，便于 UI 展示所有字段。
    // 返回行为：unsupported 表示旧驱动缺安全审计入口。
    struct SecurityStatusAuditResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE response{};
    };

    // DriverTrustViewAuditResult 承载已加载驱动签名/模块 cross-view 行。
    // 输入：queryDriverTrustView 返回。
    // 处理：entries 保存模块名、imageBase、signingLevel 和 conflictFlags。
    // 返回行为：不执行签名策略修改或 CI 绕过。
    struct DriverTrustViewAuditResult : VariableAuditResultBase
    {
        std::uint32_t fieldFlags = 0;
        std::uint32_t sourceMask = 0;
        std::uint32_t maxEntriesAccepted = 0;
        std::uint32_t truncated = 0;
        long moduleQueryStatus = 0;
        long signingResolverStatus = 0;
        std::vector<KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY> entries;
    };

    // HyperVSummaryAuditResult 承载 Hyper-V/VBS 相关模块和 CPUID 固定摘要。
    // 输入：queryHyperVSummary 返回。
    // 处理：response 保留 vendor、module status 和 sourceMask。
    // 返回行为：只读展示，不关闭 Hyper-V 或 VBS。
    struct HyperVSummaryAuditResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE response{};
    };

    // AppControlStatusAuditResult 承载 AppID/AppLocker/mssecflt/BAM 只读状态。
    // 输入：queryAppControlStatus 返回。
    // 处理：response 保留 callback owner module 和模块状态。
    // 返回行为：不改 AppLocker/WDAC/CI 策略。
    struct AppControlStatusAuditResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE response{};
    };

    // Win32kProfileStatusResult 承载 win32k/win32kbase/win32kfull profile 和 session 摘要。
    // 输入：queryWin32kProfileStatus 返回。
    // 处理：sessions 保存 per-session readiness；fieldOffsets 保存 PDB offset 状态。
    // 返回行为：只读状态，不安装窗口 hook。
    struct Win32kProfileStatusResult : VariableAuditResultBase
    {
        std::uint64_t capabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        std::uint64_t userGetSiloGlobals = 0;
        KSWORD_ARK_WIN32K_MODULE_STATE win32k{};
        KSWORD_ARK_WIN32K_MODULE_STATE win32kbase{};
        KSWORD_ARK_WIN32K_MODULE_STATE win32kfull{};
        KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets{};
        std::vector<KSWORD_ARK_WIN32K_SESSION_ENTRY> entries;
    };

    // Win32kWindowsResult 承载 HWND/tagWND cross-view 行。
    // 输入：queryWin32kWindows 返回。
    // 处理：entries 保存 HWND、PID/TID、desktop、rect、title/class 状态。
    // 返回行为：不读取消息 payload，不改变窗口状态。
    struct Win32kWindowsResult : VariableAuditResultBase
    {
        std::uint64_t capabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets{};
        std::vector<KSWORD_ARK_WIN32K_WINDOW_ENTRY> entries;
    };

    // Win32kGuiThreadsResult 承载 GUI thread/tagQ/focus/capture/caret 快照。
    // 输入：queryWin32kGuiThreads 返回。
    // 处理：entries 保存队列对象和活跃 HWND 诊断地址。
    // 返回行为：不 hook、不阻断、不重放窗口消息。
    struct Win32kGuiThreadsResult : VariableAuditResultBase
    {
        std::uint64_t capabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets{};
        std::vector<KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY> entries;
    };

    // Win32kHotkeysPdbResult 承载 PDB-backed hotkey 快照。
    // 输入：queryWin32kHotkeysPdb 返回。
    // 处理：entries 保存 hotkey object、VK/modifiers 和关联 HWND/threadInfo。
    // 返回行为：不删除热键，不修改链表。
    struct Win32kHotkeysPdbResult : VariableAuditResultBase
    {
        std::uint64_t capabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets{};
        std::vector<KSWORD_ARK_WIN32K_HOTKEY_ENTRY> entries;
    };

    // Win32kHooksPdbResult 承载 PDB-backed hook 链快照。
    // 输入：queryWin32kHooksPdb 返回。
    // 处理：entries 保存 hook object、procedure、moduleBase 和 target threadInfo。
    // 返回行为：不 remove/unlink hook 链。
    struct Win32kHooksPdbResult : VariableAuditResultBase
    {
        std::uint64_t capabilityMask = 0;
        std::uint64_t missingCapabilityMask = 0;
        KSWORD_ARK_WIN32K_FIELD_OFFSETS fieldOffsets{};
        std::vector<KSWORD_ARK_WIN32K_HOOK_ENTRY> entries;
    };

    // Win32kWindowRuntimeDetailResult 承载单 HWND 的 win32k readiness/detail。
    // 输入：queryWin32kWindowDetail 返回。
    // 处理：response 保存 module/profile/capability/offset 状态；tagWND reader 未启用时也能解释原因。
    // 返回行为：只读展示，不安装窗口 hook，不读取消息 payload。
    struct Win32kWindowRuntimeDetailResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_WIN32K_WINDOW_DETAIL_RESPONSE response{};
    };

    // DeviceAuditResult 承载 Device/Input/USB/GPU 统一只读设备审计结果。
    // 输入：queryDeviceStackAudit/queryInputStackAudit/queryUsbTopologyAudit/queryGpuDisplayWatchdogAudit 返回。
    // 处理：entries 保存 DriverObject、DeviceObject、attached/next 链和风险标志。
    // 返回行为：不禁用设备、不卸载驱动、不 detach stack。
    struct DeviceAuditResult : VariableAuditResultBase
    {
        std::uint32_t profileFlags = 0;
        std::uint32_t responseFlags = 0;
        std::uint32_t targetCount = 0;
        std::uint32_t driverCount = 0;
        std::uint32_t deviceCount = 0;
        std::vector<KSWORD_ARK_DEVICE_AUDIT_ENTRY> entries;
    };

    // CidTableAuditResult 承载 PspCidTable 只读枚举行。
    // 输入：enumCidTable 返回。
    // 处理：entries 保存 CID、对象类型、引用状态和对象地址。
    // 返回行为：不删除 CID，不隐藏进程/线程。
    struct CidTableAuditResult : VariableAuditResultBase
    {
        std::uint32_t visitedCount = 0;
        std::uint32_t maxVisitCount = 0;
        std::uint64_t pspCidTableAddress = 0;
        std::uint64_t dynDataCapabilityMask = 0;
        std::uint32_t htTableCodeOffset = KSWORD_ARK_KERNEL_OBJECT_OFFSET_UNAVAILABLE;
        std::uint32_t hteLowValueOffset = KSWORD_ARK_KERNEL_OBJECT_OFFSET_UNAVAILABLE;
        std::vector<KSWORD_ARK_CID_TABLE_ENTRY> entries;
    };

    // KernelObjectSummaryAuditResult 承载单对象 header/type/counter 摘要。
    // 输入：queryKernelObjectSummary 返回。
    // 处理：response 直接保存共享固定响应结构。
    // 返回行为：只读展示对象元数据，不改对象头或引用计数。
    struct KernelObjectSummaryAuditResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE response{};
    };

    // IpcSummaryAuditResult 承载 ALPC/Pipe/Mailslot IPC 摘要。
    // 输入：queryIpcSummary 返回。
    // 处理：response 保留句柄、对象地址、typeName 和降级详情。
    // 返回行为：不关闭句柄、不发送消息、不修改 IPC 对象。
    struct IpcSummaryAuditResult
    {
        IoResult io;
        bool unsupported = false;
        KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE response{};
    };

    // DynDataV4ApplyInput 是 v4 PDB profile 的 R3 打包输入。
    // 输入：module/capabilityGroups/items 来自 PDB extractor 生成的已校验 profile。
    // 处理：ArkDriverClient 只负责长度校验和协议传输。
    // 返回行为：传入 applyDynDataProfileV4 后得到 DynDataV4ApplyResult。
    struct DynDataV4ApplyInput
    {
        KSW_DYN_V4_MODULE_IDENTITY_PACKET module{};
        std::vector<KSW_DYN_V4_CAPABILITY_GROUP_PACKET> capabilityGroups;
        std::vector<KSW_DYN_V4_ITEM_PACKET> items;
        std::uint32_t flags = 0;
    };

    // DynDataV4ApplyResult 承载 R0 接收 v4 module profile 后的固定响应。
    // 输入：无，由 applyDynDataProfileV4 返回。
    // 处理：response 保留模块身份、statusFlags、capabilityMask 和消息。
    // 返回行为：io.ok 表示传输/协议成功，unsupported 表示旧驱动缺 v4 入口。
    struct DynDataV4ApplyResult
    {
        IoResult io;
        bool unsupported = false;
        KSW_APPLY_DYN_PROFILE_V4_RESPONSE response{};
    };

    // DynDataV4ModulesResult 承载已加载 v4 module profile 状态。
    // 输入：queryDynDataV4Modules 返回。
    // 处理：entries 保存每个模块 class/profile/status/capability 状态。
    // 返回行为：只读查询当前 R0 v4 profile cache。
    struct DynDataV4ModulesResult : VariableAuditResultBase
    {
        std::vector<KSW_DYN_V4_MODULE_STATUS_ENTRY> entries;
    };

    // DynDataV4CapabilityGroupsResult 承载 v4 capability group 完整性状态。
    // 输入：queryDynDataV4CapabilityGroups 返回。
    // 处理：entries 保存 required/optional present/missing 计数。
    // 返回行为：只读展示 profile 缺口。
    struct DynDataV4CapabilityGroupsResult : VariableAuditResultBase
    {
        std::vector<KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY> entries;
    };

    // DynDataV4MissingItemsResult 承载 v4 required/optional missing 摘要。
    // 输入：queryDynDataV4MissingItems 返回。
    // 处理：entries 保存 itemName/reason，供 UI 展示 profile 缺失项。
    // 返回行为：只读查询，不修改 DynData 状态。
    struct DynDataV4MissingItemsResult : VariableAuditResultBase
    {
        std::vector<KSW_DYN_V4_MISSING_ITEM_ENTRY> entries;
    };

    // DynDataV4ItemsResult 承载 v4 已接受 item 的只读清单。
    // 输入：queryDynDataV4Items 返回。
    // 处理：entries 保存 moduleClassId、itemIndex 和完整 KSW_DYN_V4_ITEM_PACKET。
    // 返回行为：只读查询，不重新应用 profile，也不把 item 接入业务路径。
    struct DynDataV4ItemsResult : VariableAuditResultBase
    {
        std::vector<KSW_DYN_V4_ITEM_STATUS_ENTRY> entries;
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
