/*++

Module Name:

    pool_compat.c

Abstract:

    Nonpaged-pool allocation compatibility for kernels that do not export
    ExAllocatePool2.

Environment:

    Kernel-mode Driver Framework

--*/

#include "pool_compat.h"

typedef PVOID
(NTAPI* KSW_EX_ALLOCATE_POOL2_ROUTINE)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

PVOID
KswordARKAllocateNonPagedPool(
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG PoolTag
    )
/*++

Routine Description:

    Allocate nonpaged memory without creating a static ExAllocatePool2 import.
    New kernels use ExAllocatePool2; earlier kernels fall back to
    ExAllocatePoolWithTag with the non-executable nonpaged pool type.

Arguments:

    NumberOfBytes - Number of bytes requested by the caller.
    PoolTag - Four-character allocation tag owned by the caller.

Return Value:

    Allocated nonpaged buffer, or NULL when the request cannot be satisfied.

--*/
{
    UNICODE_STRING routineName;
    KSW_EX_ALLOCATE_POOL2_ROUTINE allocatePool2 = NULL;

    if (NumberOfBytes == 0U) {
        return NULL;
    }

    RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
    allocatePool2 = (KSW_EX_ALLOCATE_POOL2_ROUTINE)MmGetSystemRoutineAddress(&routineName);
    if (allocatePool2 != NULL) {
        return allocatePool2(POOL_FLAG_NON_PAGED, NumberOfBytes, PoolTag);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, NumberOfBytes, PoolTag);
#pragma warning(pop)
}
