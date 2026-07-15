#pragma once

#include <wdf.h>

#include "driver/KswordArkDebugOutputIoctl.h"

EXTERN_C_START

// 内部 slot 使用提交序列保护快照复制；负值表示写入中，正值表示已提交。
typedef struct _KSWORD_ARK_DEBUG_OUTPUT_SLOT
{
    volatile LONG64 CommitSequence;
    KSWORD_ARK_DEBUG_OUTPUT_RECORD Record;
} KSWORD_ARK_DEBUG_OUTPUT_SLOT, *PKSWORD_ARK_DEBUG_OUTPUT_SLOT;

// 初始化指定设备上下文中的固定缓冲区；本函数不会立即注册系统回调。
NTSTATUS
KswordARKDebugOutputInitialize(
    _In_ WDFDEVICE Device
    );

// 执行 START/STOP/QUERY，并返回可直接展示的运行时状态快照。
NTSTATUS
KswordARKDebugOutputControl(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST* ControlRequest,
    _Out_ KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE* ControlResponse
    );

// 从 afterSequence 开始按升序复制记录，不在回调路径分配内存或等待锁。
NTSTATUS
KswordARKDebugOutputDrain(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST* DrainRequest,
    _Out_writes_bytes_(OutputBufferLength) KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE* OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

// 驱动卸载前注销回调，保证全局回调不再引用设备上下文。
VOID
KswordARKDebugOutputUninitialize(
    VOID
    );

EXTERN_C_END
