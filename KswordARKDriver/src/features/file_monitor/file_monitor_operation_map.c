/*++

Module Name:

    file_monitor_operation_map.c

Abstract:

    Shared operation mapping for KswordARK minifilter callbacks and monitor events.

Environment:

    Kernel-mode minifilter

--*/

#include "file_monitor_internal.h"

ULONG
KswordARKMinifilterMapMajorToOperation(
    _In_ UCHAR MajorFunction,
    _In_ UCHAR MinorFunction,
    _In_opt_ PFLT_PARAMETERS Parameters
    )
{
    UNREFERENCED_PARAMETER(MinorFunction);

    switch (MajorFunction) {
    case IRP_MJ_CREATE:
        return KSWORD_ARK_MINIFILTER_OP_CREATE;
    case IRP_MJ_READ:
        return KSWORD_ARK_MINIFILTER_OP_READ;
    case IRP_MJ_WRITE:
        return KSWORD_ARK_MINIFILTER_OP_WRITE;
    case IRP_MJ_CLEANUP:
        return KSWORD_ARK_MINIFILTER_OP_CLEANUP;
    case IRP_MJ_CLOSE:
        return KSWORD_ARK_MINIFILTER_OP_CLOSE;
    case IRP_MJ_SET_INFORMATION:
        if (Parameters != NULL) {
            FILE_INFORMATION_CLASS informationClass = Parameters->SetFileInformation.FileInformationClass;
            if (informationClass == FileRenameInformation ||
                informationClass == FileRenameInformationEx ||
                informationClass == FileLinkInformation ||
                informationClass == FileLinkInformationEx) {
                return KSWORD_ARK_MINIFILTER_OP_RENAME;
            }

            if (informationClass == FileDispositionInformation ||
                informationClass == FileDispositionInformationEx) {
                return KSWORD_ARK_MINIFILTER_OP_DELETE;
            }
        }
        return KSWORD_ARK_MINIFILTER_OP_SETINFO;
    default:
        return 0UL;
    }
}

ULONG
KswordARKFileMonitorMapMajorToOperation(
    _In_ UCHAR MajorFunction,
    _In_ UCHAR MinorFunction,
    _In_opt_ PFLT_PARAMETERS Parameters
    )
{
    ULONG minifilterOperation = KswordARKMinifilterMapMajorToOperation(
        MajorFunction,
        MinorFunction,
        Parameters);

    switch (minifilterOperation) {
    case KSWORD_ARK_MINIFILTER_OP_CREATE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_CREATE;
    case KSWORD_ARK_MINIFILTER_OP_READ:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_READ;
    case KSWORD_ARK_MINIFILTER_OP_WRITE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_WRITE;
    case KSWORD_ARK_MINIFILTER_OP_SETINFO:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_SETINFO;
    case KSWORD_ARK_MINIFILTER_OP_RENAME:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_RENAME;
    case KSWORD_ARK_MINIFILTER_OP_DELETE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_DELETE;
    case KSWORD_ARK_MINIFILTER_OP_CLEANUP:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_CLEANUP;
    case KSWORD_ARK_MINIFILTER_OP_CLOSE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_CLOSE;
    default:
        return 0UL;
    }
}
