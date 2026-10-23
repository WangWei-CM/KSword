#pragma once

#include "KswordArkProcessIoctl.h"

// Debugger-neutral R0 operations. Context bytes use Windows AMD64 CONTEXT layout.
#define KSWORD_ARK_DEBUGGER_VERSION 1UL
#define KSWORD_ARK_IOCTL_FUNCTION_DEBUGGER 0x91FUL
#define IOCTL_KSWORD_ARK_DEBUGGER \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_DEBUGGER, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define KSWORD_ARK_DEBUGGER_QUERY 0UL
#define KSWORD_ARK_DEBUGGER_GET_CONTEXT 1UL
#define KSWORD_ARK_DEBUGGER_SET_CONTEXT 2UL
#define KSWORD_ARK_DEBUGGER_SUSPEND 3UL
#define KSWORD_ARK_DEBUGGER_RESUME 4UL
#define KSWORD_ARK_DEBUGGER_ALLOCATE 5UL
#define KSWORD_ARK_DEBUGGER_PROTECT 6UL
#define KSWORD_ARK_DEBUGGER_FREE 7UL
/* Add/remove one execution-only INT3 in a pinned user ShadowPage. */
#define KSWORD_ARK_DEBUGGER_SHADOW_ADD 8UL
#define KSWORD_ARK_DEBUGGER_SHADOW_REMOVE 9UL
/* Execution-view code patches; original bytes and ordinary reads stay intact.
 * Private allocations and proven private COW image/mapped pages are accepted;
 * pinned working-set Shared=0 and the current VA/PFN must both be verified.
 * WRITE uses context[0..bytes), at most 1232 bytes in one executable page.
 * RESTORE clears only memory patches, preserving INT3s; address=bytes=0 clears
 * all pages owned by the real requestor for this target. */
#define KSWORD_ARK_DEBUGGER_SHADOW_WRITE 10UL
#define KSWORD_ARK_DEBUGGER_SHADOW_RESTORE 11UL
/* Latch an owned target after incomplete content rollback. Only a successful
 * SHADOW_RESTORE with address=bytes=0 clears it; all direct STARTs are blocked. */
#define KSWORD_ARK_DEBUGGER_SHADOW_QUARANTINE 12UL
/* Open a process or thread handle via ObOpenObjectByPointer with
 * PreviousMode=KernelMode so Ob callbacks that strip access only for
 * UserMode opens (the common anti-cheat pattern) are bypassed. The
 * resulting handle is inserted into the requesting R3 process's handle
 * table and returned verbatim in Response.reserved0; callers close it
 * with CloseHandle. threadId=0 selects the process kind; threadId!=0
 * selects the thread kind (processId is still echoed). The desired
 * ACCESS_MASK is passed in Request.address (low 32 bits). */
#define KSWORD_ARK_DEBUGGER_OPEN_HANDLE 13UL
#define KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED 1UL
#define KSWORD_ARK_DEBUGGER_CAP_CONTEXT 1UL
#define KSWORD_ARK_DEBUGGER_CAP_SUSPEND 2UL
#define KSWORD_ARK_DEBUGGER_CAP_MEMORY 4UL
#define KSWORD_ARK_DEBUGGER_CAP_SHADOW_INT3 8UL
#define KSWORD_ARK_DEBUGGER_CAP_SHADOW_WRITES 16UL
/* Advertised whenever the driver can broker a KernelMode-opened handle
 * duplicated into the requesting R3 handle table. Always on when the
 * debugger module is linked; included so adapters can gate the attach
 * path without probing. */
#define KSWORD_ARK_DEBUGGER_CAP_HANDLE_BROKER 32UL
/* QUERY response.reserved1: an owned view or content rollback is quarantined;
 * residency must remain stopped until restore/reinstall repairs that page. */
#define KSWORD_ARK_DEBUGGER_SHADOW_INVALID_VIEW 1ULL
/* ADD/WRITE response.reserved1 contains a concrete backing rejection reason.
 * These diagnostics do not change the packet layout or QUERY health semantics. */
#define KSWORD_ARK_DEBUGGER_SHADOW_BACKING_SHARED 2ULL
#define KSWORD_ARK_DEBUGGER_SHADOW_BACKING_UNVERIFIABLE 4ULL
#define KSWORD_ARK_DEBUGGER_SHADOW_MAPPING_CHANGED 8ULL
#define KSWORD_ARK_DEBUGGER_CONTEXT_BYTES 1232UL

typedef struct _KSWORD_ARK_DEBUGGER_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long operation;
    unsigned long flags;
    unsigned long processId;
    unsigned long threadId;
    unsigned long contextFlags;
    unsigned long reserved;
    unsigned long long address;
    unsigned long long bytes;
    unsigned long long reservedArgument;
    unsigned long protection;
    unsigned long allocationType;
    unsigned char context[KSWORD_ARK_DEBUGGER_CONTEXT_BYTES];
} KSWORD_ARK_DEBUGGER_REQUEST;

typedef struct _KSWORD_ARK_DEBUGGER_RESPONSE
{
    unsigned long version;
    unsigned long size;
    long status;
    unsigned long capabilities;
    unsigned long processId;
    unsigned long threadId;
    unsigned long previousSuspendCount;
    unsigned long previousProtection;
    unsigned long long address;
    unsigned long long bytes;
    unsigned long long reserved0;
    unsigned long long reserved1;
    unsigned char context[KSWORD_ARK_DEBUGGER_CONTEXT_BYTES];
} KSWORD_ARK_DEBUGGER_RESPONSE;
