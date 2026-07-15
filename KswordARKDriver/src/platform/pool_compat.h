#pragma once

#include "ark/ark_driver.h"

EXTERN_C_START

PVOID
KswordARKAllocateNonPagedPool(
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG PoolTag
    );

EXTERN_C_END
