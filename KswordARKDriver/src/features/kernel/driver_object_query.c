/*++

Module Name:

    driver_object_query.c

Abstract:

    Phase-9 DriverObject / DeviceObject diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntstrsafe.h>

#define KSW_DRIVER_OBJECT_QUERY_TAG 'oDsK'

// ObReferenceObjectByName 是内核未公开但常用的命名对象引用入口：
// - System Informer 的 KphOpenDriver 最终也围绕 DriverObject type 做引用/打开；
// - 这里仅使用名称引用，不允许用户传入任意内核地址。
NTSYSAPI
NTSTATUS
NTAPI
ObReferenceObjectByName(
    _In_ PUNICODE_STRING ObjectName,
    _In_ ULONG Attributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Inout_opt_ PVOID ParseContext,
    _Out_ PVOID* Object
    );

extern POBJECT_TYPE* IoDriverObjectType;

typedef struct _KSW_SYSTEM_MODULE_ENTRY
{
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} KSW_SYSTEM_MODULE_ENTRY, *PKSW_SYSTEM_MODULE_ENTRY;

typedef struct _KSW_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSW_SYSTEM_MODULE_ENTRY Modules[1];
} KSW_SYSTEM_MODULE_INFORMATION, *PKSW_SYSTEM_MODULE_INFORMATION;

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

NTSYSAPI
NTSTATUS
NTAPI
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

static PVOID
KswordARKDriverObjectAllocate(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    Allocate transient nonpaged memory for module snapshots.

Arguments:

    BufferBytes - Number of bytes to allocate.

Return Value:

    Allocation pointer or NULL.

--*/
{
    if (BufferBytes == 0U) {
        return NULL;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSW_DRIVER_OBJECT_QUERY_TAG);
#pragma warning(pop)
}

static VOID
KswordARKCopyUnicodeStringToFixed(
    _In_opt_ PCUNICODE_STRING SourceString,
    _Out_writes_(DestinationChars) PWCHAR DestinationText,
    _In_ ULONG DestinationChars
    )
/*++

Routine Description:

    Copy a UNICODE_STRING into a fixed WCHAR buffer with guaranteed NUL.

Arguments:

    SourceString - Source text, may be NULL.
    DestinationText - Fixed output buffer.
    DestinationChars - Output character capacity.

Return Value:

    None.

--*/
{
    ULONG copyChars = 0UL;

    if (DestinationText == NULL || DestinationChars == 0UL) {
        return;
    }

    DestinationText[0] = L'\0';
    if (SourceString == NULL || SourceString->Buffer == NULL || SourceString->Length == 0) {
        return;
    }

    copyChars = (ULONG)(SourceString->Length / sizeof(WCHAR));
    if (copyChars >= DestinationChars) {
        copyChars = DestinationChars - 1UL;
    }

    RtlCopyMemory(DestinationText, SourceString->Buffer, copyChars * sizeof(WCHAR));
    DestinationText[copyChars] = L'\0';
}

static VOID
KswordARKCopyBoundedAnsiToWide(
    _In_reads_bytes_(SourceBytes) const UCHAR* SourceText,
    _In_ ULONG SourceBytes,
    _Out_writes_(DestinationChars) PWCHAR DestinationText,
    _In_ ULONG DestinationChars
    )
/*++

Routine Description:

    Copy an ANSI module filename to a fixed WCHAR buffer.

Arguments:

    SourceText - Bounded ANSI text.
    SourceBytes - Maximum readable bytes.
    DestinationText - Wide output buffer.
    DestinationChars - Output character capacity.

Return Value:

    None.

--*/
{
    ULONG index = 0UL;

    if (DestinationText == NULL || DestinationChars == 0UL) {
        return;
    }

    DestinationText[0] = L'\0';
    if (SourceText == NULL || SourceBytes == 0UL) {
        return;
    }

    for (index = 0UL; index + 1UL < DestinationChars && index < SourceBytes; ++index) {
        if (SourceText[index] == '\0') {
            break;
        }
        DestinationText[index] = (WCHAR)SourceText[index];
    }
    DestinationText[index] = L'\0';
}

static NTSTATUS
KswordARKBuildSystemModuleSnapshot(
    _Outptr_result_bytebuffer_(*BufferBytesOut) KSW_SYSTEM_MODULE_INFORMATION** ModuleInfoOut,
    _Out_ ULONG* BufferBytesOut
    )
/*++

Routine Description:

    Query SystemModuleInformation for dispatch address ownership checks.

Arguments:

    ModuleInfoOut - Receives allocated module information.
    BufferBytesOut - Receives allocated byte count.

Return Value:

    STATUS_SUCCESS or query/allocation failure.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    KSW_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;

    if (ModuleInfoOut == NULL || BufferBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ModuleInfoOut = NULL;
    *BufferBytesOut = 0UL;

    status = ZwQuerySystemInformation(11UL, NULL, 0UL, &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

    moduleInfo = (KSW_SYSTEM_MODULE_INFORMATION*)KswordARKDriverObjectAllocate(requiredBytes);
    if (moduleInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySystemInformation(11UL, moduleInfo, requiredBytes, &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(moduleInfo, KSW_DRIVER_OBJECT_QUERY_TAG);
        return status;
    }

    *ModuleInfoOut = moduleInfo;
    *BufferBytesOut = requiredBytes;
    return STATUS_SUCCESS;
}

static VOID
KswordARKResolveModuleForAddress(
    _In_opt_ const KSW_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ PVOID Address,
    _Out_ ULONGLONG* ModuleBaseOut,
    _Out_writes_(ModuleNameChars) PWCHAR ModuleNameOut,
    _In_ ULONG ModuleNameChars
    )
/*++

Routine Description:

    Resolve a kernel address to the loaded module that owns it.

Arguments:

    ModuleInfo - Optional system module snapshot.
    Address - Address being classified.
    ModuleBaseOut - Receives owning module base when found.
    ModuleNameOut - Receives module filename when found.
    ModuleNameChars - ModuleNameOut capacity.

Return Value:

    None.

--*/
{
    ULONG moduleIndex = 0UL;
    ULONG_PTR addressValue = (ULONG_PTR)Address;

    if (ModuleBaseOut != NULL) {
        *ModuleBaseOut = 0ULL;
    }
    if (ModuleNameOut != NULL && ModuleNameChars != 0UL) {
        ModuleNameOut[0] = L'\0';
    }
    if (ModuleInfo == NULL || Address == NULL) {
        return;
    }

    for (moduleIndex = 0UL; moduleIndex < ModuleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_SYSTEM_MODULE_ENTRY* moduleEntry = &ModuleInfo->Modules[moduleIndex];
        const ULONG_PTR imageBase = (ULONG_PTR)moduleEntry->ImageBase;
        const ULONG_PTR imageEnd = imageBase + (ULONG_PTR)moduleEntry->ImageSize;
        const UCHAR* fileName = moduleEntry->FullPathName;
        ULONG fileNameBytes = (ULONG)sizeof(moduleEntry->FullPathName);

        if (addressValue < imageBase || addressValue >= imageEnd) {
            continue;
        }

        if (moduleEntry->OffsetToFileName < sizeof(moduleEntry->FullPathName)) {
            fileName = moduleEntry->FullPathName + moduleEntry->OffsetToFileName;
            fileNameBytes = (ULONG)(sizeof(moduleEntry->FullPathName) - moduleEntry->OffsetToFileName);
        }

        if (ModuleBaseOut != NULL) {
            *ModuleBaseOut = (ULONGLONG)imageBase;
        }
        KswordARKCopyBoundedAnsiToWide(fileName, fileNameBytes, ModuleNameOut, ModuleNameChars);
        return;
    }
}

static VOID
KswordARKFillMajorFunctionRows(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ const KSW_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _Inout_ KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE* Response
    )
/*++

Routine Description:

    Copy the driver dispatch table and classify each dispatch address.

Arguments:

    DriverObject - Referenced driver object.
    ModuleInfo - Optional module snapshot.
    Response - Response header being filled.

Return Value:

    None.

--*/
{
    ULONG majorIndex = 0UL;

    if (DriverObject == NULL || Response == NULL) {
        return;
    }

    Response->majorFunctionCount = KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT;
    for (majorIndex = 0UL; majorIndex < KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT; ++majorIndex) {
        KSWORD_ARK_DRIVER_MAJOR_FUNCTION_ENTRY* row = &Response->majorFunctions[majorIndex];
        PVOID dispatchAddress = NULL;

        RtlZeroMemory(row, sizeof(*row));
        row->majorFunction = majorIndex;
        dispatchAddress = (PVOID)DriverObject->MajorFunction[majorIndex];
        row->dispatchAddress = (ULONGLONG)(ULONG_PTR)dispatchAddress;
        KswordARKResolveModuleForAddress(
            ModuleInfo,
            dispatchAddress,
            &row->moduleBase,
            row->moduleName,
            KSWORD_ARK_DRIVER_MODULE_NAME_CHARS);
        if (row->moduleBase != 0ULL) {
            row->flags |= 0x00000001UL;
        }
        if ((ULONG_PTR)dispatchAddress >= (ULONG_PTR)DriverObject->DriverStart &&
            (ULONG_PTR)dispatchAddress < ((ULONG_PTR)DriverObject->DriverStart + (ULONG_PTR)DriverObject->DriverSize)) {
            row->flags |= 0x00000002UL;
        }
    }

    Response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_MAJOR_PRESENT;
}

static NTSTATUS
KswordARKQueryObjectNameToFixed(
    _In_ PVOID Object,
    _Out_writes_(DestinationChars) PWCHAR DestinationText,
    _In_ ULONG DestinationChars
    )
/*++

Routine Description:

    Query a kernel object's name into a fixed WCHAR buffer.

Arguments:

    Object - Referenced object pointer.
    DestinationText - Fixed output text.
    DestinationChars - Output character capacity.

Return Value:

    STATUS_SUCCESS when a name was copied; otherwise query/allocation failure.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    POBJECT_NAME_INFORMATION nameInfo = NULL;

    if (DestinationText == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    DestinationText[0] = L'\0';
    if (Object == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ObQueryNameString(Object, NULL, 0UL, &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_OBJECT_NAME_NOT_FOUND : status;
    }

    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKDriverObjectAllocate(requiredBytes);
    if (nameInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ObQueryNameString(Object, nameInfo, requiredBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        KswordARKCopyUnicodeStringToFixed(&nameInfo->Name, DestinationText, DestinationChars);
    }

    ExFreePoolWithTag(nameInfo, KSW_DRIVER_OBJECT_QUERY_TAG);
    return status;
}

static VOID
KswordARKFillOneDeviceRow(
    _In_ PDEVICE_OBJECT RootDevice,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG RelationDepth,
    _Inout_ KSWORD_ARK_DRIVER_DEVICE_ENTRY* Row
    )
/*++

Routine Description:

    Copy one DeviceObject snapshot into the shared response row.

Arguments:

    RootDevice - DeviceObject from DriverObject->DeviceObject chain.
    DeviceObject - Current device or attached device.
    RelationDepth - 0 for root/NextDevice chain; >0 for AttachedDevice chain.
    Row - Output row.

Return Value:

    None.

--*/
{
    if (Row == NULL) {
        return;
    }

    RtlZeroMemory(Row, sizeof(*Row));
    if (DeviceObject == NULL) {
        return;
    }

    Row->relationDepth = RelationDepth;
    Row->deviceType = DeviceObject->DeviceType;
    Row->flags = DeviceObject->Flags;
    Row->characteristics = DeviceObject->Characteristics;
    Row->stackSize = (ULONG)(UCHAR)DeviceObject->StackSize;
    Row->alignmentRequirement = DeviceObject->AlignmentRequirement;
    Row->rootDeviceObjectAddress = (ULONGLONG)(ULONG_PTR)RootDevice;
    Row->deviceObjectAddress = (ULONGLONG)(ULONG_PTR)DeviceObject;
    Row->nextDeviceObjectAddress = (ULONGLONG)(ULONG_PTR)DeviceObject->NextDevice;
    Row->attachedDeviceObjectAddress = (ULONGLONG)(ULONG_PTR)DeviceObject->AttachedDevice;
    Row->driverObjectAddress = (ULONGLONG)(ULONG_PTR)DeviceObject->DriverObject;
    Row->nameStatus = KswordARKQueryObjectNameToFixed(
        DeviceObject,
        Row->deviceName,
        KSWORD_ARK_DRIVER_DEVICE_NAME_CHARS);
}

static VOID
KswordARKFillDeviceRows(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG MaxDevices,
    _In_ ULONG MaxAttachedDevices,
    _In_ BOOLEAN IncludeAttached,
    _Inout_ KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE* Response,
    _In_ ULONG DeviceCapacity
    )
/*++

Routine Description:

    Enumerate DriverObject->DeviceObject and optional AttachedDevice chains.

Arguments:

    DriverObject - Referenced driver object.
    MaxDevices - User-requested root device limit.
    MaxAttachedDevices - User-requested attached-chain limit per root.
    IncludeAttached - Whether to include attached devices.
    Response - Response header and variable row array.
    DeviceCapacity - Number of rows fitting output buffer.

Return Value:

    None.

--*/
{
    PDEVICE_OBJECT rootDevice = NULL;
    ULONG rootVisited = 0UL;
    ULONG returnedRows = 0UL;

    if (DriverObject == NULL || Response == NULL || DeviceCapacity == 0UL) {
        return;
    }

    if (MaxDevices == 0UL || MaxDevices > KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT) {
        MaxDevices = KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT;
    }
    if (MaxAttachedDevices == 0UL || MaxAttachedDevices > KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT) {
        MaxAttachedDevices = KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT;
    }

    rootDevice = DriverObject->DeviceObject;
    while (rootDevice != NULL && rootVisited < MaxDevices) {
        PDEVICE_OBJECT attachedDevice = NULL;
        ULONG attachedDepth = 0UL;

        ++Response->totalDeviceCount;
        if (returnedRows < DeviceCapacity) {
            KswordARKFillOneDeviceRow(
                rootDevice,
                rootDevice,
                0UL,
                &Response->devices[returnedRows]);
            ++returnedRows;
        }
        else {
            Response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_TRUNCATED;
        }

        if (IncludeAttached) {
            attachedDevice = rootDevice->AttachedDevice;
            while (attachedDevice != NULL && attachedDepth < MaxAttachedDevices) {
                ++attachedDepth;
                ++Response->totalDeviceCount;
                if (returnedRows < DeviceCapacity) {
                    KswordARKFillOneDeviceRow(
                        rootDevice,
                        attachedDevice,
                        attachedDepth,
                        &Response->devices[returnedRows]);
                    ++returnedRows;
                }
                else {
                    Response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_TRUNCATED;
                    break;
                }
                attachedDevice = attachedDevice->AttachedDevice;
            }
            if (attachedDevice != NULL) {
                Response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_ATTACHED_TRUNCATED;
            }
        }

        ++rootVisited;
        rootDevice = rootDevice->NextDevice;
    }

    if (rootDevice != NULL) {
        Response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_TRUNCATED;
    }
    if (Response->totalDeviceCount != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_PRESENT;
    }
    Response->returnedDeviceCount = returnedRows;
}

static NTSTATUS
KswordARKReferenceDriverObjectByRequestName(
    _In_ const KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST* Request,
    _Outptr_ PDRIVER_OBJECT* DriverObjectOut
    )
/*++

Routine Description:

    Validate and reference a DriverObject by its object namespace name.

Arguments:

    Request - User request containing \Driver\xxx or xxx.
    DriverObjectOut - Receives referenced driver object.

Return Value:

    STATUS_SUCCESS or validation/reference failure.

--*/
{
    WCHAR objectNameBuffer[KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS + 16U] = { 0 };
    UNICODE_STRING objectName;
    ULONG inputChars = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL || DriverObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DriverObjectOut = NULL;
    while (inputChars < KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS &&
        Request->driverName[inputChars] != L'\0') {
        ++inputChars;
    }
    if (inputChars == 0UL || inputChars >= KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (inputChars >= 8UL &&
        (Request->driverName[0] == L'\\') &&
        ((Request->driverName[1] == L'D') || (Request->driverName[1] == L'd')) &&
        ((Request->driverName[2] == L'R') || (Request->driverName[2] == L'r')) &&
        ((Request->driverName[3] == L'I') || (Request->driverName[3] == L'i')) &&
        ((Request->driverName[4] == L'V') || (Request->driverName[4] == L'v')) &&
        ((Request->driverName[5] == L'E') || (Request->driverName[5] == L'e')) &&
        ((Request->driverName[6] == L'R') || (Request->driverName[6] == L'r')) &&
        Request->driverName[7] == L'\\') {
        RtlCopyMemory(objectNameBuffer, Request->driverName, inputChars * sizeof(WCHAR));
        objectNameBuffer[inputChars] = L'\0';
    }
    else {
        status = RtlStringCchPrintfW(
            objectNameBuffer,
            RTL_NUMBER_OF(objectNameBuffer),
            L"\\Driver\\%ws",
            Request->driverName);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    RtlInitUnicodeString(&objectName, objectNameBuffer);
    status = ObReferenceObjectByName(
        &objectName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        0,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)DriverObjectOut);
    return status;
}

NTSTATUS
KswordARKDriverQueryDriverObject(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_ const KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    )
/*++

Routine Description:

    Query a DriverObject by name and return DriverObject, dispatch table and
    DeviceObject chain diagnostics.

Arguments:

    outputBuffer - Shared response buffer.
    outputBufferLength - Output byte capacity.
    request - Validated R3 request.
    bytesWrittenOut - Receives bytes populated.

Return Value:

    STATUS_SUCCESS for valid response packets, or NTSTATUS for IO/buffer errors.

--*/
{
    const size_t responseHeaderSize =
        sizeof(KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE) - sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY);
    KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE* response = NULL;
    PDRIVER_OBJECT driverObject = NULL;
    KSW_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG deviceCapacity = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN includeMajor = FALSE;
    BOOLEAN includeDevices = FALSE;
    BOOLEAN includeNames = FALSE;
    BOOLEAN includeAttached = FALSE;

    if (outputBuffer == NULL || bytesWrittenOut == NULL || request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *bytesWrittenOut = 0;
    if (outputBufferLength < responseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    response = (KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE*)outputBuffer;
    RtlZeroMemory(outputBuffer, outputBufferLength);
    response->version = KSWORD_ARK_DRIVER_OBJECT_PROTOCOL_VERSION;
    response->queryStatus = KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_UNAVAILABLE;
    response->deviceEntrySize = sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY);
    response->lastStatus = STATUS_SUCCESS;

    deviceCapacity = (ULONG)((outputBufferLength - responseHeaderSize) / sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY));
    includeMajor = ((request->flags & KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_MAJOR_FUNCTIONS) != 0UL) ? TRUE : FALSE;
    includeDevices = ((request->flags & KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_DEVICES) != 0UL) ? TRUE : FALSE;
    includeNames = ((request->flags & KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_NAMES) != 0UL) ? TRUE : FALSE;
    includeAttached = ((request->flags & KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ATTACHED) != 0UL) ? TRUE : FALSE;

    status = KswordARKReferenceDriverObjectByRequestName(request, &driverObject);
    response->lastStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus =
            (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_TYPE_MISMATCH)
            ? KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_NOT_FOUND
            : KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_REFERENCE_FAILED;
        *bytesWrittenOut = responseHeaderSize;
        return STATUS_SUCCESS;
    }

    response->queryStatus = KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK;
    response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_BASIC_PRESENT;
    response->driverObjectAddress = (ULONGLONG)(ULONG_PTR)driverObject;
    response->driverFlags = driverObject->Flags;
    response->driverStart = (ULONGLONG)(ULONG_PTR)driverObject->DriverStart;
    response->driverSize = driverObject->DriverSize;
    response->driverSection = (ULONGLONG)(ULONG_PTR)driverObject->DriverSection;
    response->driverUnload = (ULONGLONG)(ULONG_PTR)driverObject->DriverUnload;
    KswordARKCopyUnicodeStringToFixed(
        &driverObject->DriverName,
        response->driverName,
        KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
    response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_DRIVER_NAME_PRESENT;

    if (driverObject->DriverExtension != NULL) {
        KswordARKCopyUnicodeStringToFixed(
            &driverObject->DriverExtension->ServiceKeyName,
            response->serviceKeyName,
            KSWORD_ARK_DRIVER_SERVICE_KEY_CHARS);
        if (response->serviceKeyName[0] != L'\0') {
            response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_SERVICE_KEY_PRESENT;
        }
    }

    if (includeNames) {
        UNICODE_STRING imagePath;
        RtlZeroMemory(&imagePath, sizeof(imagePath));
        status = IoQueryFullDriverPath(driverObject, &imagePath);
        if (NT_SUCCESS(status)) {
            KswordARKCopyUnicodeStringToFixed(
                &imagePath,
                response->imagePath,
                KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS);
            if (response->imagePath[0] != L'\0') {
                response->fieldFlags |= KSWORD_ARK_DRIVER_OBJECT_FIELD_IMAGE_PATH_PRESENT;
            }
            ExFreePool(imagePath.Buffer);
        }
        else if (response->queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK) {
            response->queryStatus = KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL;
            response->lastStatus = status;
        }
    }

    status = KswordARKBuildSystemModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    if (!NT_SUCCESS(status)) {
        moduleInfo = NULL;
    }

    if (includeMajor) {
        KswordARKFillMajorFunctionRows(driverObject, moduleInfo, response);
    }
    if (includeDevices) {
        KswordARKFillDeviceRows(
            driverObject,
            request->maxDevices,
            request->maxAttachedDevices,
            includeAttached,
            response,
            deviceCapacity);
        if ((response->fieldFlags & (KSWORD_ARK_DRIVER_OBJECT_FIELD_DEVICE_TRUNCATED | KSWORD_ARK_DRIVER_OBJECT_FIELD_ATTACHED_TRUNCATED)) != 0UL) {
            response->queryStatus = KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL;
        }
    }

    *bytesWrittenOut = responseHeaderSize +
        ((size_t)response->returnedDeviceCount * sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY));

    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_DRIVER_OBJECT_QUERY_TAG);
    }
    ObDereferenceObject(driverObject);
    return STATUS_SUCCESS;
}
