/*++

Module Name:

    rxpf_ioctl.c

Abstract:

    WDF adapters for the administrator-only RXPF research protocol.

Environment:

    Kernel-mode Driver Framework at PASSIVE_LEVEL.

--*/

#include "ark/ark_driver.h"

#include "rxpf_runtime.h"
#include "src/dispatch/ioctl_validation.h"

typedef enum _KSW_RXPF_IOCTL_OPERATION
{
    KswRxpfIoctlQuerySupport = 0,
    KswRxpfIoctlRegisterPage,
    KswRxpfIoctlChangePage,
    KswRxpfIoctlQueryPage,
    KswRxpfIoctlWritePage,
    KswRxpfIoctlSetEmulation,
    KswRxpfIoctlQueryStats,
    KswRxpfIoctlDrainEvents,
    KswRxpfIoctlUnregisterPage,
    KswRxpfIoctlRunSelfTest,
    KswRxpfIoctlSetHookPoint,
    /* Phase G：读 Record->Backup 的子段，供 R3 wizard 在 Unregister(SKIP) 后恢复原字节用 */
    KswRxpfIoctlQueryBackup,
    /* 原子提交：Register→Change→Write→Unregister 压缩到单次 IOCTL */
    KswRxpfIoctlAtomicCommit,
    /* 执行证明：Register→Change→Write(custom)→InstallIDT→ConcurrentExec 压缩到单次 IOCTL */
    KswRxpfIoctlExecProof
} KSW_RXPF_IOCTL_OPERATION;

typedef union _KSW_RXPF_REQUEST_SNAPSHOT
{
    KSWORD_ARK_RXPF_REQUEST_HEADER Header;
    KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST RegisterPage;
    KSWORD_ARK_RXPF_RECORD_REQUEST Record;
    KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST WritePage;
    KSWORD_ARK_RXPF_SET_EMULATION_REQUEST SetEmulation;
    KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST DrainEvents;
    KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST SetHookPoint;
    KSWORD_ARK_RXPF_QUERY_BACKUP_REQUEST QueryBackup;
    KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST AtomicCommit;
    KSWORD_ARK_RXPF_EXEC_PROOF_REQUEST ExecProof;
} KSW_RXPF_REQUEST_SNAPSHOT;

static BOOLEAN
KswRxpfIoctlIsMutation(
    _In_ KSW_RXPF_IOCTL_OPERATION Operation
    )
{
    /* Queries and event export do not change RXPF or machine state. */
    return Operation == KswRxpfIoctlRegisterPage ||
        Operation == KswRxpfIoctlChangePage ||
        Operation == KswRxpfIoctlWritePage ||
        Operation == KswRxpfIoctlSetEmulation ||
        Operation == KswRxpfIoctlUnregisterPage ||
        Operation == KswRxpfIoctlRunSelfTest ||
        Operation == KswRxpfIoctlSetHookPoint ||
        Operation == KswRxpfIoctlAtomicCommit ||
        Operation == KswRxpfIoctlExecProof;
}

static NTSTATUS
KswRxpfIoctlEvaluateSafety(
    _In_ WDFDEVICE Device,
    _In_ KSW_RXPF_IOCTL_OPERATION Operation,
    _In_ ULONG RequestFlags
    )
{
    KSWORD_ARK_SAFETY_CONTEXT safetyContext = { 0 };

    /* Bind every mutation to the central critical kernel-patch policy class. */
    safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
    safetyContext.ContextFlags =
        (RequestFlags & KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED) != 0UL
        ? KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED
        : 0UL;
    switch (Operation) {
    case KswRxpfIoctlRegisterPage:
        safetyContext.TargetText =
            L"Register one driver-owned RXPF research page";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Register one driver-owned RXPF research page") - 1U);
        break;
    case KswRxpfIoctlChangePage:
        safetyContext.TargetText =
            L"Transition one RXPF research page to persistent RW/NX";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Transition one RXPF research page to persistent RW/NX") - 1U);
        break;
    case KswRxpfIoctlWritePage:
        safetyContext.TargetText =
            L"Write bounded bytes to one RXPF writable alias";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Write bounded bytes to one RXPF writable alias") - 1U);
        break;
    case KswRxpfIoctlSetEmulation:
        safetyContext.TargetText =
            L"Install or restore per-processor shadow IDTs for RXPF";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Install or restore per-processor shadow IDTs for RXPF") - 1U);
        break;
    case KswRxpfIoctlUnregisterPage:
        safetyContext.TargetText =
            L"Terminate and release one RXPF research page";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Terminate and release one RXPF research page") - 1U);
        break;
    case KswRxpfIoctlSetHookPoint:
        safetyContext.TargetText =
            L"Set or clear an RXPF hook point redirect on a managed page";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Set or clear an RXPF hook point redirect on a managed page") - 1U);
        break;
    case KswRxpfIoctlAtomicCommit:
        safetyContext.TargetText =
            L"Atomic RXPF register+write+restore on an external image page";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Atomic RXPF register+write+restore on an external image page") - 1U);
        break;
    case KswRxpfIoctlExecProof:
        safetyContext.TargetText =
            L"Atomic RXPF alloc+write+emulated execute of a custom payload on a driver-owned page";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Atomic RXPF alloc+write+emulated execute of a custom payload on a driver-owned page") - 1U);
        break;
    default:
        safetyContext.TargetText =
            L"Execute an RXPF driver-owned-page instruction self-test";
        safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(
            L"Execute an RXPF driver-owned-page instruction self-test") - 1U);
        break;
    }
    return KswordARKSafetyEvaluate(Device, &safetyContext);
}

static NTSTATUS
KswRxpfIoctlDispatchFixed(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned,
    _In_ KSW_RXPF_IOCTL_OPERATION Operation,
    _In_ size_t RequiredInputLength,
    _In_ size_t RequiredOutputLength,
    _In_ ULONG AllowedFlags
    )
{
    KSW_RXPF_REQUEST_SNAPSHOT snapshot;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    /* Reject malformed completion and fixed-buffer contracts before mutation. */
    if (BytesReturned == NULL || Request == NULL ||
        RequiredInputLength > sizeof(snapshot)) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;
    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfRequestRetrieveInputBuffer(
        Request,
        RequiredInputLength,
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status) ||
        InputBufferLength != RequiredInputLength ||
        actualInputLength < RequiredInputLength) {
        return NT_SUCCESS(status)
            ? STATUS_INFO_LENGTH_MISMATCH
            : status;
    }

    /* METHOD_BUFFERED uses one system buffer, so preserve input before output. */
    RtlZeroMemory(&snapshot, sizeof(snapshot));
    RtlCopyMemory(&snapshot, inputBuffer, RequiredInputLength);
    if (snapshot.Header.version != KSWORD_ARK_RXPF_PROTOCOL_VERSION ||
        snapshot.Header.size != RequiredInputLength ||
        snapshot.Header.confirmationToken !=
            KSWORD_ARK_RXPF_CONFIRMATION_TOKEN ||
        (snapshot.Header.flags & KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED) == 0UL ||
        (snapshot.Header.flags & ~AllowedFlags) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = WdfRequestRetrieveOutputBuffer(
        Request,
        RequiredOutputLength,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status) ||
        OutputBufferLength < RequiredOutputLength ||
        actualOutputLength < RequiredOutputLength) {
        return NT_SUCCESS(status)
            ? STATUS_BUFFER_TOO_SMALL
            : status;
    }
    RtlZeroMemory(outputBuffer, RequiredOutputLength);

    /* Central safety policy evaluates only operations that change machine state. */
    if (KswRxpfIoctlIsMutation(Operation)) {
        status = KswRxpfIoctlEvaluateSafety(
            Device,
            Operation,
            snapshot.Header.flags);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Dispatch the validated snapshot to the type-owning runtime entry point. */
    switch (Operation) {
    case KswRxpfIoctlQuerySupport:
        status = KswRxpfRuntimeQuerySupport(
            (KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlRegisterPage:
        status = KswRxpfRuntimeRegisterPage(
            &snapshot.RegisterPage,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlChangePage:
        status = KswRxpfRuntimeChangePage(
            &snapshot.Record,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlQueryPage:
        status = KswRxpfRuntimeQueryPage(
            &snapshot.Record,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlWritePage:
        status = KswRxpfRuntimeWritePage(
            &snapshot.WritePage,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlSetEmulation:
        status = KswRxpfRuntimeSetEmulation(
            &snapshot.SetEmulation,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlQueryStats:
        status = KswRxpfRuntimeQueryStats(
            (KSWORD_ARK_RXPF_STATS_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlDrainEvents:
        status = KswRxpfRuntimeDrainEvents(
            &snapshot.DrainEvents,
            (KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlUnregisterPage:
        status = KswRxpfRuntimeUnregisterPage(
            &snapshot.Record,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlRunSelfTest:
        status = KswRxpfRuntimeRunSelfTest(
            &snapshot.Record,
            (KSWORD_ARK_RXPF_SELF_TEST_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlSetHookPoint:
        status = KswRxpfRuntimeSetHookPoint(
            &snapshot.SetHookPoint,
            (KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlQueryBackup:
        /* Phase G：读 Record->Backup 子段，供 wizard 恢复原字节 */
        status = KswRxpfRuntimeQueryBackup(
            &snapshot.QueryBackup,
            (KSWORD_ARK_RXPF_QUERY_BACKUP_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlAtomicCommit:
        /* 原子提交：Register→Change→Write→Unregister 在单个 handler 内完成 */
        status = KswRxpfRuntimeAtomicCommit(
            &snapshot.AtomicCommit,
            (KSWORD_ARK_RXPF_PAGE_RESPONSE*)outputBuffer);
        break;
    case KswRxpfIoctlExecProof:
        /* 执行证明：Register(ALLOCATED_TEST)→Change→Write(custom)→InstallIDT→
         * ConcurrentExec→Unregister 在单个 handler 内完成，shadow IDT emulator
         * 服务每一次 payload 字节 fetch。 */
        status = KswRxpfRuntimeExecProof(
            &snapshot.ExecProof,
            (KSWORD_ARK_RXPF_EXEC_PROOF_RESPONSE*)outputBuffer);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    *BytesReturned = RequiredOutputLength;
    return status;
}

#define KSW_RXPF_DEFINE_IOCTL_HANDLER(                                      \
    FunctionName, OperationValue, RequestType, ResponseType, FlagMask)      \
NTSTATUS                                                                     \
FunctionName(                                                                \
    _In_ WDFDEVICE Device,                                                   \
    _In_ WDFREQUEST Request,                                                 \
    _In_ size_t InputBufferLength,                                           \
    _In_ size_t OutputBufferLength,                                          \
    _Out_ size_t* BytesReturned                                              \
    )                                                                        \
{                                                                            \
    return KswRxpfIoctlDispatchFixed(                                        \
        Device,                                                              \
        Request,                                                             \
        InputBufferLength,                                                   \
        OutputBufferLength,                                                  \
        BytesReturned,                                                       \
        OperationValue,                                                      \
        sizeof(RequestType),                                                 \
        sizeof(ResponseType),                                                \
        FlagMask);                                                           \
}

/* Fixed adapters keep protocol sizing in one auditable declaration each. */
KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlQuerySupport,
    KswRxpfIoctlQuerySupport,
    KSWORD_ARK_RXPF_REQUEST_HEADER,
    KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlRegisterPage,
    KswRxpfIoctlRegisterPage,
    KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED |
        KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlChangePage,
    KswRxpfIoctlChangePage,
    KSWORD_ARK_RXPF_RECORD_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlQueryPage,
    KswRxpfIoctlQueryPage,
    KSWORD_ARK_RXPF_RECORD_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlWritePage,
    KswRxpfIoctlWritePage,
    KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlSetEmulation,
    KswRxpfIoctlSetEmulation,
    KSWORD_ARK_RXPF_SET_EMULATION_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlQueryStats,
    KswRxpfIoctlQueryStats,
    KSWORD_ARK_RXPF_REQUEST_HEADER,
    KSWORD_ARK_RXPF_STATS_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlDrainEvents,
    KswRxpfIoctlDrainEvents,
    KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST,
    KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlUnregisterPage,
    KswRxpfIoctlUnregisterPage,
    KSWORD_ARK_RXPF_RECORD_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    (KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED |
     KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE))

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlRunSelfTest,
    KswRxpfIoctlRunSelfTest,
    KSWORD_ARK_RXPF_RECORD_REQUEST,
    KSWORD_ARK_RXPF_SELF_TEST_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlSetHookPoint,
    KswRxpfIoctlSetHookPoint,
    KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST,
    KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlQueryBackup,
    KswRxpfIoctlQueryBackup,
    KSWORD_ARK_RXPF_QUERY_BACKUP_REQUEST,
    KSWORD_ARK_RXPF_QUERY_BACKUP_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlAtomicCommit,
    KswRxpfIoctlAtomicCommit,
    KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST,
    KSWORD_ARK_RXPF_PAGE_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

KSW_RXPF_DEFINE_IOCTL_HANDLER(
    KswordARKRxpfIoctlExecProof,
    KswRxpfIoctlExecProof,
    KSWORD_ARK_RXPF_EXEC_PROOF_REQUEST,
    KSWORD_ARK_RXPF_EXEC_PROOF_RESPONSE,
    KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED)

#undef KSW_RXPF_DEFINE_IOCTL_HANDLER
