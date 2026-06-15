# Memory Evidence R0 Manifest

## Scope

This phase adds the read-only R0 protocol and implementation for kernel memory evidence collection. It is intentionally not registered in `ioctl_registry.c`; session 6 should wire the project and registry entries after review.

Hard constraints preserved:

- PASSIVE_LEVEL only.
- No live driver run and no build in this session.
- No PTE write, no CR0 WP change, and no kernel memory write path.
- R0 does not read disk files for text diff; it returns memory section metadata, hash, and sample bytes for R3 comparison.

## New IOCTL

- Name: `IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE`
- Function id: `KSWORD_ARK_IOCTL_FUNCTION_SCAN_KERNEL_MEMORY_EVIDENCE` = `0x832`
- Method/access: `METHOD_BUFFERED`, `FILE_ANY_ACCESS`
- Shared header: `shared/driver/KswordArkMemoryIoctl.h`

## Handler/backend names

- WDF handler: `KswordARKMemoryIoctlScanKernelMemoryEvidence`
  - File: `KswordARKDriver/src/features/memory/memory_ioctl.c`
  - Status: implemented but not registered.
- Backend: `KswordARKDriverScanKernelMemoryEvidence`
  - Declaration: `KswordARKDriver/include/ark/ark_memory_evidence.h`
  - Implementation: `KswordARKDriver/src/features/memory/memory_kernel_evidence.c`

## Protocol structures

Shared structs added in `shared/driver/KswordArkMemoryIoctl.h`:

- `KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE_REQUEST`
  - `flags`
  - `maxRows`
  - `startAddress`, `endAddress`
  - `maxBytes`
  - `maxBigPoolRows`
  - `sampleBytes`
  - `reserved0`, `reserved1`
- `KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW`
  - `virtualAddress`, `regionSize`, `pageSize`
  - `permissionFlags`: Present/Read/Write/Execute/NX/Large/Global/User
  - `ownerKind`: LoadedModule/NonModule/BigPool/SystemPte/MdlLike/Unknown
  - `riskFlags`: RWX/NonModuleExecutable/ModuleNonTextExecutable/ExecutablePool/LargeExecutable/OwnerMissing
  - `moduleBase`, `moduleSize`, `ownerAddress`
  - `ownerName`, `detail`
  - BigPool fields: `bigPoolTag`, `bigPoolFlags`
  - text diff fields: `sectionName`, `sectionRva`, `sectionSize`, `hashAlgorithm`, `contentHash`, `sampleSize`, `sample`
  - `lastStatus`, `confidence`
- `KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE_RESPONSE`
  - `status`, `responseFlags`, `sourceFlags`
  - `totalRows`, `returnedRows`, `rowSize`, `maxRows`
  - `maxBytes`, `bytesScanned`
  - `moduleCount`, `bigPoolRowsSeen`
  - `lastStatus`
  - variable rows array

## Request flags

- `KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_LOADED_MODULE_EXECUTABLE`
- `KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_NONMODULE_EXECUTABLE_RANGES`
- `KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_BIGPOOL`
- `KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_TEXT_SECTION_SAMPLES`
- `KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_SUSPECTED_BIGPOOL`
- `KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_ALL`

`flags == 0` maps to the conservative default: loaded module executable + BigPool + text section samples + suspected BigPool. It does not include non-module executable range scan. Non-module scanning must be explicitly requested with a bounded `startAddress/endAddress`.

## Limits

- `KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_ROWS` = 256
- `KSWORD_ARK_MEMORY_EVIDENCE_HARD_MAX_ROWS` = 4096
- `KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_BYTES` = 16 MiB
- `KSWORD_ARK_MEMORY_EVIDENCE_HARD_MAX_BYTES` = 64 MiB
- `KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_BIGPOOL_ROWS` = 256
- `KSWORD_ARK_MEMORY_EVIDENCE_HARD_MAX_BIGPOOL_ROWS` = 1024
- `KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_SAMPLE_BYTES` = 32
- `KSWORD_ARK_MEMORY_EVIDENCE_HARD_MAX_SAMPLE_BYTES` = 64

## Implementation notes

- Page permissions reuse the existing read-only path through `KswordARKKernelExecQueryPage`, which delegates to the current page-table backend.
- Loaded module pages are scanned from `SystemModuleInformation` snapshots and PE section headers copied through safe image-read helpers.
- Non-module executable rows are merged when contiguous and permission/risk/page-size compatible.
- BigPool is queried through `SystemBigPoolInformation` (`0x42`) only; rows are bounded by `maxBigPoolRows` and overall `maxRows/maxBytes`.
- BigPool owner heuristics are conservative:
  - tag contains `PTE` => `SystemPte`
  - tag contains `MDL` => `MdlLike`
  - nonpaged + PTE/MDL-like => suspected executable pool
  - page-table executable observation => executable pool
- Text diff support is memory-only: R0 emits section RVA/size/name plus `FNV1A64` over the first bounded bytes and sample bytes. R3 must do disk image loading and comparison.

## Session 6 required integration

Do not add these in this session. Session 6 should add:

1. Project files:
   - Add `KswordARKDriver/src/features/memory/memory_kernel_evidence.c` to the driver `.vcxproj`.
   - Add the same source to the matching `.vcxproj.filters` memory folder.
   - Add `KswordARKDriver/include/ark/ark_memory_evidence.h` if the project explicitly lists headers.
2. Registry dispatch:
   - Add forward declaration in `KswordARKDriver/src/dispatch/ioctl_registry.c`:
     - `NTSTATUS KswordARKMemoryIoctlScanKernelMemoryEvidence(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);`
   - Add registry row:
     - `{ IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE, KswordARKMemoryIoctlScanKernelMemoryEvidence, "IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE }`
3. R3/client/UI follow-up:
   - Add ArkDriverClient wrapper and UI only in a later allowed phase.
   - Keep disk-vs-memory text diff in R3.

## Files changed in this phase

- `shared/driver/KswordArkMemoryIoctl.h`
- `KswordARKDriver/src/features/memory/memory_ioctl.c`
- `KswordARKDriver/src/features/memory/memory_kernel_evidence.c`
- `KswordARKDriver/include/ark/ark_memory_evidence.h`
- `docs/next_phase_manifests/memory_evidence.md`
