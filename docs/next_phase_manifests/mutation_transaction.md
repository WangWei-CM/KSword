# Controlled Kernel Mutation Transaction Manifest

## Scope

This phase adds the R0/R3 protocol and R0 backend for controlled kernel mutation transactions. It does not expose an arbitrary-write UI, does not register IOCTLs, and does not connect any existing repair button.

Future inline hook restore, process protection byte restore, and callback unlink actions must pass through:

1. `PREPARE` - validate target and snapshot before bytes.
2. `COMMIT` - revalidate transaction id, before bytes, FORCE, and safety policy before any supported write.
3. `ROLLBACK` - restore the before snapshot when supported; already-restored targets are idempotent.
4. `QUERY_AUDIT` - read recent transaction audit entries from the R0 ring.

## Shared protocol

Protocol header:

- `shared/driver/KswordArkMutationIoctl.h`

Version:

- `KSWORD_ARK_MUTATION_PROTOCOL_VERSION = 1`

Future IOCTL constants, intentionally not registered in this phase:

- `IOCTL_KSWORD_ARK_MUTATION_PREPARE`
- `IOCTL_KSWORD_ARK_MUTATION_COMMIT`
- `IOCTL_KSWORD_ARK_MUTATION_ROLLBACK`
- `IOCTL_KSWORD_ARK_MUTATION_QUERY_AUDIT`

Primary request/response types:

- `KSWORD_ARK_MUTATION_PREPARE_REQUEST`
- `KSWORD_ARK_MUTATION_TRANSACTION_REQUEST`
- `KSWORD_ARK_MUTATION_RESPONSE`
- `KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST`
- `KSWORD_ARK_MUTATION_AUDIT_ENTRY`
- `KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE`

Target kinds:

- `KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL`
  - Max 64 bytes.
  - Must be canonical kernel address, nonpaged, and readable.
  - Commit/rollback write path is blocked by design in this phase.
- `KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES`
  - Max 3 bytes.
  - Only DynData-confirmed `EPROCESS` protection fields are accepted.
  - Commit/rollback can write only these logical bytes after FORCE, before-match, and safety policy allow.
- `KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN`
  - Plan/snapshot only in this phase.
  - Commit/rollback actual unlink is rejected by design.

Transaction and audit records include:

- `transactionId`
- `targetKind`
- `targetAddress`
- `bytes`
- `beforeHash`
- `afterHash`
- `status`
- `lastStatus`
- `timestampTick`
- `riskFlags`

## R0 source files

Public R0 header:

- `KswordARKDriver/include/ark/ark_mutation.h`

Feature backend:

- `KswordARKDriver/src/features/mutation/mutation_transaction.h`
- `KswordARKDriver/src/features/mutation/mutation_transaction.c`

Future IOCTL handlers:

- `KswordARKDriver/src/features/mutation/mutation_ioctl.c`

Handler names for later registry integration:

- `KswordARKMutationIoctlPrepare`
- `KswordARKMutationIoctlCommit`
- `KswordARKMutationIoctlRollback`
- `KswordARKMutationIoctlQueryAudit`

Backend entry names:

- `KswordARKMutationInitialize`
- `KswordARKMutationPrepare`
- `KswordARKMutationCommit`
- `KswordARKMutationRollback`
- `KswordARKMutationQueryAudit`

## Default refusal policy

The backend fails closed by default:

- Missing `FORCE` on `COMMIT` or `ROLLBACK` returns dry-run status.
- Unknown `targetKind` is rejected.
- Zero length or length over target limits is rejected.
- Safety policy denial rejects commit/rollback writes.
- Before-byte mismatch rejects commit.
- Target identity changes reject commit/rollback.
- `KernelVirtualBytesSmall` write path is rejected by design.
- `CallbackEntryUnlinkPlan` actual unlink is rejected by design.
- Audit query redacts `byteData` unless `KSWORD_ARK_MUTATION_QUERY_AUDIT_FLAG_INCLUDE_BYTES` is set.

The implementation does not bypass PatchGuard, does not modify CR0 WP, does not write PTEs, and does not perform arbitrary physical memory writes.

## Later integration points

The next integration session should:

1. Include `driver/KswordArkMutationIoctl.h` from `KswordARKDriver/include/ark/ark_ioctl.h` only when registering the feature.
2. Add handler declarations and table entries in `KswordARKDriver/src/dispatch/ioctl_registry.c` for the four mutation IOCTLs.
3. Add the new `.c/.h` files to `KswordARKDriver.vcxproj` and `.vcxproj.filters`.
4. Route future inline hook restore through `KswordARKMutationPrepare` and `KswordARKMutationCommit` instead of direct patch calls.
5. Route future PPL byte restore through `ProcessProtectionBytes` transactions.
6. Route future callback unlink work through `CallbackEntryUnlinkPlan` first, then add a separate reviewed write backend only after plan validation and safety policy semantics are finalized.
7. Keep `ArkDriverClient` and UI Dock disconnected until the transaction IOCTLs are registered and separately reviewed.
