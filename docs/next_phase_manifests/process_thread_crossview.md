# Process / Thread Cross-View R0 Manifest

## Scope

This phase adds read-only R0 process and thread cross-view evidence collection
for DKOM diagnostics. The handlers are implemented but intentionally not
registered in `KswordARKDriver/src/dispatch/ioctl_registry.c`.

Hard constraints preserved:

- No build and no live driver execution in this session.
- No DKOM writes, restore, unlink, field clearing, process kill, or thread kill.
- No R3-supplied EPROCESS/ETHREAD address is accepted as a trusted object.
- Object references taken by public walkers or CID-table validation are released
  in the same collection path.
- Private-list walks are bounded by `maxNodes`, use loop detection, pointer
  alignment checks, and guarded reads.

## New IOCTLs

### Process cross-view

- Name: `IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW`
- Function id: `KSWORD_ARK_IOCTL_FUNCTION_QUERY_PROCESS_CROSSVIEW` = `0x836`
- Method/access: `METHOD_BUFFERED`, `FILE_ANY_ACCESS`
- Shared header: `shared/driver/KswordArkProcessIoctl.h`
- Status: protocol and handler are present, but the IOCTL is not registered.

### Thread cross-view

- Name: `IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW`
- Function id: `KSWORD_ARK_IOCTL_FUNCTION_QUERY_THREAD_CROSSVIEW` = `0x837`
- Method/access: `METHOD_BUFFERED`, `FILE_ANY_ACCESS`
- Shared header: `shared/driver/KswordArkThreadIoctl.h`
- Status: protocol and handler are present, but the IOCTL is not registered.

The `0x836` and `0x837` function ids intentionally avoid the existing mutation
transaction ids `0x832` through `0x835`.

## Handler and backend names

### Process

- WDF handler: `KswordARKProcessIoctlQueryCrossView`
  - Implementation: `KswordARKDriver/src/features/process/process_crossview.c`
  - Declaration: `KswordARKDriver/include/ark/ark_process.h`
  - Status: implemented but not registered.
- Backend: `KswordARKDriverQueryProcessCrossView`
  - Implementation: `KswordARKDriver/src/features/process/process_crossview.c`
  - Declaration: `KswordARKDriver/include/ark/ark_process.h`

### Thread

- WDF handler: `KswordARKThreadIoctlQueryCrossView`
  - Implementation: `KswordARKDriver/src/features/thread/thread_crossview.c`
  - Declaration: `KswordARKDriver/include/ark/ark_thread_crossview.h`
  - Status: implemented but not registered.
- Backend: `KswordARKDriverQueryThreadCrossView`
  - Implementation: `KswordARKDriver/src/features/thread/thread_crossview.c`
  - Declaration: `KswordARKDriver/include/ark/ark_thread_crossview.h`

## Protocol structures

Shared structs added in `shared/driver/KswordArkProcessIoctl.h`:

- `KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS`
  - EPROCESS offsets: `epUniqueProcessId`, `epActiveProcessLinks`,
    `epThreadListHead`, `epImageFileName`
  - ETHREAD/KTHREAD offsets: `etCid`, `etThreadListEntry`, `etStartAddress`,
    `etWin32StartAddress`, `ktProcess`
  - HANDLE_TABLE offsets/globals: `htTableCode`, `hteLowValue`,
    `pspCidTableRva`, `pspCidTableAddress`
- `KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST`
- `KSWORD_ARK_PROCESS_CROSSVIEW_ROW`
- `KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE`

Shared structs added in `shared/driver/KswordArkThreadIoctl.h`:

- `KSWORD_ARK_THREAD_CROSSVIEW_REQUEST`
- `KSWORD_ARK_THREAD_CROSSVIEW_ROW`
- `KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE`

Common row fields include:

- `objectAddress`
- Process row: `processId`, `parentProcessId`, `imageName`
- Thread row: `processObjectAddress`, `processId`, `threadId`, `imageName`,
  `startAddress`
- `sourceMask`
- `anomalyFlags`
- `dynDataCapabilityMask`
- `fieldOffsets`
- `lastStatus`
- `confidence`
- `detail`

## Evidence sources

### Process

- Public walk: `PsGetNextProcess`
- Active list: `EPROCESS.ActiveProcessLinks` from `PsInitialSystemProcess`
- CID table: read-only `PspCidTable` walk
  - Prefers DynData `KernelGlobals.PspCidTable`
  - Falls back to the existing resolver pattern used by the DKOM diagnostics
  - Uses HANDLE_TABLE `TableCode` and HANDLE_TABLE_ENTRY `LowValue` only for
    read-only enumeration

### Thread

- Public walk: `PsGetNextProcess` plus `PsGetNextProcessThread`
- Process thread list: `EPROCESS.ThreadListHead` plus
  `ETHREAD.ThreadListEntry`
- CID table: read-only `PspCidTable` walk filtered to thread objects
- Optional start-address validation against a `SystemModuleInformation` snapshot

## Source and anomaly bits

Source bits:

- `KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK`
- `KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST`
- `KSWORD_ARK_CROSSVIEW_SOURCE_CID_TABLE`
- `KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST`

Anomaly bits:

- `KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE`
- `KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT`

## Limits and capability behavior

- Protocol version: `KSWORD_ARK_CROSSVIEW_PROTOCOL_VERSION` = `1`
- Default node budget: `KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES` = `8192`
- Hard node budget: `KSWORD_ARK_CROSSVIEW_HARD_MAX_NODES` = `16384`
- Missing DynData capability is reported as
  `KSWORD_ARK_CROSSVIEW_STATUS_CAPABILITY_MISSING` with
  `missingCapabilityMask`.
- Partial output or traversal budget exhaustion is reported as
  `KSWORD_ARK_CROSSVIEW_STATUS_PARTIAL`.

## Session 6 required integration

Do not add these in this session. Session 6 should add:

1. Project files:
   - Add `KswordARKDriver/src/features/process/process_crossview.c` to the
     driver `.vcxproj`.
   - Add `KswordARKDriver/src/features/thread/thread_crossview.c` to the driver
     `.vcxproj`.
   - Add both sources to the matching `.vcxproj.filters` feature folders.
   - Add `KswordARKDriver/include/ark/ark_thread_crossview.h` if headers are
     explicitly listed.
2. Registry dispatch:
   - Add forward declarations in `KswordARKDriver/src/dispatch/ioctl_registry.c`
     for:
     - `KswordARKProcessIoctlQueryCrossView`
     - `KswordARKThreadIoctlQueryCrossView`
   - Add registry rows:
     - `{ IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW, KswordARKProcessIoctlQueryCrossView, "IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW", KSW_CAP_PROCESS_LIST_FIELDS | KSW_CAP_CID_TABLE_WALK, KSWORD_ARK_IOCTL_FLAG_NONE }`
     - `{ IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW, KswordARKThreadIoctlQueryCrossView, "IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW", KSW_CAP_THREAD_LIST_FIELDS | KSW_CAP_CID_TABLE_WALK, KSWORD_ARK_IOCTL_FLAG_NONE }`
3. ArkDriverClient:
   - Add typed wrappers for the two query IOCTLs.
   - Add row decoding for process and thread cross-view responses.
4. UI:
   - Add a process/thread cross-view evidence view.
   - Display source masks, anomaly flags, DynData capabilities, offsets,
     confidence, and detail strings.
   - Keep destructive DKOM actions separate from evidence collection.
5. Capability/preflight:
   - Surface cross-view support only after registry/project wiring is complete.
   - Keep capability-missing rows visible so R3 can explain missing DynData.

## Files changed in this phase

- `shared/driver/KswordArkProcessIoctl.h`
- `shared/driver/KswordArkThreadIoctl.h`
- `KswordARKDriver/include/ark/ark_process.h`
- `KswordARKDriver/include/ark/ark_thread_crossview.h`
- `KswordARKDriver/src/features/process/process_crossview.c`
- `KswordARKDriver/src/features/process/process_crossview.h`
- `KswordARKDriver/src/features/thread/thread_crossview.c`
- `KswordARKDriver/src/features/thread/thread_crossview.h`
- `docs/next_phase_manifests/process_thread_crossview.md`
