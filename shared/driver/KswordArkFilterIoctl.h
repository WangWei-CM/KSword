#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkFilterIoctl.h
// Purpose:
// - Define read-only R3 <-> R0 minifilter inventory packets.
// - The protocol reports Filter Manager public API evidence only.
// - It never unloads filters, detaches instances, patches callbacks, or
//   modifies fltMgr private lists.
// ============================================================

#define KSWORD_ARK_FILTER_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_MINIFILTER_INVENTORY 0x8B0UL

#define IOCTL_KSWORD_ARK_QUERY_MINIFILTER_INVENTORY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_MINIFILTER_INVENTORY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_MINIFILTER_INVENTORY_FLAG_INCLUDE_VOLUMES 0x00000001UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_FLAG_INCLUDE_CALLBACK_OWNER_HINT 0x00000002UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_FLAG_INCLUDE_ALL \
    (KSWORD_ARK_MINIFILTER_INVENTORY_FLAG_INCLUDE_VOLUMES | \
     KSWORD_ARK_MINIFILTER_INVENTORY_FLAG_INCLUDE_CALLBACK_OWNER_HINT)

#define KSWORD_ARK_MINIFILTER_INVENTORY_RESPONSE_FLAG_TRUNCATED 0x00000001UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_RESPONSE_FLAG_PARTIAL 0x00000002UL

#define KSWORD_ARK_MINIFILTER_INVENTORY_ROW_FLAG_FILTER_PRESENT 0x00000001UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_ROW_FLAG_NAME_PRESENT 0x00000002UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_ROW_FLAG_ALTITUDE_PRESENT 0x00000004UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_ROW_FLAG_VOLUME_PRESENT 0x00000008UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_ROW_FLAG_VOLUME_NAME_PRESENT 0x00000010UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_ROW_FLAG_CALLBACK_OWNER_UNSUPPORTED 0x00000020UL

#define KSWORD_ARK_MINIFILTER_INVENTORY_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_STATUS_OK 1UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_STATUS_PARTIAL 2UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_STATUS_QUERY_FAILED 3UL
#define KSWORD_ARK_MINIFILTER_INVENTORY_STATUS_BUFFER_TRUNCATED 4UL

#define KSWORD_ARK_MINIFILTER_INVENTORY_NAME_CHARS 128U
#define KSWORD_ARK_MINIFILTER_INVENTORY_ALTITUDE_CHARS 64U
#define KSWORD_ARK_MINIFILTER_INVENTORY_VOLUME_NAME_CHARS 260U
#define KSWORD_ARK_MINIFILTER_INVENTORY_MODULE_NAME_CHARS 260U

typedef struct _KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_REQUEST
{
    // size is validated by the IOCTL handler so older clients can fail closed.
    unsigned long size;
    // version is KSWORD_ARK_FILTER_PROTOCOL_VERSION for this packet layout.
    unsigned long version;
    // flags controls optional volume and callback-owner hint collection.
    unsigned long flags;
    // maxRows caps variable output rows and prevents excessive enumeration.
    unsigned long maxRows;
} KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_REQUEST;

typedef struct _KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY
{
    // size lets R3 validate the row layout without guessing structure packing.
    unsigned long size;
    // status is per-row so one failed volume/name query does not poison the table.
    unsigned long status;
    // fieldFlags records which optional addresses/text fields were populated.
    unsigned long fieldFlags;
    // sourceFlags records the evidence source; P0 uses Filter Manager public API.
    unsigned long sourceFlags;
    // instanceCount is the filter-wide count reported by Filter Manager.
    unsigned long instanceCount;
    // volumeBindingInstanceCount is the count observed for this volume binding.
    unsigned long volumeBindingInstanceCount;
    // frameId is the fltMgr frame identifier reported by aggregate information.
    unsigned long frameId;
    // reserved keeps the fixed header 8-byte aligned for addresses below.
    unsigned long reserved0;
    // filterObject is the referenced PFLT_FILTER value observed during enumeration.
    unsigned long long filterObject;
    // volumeObject is the PFLT_VOLUME value when the row represents a binding.
    unsigned long long volumeObject;
    // callbackOwnerModuleBase is reserved for a future PDB-backed owner resolver.
    unsigned long long callbackOwnerModuleBase;
    // callbackOwnerModuleSize is reserved for a future PDB-backed owner resolver.
    unsigned long callbackOwnerModuleSize;
    // callbackOwnerStatus explains whether callback owner data is available.
    long callbackOwnerStatus;
    // filterName is the minifilter name copied from Filter Manager.
    wchar_t filterName[KSWORD_ARK_MINIFILTER_INVENTORY_NAME_CHARS];
    // altitude is the minifilter altitude copied from Filter Manager.
    wchar_t altitude[KSWORD_ARK_MINIFILTER_INVENTORY_ALTITUDE_CHARS];
    // volumeName is present for per-volume binding rows.
    wchar_t volumeName[KSWORD_ARK_MINIFILTER_INVENTORY_VOLUME_NAME_CHARS];
    // callbackOwnerModule is reserved for future PDB/module ownership evidence.
    wchar_t callbackOwnerModule[KSWORD_ARK_MINIFILTER_INVENTORY_MODULE_NAME_CHARS];
} KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY;

typedef struct _KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_RESPONSE
{
    // size is the fixed response header size including the placeholder row.
    unsigned long size;
    // version is KSWORD_ARK_FILTER_PROTOCOL_VERSION for this packet layout.
    unsigned long version;
    // totalCount counts all rows observed before output-capacity truncation.
    unsigned long totalCount;
    // returnedCount counts rows copied into entries[].
    unsigned long returnedCount;
    // entrySize is sizeof(KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY).
    unsigned long entrySize;
    // flags reports response-level conditions such as truncation or partial data.
    unsigned long flags;
    // queryStatus is the overall query status while preserving partial rows.
    unsigned long queryStatus;
    // lastStatus stores the most recent NTSTATUS from Filter Manager.
    long lastStatus;
    // entries is a variable-length METHOD_BUFFERED row array.
    KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY entries[1];
} KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_RESPONSE;
