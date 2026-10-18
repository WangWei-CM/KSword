/*++

Module Name:

    device_control.c

Abstract:

    This file contains control-device creation and compatibility PnP path.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../features/hvm/hvm_noattach.h"
#include "device_control.tmh"

#include <wdmsec.h>

#include "KswordArkLogProtocol.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KswordARKDriverCreateDevice)
#pragma alloc_text (PAGE, KswordARKDriverCreateControlDevice)
#pragma alloc_text (PAGE, KswordARKDriverPublishControlDevice)
#pragma alloc_text (PAGE, KswordARKDriverEvtDevicePrepareHardware)
#endif

/*
 * Control device security: SYSTEM full, Administrators read/write/execute.
 * Nobody else can open it at all.
 *
 * World and Restricted used to hold GENERIC_READ here, on the reasoning that
 * reading the log stream is harmless.  That reasoning does not survive contact
 * with how the IOCTLs are declared: FILE_ANY_ACCESS tells the I/O manager to
 * perform no access check on the request, so a read-only handle can issue any
 * IOCTL declared that way.  101 of them still are.  The effect was that every
 * user on the machine could reach a hundred kernel entry points through a
 * handle the descriptor described as read-only.
 *
 * Removing the two ACEs costs nothing in this repository.  Every consumer -
 * including the one that opens with GENERIC_READ alone to read the log stream -
 * also performs read/write operations elsewhere, so all of them already require
 * Administrators, and Administrators keep GENERIC_READ through the BA entry.
 * What changes is that a non-administrator can no longer open the device, which
 * is the whole of the unprivileged reach.
 *
 * This narrows who may knock.  It does not make the 101 FILE_ANY_ACCESS
 * declarations correct - an IOCTL that mutates state should say so in its
 * control code, so that a read-only handle held by an administrator is refused
 * too.  That work is separate and still open.
 */
static const WCHAR g_KswordArkControlDeviceSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)";

NTSTATUS
KswordARKDriverCreateControlDevice(
    _In_ WDFDRIVER Driver,
    _Out_opt_ WDFDEVICE* DeviceOut
    )
/*++

Routine Description:

    Create a control device used by R3 to read driver log stream. The device is
    fully built here but stays unpublished: KswordARKDriverPublishControlDevice
    must be called once every dependent runtime is up, so R3 can never open the
    device while callback state is still being assembled.

Arguments:

    Driver - WDF driver handle created in DriverEntry.
    DeviceOut - Receives the created (still unpublished) control device.

Return Value:

    NTSTATUS

--*/
{
    PWDFDEVICE_INIT deviceInit = NULL;
    WDFDEVICE device = WDF_NO_HANDLE;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    /* 控制文件 Cleanup 撤销其独占不附加会话，不影响其它 IOCTL。 */
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    /* 关闭标志处理 Cleanup 早于 OPEN 发布的竞态。 */
    WDF_FILEOBJECT_CONFIG fileConfiguration;
    UNICODE_STRING sddlText;
    NTSTATUS status = STATUS_SUCCESS;

    DECLARE_CONST_UNICODE_STRING(deviceName, KSWORD_ARK_LOG_DEVICE_NT_NAME);
    DECLARE_CONST_UNICODE_STRING(symbolicName, KSWORD_ARK_LOG_DOS_NAME);

    PAGED_CODE();

    if (DeviceOut != NULL) {
        *DeviceOut = WDF_NO_HANDLE;
    }

    RtlInitUnicodeString(&sddlText, g_KswordArkControlDeviceSddl);
    KswordArkStartupStage(KswordArkStartStageControlInitAllocate);
    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddlText);
    if (deviceInit == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfControlDeviceInitAllocate failed");
        return KswordArkStartupFailure(
            KswordArkStartStageControlInitAllocate,
            STATUS_INSUFFICIENT_RESOURCES);
    }

    KswordArkStartupStage(KswordArkStartStageDeviceAssignName);
    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceInitAssignName failed %!STATUS!", status);
        WdfDeviceInitFree(deviceInit);
        return KswordArkStartupFailure(KswordArkStartStageDeviceAssignName, status);
    }

    WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    /* 文件对象拥有轻量 Closing 状态，所有打开方式都能正常关闭。 */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfiguration, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, KswordARKHvmNoAttachFileCleanup);
    /* 不替换设备自己的 DEVICE_CONTEXT。 */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, KSW_HVM_NOATTACH_FILE_CONTEXT);
    /* 生命周期回调仅撤销 publication 和排队，不在 Cleanup 等待 VMX。 */
    WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfiguration, &fileAttributes);
    WdfDeviceInitSetIoInCallerContextCallback(deviceInit, KswordARKDriverEvtIoInCallerContext);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    KswordArkStartupStage(KswordArkStartStageDeviceCreate);
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreate(control) failed %!STATUS!", status);
        if (deviceInit != NULL) {
            WdfDeviceInitFree(deviceInit);
        }
        // 固定设备名冲突（例如旧实例未卸载干净）会在这里被记录成可定位的阶段。
        return KswordArkStartupFailure(KswordArkStartStageDeviceCreate, status);
    }

    KswordArkStartupStage(KswordArkStartStageLogChannel);
    status = KswordARKDriverInitializeLogChannel(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverInitializeLogChannel failed %!STATUS!", status);
        WdfObjectDelete(device);
        return KswordArkStartupFailure(KswordArkStartStageLogChannel, status);
    }

    // 初始化内核调试输出环形缓冲区；只有用户显式开始捕获时才注册系统回调。
    KswordArkStartupStage(KswordArkStartStageDebugOutput);
    status = KswordARKDebugOutputInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDebugOutputInitialize failed %!STATUS!", status);
        WdfObjectDelete(device);
        return KswordArkStartupFailure(KswordArkStartStageDebugOutput, status);
    }

    KswordArkStartupStage(KswordArkStartStageSymbolicLink);
    status = WdfDeviceCreateSymbolicLink(device, &symbolicName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreateSymbolicLink failed %!STATUS!", status);
        WdfObjectDelete(device);
        // 符号链接冲突同样指向机器上仍存在的另一个 KswordARK 实例。
        return KswordArkStartupFailure(KswordArkStartStageSymbolicLink, status);
    }

    KswordArkStartupStage(KswordArkStartStageDefaultQueue);
    status = KswordARKDriverQueueInitialize(device, FALSE);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverQueueInitialize(control) failed %!STATUS!", status);
        WdfObjectDelete(device);
        // WdfIoQueueCreate 返回 STATUS_UNSUCCESSFUL 时这里是唯一能记录原始状态的地方。
        return KswordArkStartupFailure(KswordArkStartStageDefaultQueue, status);
    }

    status = KswordARKDynDataInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "KswordARKDynDataInitialize recorded failure %!STATUS!", status);
    }

    status = KswordARKDriverEnqueueLogFrame(device, "Info", "KswordARK driver started.");
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "KswordARKDriverEnqueueLogFrame(startup) failed %!STATUS!", status);
    }

    if (DeviceOut != NULL) {
        *DeviceOut = device;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Control log device created successfully");
    return STATUS_SUCCESS;
}

VOID
KswordARKDriverPublishControlDevice(
    _In_ WDFDEVICE Device
    )
/*++

Routine Description:

    Make the control device visible to user mode. Creating and publishing are
    kept apart on purpose: WdfControlFinishInitializing used to run before the
    callback runtime existed, which let R3 reach IOCTL handlers whose backing
    state was still being built.

Arguments:

    Device - Control device returned by KswordARKDriverCreateControlDevice.

Return Value:

    VOID

--*/
{
    PAGED_CODE();

    // 所有依赖运行时都已就绪，此时才允许 I/O 管理器分发请求。
    WdfControlFinishInitializing(Device);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Control log device published");
}

NTSTATUS
KswordARKDriverCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
/*++

Routine Description:

    Keep a minimal PnP path for compatibility; primary path is control device.

Arguments:

    DeviceInit - Framework-allocated init structure.

Return Value:

    NTSTATUS

--*/
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device = WDF_NO_HANDLE;
    PDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = KswordARKDriverEvtDevicePrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, KswordARKDriverEvtIoInCallerContext);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreate(PnP) failed %!STATUS!", status);
        return status;
    }

    deviceContext = DeviceGetContext(device);
    deviceContext->UsbDevice = WDF_NO_HANDLE;
    deviceContext->PrivateDeviceData = 0U;

    status = KswordARKDriverInitializeLogChannel(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // 兼容 PnP 设备路径也必须拥有完整的调试输出上下文，避免 IOCTL 访问未初始化状态。
    status = KswordARKDebugOutputInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_KswordARKDriver,
        NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreateDeviceInterface failed %!STATUS!", status);
        return status;
    }

    // 兼容 PnP 设备参与电源管理，因此默认队列保留电源管理语义和 EvtIoStop。
    status = KswordARKDriverQueueInitialize(device, TRUE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDynDataInitialize(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "KswordARKDynDataInitialize(PnP) recorded failure %!STATUS!", status);
    }

    (void)KswordARKDriverEnqueueLogFrame(device, "Info", "KswordARK PnP device initialized.");
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
    )
/*++

Routine Description:

    Placeholder callback retained for compatibility.

Arguments:

    Device - Framework device handle.
    ResourceList - Raw resource list.
    ResourceListTranslated - Translated resource list.

Return Value:

    NTSTATUS

--*/
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}
