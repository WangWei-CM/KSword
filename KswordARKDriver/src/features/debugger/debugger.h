#pragma once

#include "ark/ark_driver.h"
#include "driver/KswordArkDebuggerIoctl.h"

/* Execute a validated, ID-scoped user-process debugger operation at PASSIVE_LEVEL. */
NTSTATUS KswordARKDebuggerControl(WDFDEVICE Device, ULONG OwnerProcessId, PEPROCESS OwnerProcess, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response);
NTSTATUS KswordARKDebuggerShadow(ULONG OwnerProcessId, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response);
VOID KswordARKDebuggerProcessExit(ULONG ProcessId);
VOID KswordARKDebuggerShutdown(VOID);
/* OwnerProcessId=0 checks all debugger-owned execution views. */
NTSTATUS KswordARKDebuggerShadowValidateViews(ULONG OwnerProcessId);
/* Successful acquire retains a shared registry lease until ReleaseStartLease.
 * Acquire before HVM state locks, release after them. Failure retains no lock. */
NTSTATUS KswordARKDebuggerShadowAcquireStartLease(VOID);
VOID KswordARKDebuggerShadowReleaseStartLease(VOID);
