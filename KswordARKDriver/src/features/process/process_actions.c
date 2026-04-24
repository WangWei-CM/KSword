/*++

Module Name:

    process_actions.c

Abstract:

    This file contains kernel process control operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "..\..\platform\process_resolver.h"

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE (0x0001)
#endif

#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME (0x0800)
#endif

#ifndef SE_SIGNING_LEVEL_UNCHECKED
#define SE_SIGNING_LEVEL_UNCHECKED 0x00
#endif

#ifndef SE_SIGNING_LEVEL_AUTHENTICODE
#define SE_SIGNING_LEVEL_AUTHENTICODE 0x04
#endif

#ifndef SE_SIGNING_LEVEL_STORE
#define SE_SIGNING_LEVEL_STORE 0x06
#endif

#ifndef SE_SIGNING_LEVEL_ANTIMALWARE
#define SE_SIGNING_LEVEL_ANTIMALWARE 0x07
#endif

#ifndef SE_SIGNING_LEVEL_MICROSOFT
#define SE_SIGNING_LEVEL_MICROSOFT 0x08
#endif

#ifndef SE_SIGNING_LEVEL_DYNAMIC_CODEGEN
#define SE_SIGNING_LEVEL_DYNAMIC_CODEGEN 0x0B
#endif

#ifndef SE_SIGNING_LEVEL_WINDOWS
#define SE_SIGNING_LEVEL_WINDOWS 0x0C
#endif

#ifndef SE_SIGNING_LEVEL_WINDOWS_TCB
#define SE_SIGNING_LEVEL_WINDOWS_TCB 0x0E
#endif

#define KSWORD_PS_PROTECTED_SIGNER_NONE ((UCHAR)0x00)
#define KSWORD_PS_PROTECTED_SIGNER_AUTHENTICODE ((UCHAR)0x01)
#define KSWORD_PS_PROTECTED_SIGNER_CODEGEN ((UCHAR)0x02)
#define KSWORD_PS_PROTECTED_SIGNER_ANTIMALWARE ((UCHAR)0x03)
#define KSWORD_PS_PROTECTED_SIGNER_LSA ((UCHAR)0x04)
#define KSWORD_PS_PROTECTED_SIGNER_WINDOWS ((UCHAR)0x05)
#define KSWORD_PS_PROTECTED_SIGNER_WINTCB ((UCHAR)0x06)

static NTSTATUS
KswordARKDriverResolveSignatureLevelsFromSigner(
    _In_ UCHAR signerType,
    _Out_ UCHAR* signatureLevel,
    _Out_ UCHAR* sectionSignatureLevel
    )
{
    if (signatureLevel == NULL || sectionSignatureLevel == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (signerType) {
    case KSWORD_PS_PROTECTED_SIGNER_NONE:
        *signatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_AUTHENTICODE:
        *signatureLevel = SE_SIGNING_LEVEL_AUTHENTICODE;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_AUTHENTICODE;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_CODEGEN:
        *signatureLevel = SE_SIGNING_LEVEL_DYNAMIC_CODEGEN;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_STORE;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_ANTIMALWARE:
        *signatureLevel = SE_SIGNING_LEVEL_ANTIMALWARE;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_ANTIMALWARE;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_LSA:
        *signatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_MICROSOFT;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_WINDOWS:
        *signatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_WINTCB:
        *signatureLevel = SE_SIGNING_LEVEL_WINDOWS_TCB;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

// PPLcontrol-style fallback: patch EPROCESS protection/signature bytes directly.
static NTSTATUS
KswordARKDriverPatchProcessProtectionStateByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
{
    const LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();
    const LONG signatureLevelOffset = KswordARKDriverResolveProcessSignatureLevelOffset();
    const LONG sectionSignatureLevelOffset = KswordARKDriverResolveProcessSectionSignatureLevelOffset();
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR signerType = (UCHAR)((protectionLevel & 0xF0U) >> 4U);
    UCHAR signatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
    UCHAR sectionSignatureLevel = SE_SIGNING_LEVEL_UNCHECKED;

    if (protectionOffset <= 0 ||
        signatureLevelOffset <= 0 ||
        sectionSignatureLevelOffset <= 0) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (protectionLevel == 0U) {
        signerType = KSWORD_PS_PROTECTED_SIGNER_NONE;
    }

    status = KswordARKDriverResolveSignatureLevelsFromSigner(
        signerType,
        &signatureLevel,
        &sectionSignatureLevel);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        PUCHAR processBase = (PUCHAR)processObject;
        volatile UCHAR* protectionByte = (volatile UCHAR*)(processBase + (ULONG)protectionOffset);
        volatile UCHAR* signatureByte = (volatile UCHAR*)(processBase + (ULONG)signatureLevelOffset);
        volatile UCHAR* sectionSignatureByte =
            (volatile UCHAR*)(processBase + (ULONG)sectionSignatureLevelOffset);

        *protectionByte = protectionLevel;
        *signatureByte = signatureLevel;
        *sectionSignatureByte = sectionSignatureLevel;

        if (*protectionByte != protectionLevel ||
            *signatureByte != signatureLevel ||
            *sectionSignatureByte != sectionSignatureLevel) {
            status = STATUS_UNSUCCESSFUL;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ObDereferenceObject(processObject);
    return status;
}

NTSTATUS
KswordARKDriverTerminateProcessByPid(
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
/*++

Routine Description:

    Open the target process by PID and terminate it via ZwTerminateProcess.

Arguments:

    processId - Target process ID.
    exitStatus - Exit status to report.

Return Value:

    NTSTATUS

--*/
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_TERMINATE,
        &objectAttributes,
        &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwTerminateProcess(processHandle, exitStatus);
    ZwClose(processHandle);
    return status;
}

NTSTATUS
KswordARKDriverSuspendProcessByPid(
    _In_ ULONG processId
    )
/*++

Routine Description:

    Suspend target process by PID (PsSuspendProcess preferred, Zw/Nt fallback).

Arguments:

    processId - Target process ID.

Return Value:

    NTSTATUS

--*/
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    KSWORD_PS_SUSPEND_PROCESS_FN psSuspendProcess = NULL;
    KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN zwOrNtSuspendProcess = NULL;

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    // Prefer PsSuspendProcess with PEPROCESS input for wider compatibility.
    psSuspendProcess = KswordARKDriverResolvePsSuspendProcess();
    if (psSuspendProcess != NULL) {
        status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        status = psSuspendProcess(processObject);
        ObDereferenceObject(processObject);
        return status;
    }

    // Fallback to Zw/NtSuspendProcess with process-handle input.
    zwOrNtSuspendProcess = KswordARKDriverResolveZwOrNtSuspendProcess();
    if (zwOrNtSuspendProcess == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_SUSPEND_RESUME,
        &objectAttributes,
        &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = zwOrNtSuspendProcess(processHandle);
    ZwClose(processHandle);
    return status;
}

NTSTATUS
KswordARKDriverSetProcessPplLevelByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
/*++

Routine Description:

    Set target process protection state by PID using PPLcontrol-style direct
    EPROCESS patching (Protection + SignatureLevel + SectionSignatureLevel).

Arguments:

    processId - Target process ID.
    protectionLevel - Target protection level byte.

Return Value:

    NTSTATUS

--*/
{
    const UCHAR protectionType = (UCHAR)(protectionLevel & 0x07U);
    const UCHAR signerType = (UCHAR)((protectionLevel & 0xF0U) >> 4U);

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    // PPLcontrol-compatible validation:
    // - 0x00 disables PPL and clears signature levels;
    // - non-zero requires PPL Type==1 and non-zero signer.
    if (protectionLevel == 0U) {
        return KswordARKDriverPatchProcessProtectionStateByPid(processId, 0U);
    }

    if (protectionType != 0x01U || signerType == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    return KswordARKDriverPatchProcessProtectionStateByPid(processId, protectionLevel);
}
