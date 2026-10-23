#include <ntifs.h>
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

/* Map Windows process/thread ACCESS_MASK bits to the existing safety operations. */
#define KSW_DEBUGGER_POLICY_TERMINATE 0x01UL
#define KSW_DEBUGGER_POLICY_SUSPEND 0x02UL
#define KSW_DEBUGGER_POLICY_PROTECTION 0x04UL
#define KSW_DEBUGGER_POLICY_INJECT 0x08UL
#define KSW_DEBUGGER_POLICY_ALL 0x0FUL

/* Process-specific rights are local constants because kernel WDK headers expose only a subset. */
#define KSW_DEBUGGER_PROCESS_TERMINATE 0x0001UL
#define KSW_DEBUGGER_PROCESS_CREATE_THREAD 0x0002UL
#define KSW_DEBUGGER_PROCESS_SET_SESSIONID 0x0004UL
#define KSW_DEBUGGER_PROCESS_VM_OPERATION 0x0008UL
#define KSW_DEBUGGER_PROCESS_VM_READ 0x0010UL
#define KSW_DEBUGGER_PROCESS_VM_WRITE 0x0020UL
#define KSW_DEBUGGER_PROCESS_CREATE_PROCESS 0x0080UL
#define KSW_DEBUGGER_PROCESS_SET_QUOTA 0x0100UL
#define KSW_DEBUGGER_PROCESS_SET_INFORMATION 0x0200UL
#define KSW_DEBUGGER_PROCESS_QUERY_INFORMATION 0x0400UL
#define KSW_DEBUGGER_PROCESS_SUSPEND_RESUME 0x0800UL
#define KSW_DEBUGGER_PROCESS_QUERY_LIMITED_INFORMATION 0x1000UL
#define KSW_DEBUGGER_PROCESS_SET_LIMITED_INFORMATION 0x2000UL

/* Thread-specific rights beyond the WDK subset retain their documented bit values. */
#define KSW_DEBUGGER_THREAD_QUERY_INFORMATION 0x0040UL
#define KSW_DEBUGGER_THREAD_SET_THREAD_TOKEN 0x0080UL
#define KSW_DEBUGGER_THREAD_IMPERSONATE 0x0100UL
#define KSW_DEBUGGER_THREAD_DIRECT_IMPERSONATION 0x0200UL

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

/* Evaluate all existing safety groups reachable through the requested handle rights. */
static NTSTATUS KswordARKDebuggerEvaluateHandleSafety(WDFDEVICE Device,
    const KSWORD_ARK_DEBUGGER_REQUEST* Request)
{
    /* Preserve the previous memory-write policy check for every broker request. */
    KSWORD_ARK_SAFETY_CONTEXT safety = { 0 };
    /* Classify the exact 32-bit ACCESS_MASK without changing the wire layout. */
    ACCESS_MASK access = (ACCESS_MASK)Request->address;
    /* The known mask lets future or generic rights conservatively use all groups. */
    ACCESS_MASK known;
    /* Track only policy groups in addition to the existing memory-write check. */
    ULONG groups = 0UL;
    /* Keep the first authoritative policy failure. */
    NTSTATUS status;
    /* Common object rights include read, deletion and security mutation. */
    known = READ_CONTROL | SYNCHRONIZE | GENERIC_READ | DELETE |
        WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY;
    /* Standard DELETE and security-control rights have distinct existing policies. */
    if ((access & DELETE) != 0UL) { groups |= KSW_DEBUGGER_POLICY_TERMINATE; }
    if ((access & (WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY)) != 0UL) {
        groups |= KSW_DEBUGGER_POLICY_PROTECTION;
    }
    /* Interpret the object-specific low bits for the requested object kind. */
    if (Request->threadId != 0UL) {
        /* Read-only context and query rights require no additional policy group. */
        known |= THREAD_GET_CONTEXT | KSW_DEBUGGER_THREAD_QUERY_INFORMATION |
            THREAD_QUERY_LIMITED_INFORMATION;
        /* Termination and scheduling rights follow their dedicated policies. */
        known |= THREAD_TERMINATE | THREAD_SUSPEND_RESUME | THREAD_ALERT | THREAD_RESUME;
        if ((access & THREAD_TERMINATE) != 0UL) { groups |= KSW_DEBUGGER_POLICY_TERMINATE; }
        if ((access & (THREAD_SUSPEND_RESUME | THREAD_ALERT | THREAD_RESUME)) != 0UL) {
            groups |= KSW_DEBUGGER_POLICY_SUSPEND;
        }
        /* Context and impersonation control can alter another thread's execution. */
        known |= THREAD_SET_CONTEXT | KSW_DEBUGGER_THREAD_SET_THREAD_TOKEN |
            KSW_DEBUGGER_THREAD_IMPERSONATE | KSW_DEBUGGER_THREAD_DIRECT_IMPERSONATION;
        if ((access & (THREAD_SET_CONTEXT | KSW_DEBUGGER_THREAD_SET_THREAD_TOKEN |
            KSW_DEBUGGER_THREAD_IMPERSONATE | KSW_DEBUGGER_THREAD_DIRECT_IMPERSONATION)) != 0UL) {
            groups |= KSW_DEBUGGER_POLICY_INJECT;
        }
        /* Thread information and security changes share protection policy. */
        known |= THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION;
        if ((access & (THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION)) != 0UL) {
            groups |= KSW_DEBUGGER_POLICY_PROTECTION;
        }
    } else {
        /* Read and query rights do not imply target mutation. */
        known |= KSW_DEBUGGER_PROCESS_VM_READ | KSW_DEBUGGER_PROCESS_QUERY_INFORMATION |
            KSW_DEBUGGER_PROCESS_QUERY_LIMITED_INFORMATION;
        /* Termination, suspension and injection capabilities use their own policies. */
        known |= KSW_DEBUGGER_PROCESS_TERMINATE | KSW_DEBUGGER_PROCESS_SUSPEND_RESUME |
            KSW_DEBUGGER_PROCESS_CREATE_THREAD | KSW_DEBUGGER_PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE;
        if ((access & KSW_DEBUGGER_PROCESS_TERMINATE) != 0UL) { groups |= KSW_DEBUGGER_POLICY_TERMINATE; }
        if ((access & KSW_DEBUGGER_PROCESS_SUSPEND_RESUME) != 0UL) { groups |= KSW_DEBUGGER_POLICY_SUSPEND; }
        if ((access & (KSW_DEBUGGER_PROCESS_CREATE_THREAD | KSW_DEBUGGER_PROCESS_CREATE_PROCESS)) != 0UL) {
            groups |= KSW_DEBUGGER_POLICY_INJECT;
        }
        /* Duplicating arbitrary target handles reaches every existing policy group. */
        if ((access & PROCESS_DUP_HANDLE) != 0UL) { groups |= KSW_DEBUGGER_POLICY_ALL; }
        /* Virtual memory rights keep the existing memory-write classification. */
        known |= KSW_DEBUGGER_PROCESS_VM_OPERATION | KSW_DEBUGGER_PROCESS_VM_WRITE;
        /* Process information and quota changes follow protection policy. */
        known |= KSW_DEBUGGER_PROCESS_SET_SESSIONID | KSW_DEBUGGER_PROCESS_SET_QUOTA |
            KSW_DEBUGGER_PROCESS_SET_INFORMATION | KSW_DEBUGGER_PROCESS_SET_LIMITED_INFORMATION;
        if ((access & (KSW_DEBUGGER_PROCESS_SET_SESSIONID | KSW_DEBUGGER_PROCESS_SET_QUOTA |
            KSW_DEBUGGER_PROCESS_SET_INFORMATION | KSW_DEBUGGER_PROCESS_SET_LIMITED_INFORMATION)) != 0UL) {
            groups |= KSW_DEBUGGER_POLICY_PROTECTION;
        }
    }
    /* Generic write/all/execute, MAXIMUM_ALLOWED and future bits can expand broadly. */
    if ((access & ~known) != 0UL) { groups |= KSW_DEBUGGER_POLICY_ALL; }
    /* Bind every decision to the same selected target and confirmation state. */
    safety.TargetProcessId = Request->processId;
    safety.ContextFlags = (Request->flags & KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED) != 0UL
        ? KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED : 0UL;
    /* Preserve the broker's existing policy behavior before checking extra rights. */
    safety.Operation = KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE;
    status = KswordARKSafetyEvaluate(Device, &safety);
    if (!NT_SUCCESS(status)) { return status; }
    /* Apply only the existing termination policy when that capability is requested. */
    if ((groups & KSW_DEBUGGER_POLICY_TERMINATE) != 0UL) {
        safety.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_TERMINATE;
        status = KswordARKSafetyEvaluate(Device, &safety);
        if (!NT_SUCCESS(status)) { return status; }
    }
    /* Apply only the existing suspension policy when that capability is requested. */
    if ((groups & KSW_DEBUGGER_POLICY_SUSPEND) != 0UL) {
        safety.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_SUSPEND;
        status = KswordARKSafetyEvaluate(Device, &safety);
        if (!NT_SUCCESS(status)) { return status; }
    }
    /* Apply only the existing protection policy when that capability is requested. */
    if ((groups & KSW_DEBUGGER_POLICY_PROTECTION) != 0UL) {
        safety.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_SET_PROTECTION;
        status = KswordARKSafetyEvaluate(Device, &safety);
        if (!NT_SUCCESS(status)) { return status; }
    }
    /* Apply only the existing injection policy when that capability is requested. */
    if ((groups & KSW_DEBUGGER_POLICY_INJECT) != 0UL) {
        safety.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_INJECT;
        status = KswordARKSafetyEvaluate(Device, &safety);
        if (!NT_SUCCESS(status)) { return status; }
    }
    /* Every requested capability passed its existing policy decision. */
    return STATUS_SUCCESS;
}

/* Insert the KernelMode-opened target handle into the pinned IRP requestor's table. */
static NTSTATUS KswordARKDebuggerOpenHandleForDebug(WDFDEVICE Device, ULONG OwnerProcessId, PEPROCESS OwnerProcess,
    const KSWORD_ARK_DEBUGGER_REQUEST* Request, KSWORD_ARK_DEBUGGER_RESPONSE* Response)
{
    /* Keep the target object referenced through duplicate verification. */
    PVOID object = NULL;
    /* Match the opened object against its declared process or thread kind. */
    POBJECT_TYPE objectType = NULL;
    /* The requested ACCESS_MASK rides in the low half of the address field. */
    ACCESS_MASK desiredAccess = (ACCESS_MASK)(Request->address & 0xFFFFFFFFULL);
    /* All three private handles live in the system handle table. */
    HANDLE sourceHandle = NULL, systemProcessHandle = NULL, ownerProcessHandle = NULL;
    /* Only this handle is created in the original requestor's table. */
    HANDLE userHandle = NULL;
    /* Read the copied user's actual rights, including callback reductions. */
    PUBLIC_OBJECT_BASIC_INFORMATION userInfo = { 0 };
    /* Reference the copied user handle while checking type, identity and rights. */
    PVOID verifiedObject = NULL;
    /* Scope the short access check to the pinned requestor process. */
    KAPC_STATE attachState;
    /* Preserve the exact native result on all cleanup paths. */
    NTSTATUS status;
    /* Reject an unassociated or replaced requestor before touching a target. */
    if (OwnerProcess == NULL || OwnerProcessId == 0UL ||
        HandleToULong(PsGetProcessId(OwnerProcess)) != OwnerProcessId) { return STATUS_INVALID_CID; }
    /* An empty ACCESS_MASK offers no debug utility. */
    if (desiredAccess == 0U) { return STATUS_INVALID_PARAMETER; }
    /* The upper half of the field is not part of ACCESS_MASK. */
    if ((Request->address & 0xFFFFFFFF00000000ULL) != 0ULL) { return STATUS_INVALID_PARAMETER; }
    /* This operation has no memory, protection, allocation or context payload. */
    if (Request->bytes != 0ULL || Request->protection != 0UL ||
        Request->allocationType != 0UL || Request->contextFlags != 0UL) { return STATUS_INVALID_PARAMETER; }
    /* Resolve the real target object by its authoritative identifier. */
    if (Request->threadId != 0UL) {
        PETHREAD thread = NULL;
        status = PsLookupThreadByThreadId(ULongToHandle(Request->threadId), &thread);
        if (!NT_SUCCESS(status)) { return status; }
        /* A recycled thread ID must still belong to the declared target process. */
        if (HandleToULong(PsGetThreadProcessId(thread)) != Request->processId) {
            ObDereferenceObject(thread);
            return STATUS_INVALID_PARAMETER;
        }
        object = thread;
        objectType = *PsThreadType;
    } else {
        PEPROCESS process = NULL;
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &process);
        if (!NT_SUCCESS(status)) { return status; }
        object = process;
        objectType = *PsProcessType;
    }
    /* Evaluate requested rights only after the target object is referenced. */
    status = KswordARKDebuggerEvaluateHandleSafety(Device, Request);
    if (!NT_SUCCESS(status)) { goto Complete; }
    /* Never place a private source handle in an arbitrary WDF worker process. */
    status = ObOpenObjectByPointer(object, OBJ_KERNEL_HANDLE, NULL, desiredAccess,
        objectType, KernelMode, &sourceHandle);
    if (!NT_SUCCESS(status)) { goto Complete; }
    /* Kernel handles are stored in the System process handle table. */
    status = ObOpenObjectByPointer(PsInitialSystemProcess, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_DUP_HANDLE, *PsProcessType, KernelMode, &systemProcessHandle);
    if (!NT_SUCCESS(status)) { goto Complete; }
    /* The requestor's referenced process object pins the intended destination. */
    status = ObOpenObjectByPointer(OwnerProcess, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_DUP_HANDLE, *PsProcessType, KernelMode, &ownerProcessHandle);
    if (!NT_SUCCESS(status)) { goto Complete; }
    /* An explicit target process makes duplication independent of queue-thread context. */
    status = ZwDuplicateObject(systemProcessHandle, sourceHandle, ownerProcessHandle,
        &userHandle, 0UL, 0UL, DUPLICATE_SAME_ACCESS);
    if (!NT_SUCCESS(status)) { goto Complete; }
    /* A successful duplicate must still yield a real user-table handle. */
    if (userHandle == NULL) { status = STATUS_INVALID_HANDLE; goto Complete; }
    /* Read its actual grant and identity in the owning table; this window sends no I/O. */
    KeStackAttachProcess((PRKPROCESS)OwnerProcess, &attachState);
    status = ZwQueryObject(userHandle, ObjectBasicInformation, &userInfo,
        (ULONG)sizeof(userInfo), NULL);
    if (NT_SUCCESS(status)) {
        status = ObReferenceObjectByHandle(userHandle, 0UL,
            objectType, UserMode, &verifiedObject, NULL);
    }
    KeUnstackDetachProcess(&attachState);
    /* A wrong-type or unexpected object must never be reported as success. */
    if (!NT_SUCCESS(status)) { goto Complete; }
    if (verifiedObject != object) { status = STATUS_OBJECT_TYPE_MISMATCH; goto Complete; }
    /* Publish even a reduced-rights handle so available operations remain usable. */
    Response->reserved0 = (ULONGLONG)(ULONG_PTR)userHandle;
    Response->reserved1 = (ULONGLONG)userInfo.GrantedAccess;
Complete:
    /* Release the short verification reference before the original target reference. */
    if (verifiedObject != NULL) { ObDereferenceObject(verifiedObject); }
    /* Close a destination handle created before any later verification failure. */
    if (!NT_SUCCESS(status) && userHandle != NULL && ownerProcessHandle != NULL) {
        NTSTATUS closeStatus = ZwDuplicateObject(ownerProcessHandle, userHandle, NULL,
            NULL, 0UL, 0UL, DUPLICATE_CLOSE_SOURCE);
        /* A concurrent close may invalidate the handle; try in-table close if needed. */
        if (!NT_SUCCESS(closeStatus)) {
            KeStackAttachProcess((PRKPROCESS)OwnerProcess, &attachState);
            (void)ZwClose(userHandle);
            KeUnstackDetachProcess(&attachState);
        }
    }
    /* Balance every kernel handle and target lookup on both success and failure. */
    if (ownerProcessHandle != NULL) { (void)ZwClose(ownerProcessHandle); }
    if (systemProcessHandle != NULL) { (void)ZwClose(systemProcessHandle); }
    if (sourceHandle != NULL) { (void)ZwClose(sourceHandle); }
    if (object != NULL) { ObDereferenceObject(object); }
    /* Keep operation failure inside the typed response envelope. */
    return status;
}

NTSTATUS KswordARKDebuggerControl(WDFDEVICE Device, ULONG OwnerProcessId, PEPROCESS OwnerProcess, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
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
    /* 保留既有能力声明，并额外声明独立的仅引用身份租约。 */
    Response->capabilities = KSWORD_ARK_DEBUGGER_CAP_MEMORY | KSWORD_ARK_DEBUGGER_CAP_SHADOW_INT3 | KSWORD_ARK_DEBUGGER_CAP_SHADOW_WRITES |
        KSWORD_ARK_DEBUGGER_CAP_HANDLE_BROKER | KSWORD_ARK_DEBUGGER_CAP_IDENTITY_RETAIN;
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
    /* Preserve central policy for every requested mutating operation. */
    if (Request->operation != KSWORD_ARK_DEBUGGER_GET_CONTEXT && Request->operation != KSWORD_ARK_DEBUGGER_RESUME &&
        Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_REMOVE && Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_RESTORE &&
         Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_QUARANTINE &&
         Request->operation != KSWORD_ARK_DEBUGGER_OPEN_HANDLE) {
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
        Response->status = KswordARKDebuggerOpenHandleForDebug(Device, OwnerProcessId, OwnerProcess, Request, Response);
    } else {
        Response->status = Request->operation >= KSWORD_ARK_DEBUGGER_SHADOW_ADD
            ? KswordARKDebuggerShadow(OwnerProcessId, Request, Response)
            : Request->operation <= KSWORD_ARK_DEBUGGER_RESUME
                ? KswordARKDebuggerThread(Request, Response) : KswordARKDebuggerMemory(Request, Response);
    }
    /* Keep operation NTSTATUS inside the typed packet. */
    return STATUS_SUCCESS;
}
