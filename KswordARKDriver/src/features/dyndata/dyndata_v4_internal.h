#pragma once

#include "ark/ark_dyndata.h"

EXTERN_C_START

// Per-capability-group v4 state. The public row is stored directly so query
// IOCTLs can copy stable coverage counters without recomputing them.
typedef struct _KSW_DYN_V4_GROUP_STATE
{
    KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY PublicEntry;
} KSW_DYN_V4_GROUP_STATE;

// Per-loaded-module v4 profile state. Occupied marks whether a module profile
// has passed identity validation, StoredItemCount bounds Items, PublicEntry is
// the module status returned to R3, Groups holds derived capability coverage,
// and Items preserves the accepted compact PDB facts for future consumers.
typedef struct _KSW_DYN_V4_MODULE_STATE
{
    BOOLEAN Occupied;
    ULONG StoredItemCount;
    KSW_DYN_V4_MODULE_STATUS_ENTRY PublicEntry;
    KSW_DYN_V4_GROUP_STATE Groups[KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE];
    KSW_DYN_V4_ITEM_PACKET Items[KSW_DYN_V4_MAX_ITEMS_PER_MODULE];
} KSW_DYN_V4_MODULE_STATE;

// Global v4 state. Modules is indexed by the compact v4 module-slot mapping,
// Missing stores bounded required/optional diagnostics, and MissingCount bounds
// the number of valid Missing rows.
typedef struct _KSW_DYN_V4_STATE
{
    KSW_DYN_V4_MODULE_STATE Modules[KSW_DYN_V4_MAX_MODULES];
    KSW_DYN_V4_MISSING_ITEM_ENTRY Missing[KSW_DYN_V4_MAX_MISSING_SUMMARY];
    ULONG MissingCount;
} KSW_DYN_V4_STATE;

extern EX_PUSH_LOCK g_KswordDynDataV4Lock;
extern KSW_DYN_V4_STATE g_KswordDynDataV4State;

EXTERN_C_END
