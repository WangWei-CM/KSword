#pragma once

#include "../dyndata/dyndata_v4_internal.h"
#include "driver/KswordArkThreadIoctl.h"

EXTERN_C_START

// 可选线程字段读取失败时统一标记当前响应行。
VOID
KswordARKThreadMarkFailure(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ NTSTATUS Status
    );

// 读取一个 ETHREAD 的 ActiveExWorker 位，并写入共享线程响应标志。
VOID
KswordARKThreadPopulateWorkerField(
    _Inout_ KSWORD_ARK_THREAD_ENTRY* Entry,
    _In_ PETHREAD ThreadObject,
    _In_ const KSW_DYN_V4_BIT_FIELD_LAYOUT* ActiveExWorkerField
    );

EXTERN_C_END
