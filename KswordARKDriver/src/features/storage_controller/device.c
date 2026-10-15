#include "controller.h"

#define KSCC_PCI_BAR_IO_SPACE 0x00000001UL
#define KSCC_PCI_BAR_MEMORY_TYPE_MASK 0x00000006UL
#define KSCC_PCI_BAR_MEMORY_TYPE_64 0x00000004UL
#define KSCC_PCI_BAR_MEMORY_ADDRESS_MASK 0xFFFFFFF0UL
#define KSCC_PCI_BAR_IO_ADDRESS_MASK 0xFFFFFFFCUL
#define KSCC_IDE_PRIMARY_TASK_PORT 0x01F0UL
#define KSCC_IDE_PRIMARY_CONTROL_PORT 0x03F6UL
#define KSCC_IDE_SECONDARY_TASK_PORT 0x0170UL
#define KSCC_IDE_SECONDARY_CONTROL_PORT 0x0376UL

typedef struct _KSCC_PORT_RESOURCE_MATCH {
    PUCHAR Address;
    ULONG DescriptorIndex;
} KSCC_PORT_RESOURCE_MATCH;

/* Decode the bus-relative address identity of one PCI memory BAR. */
static BOOLEAN
KsccDecodeMemoryBar(
    _In_ const KSCC_DEVICE_CONTEXT* Context,
    _In_ ULONG BarIndex,
    _Out_ ULONGLONG* Address)
{
    ULONG low;
    ULONG memoryType;

    if (BarIndex >= KSCC_MAX_BARS || Address == NULL) {
        return FALSE;
    }
    low = Context->PciBarValues[BarIndex];
    if (low == 0U || low == MAXULONG ||
        (low & KSCC_PCI_BAR_IO_SPACE) != 0U) {
        return FALSE;
    }
    memoryType = low & KSCC_PCI_BAR_MEMORY_TYPE_MASK;
    if (memoryType == KSCC_PCI_BAR_MEMORY_TYPE_64) {
        if (BarIndex + 1U >= KSCC_MAX_BARS) {
            return FALSE;
        }
        *Address =
            ((ULONGLONG)Context->PciBarValues[BarIndex + 1U] << 32U) |
            (ULONGLONG)(low & KSCC_PCI_BAR_MEMORY_ADDRESS_MASK);
    } else if (memoryType == 0U) {
        *Address = (ULONGLONG)(low & KSCC_PCI_BAR_MEMORY_ADDRESS_MASK);
    } else {
        return FALSE;
    }
    return *Address != 0ULL && *Address != MAXULONGLONG;
}

/* Decode the bus-relative base of a PCI I/O BAR. */
static BOOLEAN
KsccDecodeIoBar(
    _In_ const KSCC_DEVICE_CONTEXT* Context,
    _In_ ULONG BarIndex,
    _Out_ ULONG* Address)
{
    ULONG value;

    if (BarIndex >= KSCC_MAX_BARS || Address == NULL) {
        return FALSE;
    }
    value = Context->PciBarValues[BarIndex];
    if (value == MAXULONG || (value & KSCC_PCI_BAR_IO_SPACE) == 0U) {
        return FALSE;
    }
    *Address = value & KSCC_PCI_BAR_IO_ADDRESS_MASK;
    return *Address != 0U;
}

/*
 * Match an exact PCI BAR identity in the raw list and map only the
 * corresponding translated descriptor.  Length/order heuristics are
 * deliberately not used.
 */
static NTSTATUS
KsccMapRegisterBar(
    _Inout_ KSCC_DEVICE_CONTEXT* Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    ULONGLONG expectedAddress;
    ULONG barIndex;
    ULONG rawCount;
    ULONG translatedCount;
    ULONG descriptorIndex;
    PHYSICAL_ADDRESS translatedStart;
    ULONG translatedLength;
    BOOLEAN found;

    barIndex = Context->ControllerType ==
        KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI
        ? 5U
        : 0U;
    if (!KsccDecodeMemoryBar(Context, barIndex, &expectedAddress)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    rawCount = WdfCmResourceListGetCount(ResourcesRaw);
    translatedCount = WdfCmResourceListGetCount(ResourcesTranslated);
    if (rawCount != translatedCount) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    RtlZeroMemory(&translatedStart, sizeof(translatedStart));
    translatedLength = 0U;
    found = FALSE;
    for (descriptorIndex = 0U;
         descriptorIndex < rawCount;
         ++descriptorIndex) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDescriptor;
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translatedDescriptor;

        rawDescriptor =
            WdfCmResourceListGetDescriptor(ResourcesRaw, descriptorIndex);
        translatedDescriptor =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, descriptorIndex);
        if (rawDescriptor == NULL || translatedDescriptor == NULL) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (rawDescriptor->Type != CmResourceTypeMemory ||
            (ULONGLONG)rawDescriptor->u.Memory.Start.QuadPart !=
                expectedAddress) {
            continue;
        }
        if (found ||
            translatedDescriptor->Type != CmResourceTypeMemory ||
            rawDescriptor->u.Memory.Length == 0U ||
            rawDescriptor->u.Memory.Length !=
                translatedDescriptor->u.Memory.Length) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        found = TRUE;
        translatedStart = translatedDescriptor->u.Memory.Start;
        translatedLength = translatedDescriptor->u.Memory.Length;
    }
    if (!found) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Context->Bars[0] = MmMapIoSpaceEx(
        translatedStart,
        translatedLength,
        PAGE_READWRITE | PAGE_NOCACHE);
    if (Context->Bars[0] == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Context->BarPhysical[0] = translatedStart;
    Context->BarLengths[0] = translatedLength;
    Context->BarCount = 1U;
    Context->RegisterBarIndex = 0U;
    return STATUS_SUCCESS;
}

/*
 * Locate one expected raw I/O-port interval and return the corresponding
 * translated port.  Multiple matches or a memory-space port descriptor are
 * rejected instead of resolved by resource-list order.
 */
static NTSTATUS
KsccFindPortResource(
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated,
    _In_ ULONG ExpectedPort,
    _In_ ULONG RequiredLength,
    _Out_ KSCC_PORT_RESOURCE_MATCH* Match)
{
    ULONG count;
    ULONG index;
    BOOLEAN found;

    if (Match == NULL || RequiredLength == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    count = WdfCmResourceListGetCount(ResourcesRaw);
    if (count != WdfCmResourceListGetCount(ResourcesTranslated)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(Match, sizeof(*Match));
    Match->DescriptorIndex = MAXULONG;
    found = FALSE;
    for (index = 0U; index < count; ++index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDescriptor;
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translatedDescriptor;
        ULONG rawStart;
        ULONG offset;
        ULONG translatedStart;

        rawDescriptor = WdfCmResourceListGetDescriptor(ResourcesRaw, index);
        translatedDescriptor =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, index);
        if (rawDescriptor == NULL || translatedDescriptor == NULL) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (rawDescriptor->Type != CmResourceTypePort ||
            rawDescriptor->u.Port.Start.HighPart != 0 ||
            rawDescriptor->u.Port.Length < RequiredLength) {
            continue;
        }
        rawStart = rawDescriptor->u.Port.Start.LowPart;
        if (ExpectedPort < rawStart) {
            continue;
        }
        offset = ExpectedPort - rawStart;
        if (offset > rawDescriptor->u.Port.Length - RequiredLength) {
            continue;
        }
        if ((rawDescriptor->Flags & CM_RESOURCE_PORT_IO) == 0U ||
            translatedDescriptor->Type != CmResourceTypePort ||
            (translatedDescriptor->Flags & CM_RESOURCE_PORT_IO) == 0U ||
            translatedDescriptor->u.Port.Start.HighPart != 0 ||
            translatedDescriptor->u.Port.Length !=
                rawDescriptor->u.Port.Length) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (found) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        translatedStart = translatedDescriptor->u.Port.Start.LowPart;
        if (translatedStart > MAXULONG - offset) {
            return STATUS_INTEGER_OVERFLOW;
        }
        Match->Address =
            (PUCHAR)(ULONG_PTR)(translatedStart + offset);
        Match->DescriptorIndex = index;
        found = TRUE;
    }
    return found ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

/*
 * Canonicalize exactly one provable IDE channel into task-file index 0 and
 * device-control index 1.  Native channels use their PCI BAR pairs; legacy
 * channels use the architected fixed ports.  Partial or multiple pairs fail
 * closed.
 */
static NTSTATUS
KsccSelectIdePortPair(
    _Inout_ KSCC_DEVICE_CONTEXT* Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    KSCC_PORT_RESOURCE_MATCH selectedTask;
    KSCC_PORT_RESOURCE_MATCH selectedControl;
    ULONG selectedCount;
    ULONG channel;

    RtlZeroMemory(&selectedTask, sizeof(selectedTask));
    RtlZeroMemory(&selectedControl, sizeof(selectedControl));
    selectedCount = 0U;
    for (channel = 0U; channel < 2U; ++channel) {
        KSCC_PORT_RESOURCE_MATCH task;
        KSCC_PORT_RESOURCE_MATCH control;
        ULONG taskPort;
        ULONG controlPort;
        ULONG controlBase;
        ULONG taskBar;
        BOOLEAN nativeMode;
        NTSTATUS taskStatus;
        NTSTATUS controlStatus;

        taskBar = channel * 2U;
        nativeMode = (Context->PciProgIf &
            (channel == 0U ? 0x01U : 0x04U)) != 0U;
        if (nativeMode) {
            if (!KsccDecodeIoBar(Context, taskBar, &taskPort) ||
                !KsccDecodeIoBar(Context, taskBar + 1U, &controlBase) ||
                controlBase > MAXULONG - 2U) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            controlPort = controlBase + 2U;
        } else if (channel == 0U) {
            taskPort = KSCC_IDE_PRIMARY_TASK_PORT;
            controlPort = KSCC_IDE_PRIMARY_CONTROL_PORT;
        } else {
            taskPort = KSCC_IDE_SECONDARY_TASK_PORT;
            controlPort = KSCC_IDE_SECONDARY_CONTROL_PORT;
        }

        taskStatus = KsccFindPortResource(
            ResourcesRaw,
            ResourcesTranslated,
            taskPort,
            8U,
            &task);
        controlStatus = KsccFindPortResource(
            ResourcesRaw,
            ResourcesTranslated,
            controlPort,
            1U,
            &control);
        if ((taskStatus == STATUS_NOT_FOUND) !=
            (controlStatus == STATUS_NOT_FOUND)) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (taskStatus == STATUS_NOT_FOUND) {
            continue;
        }
        if (!NT_SUCCESS(taskStatus) || !NT_SUCCESS(controlStatus) ||
            task.DescriptorIndex == control.DescriptorIndex ||
            selectedCount != 0U) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        selectedTask = task;
        selectedControl = control;
        selectedCount = 1U;
    }
    if (selectedCount != 1U) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Context->IoPorts[0] = selectedTask.Address;
    Context->IoPortLengths[0] = 8U;
    Context->IoPorts[1] = selectedControl.Address;
    Context->IoPortLengths[1] = 1U;
    Context->IoPortCount = 2U;
    return STATUS_SUCCESS;
}

/* Persist an unverified stop and prevent KMDF from automatically restarting it. */
static VOID
KsccMarkStopUnverified(
    _Inout_ KSCC_DEVICE_CONTEXT* Context,
    _In_ NTSTATUS Status)
{
    Context->Prepared = FALSE;
    Context->RequiresReset = TRUE;
    Context->RiskFlags |=
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_NO_RECOVERY_GUARANTEE;
    if (Status == STATUS_IO_TIMEOUT) {
        Context->RiskFlags |= KSWORD_ARK_STORAGE_CONTROLLER_RISK_TIMEOUT;
    }
    Context->LastStatus = Status;
    WdfDeviceSetFailed(Context->Device, WdfDeviceFailedNoRestart);
}

/* Invalidate all session-bound recovery state after a verified stop. */
VOID
KsccClearSessionState(
    _Inout_ KSCC_DEVICE_CONTEXT* Context)
{
    Context->Acquired = FALSE;
    Context->SessionId = 0ULL;
    if (Context->Rollback != NULL) {
        RtlSecureZeroMemory(Context->Rollback, KSCC_BACKUP_BYTES);
    }
    Context->RollbackValid = FALSE;
    Context->RollbackLength = 0U;
    Context->RollbackOffset = 0ULL;
    Context->RollbackSessionId = 0ULL;
    RtlZeroMemory(
        Context->RollbackBeforeHash,
        sizeof(Context->RollbackBeforeHash));
    RtlZeroMemory(
        Context->RollbackAfterHash,
        sizeof(Context->RollbackAfterHash));
}

/* Keep zero reserved for an unavailable generation. */
VOID
KsccAdvanceGeneration(
    _Inout_ KSCC_DEVICE_CONTEXT* Context)
{
    Context->Generation += 1U;
    if (Context->Generation == 0U) {
        Context->Generation = 1U;
    }
}

/* ATA command FIS/task-files contain only 48 LBA bits. */
NTSTATUS
KsccValidateAtaGeometry(
    _In_ ULONG LogicalSectorSize,
    _In_ ULONG PhysicalSectorSize,
    _In_ ULONGLONG Sectors,
    _In_ ULONG MaximumTransferBytes)
{
    if (LogicalSectorSize < 512U ||
        LogicalSectorSize > MaximumTransferBytes ||
        (LogicalSectorSize & (LogicalSectorSize - 1U)) != 0U ||
        PhysicalSectorSize < LogicalSectorSize ||
        (PhysicalSectorSize & (PhysicalSectorSize - 1U)) != 0U ||
        Sectors == 0ULL || Sectors > (1ULL << 48U)) {
        return STATUS_DEVICE_DATA_ERROR;
    }
    if (Sectors > MAXULONGLONG / LogicalSectorSize) {
        return STATUS_INTEGER_OVERFLOW;
    }
    return STATUS_SUCCESS;
}

/* Publish removal before D0Exit; removed hardware must never be polled. */
VOID
KsccEvtSurpriseRemoval(
    _In_ WDFDEVICE Device)
{
    KSCC_DEVICE_CONTEXT* context;

    context = KsccGetContext(Device);
    InterlockedExchange(&context->SurpriseRemoved, TRUE);
}

/*
 * Read the PnP compatible-ID MULTI_SZ and accept only recognized storage class
 * triples.  This completes before any MMIO/port read or write is attempted.
 */
NTSTATUS
KsccDetermineControllerType(
    _Inout_ KSCC_DEVICE_CONTEXT* Context)
{
    ULONG requiredBytes;
    WCHAR* identifiers;
    NTSTATUS status;
    const WCHAR* cursor;
    const WCHAR* identifiersEnd;
    UNICODE_STRING nvmePrefix;
    UNICODE_STRING ahciPrefix;
    UNICODE_STRING idePrefix;

    requiredBytes = 0U;
    status = WdfDeviceQueryProperty(
        Context->Device,
        DevicePropertyCompatibleIDs,
        0U,
        NULL,
        &requiredBytes);
    if (status != STATUS_BUFFER_TOO_SMALL || requiredBytes < sizeof(WCHAR) * 2U) {
        return NT_SUCCESS(status) ? STATUS_NOT_SUPPORTED : status;
    }
    identifiers = KsccAllocatePool(requiredBytes);
    if (identifiers == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = WdfDeviceQueryProperty(
        Context->Device,
        DevicePropertyCompatibleIDs,
        requiredBytes,
        identifiers,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(identifiers, KSCC_POOL_TAG);
        return status;
    }

    status = STATUS_NOT_SUPPORTED;
    RtlInitUnicodeString(&nvmePrefix, L"PCI\\CC_010802");
    RtlInitUnicodeString(&ahciPrefix, L"PCI\\CC_0106");
    RtlInitUnicodeString(&idePrefix, L"PCI\\CC_0101");
    cursor = identifiers;
    identifiersEnd =
        (const WCHAR*)((const UCHAR*)identifiers + requiredBytes);
    while (cursor < identifiersEnd && *cursor != L'\0') {
        UNICODE_STRING identifier;
        SIZE_T characterCount;
        const WCHAR* terminator;

        terminator = cursor;
        while (terminator < identifiersEnd && *terminator != L'\0') {
            terminator += 1;
        }
        if (terminator == identifiersEnd) {
            status = STATUS_DEVICE_DATA_ERROR;
            break;
        }
        characterCount = (SIZE_T)(terminator - cursor);
        if (characterCount > (MAXUSHORT / sizeof(WCHAR))) {
            status = STATUS_NAME_TOO_LONG;
            break;
        }
        identifier.Buffer = (PWSTR)cursor;
        identifier.Length = (USHORT)(characterCount * sizeof(WCHAR));
        identifier.MaximumLength = identifier.Length;
        if (RtlPrefixUnicodeString(&nvmePrefix, &identifier, TRUE)) {
            Context->ControllerType = KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME;
            status = STATUS_SUCCESS;
            break;
        }
        if (RtlPrefixUnicodeString(&ahciPrefix, &identifier, TRUE)) {
            Context->ControllerType = KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI;
            status = STATUS_SUCCESS;
            break;
        }
        if (RtlPrefixUnicodeString(&idePrefix, &identifier, TRUE)) {
            Context->ControllerType = KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE;
            status = STATUS_SUCCESS;
            break;
        }
        cursor = terminator + 1;
    }
    ExFreePoolWithTag(identifiers, KSCC_POOL_TAG);
    return status;
}

/*
 * Confirm the compatible-ID decision with a read-only BUS_INTERFACE_STANDARD
 * PCI configuration read.  The driver performs this before mapping or touching
 * any register resource.
 */
NTSTATUS
KsccValidatePciClass(
    _In_ KSCC_DEVICE_CONTEXT* Context)
{
    BUS_INTERFACE_STANDARD busInterface;
    PCI_COMMON_CONFIG pciConfig;
    NTSTATUS status;
    ULONG bytesRead;
    ULONG requiredBytes;
    ULONG busNumber;
    ULONG address;
    BOOLEAN matched;

    RtlZeroMemory(&busInterface, sizeof(busInterface));
    status = WdfFdoQueryForInterface(
        Context->Device,
        &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&busInterface,
        sizeof(busInterface),
        1U,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlZeroMemory(&pciConfig, sizeof(pciConfig));
    bytesRead = busInterface.GetBusData(
        busInterface.Context,
        PCI_WHICHSPACE_CONFIG,
        &pciConfig,
        0U,
        sizeof(pciConfig));
    matched = FALSE;
    if (bytesRead >= (ULONG)FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0) &&
        pciConfig.BaseClass == 0x01U) {
        if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME) {
            matched = pciConfig.SubClass == 0x08U &&
                pciConfig.ProgIf == 0x02U;
        } else if (Context->ControllerType ==
                   KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI) {
            matched = pciConfig.SubClass == 0x06U &&
                pciConfig.ProgIf == 0x01U;
        } else if (Context->ControllerType ==
                   KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE) {
            matched = pciConfig.SubClass == 0x01U;
        }
    }
    if (matched &&
        bytesRead >=
            (ULONG)FIELD_OFFSET(
                PCI_COMMON_CONFIG,
                u.type0.BaseAddresses) +
            sizeof(pciConfig.u.type0.BaseAddresses)) {
        Context->PciProgIf = pciConfig.ProgIf;
        RtlCopyMemory(
            Context->PciBarValues,
            pciConfig.u.type0.BaseAddresses,
            sizeof(Context->PciBarValues));
    } else {
        matched = FALSE;
    }
    busInterface.InterfaceDereference(busInterface.Context);
    if (!matched) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    requiredBytes = 0U;
    status = WdfDeviceQueryProperty(
        Context->Device,
        DevicePropertyBusNumber,
        sizeof(busNumber),
        &busNumber,
        &requiredBytes);
    if (NT_SUCCESS(status)) {
        Context->PciBus = busNumber;
    }
    requiredBytes = 0U;
    status = WdfDeviceQueryProperty(
        Context->Device,
        DevicePropertyAddress,
        sizeof(address),
        &address,
        &requiredBytes);
    if (NT_SUCCESS(status) && (address >> 16U) <= 31U &&
        (address & 0xFFFFU) <= 7U) {
        Context->PciDevice = address >> 16U;
        Context->PciFunction = address & 0xFFFFU;
    }
    return STATUS_SUCCESS;
}

/*
 * PrepareHardware matches PCI BAR or IDE-channel identity in the raw list,
 * then maps only the corresponding translated PnP resource.
 */
NTSTATUS
KsccEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    KSCC_DEVICE_CONTEXT* context;
    WDF_DMA_ENABLER_CONFIG dmaConfig;
    WDF_DMA_PROFILE dmaProfile;
    NTSTATUS status;

    context = KsccGetContext(Device);
    context->ResourcesPrepared = FALSE;
    context->Prepared = FALSE;
    status = KsccValidatePciClass(context);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    context->BarCount = 0U;
    context->RegisterBarIndex = 0U;
    context->IoPortCount = 0U;
    RtlZeroMemory(context->Bars, sizeof(context->Bars));
    RtlZeroMemory(context->BarLengths, sizeof(context->BarLengths));
    RtlZeroMemory(context->BarPhysical, sizeof(context->BarPhysical));
    RtlZeroMemory(context->IoPorts, sizeof(context->IoPorts));
    RtlZeroMemory(context->IoPortLengths, sizeof(context->IoPortLengths));

    status = context->ControllerType ==
        KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE
        ? KsccSelectIdePortPair(context, ResourcesRaw, ResourcesTranslated)
        : KsccMapRegisterBar(context, ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(status)) {
        KsccEvtReleaseHardware(Device, ResourcesTranslated);
        return status;
    }

    if (context->ControllerType != KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE) {
        /*
         * Page alignment satisfies NVMe queue/PRP alignment and is stricter
         * than AHCI CLB=1K, FB=256, and CTBA=128 requirements.
         */
        WdfDeviceSetAlignmentRequirement(Device, PAGE_SIZE - 1U);
        /*
         * NVMe requires 64-bit PRPs.  AHCI uses a 32-bit profile so common
         * buffers also work on HBAs that clear CAP.S64A; a 64-bit-capable HBA
         * can consume those addresses without a different allocation path.
         */
        dmaProfile = context->ControllerType ==
            KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME
            ? WdfDmaProfileScatterGather64Duplex
            : WdfDmaProfileScatterGatherDuplex;
        WDF_DMA_ENABLER_CONFIG_INIT(
            &dmaConfig,
            dmaProfile,
            KSWORD_ARK_STORAGE_CONTROLLER_MAX_TRANSFER_BYTES);
        status = WdfDmaEnablerCreate(
            Device,
            &dmaConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &context->DmaEnabler);
        if (!NT_SUCCESS(status)) {
            KsccEvtReleaseHardware(Device, ResourcesTranslated);
            return status;
        }
    }

    /*
     * Hardware activation belongs to D0Entry.  PrepareHardware retains only
     * translated resources and the DMA enabler across D-state transitions.
     */
    context->ResourcesPrepared = TRUE;
    return STATUS_SUCCESS;
}

/*
 * Enter D0 by rebuilding only the queues and task-file state owned by this
 * function driver.  Prepared becomes visible only after full initialization.
 */
NTSTATUS
KsccEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    KSCC_DEVICE_CONTEXT* context;
    NTSTATUS status;
    NTSTATUS stopStatus;

    UNREFERENCED_PARAMETER(PreviousState);
    context = KsccGetContext(Device);
    WdfWaitLockAcquire(context->IoLock, NULL);
    if (!context->ResourcesPrepared || context->SurpriseRemoved) {
        WdfWaitLockRelease(context->IoLock);
        return STATUS_DEVICE_NOT_READY;
    }
    if (context->HardwareActivated) {
        WdfWaitLockRelease(context->IoLock);
        return STATUS_DEVICE_BUSY;
    }
    KsccResetIdentity(context);
    status = KsccDetectAndInitialize(context);
    if (NT_SUCCESS(status)) {
        context->Prepared = TRUE;
        context->Ownership = KSWORD_ARK_STORAGE_OWNERSHIP_EXCLUSIVE;
        context->Coherency = KSWORD_ARK_STORAGE_COHERENCY_EXCLUSIVE;
        context->Capabilities |=
            KSWORD_ARK_STORAGE_CONTROLLER_CAP_EXCLUSIVE |
            KSWORD_ARK_STORAGE_CONTROLLER_CAP_AUDIT |
            KSWORD_ARK_STORAGE_CONTROLLER_CAP_WRITE_VERIFY |
            KSWORD_ARK_STORAGE_CONTROLLER_CAP_ROLLBACK;
        KsccAdvanceGeneration(context);
    } else {
        context->Prepared = FALSE;
        context->RequiresReset = TRUE;
        if (context->HardwareActivated) {
            stopStatus = KsccStopController(context);
            if (!NT_SUCCESS(stopStatus)) {
                KsccMarkStopUnverified(context, stopStatus);
                status = stopStatus;
            }
        }
    }
    context->LastStatus = status;
    WdfWaitLockRelease(context->IoLock);
    return status;
}

/*
 * Leave D0 only after the backend proves that it no longer fetches queue or
 * data memory.  A failed stop retains all DMA objects and BAR mappings.
 */
NTSTATUS
KsccEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    KSCC_DEVICE_CONTEXT* context;
    NTSTATUS status;

    /*
     * WdfPowerDevicePrepareForHibernation is a framework pseudo-state: the
     * system can still use this device while writing the S4 image.  KMDF
     * explicitly requires that D0Exit not shut it down in this transition.
     * Preserve Prepared, queue ownership, session, rollback, and generation.
     * Real Dx/D3Final targets below continue through the normal stop path.
     */
    if (TargetState == WdfPowerDevicePrepareForHibernation) {
        return STATUS_SUCCESS;
    }
    context = KsccGetContext(Device);
    WdfWaitLockAcquire(context->IoLock, NULL);
    context->Prepared = FALSE;
    status = context->HardwareActivated && !context->SurpriseRemoved
        ? KsccStopController(context)
        : STATUS_SUCCESS;
    if (context->SurpriseRemoved) {
        context->HardwareActivated = FALSE;
    }
    if (!NT_SUCCESS(status)) {
        /*
         * Keep the session and original-byte snapshot intact.  They describe
         * the only known recovery point if an operator inspects the failed
         * device before removal.
         */
        KsccMarkStopUnverified(context, status);
    } else {
        KsccClearSessionState(context);
        KsccAdvanceGeneration(context);
    }
    context->LastStatus = status;
    WdfWaitLockRelease(context->IoLock);
    return status;
}

/*
 * Release only framework and mapping resources.  The device is already out of
 * D0, so this callback must not read or write controller registers.
 */
NTSTATUS
KsccEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    KSCC_DEVICE_CONTEXT* context;
    ULONG index;
    BOOLEAN stopUnverified;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    context = KsccGetContext(Device);
    context->Prepared = FALSE;
    /*
     * KMDF calls ReleaseHardware after the device has been powered off; this
     * boundary (or physical surprise removal) permits DMA resource release
     * even when the previous powered-on stop could not be verified.
     * Never touch controller registers here.  HardwareActivated remaining set
     * means D0Exit (or failed D0 initialization) could not prove a stop.
     */
    /* Surprise removal also lacks proof that the controller stopped DMA. */
    stopUnverified = context->HardwareActivated ||
        InterlockedCompareExchange(&context->SurpriseRemoved, 0L, 0L) != 0L;
    if (stopUnverified) {
        NTSTATUS failureStatus;

        failureStatus = NT_SUCCESS(context->LastStatus)
            ? STATUS_DEVICE_HARDWARE_ERROR
            : context->LastStatus;
        KsccMarkStopUnverified(context, failureStatus);
        /*
         * Never free a queue, common buffer, or BAR while hardware stop is
         * unproven.  The failed FDO remains marked NoRestart and this callback
         * returns the failure so KMDF cannot complete a normal release path;
         * a later PnP recovery/reset must establish quiescence before cleanup.
         */
        context->ResourcesPrepared = TRUE;
        return failureStatus;
    } else {
        KsccClearSessionState(context);
    }
    KsccFreeDmaRegion(&context->Auxiliary);
    KsccFreeDmaRegion(&context->Prp);
    KsccFreeDmaRegion(&context->Data);
    KsccFreeDmaRegion(&context->Completion);
    KsccFreeDmaRegion(&context->Command);
    if (context->DmaEnabler != WDF_NO_HANDLE) {
        WdfObjectDelete(context->DmaEnabler);
        context->DmaEnabler = WDF_NO_HANDLE;
    }
    for (index = 0U; index < context->BarCount; ++index) {
        if (context->Bars[index] != NULL) {
            MmUnmapIoSpace(context->Bars[index], context->BarLengths[index]);
            context->Bars[index] = NULL;
        }
        context->BarLengths[index] = 0U;
    }
    context->BarCount = 0U;
    RtlZeroMemory(context->Bars, sizeof(context->Bars));
    RtlZeroMemory(context->BarLengths, sizeof(context->BarLengths));
    RtlZeroMemory(context->BarPhysical, sizeof(context->BarPhysical));
    context->IoPortCount = 0U;
    RtlZeroMemory(context->IoPorts, sizeof(context->IoPorts));
    RtlZeroMemory(context->IoPortLengths, sizeof(context->IoPortLengths));
    context->ResourcesPrepared = FALSE;
    context->Prepared = FALSE;
    context->HardwareActivated = FALSE;
    if (!stopUnverified) {
        context->RequiresReset = FALSE;
    }
    context->WriteObserved = FALSE;
    context->Capabilities = 0U;
    context->Ownership = KSWORD_ARK_STORAGE_OWNERSHIP_NONE;
    context->Coherency = KSWORD_ARK_STORAGE_COHERENCY_UNKNOWN;
    context->PortOrNamespace = MAXULONG;
    context->IdeControlPortIndex = MAXULONG;
    context->LogicalSectorSize = 0U;
    context->PhysicalSectorSize = 0U;
    context->CapacityBytes = 0ULL;
    return STATUS_SUCCESS;
}

/*
 * Common buffers are created by the DMA enabler attached to this function
 * device, preserving the framework's logical-address constraints.
 */
NTSTATUS
KsccAllocateDmaRegion(
    _In_ KSCC_DEVICE_CONTEXT* Context,
    _Inout_ KSCC_DMA_REGION* Region,
    _In_ SIZE_T Length)
{
    NTSTATUS status;
    WDF_COMMON_BUFFER_CONFIG bufferConfig;

    if (Region->Object != WDF_NO_HANDLE) {
        if (Region->Virtual == NULL || Region->Length < Length) {
            return STATUS_INVALID_BUFFER_SIZE;
        }
        RtlZeroMemory(Region->Virtual, Region->Length);
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(Region, sizeof(*Region));
    Region->Length = Length;
    WDF_COMMON_BUFFER_CONFIG_INIT(
        &bufferConfig,
        PAGE_SIZE - 1U);
    status = WdfCommonBufferCreateWithConfig(
        Context->DmaEnabler,
        Length,
        &bufferConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &Region->Object);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(Region, sizeof(*Region));
        return status;
    }
    Region->Virtual = WdfCommonBufferGetAlignedVirtualAddress(Region->Object);
    Region->Logical = WdfCommonBufferGetAlignedLogicalAddress(Region->Object);
    if ((((ULONG_PTR)Region->Virtual) & (PAGE_SIZE - 1U)) != 0U ||
        (((ULONGLONG)Region->Logical.QuadPart) & (PAGE_SIZE - 1U)) != 0ULL) {
        WdfObjectDelete(Region->Object);
        RtlZeroMemory(Region, sizeof(*Region));
        return STATUS_DATATYPE_MISALIGNMENT;
    }
    RtlZeroMemory(Region->Virtual, Region->Length);
    return STATUS_SUCCESS;
}

/* Delete a WDF common-buffer object and clear all stale addresses. */
VOID
KsccFreeDmaRegion(
    _Inout_ KSCC_DMA_REGION* Region)
{
    if (Region->Object != WDF_NO_HANDLE) {
        WdfObjectDelete(Region->Object);
    }
    RtlZeroMemory(Region, sizeof(*Region));
}

/* Reset user-visible media identity without altering PnP resource ownership. */
VOID
KsccResetIdentity(
    _Inout_ KSCC_DEVICE_CONTEXT* Context)
{
    /* Retain the PnP-confirmed controller type across resource stop/start. */
    Context->Capabilities = 0U;
    Context->RiskFlags =
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_SYSTEM_DISK_UNKNOWN |
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_LIVE_VOLUMES_UNKNOWN |
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_NO_RECOVERY_GUARANTEE;
    Context->Ownership = KSWORD_ARK_STORAGE_OWNERSHIP_NONE;
    Context->Coherency = KSWORD_ARK_STORAGE_COHERENCY_UNKNOWN;
    Context->PortOrNamespace = MAXULONG;
    Context->IdeControlPortIndex = MAXULONG;
    Context->LogicalSectorSize = 0U;
    Context->PhysicalSectorSize = 0U;
    Context->CapacityBytes = 0ULL;
    Context->MaximumTransferBytes =
        KSWORD_ARK_STORAGE_CONTROLLER_MAX_TRANSFER_BYTES;
    Context->RollbackValid = FALSE;
    Context->RollbackSessionId = 0ULL;
    Context->RequiresReset = FALSE;
    Context->WriteObserved = FALSE;
    RtlZeroMemory(Context->Model, sizeof(Context->Model));
    RtlZeroMemory(Context->Serial, sizeof(Context->Serial));
    RtlZeroMemory(Context->Detail, sizeof(Context->Detail));
}

/*
 * Detection uses register signatures only after PnP has assigned a BAR.  AHCI
 * exposes GHC/PI, while NVMe exposes CAP/VS; IDE uses a separate I/O-resource
 * path and therefore is rejected here when no legacy resource was translated.
 */
NTSTATUS
KsccDetectAndInitialize(
    _Inout_ KSCC_DEVICE_CONTEXT* Context)
{
    if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME &&
        Context->BarCount == 1U &&
        Context->Bars[0] != NULL) {
        Context->RegisterBarIndex = 0U;
        return KsccNvmeInitialize(Context);
    }
    if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI &&
        Context->BarCount == 1U &&
        Context->Bars[0] != NULL) {
        Context->RegisterBarIndex = 0U;
        return KsccAhciInitialize(Context);
    }
    if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE &&
        Context->IoPortCount == 2U) {
        return KsccIdeInitialize(Context);
    }
    return STATUS_NOT_SUPPORTED;
}

/* Dispatch a transfer only to the backend that successfully initialized. */
NTSTATUS
KsccHardwareTransfer(
    _Inout_ KSCC_DEVICE_CONTEXT* Context,
    _In_ BOOLEAN Write,
    _In_ ULONGLONG Offset,
    _Inout_updates_bytes_(Length) UCHAR* Buffer,
    _In_ ULONG Length,
    _In_ ULONG Flags,
    _In_ ULONG TimeoutMilliseconds,
    _Out_ ULONG* ControllerStatus)
{
    NTSTATUS status;

    if (Context->SurpriseRemoved) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI) {
        status = KsccAhciTransfer(
            Context,
            Write,
            Offset,
            Buffer,
            Length,
            Flags,
            TimeoutMilliseconds,
            ControllerStatus);
    } else if (Context->ControllerType ==
               KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME) {
        status = KsccNvmeTransfer(
            Context,
            Write,
            Offset,
            Buffer,
            Length,
            Flags,
            TimeoutMilliseconds,
            ControllerStatus);
    } else if (Context->ControllerType ==
               KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE) {
        status = KsccIdeTransfer(
            Context,
            Write,
            Offset,
            Buffer,
            Length,
            Flags,
            TimeoutMilliseconds,
            ControllerStatus);
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    if (Write) {
        Context->WriteObserved = TRUE;
    }
    if (status == STATUS_IO_TIMEOUT) {
        Context->RequiresReset = TRUE;
        Context->RiskFlags |= KSWORD_ARK_STORAGE_CONTROLLER_RISK_TIMEOUT;
    }
    Context->LastControllerStatus = *ControllerStatus;
    Context->LastStatus = status;
    return status;
}

/* Dispatch a durable-cache flush through the selected backend. */
NTSTATUS
KsccHardwareFlush(
    _Inout_ KSCC_DEVICE_CONTEXT* Context,
    _In_ ULONG TimeoutMilliseconds,
    _Out_ ULONG* ControllerStatus)
{
    NTSTATUS status;

    if (Context->SurpriseRemoved) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI) {
        status = KsccAhciFlush(Context, TimeoutMilliseconds, ControllerStatus);
    } else if (Context->ControllerType ==
               KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME) {
        status = KsccNvmeFlush(Context, TimeoutMilliseconds, ControllerStatus);
    } else if (Context->ControllerType ==
               KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE) {
        status = KsccIdeFlush(Context, TimeoutMilliseconds, ControllerStatus);
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    if (status == STATUS_IO_TIMEOUT) {
        Context->RequiresReset = TRUE;
        Context->RiskFlags |= KSWORD_ARK_STORAGE_CONTROLLER_RISK_TIMEOUT;
    }
    Context->LastControllerStatus = *ControllerStatus;
    Context->LastStatus = status;
    return status;
}

/*
 * Stop only the backend initialized for this PnP-owned controller.  Success
 * means the backend proved that it can no longer fetch owned DMA memory.
 */
NTSTATUS
KsccStopController(
    _Inout_ KSCC_DEVICE_CONTEXT* Context)
{
    NTSTATUS stopStatus;

    if (!Context->HardwareActivated || Context->SurpriseRemoved) {
        return STATUS_SUCCESS;
    }
    if (Context->WriteObserved && !Context->RequiresReset) {
        ULONG controllerStatus;
        NTSTATUS flushStatus;

        controllerStatus = 0U;
        flushStatus = KsccHardwareFlush(Context, 5000U, &controllerStatus);
        if (!NT_SUCCESS(flushStatus)) {
            Context->RiskFlags |=
                KSWORD_ARK_STORAGE_CONTROLLER_RISK_NO_RECOVERY_GUARANTEE;
        }
    }
    if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_AHCI) {
        stopStatus = KsccAhciStop(Context);
    } else if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME) {
        stopStatus = KsccNvmeStop(Context);
    } else if (Context->ControllerType == KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE) {
        stopStatus = KsccIdeStop(Context);
    } else {
        stopStatus = STATUS_NOT_SUPPORTED;
    }
    if (!NT_SUCCESS(stopStatus)) {
        Context->RequiresReset = TRUE;
        Context->RiskFlags |=
            KSWORD_ARK_STORAGE_CONTROLLER_RISK_NO_RECOVERY_GUARANTEE;
        Context->LastStatus = stopStatus;
        return stopStatus;
    }
    Context->HardwareActivated = FALSE;
    Context->WriteObserved = FALSE;
    Context->LastStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}
