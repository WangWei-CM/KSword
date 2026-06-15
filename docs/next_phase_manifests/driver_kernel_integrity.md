# Driver Kernel Integrity Manifest

## Scope

This manifest records the staged DriverDock-facing R0 read-only integrity query for DriverObject, loader/module views, service metadata, and CPU kernel-entry evidence.

The implementation is intentionally not wired into `KswordARKDriver/src/dispatch/ioctl_registry.c` in this session.

## New shared protocol

- Header: `shared/driver/KswordArkKernelIoctl.h`
- Staged IOCTL function: `KSWORD_ARK_IOCTL_FUNCTION_QUERY_DRIVER_INTEGRITY` (`0x849`)
- Staged IOCTL code: `IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY`
- Protocol version: `KSWORD_ARK_DRIVER_INTEGRITY_PROTOCOL_VERSION` (`1`)
- Request: `KSWORD_ARK_QUERY_DRIVER_INTEGRITY_REQUEST`
- Response: `KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE`
- Evidence row: `KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE`

Each evidence row contains:

- `evidenceClass`
- `objectAddress`
- `targetAddress`
- `ownerModule`
- `riskFlags`
- `sourceMask`
- `confidence`
- `detail`

CPU rows also carry `processorGroup`, `processorNumber`, and `vector` for IDT evidence.

## Staged R0 handler

- Handler: `KswordARKKernelIoctlQueryDriverIntegrity`
- File: `KswordARKDriver/src/features/kernel/kernel_ioctl.c`
- Backend: `KswordARKDriverQueryDriverIntegrity`
- Public prototype: `KswordARKDriver/include/ark/ark_kernel.h`

The handler performs only WDF buffer retrieval, default request population, logging, and backend dispatch. It is not added to the IOCTL registry in this session.

## New source files

- `KswordARKDriver/src/features/kernel/driver_integrity.h`
  - Shared internal builder and helper declarations.
- `KswordARKDriver/src/features/kernel/driver_integrity.c`
  - SystemModuleInformation module evidence.
  - Dynamically resolved AuxKlib module evidence.
  - DriverObject `DriverStart`/`DriverSize`/`DriverSection` capture.
  - DynData-backed PsLoadedModuleList/KLDR target matching and DriverSection alignment evidence.
  - MajorFunction and FastIoDispatch module ownership checks.
  - DeviceObject and AttachedDevice chain loop/cross-driver checks.
  - Services key read-only availability evidence.
  - Optional MmUnloadedDrivers and PiDDBCacheTable global-address evidence, with graceful unavailable rows when DynData is missing.
- `KswordARKDriver/src/features/kernel/kernel_cpu_integrity.h`
  - CPU integrity collector declaration.
- `KswordARKDriver/src/features/kernel/kernel_cpu_integrity.c`
  - Per-CPU read-only CR0/CR4/EFER/LSTAR/SYSENTER_EIP capture.
  - Per-CPU IDTR/GDTR capture.
  - Optional IDT handler owner attribution through the loaded module snapshot.
  - Shared bounded evidence append helpers and DynData/KLDR guarded-read helpers.

## Read-only constraints honored

- No build was run.
- No driver unload path was modified or invoked.
- No hook repair path was added.
- No MSR writes are present.
- No IDT/GDT writes or patching are present.
- Per-CPU work only reads registers/MSRs/descriptors and copied IDT gate bytes.
- `ioctl_registry.c`, project files, ArkDriverClient, UI Dock, DynData files, and `driver_unload.c` were not modified.

## Current DynData behavior

When the active DynData profile exposes `PsLoadedModuleList` and KLDR offsets, the backend walks the loader list read-only with bounded guarded reads and compares the target against `DriverObject->DriverSection`, `DriverStart`, and `DriverSize`.

`MmUnloadedDrivers` and `PiDDBCacheTable` are currently reported as global-address availability evidence only. Entry-level enumeration remains deferred because no stable entry schema is defined in this feature yet. Missing or inactive DynData produces explicit unavailable evidence rows instead of guessed offsets.

## Session 6 integration checklist

1. Add `driver_integrity.c`, `driver_integrity.h`, `kernel_cpu_integrity.c`, and `kernel_cpu_integrity.h` to `KswordARKDriver.vcxproj` and `.vcxproj.filters`.
2. Register `IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY` in `KswordARKDriver/src/dispatch/ioctl_registry.c` with handler `KswordARKKernelIoctlQueryDriverIntegrity`.
3. Add ArkDriverClient wrappers and DriverDock UI model/rendering for evidence rows.
4. Decide whether DynData vNext should add stable MmUnloadedDrivers and PiDDB entry schemas for full row enumeration.
5. Add focused integration tests or manual validation steps after registration.

No project-file or IOCTL-registry edits were made in this session.
