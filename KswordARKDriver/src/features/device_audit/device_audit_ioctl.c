/*++

Module Name:

    device_audit_ioctl.c

Abstract:

    Public IOCTL entry points for read-only device/input/USB/GPU audit queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "device_audit_internal.h"

NTSTATUS
KswordARKDeviceAuditIoctlQueryDeviceStack(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_DEVICE_STACK_AUDIT for generic PnP, ACPI,
    PCI, root, and power-management driver stack evidence.

Arguments:

    Device - WDF device used for logging only.
    Request - Current IOCTL request.
    InputBufferLength - Optional request length.
    OutputBufferLength - Output buffer length supplied by the caller.
    BytesReturned - Receives the completed response byte count.

Return Value:

    NTSTATUS from shared validation or the shared read-only audit executor.

--*/
{
    return KswDeviceAuditExecute(
        Device,
        Request,
        InputBufferLength,
        OutputBufferLength,
        BytesReturned,
        KSWORD_ARK_DEVICE_AUDIT_PROFILE_DEVICE_STACK,
        "device-stack-audit");
}

NTSTATUS
KswordARKDeviceAuditIoctlQueryInputStack(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_INPUT_STACK_AUDIT for keyboard, mouse, HID,
    and PS/2 stack evidence.  The handler never reads input reports, scan codes,
    key states, mouse movement, or button data.

Arguments:

    Device - WDF device used for logging only.
    Request - Current IOCTL request.
    InputBufferLength - Optional request length.
    OutputBufferLength - Output buffer length supplied by the caller.
    BytesReturned - Receives the completed response byte count.

Return Value:

    NTSTATUS from shared validation or the shared read-only audit executor.

--*/
{
    return KswDeviceAuditExecute(
        Device,
        Request,
        InputBufferLength,
        OutputBufferLength,
        BytesReturned,
        KSWORD_ARK_DEVICE_AUDIT_PROFILE_INPUT_STACK,
        "input-stack-audit");
}

NTSTATUS
KswordARKDeviceAuditIoctlQueryUsbTopology(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_USB_TOPOLOGY_AUDIT.  This first R0 revision
    returns controller, hub, composite, and HID driver object/device-chain
    evidence only; deep USB descriptors remain an R3/PDB-aware future layer.

Arguments:

    Device - WDF device used for logging only.
    Request - Current IOCTL request.
    InputBufferLength - Optional request length.
    OutputBufferLength - Output buffer length supplied by the caller.
    BytesReturned - Receives the completed response byte count.

Return Value:

    NTSTATUS from shared validation or the shared read-only audit executor.

--*/
{
    return KswDeviceAuditExecute(
        Device,
        Request,
        InputBufferLength,
        OutputBufferLength,
        BytesReturned,
        KSWORD_ARK_DEVICE_AUDIT_PROFILE_USB_TOPOLOGY,
        "usb-topology-audit");
}

NTSTATUS
KswordARKDeviceAuditIoctlQueryGpuDisplayWatchdog(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_GPU_DISPLAY_WATCHDOG_AUDIT for graphics,
    monitor, fallback display, render, and watchdog driver evidence.

Arguments:

    Device - WDF device used for logging only.
    Request - Current IOCTL request.
    InputBufferLength - Optional request length.
    OutputBufferLength - Output buffer length supplied by the caller.
    BytesReturned - Receives the completed response byte count.

Return Value:

    NTSTATUS from shared validation or the shared read-only audit executor.

--*/
{
    return KswDeviceAuditExecute(
        Device,
        Request,
        InputBufferLength,
        OutputBufferLength,
        BytesReturned,
        KSWORD_ARK_DEVICE_AUDIT_PROFILE_GPU_DISPLAY_WATCHDOG,
        "gpu-display-watchdog-audit");
}
