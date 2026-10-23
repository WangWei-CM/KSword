#include "debugger.h"
#include "../../platform/pool_compat.h"

/* Resolve optional exports without importing an unsupported Windows version's routine. */
typedef NTSTATUS (NTAPI* KSW_CONTEXT_FN)(PETHREAD Thread, PCONTEXT Context, KPROCESSOR_MODE Mode);
/* Preserve the Win32 suspend API's previous-count contract. */
typedef NTSTATUS (NTAPI* KSW_SUSPEND_FN)(HANDLE Thread, PULONG PreviousCount);
/* Protect only an ID-bound target's user virtual memory. */
typedef NTSTATUS (NTAPI* KSW_PROTECT_FN)(HANDLE Process, PVOID* Base, PSIZE_T Bytes, ULONG Protection, PULONG Previous);
/* Lookup routines consume IDs, never user-supplied kernel object pointers. */
NTSYSAPI NTSTATUS NTAPI PsLookupThreadByThreadId(HANDLE ThreadId, PETHREAD* Thread);
/* Resolve the target process identity before opening a private kernel handle. */
NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId(HANDLE ProcessId, PEPROCESS* Process);
/* Validate real thread ownership from the referenced object. */
NTKERNELAPI HANDLE PsGetThreadProcessId(PETHREAD Thread);
/* Detect target exit before attempting a context APC. */
NTKERNELAPI BOOLEAN PsIsThreadTerminating(PETHREAD Thread);
/* Open private kernel handles to already referenced target objects. */
NTKERNELAPI NTSTATUS ObOpenObjectByPointer(PVOID Object, ULONG Attributes, PACCESS_STATE AccessState,
    ACCESS_MASK Access, POBJECT_TYPE Type, KPROCESSOR_MODE Mode, PHANDLE Handle);
/* Allocate only user virtual address space in the ID-bound process. */
NTSYSAPI NTSTATUS NTAPI ZwAllocateVirtualMemory(HANDLE Process, PVOID* Base, ULONG_PTR ZeroBits,
    PSIZE_T Bytes, ULONG Type, ULONG Protection);
/* Release only user virtual address space in the ID-bound process. */
NTSYSAPI NTSTATUS NTAPI ZwFreeVirtualMemory(HANDLE Process, PVOID* Base, PSIZE_T Bytes, ULONG Type);

/* This debugger ABI intentionally supports the AMD64 context layout only. */
C_ASSERT(sizeof(CONTEXT) == KSWORD_ARK_DEBUGGER_CONTEXT_BYTES);

/* Resolve a single named routine at PASSIVE_LEVEL. */
static PVOID KswordARKDebuggerResolve(PCWSTR Name)
{
    /* Bind the routine name without allocating persistent state. */
    UNICODE_STRING name;
    /* Initialize the exact requested routine name. */
    RtlInitUnicodeString(&name, Name);
    /* Return NULL as an explicit capability failure. */
    return MmGetSystemRoutineAddress(&name);
}

/* Get/set a native user thread context or change its suspend count. */
static NTSTATUS KswordARKDebuggerThread(const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response)
{
    /* Hold a real object reference through every blocking native operation. */
    PETHREAD thread = NULL;
    /* Use an opaque private kernel handle only for suspend-count operations. */
    HANDLE handle = NULL;
    /* Keep the aligned AMD64 context off the kernel stack. */
    PCONTEXT context = NULL;
    /* Preserve the exact native completion status. */
    NTSTATUS status;
    /* Resolve a thread by its authoritative identifier. */
    status = PsLookupThreadByThreadId(ULongToHandle(Request->threadId), &thread);
    /* Stop when the identifier no longer names a thread. */
    if (!NT_SUCCESS(status)) { return status; }
    /* Reject a recycled TID, system thread, current caller, or terminating target. */
    if (HandleToULong(PsGetThreadProcessId(thread)) != Request->processId || thread == PsGetCurrentThread() || PsIsThreadTerminating(thread)) {
        /* Return an identity/lifetime failure before invoking native routines. */
        status = STATUS_INVALID_CID;
        /* Balance the real thread reference. */
        goto Complete;
    }
    /* Context operations never touch ETHREAD trap-frame offsets directly. */
    if (Request->operation == KSWORD_ARK_DEBUGGER_GET_CONTEXT || Request->operation == KSWORD_ARK_DEBUGGER_SET_CONTEXT) {
        /* Select the optional native process-manager export. */
        KSW_CONTEXT_FN routine = (KSW_CONTEXT_FN)KswordARKDebuggerResolve(
            Request->operation == KSWORD_ARK_DEBUGGER_GET_CONTEXT ? L"PsGetContextThread" : L"PsSetContextThread");
        /* Reject XSTATE/foreign-architecture buffers instead of truncating them. */
        if (routine == NULL || (Request->contextFlags & CONTEXT_AMD64) == 0UL ||
            (Request->contextFlags & ~(CONTEXT_ALL | CONTEXT_EXCEPTION_ACTIVE | CONTEXT_EXCEPTION_REPORTING | CONTEXT_SERVICE_ACTIVE | CONTEXT_UNWOUND_TO_CALL)) != 0UL) {
            /* Return the explicit unsupported layout/routine result. */
            status = STATUS_NOT_SUPPORTED;
            /* Balance the real thread reference. */
            goto Complete;
        }
        /* Pool allocation supplies the required 16-byte AMD64 alignment. */
        context = (PCONTEXT)KswordARKAllocateNonPagedPool(sizeof(*context), 'tcDK');
        /* Refuse a context operation without aligned storage. */
        if (context == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Complete; }
        /* Preserve the caller's selected register groups. */
        RtlCopyMemory(context, Request->context, sizeof(*context));
        /* Bind flags to the validated outer protocol field. */
        context->ContextFlags = Request->contextFlags;
        /* SET accepts only user-mode control and debug addresses. */
        if (Request->operation == KSWORD_ARK_DEBUGGER_SET_CONTEXT &&
            ((((context->ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL) &&
                (context->Rip > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress || context->Rsp > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress ||
                    (context->SegCs & 3U) != 3U || (context->SegSs & 3U) != 3U || (context->EFlags & 0x3000UL) != 0UL)) ||
                (((context->ContextFlags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS) &&
                    (context->Dr0 > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress || context->Dr1 > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress ||
                        context->Dr2 > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress || context->Dr3 > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress)))) {
            /* Reject kernel control/debug state even through a privileged device handle. */
            status = STATUS_INVALID_PARAMETER;
            /* Balance all allocated resources. */
            goto Complete;
        }
        /* A trusted kernel buffer requires KernelMode; target addresses were checked above. */
        status = routine(thread, context, KernelMode);
        /* Publish register groups only after native completion succeeded. */
        if (NT_SUCCESS(status)) { RtlCopyMemory(Response->context, context, sizeof(*context)); }
    } else {
        /* Select a kernel-handle suspend/resume routine. */
        KSW_SUSPEND_FN routine = (KSW_SUSPEND_FN)KswordARKDebuggerResolve(
            Request->operation == KSWORD_ARK_DEBUGGER_SUSPEND ? L"ZwSuspendThread" : L"ZwResumeThread");
        /* Keep absent exports visible instead of changing to a different operation. */
        if (routine == NULL) { status = STATUS_NOT_SUPPORTED; goto Complete; }
        /* Open the exact referenced object with only suspend/resume access. */
        status = ObOpenObjectByPointer(thread, OBJ_KERNEL_HANDLE, NULL, 0x0002UL, *PsThreadType, KernelMode, &handle);
        /* Preserve the original suspend count for the debugger adapter. */
        if (NT_SUCCESS(status)) { status = routine(handle, &Response->previousSuspendCount); }
    }
Complete:
    /* Close only the private handle created by this operation. */
    if (handle != NULL) { (void)ZwClose(handle); }
    /* Release aligned temporary context storage. */
    if (context != NULL) { ExFreePoolWithTag(context, 'tcDK'); }
    /* Balance the target's object lifetime reference. */
    ObDereferenceObject(thread);
    /* Preserve the authoritative native result. */
    return status;
}

/* Allocate/protect/free through a validated process object and a kernel handle. */
static NTSTATUS KswordARKDebuggerMemory(const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response)
{
    /* Hold one target process reference through the virtual-memory operation. */
    PEPROCESS process = NULL;
    /* Keep the target handle private to this driver request. */
    HANDLE handle = NULL;
    /* Preserve native rounding of base and region size. */
    PVOID base = (PVOID)(ULONG_PTR)Request->address;
    /* Preserve the caller's validated bounded region length. */
    SIZE_T bytes = (SIZE_T)Request->bytes;
    /* Preserve exact native completion status. */
    NTSTATUS status;
    /* Refuse kernel addresses, overflow, and empty operations other than MEM_RELEASE. */
    if (Request->address > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress ||
        Request->bytes > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress - Request->address ||
        (bytes == 0U && !(Request->operation == KSWORD_ARK_DEBUGGER_FREE && Request->allocationType == MEM_RELEASE))) { return STATUS_INVALID_PARAMETER; }
    /* Bind the target process by PID. */
    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &process);
    /* Preserve PID lookup failure. */
    if (!NT_SUCCESS(status)) { return status; }
    /* Open only memory-operation access to the exact referenced object. */
    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL, 0x0008UL, *PsProcessType, KernelMode, &handle);
    /* Do not call native memory routines after a handle-open failure. */
    if (!NT_SUCCESS(status)) { goto Complete; }
    /* Select the concrete operation from the validated protocol enum. */
    if (Request->operation == KSWORD_ARK_DEBUGGER_ALLOCATE) {
        /* Preserve requested reserve/commit flags and page protection. */
        status = ZwAllocateVirtualMemory(handle, &base, 0U, &bytes, Request->allocationType, Request->protection);
    } else if (Request->operation == KSWORD_ARK_DEBUGGER_FREE) {
        /* Preserve MEM_RELEASE/MEM_DECOMMIT semantics and native alignment checks. */
        status = ZwFreeVirtualMemory(handle, &base, &bytes, Request->allocationType);
    } else {
        /* Resolve the optional protection routine without private offsets. */
        KSW_PROTECT_FN routine = (KSW_PROTECT_FN)KswordARKDebuggerResolve(L"ZwProtectVirtualMemory");
        /* Report unavailable protection support explicitly. */
        status = routine == NULL ? STATUS_NOT_SUPPORTED : routine(handle, &base, &bytes, Request->protection, &Response->previousProtection);
    }
    /* Publish actual native rounding only after success. */
    if (NT_SUCCESS(status)) { Response->address = (ULONGLONG)(ULONG_PTR)base; Response->bytes = bytes; }
Complete:
    /* Close only this request's private target handle. */
    if (handle != NULL) { (void)ZwClose(handle); }
    /* Balance the exact target process reference. */
    ObDereferenceObject(process);
    /* Preserve the authoritative native completion result. */
    return status;
}

/* Broker a user-mode handle to the target process/thread opened via the
 * KernelMode previous-mode path. The resulting handle is inserted into the
 * requesting R3 process's handle table because ObOpenObjectByPointer
 * without OBJ_KERNEL_HANDLE binds to PsGetCurrentProcess()'s object table,
 * and the IOCTL handler runs on the requesting thread. Ob callbacks that
 * strip access only on UserMode-sourced opens (the common anti-cheat
 * pattern) see KernelHandle=TRUE and skip stripping. */
static NTSTATUS KswordARKDebuggerOpenHandleForDebug(const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response)
{
    /* Hold an object reference across the handle-insert step. */
    PVOID object = NULL;
    /* Capture the open target's object type for pointer-based open. */
    POBJECT_TYPE objectType = NULL;
    /* The requested ACCESS_MASK rides in the ULONGLONG address field. */
    ACCESS_MASK desiredAccess = (ACCESS_MASK)(Request->address & 0xFFFFFFFFULL);
    /* The resulting R3 handle value lives in the requesting process's handle table. */
    HANDLE handle = NULL;
    /* Preserve exact native completion status for the typed response. */
    NTSTATUS status;
    /* An empty ACCESS_MASK offers no debug utility — reject to make faults loud. */
    if (desiredAccess == 0U) { return STATUS_INVALID_PARAMETER; }
    /* Reject bits set outside the low 32 bits — those are not ACCESS_MASK territory. */
    if ((Request->address & 0xFFFFFFFF00000000ULL) != 0ULL) { return STATUS_INVALID_PARAMETER; }
    /* bytes/protection/allocationType/context must stay zero on this operation. */
    if (Request->bytes != 0ULL || Request->protection != 0UL ||
        Request->allocationType != 0UL || Request->contextFlags != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Resolve the real target object by its authoritative identifier. */
    if (Request->threadId != 0UL) {
        /* Thread kind: bind to TID; processId is echoed only for adapters. */
        PETHREAD thread = NULL;
        status = PsLookupThreadByThreadId(ULongToHandle(Request->threadId), &thread);
        if (!NT_SUCCESS(status)) { return status; }
        /* Validate that the thread actually belongs to the declared process. */
        if (HandleToULong(PsGetThreadProcessId(thread)) != Request->processId) {
            ObDereferenceObject(thread);
            return STATUS_INVALID_PARAMETER;
        }
        object = thread;
        objectType = *PsThreadType;
    } else {
        /* Process kind: bind to PID. */
        PEPROCESS process = NULL;
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &process);
        if (!NT_SUCCESS(status)) { return status; }
        object = process;
        objectType = *PsProcessType;
    }
    /* Open a user-mode (non-OBJ_KERNEL_HANDLE) handle in the requesting R3 process's
     * handle table with PreviousMode=KernelMode. The callback bypass hinges on this
     * second argument being zero — adding OBJ_KERNEL_HANDLE would route the handle
     * into the system handle table and R3 cannot use it. */
    status = ObOpenObjectByPointer(object, 0UL, NULL, desiredAccess,
        objectType, KernelMode, &handle);
    /* Balance the lookup reference irrespective of open success. */
    ObDereferenceObject(object);
    /* Publish the opened R3 handle only on success; close happens in R3 via CloseHandle. */
    if (NT_SUCCESS(status)) {
        Response->reserved0 = (ULONGLONG)(ULONG_PTR)handle;
    }
    /* Return the authoritative native result inside the typed response envelope. */
    return status;
}

NTSTATUS KswordARKDebuggerControl(WDFDEVICE Device, ULONG OwnerProcessId, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response)
{
    /* Initialize the complete typed packet before returning any operation result. */
    RtlZeroMemory(Response, sizeof(*Response));
    /* Bind the response protocol identity. */
    Response->version = KSWORD_ARK_DEBUGGER_VERSION;
    /* Bind the exact shared response size. */
    Response->size = sizeof(*Response);
    /* Echo concrete target identity for adapters. */
    Response->processId = Request->processId;
    /* Echo concrete thread identity for adapters. */
    Response->threadId = Request->threadId;
    /* All routines used here require passive execution. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { Response->status = STATUS_INVALID_DEVICE_STATE; return STATUS_SUCCESS; }
    /* Reject foreign revisions, invalid operations, and undefined flags. */
    if (Request->version != KSWORD_ARK_DEBUGGER_VERSION || Request->size != sizeof(*Request) ||
        Request->operation > KSWORD_ARK_DEBUGGER_OPEN_HANDLE || Request->reserved != 0UL || Request->reservedArgument != 0ULL ||
        (Request->flags & ~KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED) != 0UL) { Response->status = STATUS_INVALID_PARAMETER; return STATUS_SUCCESS; }
    /* Advertise only native routines present on the running Windows build. */
    Response->capabilities = KSWORD_ARK_DEBUGGER_CAP_MEMORY | KSWORD_ARK_DEBUGGER_CAP_SHADOW_INT3 | KSWORD_ARK_DEBUGGER_CAP_SHADOW_WRITES |
        KSWORD_ARK_DEBUGGER_CAP_HANDLE_BROKER;
    /* Publish native context support only with both operations available. */
    if (KswordARKDebuggerResolve(L"PsGetContextThread") != NULL && KswordARKDebuggerResolve(L"PsSetContextThread") != NULL) { Response->capabilities |= KSWORD_ARK_DEBUGGER_CAP_CONTEXT; }
    /* Publish previous-count control only with both kernel-handle operations. */
    if (KswordARKDebuggerResolve(L"ZwSuspendThread") != NULL && KswordARKDebuggerResolve(L"ZwResumeThread") != NULL) { Response->capabilities |= KSWORD_ARK_DEBUGGER_CAP_SUSPEND; }
    /* A capability query performs no target operation. */
    if (Request->operation == KSWORD_ARK_DEBUGGER_QUERY) {
        Response->reserved1 = NT_SUCCESS(KswordARKDebuggerShadowValidateViews(OwnerProcessId)) ? 0ULL : 1ULL;
        return STATUS_SUCCESS;
    }
    /* Kernel/system processes and null thread IDs are outside this user-debugger contract.
     * OPEN_HANDLE permits threadId=0 (process kind), so exempt it from the null-TID check. */
    if (Request->processId <= 4UL || (Request->operation <= KSWORD_ARK_DEBUGGER_RESUME && Request->threadId == 0UL)) { Response->status = STATUS_INVALID_PARAMETER; return STATUS_SUCCESS; }
    /* Preserve central policy for each mutating user operation. OPEN_HANDLE grants an
     * access-masked handle whose practical reach spans memory_write and suspend, so it
     * belongs under the memory-write policy gate. */
    if (Request->operation != KSWORD_ARK_DEBUGGER_GET_CONTEXT && Request->operation != KSWORD_ARK_DEBUGGER_RESUME &&
        Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_REMOVE && Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_RESTORE &&
        Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_QUARANTINE) {
        /* Initialize policy evidence before mutating the target. */
        KSWORD_ARK_SAFETY_CONTEXT safety = { 0 };
        /* Classify thread suspension separately from memory/register edits. */
        safety.Operation = Request->operation == KSWORD_ARK_DEBUGGER_SUSPEND ? KSWORD_ARK_SAFETY_OPERATION_PROCESS_SUSPEND : KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE;
        /* Bind critical-process policy to the real requested target. */
        safety.TargetProcessId = Request->processId;
        /* Preserve the adapter's explicit selected debugger operation. */
        safety.ContextFlags = (Request->flags & KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED) != 0UL ? KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED : 0UL;
        /* Evaluate the existing authoritative policy without weakening it. */
        Response->status = KswordARKSafetyEvaluate(Device, &safety);
        /* Complete a typed rejection without touching the target. */
        if (!NT_SUCCESS(Response->status)) { return STATUS_SUCCESS; }
    }
    /* Dispatch target work into the corresponding feature helper. OPEN_HANDLE routes
     * to its own broker so Shadow/Thread/Memory handlers stay single-purpose. */
    if (Request->operation == KSWORD_ARK_DEBUGGER_OPEN_HANDLE) {
        Response->status = KswordARKDebuggerOpenHandleForDebug(Request, Response);
    } else {
        Response->status = Request->operation >= KSWORD_ARK_DEBUGGER_SHADOW_ADD
            ? KswordARKDebuggerShadow(OwnerProcessId, Request, Response)
            : Request->operation <= KSWORD_ARK_DEBUGGER_RESUME
                ? KswordARKDebuggerThread(Request, Response) : KswordARKDebuggerMemory(Request, Response);
    }
    /* Keep operation NTSTATUS inside the typed packet. */
    return STATUS_SUCCESS;
}
