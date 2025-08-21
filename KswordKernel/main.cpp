#include <ntddk.h>
#define EXTERN_C
extern "C"
NTSTATUS DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    KdPrint(("Hello World\n"));
    return STATUS_SUCCESS;
}
