/*++

Module Name:

    device_audit_targets.c

Abstract:

    Static target DriverObject lists used by read-only device audit profiles.

Environment:

    Kernel-mode Driver Framework

--*/

#include "device_audit_internal.h"

const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditDeviceTargets[] = {
    { L"\\Driver\\ACPI", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\pci", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\swenum", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\vdrvroot", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\intelpep", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\processr", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\PDC", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\dam", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER }
};

const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditInputTargets[] = {
    { L"\\Driver\\kbdclass", KSWORD_ARK_DEVICE_AUDIT_ROLE_CLASS_DRIVER },
    { L"\\Driver\\kbdhid", KSWORD_ARK_DEVICE_AUDIT_ROLE_INTERFACE },
    { L"\\Driver\\i8042prt", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\mouclass", KSWORD_ARK_DEVICE_AUDIT_ROLE_CLASS_DRIVER },
    { L"\\Driver\\mouhid", KSWORD_ARK_DEVICE_AUDIT_ROLE_INTERFACE },
    { L"\\Driver\\HidClass", KSWORD_ARK_DEVICE_AUDIT_ROLE_INTERFACE },
    { L"\\Driver\\HidUsb", KSWORD_ARK_DEVICE_AUDIT_ROLE_INTERFACE }
};

const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditUsbTargets[] = {
    { L"\\Driver\\USBXHCI", KSWORD_ARK_DEVICE_AUDIT_ROLE_CONTROLLER },
    { L"\\Driver\\UCX01000", KSWORD_ARK_DEVICE_AUDIT_ROLE_CONTROLLER },
    { L"\\Driver\\USBHUB3", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\usbhub", KSWORD_ARK_DEVICE_AUDIT_ROLE_BUS_DRIVER },
    { L"\\Driver\\usbccgp", KSWORD_ARK_DEVICE_AUDIT_ROLE_COMPOSITE },
    { L"\\Driver\\HidClass", KSWORD_ARK_DEVICE_AUDIT_ROLE_INTERFACE },
    { L"\\Driver\\HidUsb", KSWORD_ARK_DEVICE_AUDIT_ROLE_INTERFACE }
};

const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditGpuTargets[] = {
    { L"\\Driver\\dxgkrnl", KSWORD_ARK_DEVICE_AUDIT_ROLE_DISPLAY },
    { L"\\Driver\\dxgmms2", KSWORD_ARK_DEVICE_AUDIT_ROLE_DISPLAY },
    { L"\\Driver\\cdd", KSWORD_ARK_DEVICE_AUDIT_ROLE_DISPLAY },
    { L"\\Driver\\monitor", KSWORD_ARK_DEVICE_AUDIT_ROLE_DISPLAY },
    { L"\\Driver\\BasicDisplay", KSWORD_ARK_DEVICE_AUDIT_ROLE_DISPLAY },
    { L"\\Driver\\BasicRender", KSWORD_ARK_DEVICE_AUDIT_ROLE_DISPLAY },
    { L"\\Driver\\watchdog", KSWORD_ARK_DEVICE_AUDIT_ROLE_WATCHDOG }
};



const ULONG g_KswDeviceAuditDeviceTargetCount = RTL_NUMBER_OF(g_KswDeviceAuditDeviceTargets);
const ULONG g_KswDeviceAuditInputTargetCount = RTL_NUMBER_OF(g_KswDeviceAuditInputTargets);
const ULONG g_KswDeviceAuditUsbTargetCount = RTL_NUMBER_OF(g_KswDeviceAuditUsbTargets);
const ULONG g_KswDeviceAuditGpuTargetCount = RTL_NUMBER_OF(g_KswDeviceAuditGpuTargets);
