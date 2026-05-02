/*++

Module Name:

    driver_unload.c

Abstract:

    Force DriverObject unload by name.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntstrsafe.h>

/* 中文说明：默认等待 DriverUnload 系统线程 3 秒，避免 UI 无限阻塞。 */
#define KSW_DRIVER_UNLOAD_DEFAULT_TIMEOUT_MS 3000UL
/* 中文说明：最大等待 30 秒，防止恶意/异常 DriverUnload 挂死调用者。 */
#define KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS 30000UL
/* 中文说明：卸载线程上下文使用独立 tag，便于 pool 泄漏排查。 */
#define KSW_DRIVER_UNLOAD_TAG 'uDsK'
/* 中文说明：DeviceObject 清理最多遍历 128 个节点，避免损坏链表造成无限循环。 */
#define KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT 128UL

#ifndef THREAD_ALL_ACCESS
/* 中文说明：旧 WDK 头缺失时补齐线程全访问掩码，供 PsCreateSystemThread 使用。 */
#define THREAD_ALL_ACCESS 0x001FFFFFUL
#endif

/* 中文说明：命名 DriverObject 引用入口，和 driver_object_query.c 保持同一策略。 */
NTSYSAPI
NTSTATUS
NTAPI
ObReferenceObjectByName(
    _In_ PUNICODE_STRING ObjectName,
    _In_ ULONG Attributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Inout_opt_ PVOID ParseContext,
    _Out_ PVOID* Object
    );

/* 中文说明：IoDriverObjectType 用于 ObReferenceObjectByName 的类型约束。 */
extern POBJECT_TYPE* IoDriverObjectType;

/* 中文说明：ObMakeTemporaryObject 让命名对象在引用计数归零后可被对象管理器回收。 */
NTSYSAPI
VOID
NTAPI
ObMakeTemporaryObject(
    _In_ PVOID Object
    );

/* 中文说明：卸载线程上下文在非分页池中分配，线程退出前释放。 */
typedef struct _KSW_DRIVER_UNLOAD_CONTEXT
{
    PDRIVER_OBJECT DriverObject;
    ULONG Flags;
    NTSTATUS UnloadStatus;
    NTSTATUS CleanupStatus;
    PDRIVER_UNLOAD DriverUnload;
    ULONG DeletedDeviceCount;
    ULONG CleanupFlagsApplied;
} KSW_DRIVER_UNLOAD_CONTEXT, *PKSW_DRIVER_UNLOAD_CONTEXT;

/* 中文说明：把共享协议中的名称复制为 \Driver\Name 形式。 */
static NTSTATUS
KswordARKDriverUnloadBuildObjectName(
    _In_reads_(KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) const WCHAR* SourceName,
    _Out_writes_(DestinationChars) PWCHAR DestinationName,
    _In_ ULONG DestinationChars
    )
{
    ULONG inputChars = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (SourceName == NULL || DestinationName == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    DestinationName[0] = L'\0';

    while (inputChars < KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS &&
        SourceName[inputChars] != L'\0') {
        ++inputChars;
    }
    if (inputChars == 0UL || inputChars >= KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (inputChars >= 8UL &&
        SourceName[0] == L'\\' &&
        (SourceName[1] == L'D' || SourceName[1] == L'd') &&
        (SourceName[2] == L'R' || SourceName[2] == L'r') &&
        (SourceName[3] == L'I' || SourceName[3] == L'i') &&
        (SourceName[4] == L'V' || SourceName[4] == L'v') &&
        (SourceName[5] == L'E' || SourceName[5] == L'e') &&
        (SourceName[6] == L'R' || SourceName[6] == L'r') &&
        SourceName[7] == L'\\') {
        status = RtlStringCchCopyNW(
            DestinationName,
            DestinationChars,
            SourceName,
            inputChars);
    }
    else {
        status = RtlStringCchPrintfW(
            DestinationName,
            DestinationChars,
            L"\\Driver\\%ws",
            SourceName);
    }

    return status;
}

/* 中文说明：按对象名引用 DriverObject，不接受 R3 传入地址。 */
static NTSTATUS
KswordARKDriverUnloadReferenceByName(
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut,
    _Out_writes_(NameChars) PWCHAR NormalizedNameOut,
    _In_ ULONG NameChars
    )
{
    UNICODE_STRING objectName;
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL || DriverObjectOut == NULL || NormalizedNameOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *DriverObjectOut = NULL;
    NormalizedNameOut[0] = L'\0';

    status = KswordARKDriverUnloadBuildObjectName(
        Request->driverName,
        NormalizedNameOut,
        NameChars);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&objectName, NormalizedNameOut);
    status = ObReferenceObjectByName(
        &objectName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        0,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)DriverObjectOut);
    return status;
}

/* 中文说明：可选清理 dispatch 表，和 SKT64 的 force unload 语义对齐但由 flag 控制。 */
static VOID
KswordARKDriverUnloadClearDispatchUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject
    )
{
    if (DriverObject == NULL) {
        return;
    }

    __try {
        DriverObject->FastIoDispatch = NULL;
        RtlZeroMemory(DriverObject->MajorFunction, sizeof(DriverObject->MajorFunction));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        (VOID)0;
    }
}

/* 中文说明：可选清理 DriverUnload 指针，避免右键重复触发同一个卸载入口。 */
static VOID
KswordARKDriverUnloadClearUnloadPointerUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject
    )
{
    if (DriverObject == NULL) {
        return;
    }

    __try {
        DriverObject->DriverUnload = NULL;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        (VOID)0;
    }
}

/* 中文说明：目标没有 DriverUnload 时，按原始 DeviceObject->NextDevice 链删除设备。 */
static NTSTATUS
KswordARKDriverUnloadDeleteDeviceObjectsUnsafe(
    _Inout_ PDRIVER_OBJECT DriverObject,
    _Out_ ULONG* DeletedDeviceCountOut
    )
{
    PDEVICE_OBJECT deviceCursor = NULL;
    ULONG deletedDeviceCount = 0UL;

    if (DeletedDeviceCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *DeletedDeviceCountOut = 0UL;

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        deviceCursor = DriverObject->DeviceObject;
        while (deviceCursor != NULL &&
            deletedDeviceCount < KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT) {
            PDEVICE_OBJECT nextDevice = deviceCursor->NextDevice;

            /* 中文说明：IoDeleteDevice 会修改驱动设备链，因此先保存 NextDevice。 */
            IoDeleteDevice(deviceCursor);
            deletedDeviceCount += 1UL;
            deviceCursor = nextDevice;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *DeletedDeviceCountOut = deletedDeviceCount;
        return GetExceptionCode();
    }

    *DeletedDeviceCountOut = deletedDeviceCount;
    if (deletedDeviceCount >= KSW_DRIVER_UNLOAD_MAX_DEVICE_DELETE_COUNT &&
        deviceCursor != NULL) {
        return STATUS_BUFFER_OVERFLOW;
    }
    return STATUS_SUCCESS;
}

/* 中文说明：执行强制卸载后的附加清理，所有动作都必须由请求 flag 明确启用。 */
static NTSTATUS
KswordARKDriverUnloadApplyCleanupUnsafe(
    _Inout_ PKSW_DRIVER_UNLOAD_CONTEXT UnloadContext,
    _In_ BOOLEAN DriverUnloadWasCalled
    )
{
    NTSTATUS cleanupStatus = STATUS_SUCCESS;

    if (UnloadContext == NULL || UnloadContext->DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (DriverUnloadWasCalled &&
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD) != 0UL) {
        KswordARKDriverUnloadClearDispatchUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_AFTER_UNLOAD;
    }
    if (!DriverUnloadWasCalled &&
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD) != 0UL) {
        KswordARKDriverUnloadClearDispatchUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD;
    }
    if (!DriverUnloadWasCalled &&
        (UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD) != 0UL) {
        ULONG deletedDeviceCount = 0UL;
        NTSTATUS deleteStatus = KswordARKDriverUnloadDeleteDeviceObjectsUnsafe(
            UnloadContext->DriverObject,
            &deletedDeviceCount);
        UnloadContext->DeletedDeviceCount = deletedDeviceCount;
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD;
        if (!NT_SUCCESS(deleteStatus)) {
            cleanupStatus = deleteStatus;
        }
    }
    if ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER) != 0UL) {
        KswordARKDriverUnloadClearUnloadPointerUnsafe(UnloadContext->DriverObject);
        UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER;
    }
    if ((UnloadContext->Flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT) != 0UL) {
        __try {
            ObMakeTemporaryObject(UnloadContext->DriverObject);
            UnloadContext->CleanupFlagsApplied |= KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            cleanupStatus = GetExceptionCode();
        }
    }

    return cleanupStatus;
}

/* 中文说明：系统线程实际调用 DriverUnload，隔离调用栈和等待超时。 */
static VOID
KswordARKDriverUnloadThreadRoutine(
    _In_opt_ PVOID StartContext
    )
{
    PKSW_DRIVER_UNLOAD_CONTEXT unloadContext = (PKSW_DRIVER_UNLOAD_CONTEXT)StartContext;

    if (unloadContext == NULL || unloadContext->DriverObject == NULL) {
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
        return;
    }

    unloadContext->UnloadStatus = STATUS_SUCCESS;
    unloadContext->CleanupStatus = STATUS_SUCCESS;
    unloadContext->DriverUnload = unloadContext->DriverObject->DriverUnload;
    if (unloadContext->DriverUnload != NULL) {
        __try {
            unloadContext->DriverUnload(unloadContext->DriverObject);
            unloadContext->UnloadStatus = STATUS_SUCCESS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            unloadContext->UnloadStatus = GetExceptionCode();
        }
        unloadContext->CleanupStatus = KswordARKDriverUnloadApplyCleanupUnsafe(
            unloadContext,
            TRUE);
    }
    else if ((unloadContext->Flags & (KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_DISPATCH_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_DELETE_DEVICE_OBJECTS_ON_NO_UNLOAD |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_CLEAR_UNLOAD_POINTER |
        KSWORD_ARK_DRIVER_UNLOAD_FLAG_MAKE_TEMPORARY_OBJECT)) != 0UL) {
        unloadContext->CleanupStatus = KswordARKDriverUnloadApplyCleanupUnsafe(
            unloadContext,
            FALSE);
        unloadContext->UnloadStatus = STATUS_PROCEDURE_NOT_FOUND;
    }
    else {
        unloadContext->UnloadStatus = STATUS_PROCEDURE_NOT_FOUND;
    }

    /* 中文说明：线程持有的 DriverObject 引用在这里释放，父线程只释放自己的引用。 */
    ObDereferenceObject(unloadContext->DriverObject);
    PsTerminateSystemThread(unloadContext->UnloadStatus);
}

/* 中文说明：启动系统线程并等待卸载结果。 */
static NTSTATUS
KswordARKDriverUnloadRunThread(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG Flags,
    _In_ ULONG TimeoutMilliseconds,
    _Out_ NTSTATUS* WaitStatusOut,
    _Out_ NTSTATUS* UnloadStatusOut,
    _Out_ NTSTATUS* CleanupStatusOut,
    _Out_ PDRIVER_UNLOAD* DriverUnloadOut
    )
{
    HANDLE threadHandle = NULL;
    PETHREAD threadObject = NULL;
    LARGE_INTEGER timeoutInterval;
    PKSW_DRIVER_UNLOAD_CONTEXT unloadContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (DriverObject == NULL ||
        WaitStatusOut == NULL ||
        UnloadStatusOut == NULL ||
        CleanupStatusOut == NULL ||
        DriverUnloadOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *WaitStatusOut = STATUS_SUCCESS;
    *UnloadStatusOut = STATUS_SUCCESS;
    *CleanupStatusOut = STATUS_SUCCESS;
    *DriverUnloadOut = DriverObject->DriverUnload;
#pragma warning(push)
#pragma warning(disable:4996)
    unloadContext = (PKSW_DRIVER_UNLOAD_CONTEXT)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(*unloadContext),
        KSW_DRIVER_UNLOAD_TAG);
#pragma warning(pop)
    if (unloadContext == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 中文说明：卸载线程可能超时后继续运行，因此上下文不能放在父线程栈上。 */
    RtlZeroMemory(unloadContext, sizeof(*unloadContext));
    unloadContext->DriverObject = DriverObject;
    unloadContext->Flags = Flags;
    unloadContext->UnloadStatus = STATUS_PENDING;
    unloadContext->CleanupStatus = STATUS_SUCCESS;
    unloadContext->DriverUnload = DriverObject->DriverUnload;
    ObReferenceObject(DriverObject);

    if (TimeoutMilliseconds == 0UL) {
        TimeoutMilliseconds = KSW_DRIVER_UNLOAD_DEFAULT_TIMEOUT_MS;
    }
    if (TimeoutMilliseconds > KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS) {
        TimeoutMilliseconds = KSW_DRIVER_UNLOAD_MAX_TIMEOUT_MS;
    }

    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        KswordARKDriverUnloadThreadRoutine,
        unloadContext);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(DriverObject);
        ExFreePoolWithTag(unloadContext, KSW_DRIVER_UNLOAD_TAG);
        return status;
    }

    status = ObReferenceObjectByHandle(
        threadHandle,
        SYNCHRONIZE,
        NULL,
        KernelMode,
        (PVOID*)&threadObject,
        NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(threadHandle);
        /* 中文说明：线程可能已经运行，不能释放上下文；由超时泄漏策略兜底。 */
        return status;
    }

    timeoutInterval.QuadPart = -((LONGLONG)TimeoutMilliseconds * 10LL * 1000LL);
    *WaitStatusOut = KeWaitForSingleObject(
        threadObject,
        Executive,
        KernelMode,
        FALSE,
        &timeoutInterval);

    *UnloadStatusOut = unloadContext->UnloadStatus;
    *CleanupStatusOut = unloadContext->CleanupStatus;
    *DriverUnloadOut = unloadContext->DriverUnload;
    ObDereferenceObject(threadObject);
    ZwClose(threadHandle);

    if (*WaitStatusOut == STATUS_TIMEOUT) {
        /* 中文说明：超时后卸载线程仍可能访问上下文，宁可泄漏小块内存也不释放。 */
        return STATUS_TIMEOUT;
    }
    if (!NT_SUCCESS(*WaitStatusOut)) {
        ExFreePoolWithTag(unloadContext, KSW_DRIVER_UNLOAD_TAG);
        return *WaitStatusOut;
    }
    status = !NT_SUCCESS(*CleanupStatusOut) ? *CleanupStatusOut : *UnloadStatusOut;
    ExFreePoolWithTag(unloadContext, KSW_DRIVER_UNLOAD_TAG);
    return status;
}

NTSTATUS
KswordARKDriverForceUnloadDriver(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Force-unload a DriverObject by name. 中文说明：第一优先路径只调用目标
    DriverObject->DriverUnload；当目标没有 DriverUnload 时，只有显式 flag
    才清 dispatch 表，不主动删除 DeviceObject。

Arguments:

    OutputBuffer - 固定响应缓冲。
    OutputBufferLength - 输出缓冲长度。
    Request - R3 请求，包含 DriverObject 名称和 flags。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；底层卸载结果写入 response->lastStatus。

--*/
{
    KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE* response = NULL;
    PDRIVER_OBJECT driverObject = NULL;
    WCHAR normalizedName[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS] = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_SUCCESS;
    NTSTATUS unloadStatus = STATUS_SUCCESS;
    NTSTATUS cleanupStatus = STATUS_SUCCESS;
    PDRIVER_UNLOAD driverUnload = NULL;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNKNOWN;
    response->flags = Request->flags;
    response->lastStatus = STATUS_SUCCESS;
    response->waitStatus = STATUS_SUCCESS;

    status = KswordARKDriverUnloadReferenceByName(
        Request,
        &driverObject,
        normalizedName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_REFERENCE_FAILED;
        response->lastStatus = status;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    response->driverObjectAddress = (ULONGLONG)(ULONG_PTR)driverObject;
    response->driverUnloadAddress = (ULONGLONG)(ULONG_PTR)driverObject->DriverUnload;
    RtlStringCchCopyW(
        response->driverName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS,
        normalizedName);

    status = KswordARKDriverUnloadRunThread(
        driverObject,
        Request->flags,
        Request->timeoutMilliseconds,
        &waitStatus,
        &unloadStatus,
        &cleanupStatus,
        &driverUnload);

    response->driverUnloadAddress = (ULONGLONG)(ULONG_PTR)driverUnload;
    response->lastStatus = status;
    response->waitStatus = waitStatus;
    if (status == STATUS_TIMEOUT || waitStatus == STATUS_TIMEOUT) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_WAIT_TIMEOUT;
    }
    else if (!NT_SUCCESS(cleanupStatus)) {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_CLEANUP_FAILED;
    }
    else if (driverUnload == NULL && unloadStatus == STATUS_PROCEDURE_NOT_FOUND) {
        response->status = NT_SUCCESS(cleanupStatus) &&
            (Request->flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP) != 0UL
            ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP
            : KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOAD_ROUTINE_MISSING;
    }
    else if (NT_SUCCESS(status)) {
        response->status = (Request->flags & KSWORD_ARK_DRIVER_UNLOAD_FLAG_FORCE_CLEANUP) != 0UL
            ? KSWORD_ARK_DRIVER_UNLOAD_STATUS_FORCED_CLEANUP
            : KSWORD_ARK_DRIVER_UNLOAD_STATUS_UNLOADED;
    }
    else {
        response->status = KSWORD_ARK_DRIVER_UNLOAD_STATUS_OPERATION_FAILED;
    }

    ObDereferenceObject(driverObject);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
