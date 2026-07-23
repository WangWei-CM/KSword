#!/usr/bin/env python3
"""
KswordARK PDB offset profile generator.

Inputs:
- kphdyn.xml rows containing PE TimeDateStamp, SizeOfImage, file version, and SHA256.
- Microsoft public symbol server for PE/PDB downloads.

Processing:
- Download PE by the public symbol-server PE key.
- Verify SHA256 before trusting the image.
- Parse CodeView RSDS data from the PE to obtain PDB name, GUID, and age.
- Download the exact PDB by GUID+age.
- Use llvm-pdbutil to resolve selected private structure member offsets,
  callback v2 PDB items, and optional v3 kernel global RVAs.

    Returns:
- One JSON profile per PE/PDB identity, suitable for KswordARK R3 parsing.
- Optional callback/kernel-global diagnostics are included without making
  missing private symbols fatal, because Microsoft public PDB coverage varies by
  build.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import pefile
import requests

DEFAULT_SYMBOL_SERVER = "https://msdl.microsoft.com/download/symbols"
DEFAULT_SYMBOL_ROOT = r"D:\PDB"
DEFAULT_LLVM_PDBUTIL = r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe"
HTTP_TIMEOUT_SECONDS = 600
PDBUTIL_TIMEOUT_SECONDS = 600
KSW_SCHEMA_VERSION = 1
CALLBACK_ITEM_SCHEMA_VERSION = 2
TYPED_ITEM_SCHEMA_VERSION = 3

FIELD_MAP: dict[str, tuple[str, str]] = {
    "EpObjectTable": ("_EPROCESS", "ObjectTable"),
    "EpSectionObject": ("_EPROCESS", "SectionObject"),
    "EpUniqueProcessId": ("_EPROCESS", "UniqueProcessId"),
    "EpActiveProcessLinks": ("_EPROCESS", "ActiveProcessLinks"),
    "EpThreadListHead": ("_EPROCESS", "ThreadListHead"),
    "EpImageFileName": ("_EPROCESS", "ImageFileName"),
    "EpToken": ("_EPROCESS", "Token"),
    "EpFlags": ("_EPROCESS", "Flags"),
    "EpFlags2": ("_EPROCESS", "Flags2"),
    "EpRundownProtect": ("_EPROCESS", "RundownProtect"),
    "EpProcessLock": ("_EPROCESS", "ProcessLock"),
    "EpCreateTime": ("_EPROCESS", "CreateTime"),
    "EpExitTime": ("_EPROCESS", "ExitTime"),
    "EpExitStatus": ("_EPROCESS", "ExitStatus"),
    "EpPeb": ("_EPROCESS", "Peb"),
    "EpSession": ("_EPROCESS", "Session"),
    "EpWin32Process": ("_EPROCESS", "Win32Process"),
    "EpWow64Process": ("_EPROCESS", "WoW64Process"),
    "EpInheritedFromUniqueProcessId": ("_EPROCESS", "InheritedFromUniqueProcessId"),
    "EpSeAuditProcessCreationInfo": ("_EPROCESS", "SeAuditProcessCreationInfo"),
    "EpJob": ("_EPROCESS", "Job"),
    "EpDeviceMap": ("_EPROCESS", "DeviceMap"),
    "EpDebugPort": ("_EPROCESS", "DebugPort"),
    "EpExceptionPortData": ("_EPROCESS", "ExceptionPortData"),
    "EpSectionBaseAddress": ("_EPROCESS", "SectionBaseAddress"),
    "EpImageFilePointer": ("_EPROCESS", "ImageFilePointer"),
    "EpPriorityClass": ("_EPROCESS", "PriorityClass"),
    "EpActiveThreads": ("_EPROCESS", "ActiveThreads"),
    "EpVadRoot": ("_EPROCESS", "VadRoot"),
    "EpVadHint": ("_EPROCESS", "VadHint"),
    "EpCloneRoot": ("_EPROCESS", "CloneRoot"),
    "EpNumberOfPrivatePages": ("_EPROCESS", "NumberOfPrivatePages"),
    "EpNumberOfLockedPages": ("_EPROCESS", "NumberOfLockedPages"),
    "EpCommitCharge": ("_EPROCESS", "CommitCharge"),
    "EpCommitChargePeak": ("_EPROCESS", "CommitChargePeak"),
    "EpPeakVirtualSize": ("_EPROCESS", "PeakVirtualSize"),
    "EpVirtualSize": ("_EPROCESS", "VirtualSize"),
    "EpSessionProcessLinks": ("_EPROCESS", "SessionProcessLinks"),
    "EpMitigationFlags": ("_EPROCESS", "MitigationFlags"),
    "EpMitigationFlags2": ("_EPROCESS", "MitigationFlags2"),
    "EpProcessQuotaUsage": ("_EPROCESS", "ProcessQuotaUsage"),
    "EpProcessQuotaPeak": ("_EPROCESS", "ProcessQuotaPeak"),
    "EpAddressCreationLock": ("_EPROCESS", "AddressCreationLock"),
    "EpPageTableCommitmentLock": ("_EPROCESS", "PageTableCommitmentLock"),
    "EpRotateInProgress": ("_EPROCESS", "RotateInProgress"),
    "EpForkInProgress": ("_EPROCESS", "ForkInProgress"),
    "EpCommitChargeJob": ("_EPROCESS", "CommitChargeJob"),
    "EpCookie": ("_EPROCESS", "Cookie"),
    "EpWorkingSetWatch": ("_EPROCESS", "WorkingSetWatch"),
    "EpWin32WindowStation": ("_EPROCESS", "Win32WindowStation"),
    "EpOwnerProcessId": ("_EPROCESS", "OwnerProcessId"),
    "EpQuotaBlock": ("_EPROCESS", "QuotaBlock"),
    "EpEtwDataSource": ("_EPROCESS", "EtwDataSource"),
    "EpPageDirectoryPte": ("_EPROCESS", "PageDirectoryPte"),
    "EpSecurityPort": ("_EPROCESS", "SecurityPort"),
    "EpJobLinks": ("_EPROCESS", "JobLinks"),
    "EpHighestUserAddress": ("_EPROCESS", "HighestUserAddress"),
    "EpImagePathHash": ("_EPROCESS", "ImagePathHash"),
    "EpDefaultHardErrorProcessing": ("_EPROCESS", "DefaultHardErrorProcessing"),
    "EpLastThreadExitStatus": ("_EPROCESS", "LastThreadExitStatus"),
    "EpPrefetchTrace": ("_EPROCESS", "PrefetchTrace"),
    "EpLockedPagesList": ("_EPROCESS", "LockedPagesList"),
    "EpReadOperationCount": ("_EPROCESS", "ReadOperationCount"),
    "EpWriteOperationCount": ("_EPROCESS", "WriteOperationCount"),
    "EpOtherOperationCount": ("_EPROCESS", "OtherOperationCount"),
    "EpReadTransferCount": ("_EPROCESS", "ReadTransferCount"),
    "EpWriteTransferCount": ("_EPROCESS", "WriteTransferCount"),
    "EpOtherTransferCount": ("_EPROCESS", "OtherTransferCount"),
    "EpCommitChargeLimit": ("_EPROCESS", "CommitChargeLimit"),
    "EpVm": ("_EPROCESS", "Vm"),
    "EpMmProcessLinks": ("_EPROCESS", "MmProcessLinks"),
    "EpModifiedPageCount": ("_EPROCESS", "ModifiedPageCount"),
    "EpVadCount": ("_EPROCESS", "VadCount"),
    "EpVadPhysicalPages": ("_EPROCESS", "VadPhysicalPages"),
    "EpVadPhysicalPagesLimit": ("_EPROCESS", "VadPhysicalPagesLimit"),
    "EpAlpcContext": ("_EPROCESS", "AlpcContext"),
    "EpTimerResolutionLink": ("_EPROCESS", "TimerResolutionLink"),
    "EpTimerResolutionStackRecord": ("_EPROCESS", "TimerResolutionStackRecord"),
    "EpRequestedTimerResolution": ("_EPROCESS", "RequestedTimerResolution"),
    "EpSmallestTimerResolution": ("_EPROCESS", "SmallestTimerResolution"),
    "EpInvertedFunctionTable": ("_EPROCESS", "InvertedFunctionTable"),
    "EpInvertedFunctionTableLock": ("_EPROCESS", "InvertedFunctionTableLock"),
    "EpActiveThreadsHighWatermark": ("_EPROCESS", "ActiveThreadsHighWatermark"),
    "EpLargePrivateVadCount": ("_EPROCESS", "LargePrivateVadCount"),
    "EpThreadListLock": ("_EPROCESS", "ThreadListLock"),
    "EpWnfContext": ("_EPROCESS", "WnfContext"),
    "EpFlags3": ("_EPROCESS", "Flags3"),
    "EpDiskCounters": ("_EPROCESS", "DiskCounters"),
    "TokTokenSource": ("_TOKEN", "TokenSource"),
    "TokTokenId": ("_TOKEN", "TokenId"),
    "TokAuthenticationId": ("_TOKEN", "AuthenticationId"),
    "TokParentTokenId": ("_TOKEN", "ParentTokenId"),
    "TokExpirationTime": ("_TOKEN", "ExpirationTime"),
    "TokTokenLock": ("_TOKEN", "TokenLock"),
    "TokModifiedId": ("_TOKEN", "ModifiedId"),
    "TokPrivileges": ("_TOKEN", "Privileges"),
    "TokAuditPolicy": ("_TOKEN", "AuditPolicy"),
    "TokSessionId": ("_TOKEN", "SessionId"),
    "TokUserAndGroupCount": ("_TOKEN", "UserAndGroupCount"),
    "TokRestrictedSidCount": ("_TOKEN", "RestrictedSidCount"),
    "TokVariableLength": ("_TOKEN", "VariableLength"),
    "TokDynamicCharged": ("_TOKEN", "DynamicCharged"),
    "TokDynamicAvailable": ("_TOKEN", "DynamicAvailable"),
    "TokDefaultOwnerIndex": ("_TOKEN", "DefaultOwnerIndex"),
    "TokUserAndGroups": ("_TOKEN", "UserAndGroups"),
    "TokRestrictedSids": ("_TOKEN", "RestrictedSids"),
    "TokPrimaryGroup": ("_TOKEN", "PrimaryGroup"),
    "TokDynamicPart": ("_TOKEN", "DynamicPart"),
    "TokDefaultDacl": ("_TOKEN", "DefaultDacl"),
    "TokTokenType": ("_TOKEN", "TokenType"),
    "TokImpersonationLevel": ("_TOKEN", "ImpersonationLevel"),
    "TokTokenFlags": ("_TOKEN", "TokenFlags"),
    "TokTokenInUse": ("_TOKEN", "TokenInUse"),
    "TokIntegrityLevelIndex": ("_TOKEN", "IntegrityLevelIndex"),
    "TokMandatoryPolicy": ("_TOKEN", "MandatoryPolicy"),
    "TokLogonSession": ("_TOKEN", "LogonSession"),
    "TokOriginatingLogonSession": ("_TOKEN", "OriginatingLogonSession"),
    "TokSidHash": ("_TOKEN", "SidHash"),
    "TokRestrictedSidHash": ("_TOKEN", "RestrictedSidHash"),
    "TokPSecurityAttributes": ("_TOKEN", "pSecurityAttributes"),
    "TokPackage": ("_TOKEN", "Package"),
    "TokCapabilities": ("_TOKEN", "Capabilities"),
    "TokCapabilityCount": ("_TOKEN", "CapabilityCount"),
    "TokCapabilitiesHash": ("_TOKEN", "CapabilitiesHash"),
    "TokLowboxNumberEntry": ("_TOKEN", "LowboxNumberEntry"),
    "TokLowboxHandlesEntry": ("_TOKEN", "LowboxHandlesEntry"),
    "TokPClaimAttributes": ("_TOKEN", "pClaimAttributes"),
    "TokTrustLevelSid": ("_TOKEN", "TrustLevelSid"),
    "TokTrustLinkedToken": ("_TOKEN", "TrustLinkedToken"),
    "TokIntegrityLevelSidValue": ("_TOKEN", "IntegrityLevelSidValue"),
    "TokTokenSidValues": ("_TOKEN", "TokenSidValues"),
    "TokSessionObject": ("_TOKEN", "SessionObject"),
    "TokVariablePart": ("_TOKEN", "VariablePart"),
    "EpProtection": ("_EPROCESS", "Protection"),
    "EpSignatureLevel": ("_EPROCESS", "SignatureLevel"),
    "EpSectionSignatureLevel": ("_EPROCESS", "SectionSignatureLevel"),
    "EtCid": ("_ETHREAD", "Cid"),
    "EtThreadListEntry": ("_ETHREAD", "ThreadListEntry"),
    "EtStartAddress": ("_ETHREAD", "StartAddress"),
    "EtWin32StartAddress": ("_ETHREAD", "Win32StartAddress"),
    "KtInitialStack": ("_KTHREAD", "InitialStack"),
    "KtStackLimit": ("_KTHREAD", "StackLimit"),
    "KtStackBase": ("_KTHREAD", "StackBase"),
    "KtKernelStack": ("_KTHREAD", "KernelStack"),
    "KtProcess": ("_KTHREAD", "Process"),
    "KtReadOperationCount": ("_KTHREAD", "ReadOperationCount"),
    "KtWriteOperationCount": ("_KTHREAD", "WriteOperationCount"),
    "KtOtherOperationCount": ("_KTHREAD", "OtherOperationCount"),
    "KtReadTransferCount": ("_KTHREAD", "ReadTransferCount"),
    "KtWriteTransferCount": ("_KTHREAD", "WriteTransferCount"),
    "KtOtherTransferCount": ("_KTHREAD", "OtherTransferCount"),
    "HtTableCode": ("_HANDLE_TABLE", "TableCode"),
    "HtHandleCount": ("_HANDLE_TABLE", "HandleCount"),
    "HteLowValue": ("_HANDLE_TABLE_ENTRY", "LowValue"),
    "KldrInLoadOrderLinks": ("_KLDR_DATA_TABLE_ENTRY", "InLoadOrderLinks"),
    "KldrDllBase": ("_KLDR_DATA_TABLE_ENTRY", "DllBase"),
    "KldrSizeOfImage": ("_KLDR_DATA_TABLE_ENTRY", "SizeOfImage"),
    "KldrFullDllName": ("_KLDR_DATA_TABLE_ENTRY", "FullDllName"),
    "KldrBaseDllName": ("_KLDR_DATA_TABLE_ENTRY", "BaseDllName"),
    "KldrFlags": ("_KLDR_DATA_TABLE_ENTRY", "Flags"),
    "DoDriverStart": ("_DRIVER_OBJECT", "DriverStart"),
    "DoDriverSize": ("_DRIVER_OBJECT", "DriverSize"),
    "DoDriverSection": ("_DRIVER_OBJECT", "DriverSection"),
    "DoMajorFunction": ("_DRIVER_OBJECT", "MajorFunction"),
    "DoFastIoDispatch": ("_DRIVER_OBJECT", "FastIoDispatch"),
    "DoDriverUnload": ("_DRIVER_OBJECT", "DriverUnload"),
    "UldName": ("_UNLOADED_DRIVERS", "Name"),
    "UldStartAddress": ("_UNLOADED_DRIVERS", "StartAddress"),
    "UldEndAddress": ("_UNLOADED_DRIVERS", "EndAddress"),
    "UldCurrentTime": ("_UNLOADED_DRIVERS", "CurrentTime"),
    "RtlAvlBalancedRoot": ("_RTL_AVL_TABLE", "BalancedRoot"),
    "RtlAvlOrderedPointer": ("_RTL_AVL_TABLE", "OrderedPointer"),
    "RtlAvlWhichOrderedElement": ("_RTL_AVL_TABLE", "WhichOrderedElement"),
    "RtlAvlNumberGenericTableElements": ("_RTL_AVL_TABLE", "NumberGenericTableElements"),
    "RtlAvlDepthOfTree": ("_RTL_AVL_TABLE", "DepthOfTree"),
    "RtlAvlRestartKey": ("_RTL_AVL_TABLE", "RestartKey"),
    "RtlAvlDeleteCount": ("_RTL_AVL_TABLE", "DeleteCount"),
}

TYPE_SIZE_MAP: dict[str, str] = {
    "UldTypeSize": "_UNLOADED_DRIVERS",
    "RtlAvlTypeSize": "_RTL_AVL_TABLE",
}

# v4-only timer/DPC items. 这些字段不进入旧 v1/v2/v3 fields，
# 通过独立的 v4Items 数组携带结构偏移、类型大小和位域元数据。
V4_FIELD_MAP: dict[str, tuple[str, str]] = {
    "KprcbTimerTable": ("_KPRCB", "TimerTable"),
    "KtimerTableTimerEntries": ("_KTIMER_TABLE", "TimerEntries"),
    "KtimerTableEntryLock": ("_KTIMER_TABLE_ENTRY", "Lock"),
    "KtimerTableEntryEntry": ("_KTIMER_TABLE_ENTRY", "Entry"),
    "KtimerTableEntryTime": ("_KTIMER_TABLE_ENTRY", "Time"),
    "KtimerTimerListEntry": ("_KTIMER", "TimerListEntry"),
    "KtimerDueTime": ("_KTIMER", "DueTime"),
    "KtimerDpc": ("_KTIMER", "Dpc"),
    "KtimerTimerType": ("_KTIMER", "TimerType"),
    "KtimerPeriod": ("_KTIMER", "Period"),
    "KdpcDeferredRoutine": ("_KDPC", "DeferredRoutine"),
    "KdpcDeferredContext": ("_KDPC", "DeferredContext"),
}

# Older public PDBs describe the timer kind through the embedded dispatcher
# header instead of exposing a direct `_KTIMER.TimerType` member.  Keep the
# stable v4 item name while resolving that legacy layout as
# `_KTIMER.Header + _DISPATCHER_HEADER.Type`.
V4_MEMBER_ALIASES: dict[tuple[str, str], tuple[tuple[str, str], tuple[str, str]]] = {
    ("_KTIMER", "TimerType"): (("_KTIMER", "Header"), ("_DISPATCHER_HEADER", "Type")),
}

V4_BIT_FIELD_MAP: dict[str, tuple[str, str]] = {
    "EthActiveExWorker": ("_ETHREAD", "ActiveExWorker"),
}

V4_TYPE_SIZE_MAP: dict[str, str] = {
    "KtimerTableTypeSize": "_KTIMER_TABLE",
    "KtimerTableEntryTypeSize": "_KTIMER_TABLE_ENTRY",
    "KtimerTypeSize": "_KTIMER",
    "KdpcTypeSize": "_KDPC",
}

V4_ITEM_DEFINITIONS: dict[str, tuple[int, str, int]] = {
    "EthActiveExWorker": (1001, "BitField", 2),
    "KprcbTimerTable": (1002, "StructOffset", 2),
    "KtimerTableTimerEntries": (1003, "StructOffset", 2),
    "KtimerTableEntryLock": (1004, "StructOffset", 2),
    "KtimerTableEntryEntry": (1005, "StructOffset", 2),
    "KtimerTableEntryTime": (1006, "StructOffset", 2),
    "KtimerTimerListEntry": (1007, "StructOffset", 2),
    "KtimerDueTime": (1008, "StructOffset", 2),
    "KtimerDpc": (1009, "StructOffset", 2),
    "KtimerTimerType": (1010, "StructOffset", 2),
    "KtimerPeriod": (1011, "StructOffset", 2),
    "KdpcDeferredRoutine": (1012, "StructOffset", 2),
    "KdpcDeferredContext": (1013, "StructOffset", 2),
    "KtimerTableTypeSize": (1014, "TypeSize", 2),
    "KtimerTableEntryTypeSize": (1015, "TypeSize", 2),
    "KtimerTypeSize": (1016, "TypeSize", 2),
    "KdpcTypeSize": (1017, "TypeSize", 2),
}

CALLBACK_GLOBAL_RVA_NAMES: tuple[str, ...] = (
    "PspCreateProcessNotifyRoutine",
    "PspCreateThreadNotifyRoutine",
    "PspLoadImageNotifyRoutine",
    "PspNotifyEnableMask",
    "CmCallbackListHead",
)

KERNEL_GLOBAL_RVA_NAMES: tuple[str, ...] = (
    "PspCidTable",
    "PsLoadedModuleList",
    "MmUnloadedDrivers",
    "PiDDBCacheTable",
    "KeServiceDescriptorTableShadow",
    "MmLastUnloadedDriver",
)

GLOBAL_RVA_SYMBOL_ALIASES: dict[str, tuple[str, ...]] = {
    # Microsoft public ntoskrnl PDBs commonly publish the registry callback
    # list as `CallbackListHead` next to `CmpCallbackListLock`, while KswordARK
    # keeps the shared protocol field name as `CmCallbackListHead`.
    "CmCallbackListHead": ("CmCallbackListHead", "CallbackListHead"),
}

CALLBACK_STRUCT_FIELD_MAP: dict[str, tuple[str, str]] = {
    "_OBJECT_TYPE.CallbackList": ("_OBJECT_TYPE", "CallbackList"),
    "_CALLBACK_ENTRY_ITEM.EntryItemList": ("_CALLBACK_ENTRY_ITEM", "EntryItemList"),
    "_CALLBACK_ENTRY_ITEM.PreOperation": ("_CALLBACK_ENTRY_ITEM", "PreOperation"),
    "_CALLBACK_ENTRY_ITEM.PostOperation": ("_CALLBACK_ENTRY_ITEM", "PostOperation"),
    "_CALLBACK_ENTRY_ITEM.Operations": ("_CALLBACK_ENTRY_ITEM", "Operations"),
    "_CALLBACK_ENTRY_ITEM.CallbackEntry": ("_CALLBACK_ENTRY_ITEM", "CallbackEntry"),
    "_CALLBACK_ENTRY.Altitude": ("_CALLBACK_ENTRY", "Altitude"),
    "_CALLBACK_ENTRY.RegistrationContext": ("_CALLBACK_ENTRY", "RegistrationContext"),
}

TYPE_HEADER_RE = re.compile(r"^\s*([0-9A-Fa-fx]+)\s*\|\s*(LF_[A-Z0-9_]+)\b")
FIELD_LIST_RE = re.compile(r"field list:\s*(?:<fieldlist\s+)?([0-9A-Fa-fx]+)")
TYPE_SIZE_RE = re.compile(r"\bsizeof\s*(?:=\s*)?(0x[0-9A-Fa-f]+|\d+)", re.IGNORECASE)
OFFSET_RE = re.compile(r"offset\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
TYPE_REF_RE = re.compile(r"(?:type|Type)\s*=\s*([^,\]\s]+)")
BIT_OFFSET_RE = re.compile(r"bit offset\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
BIT_COUNT_RE = re.compile(r"# bits\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
SYMBOL_HEADER_RE = re.compile(r"^\s*([0-9A-Fa-fx]+)\s*\|\s*(S_[A-Z0-9_]+)\b.*`([^`]+)`")
SYMBOL_ADDR_RE = re.compile(r"\baddr\s*=\s*([0-9A-Fa-f]+):([0-9A-Fa-f]+)")

CODEVIEW_PRIMITIVE_TYPE_SIZES = {
    "0020": 1,  # unsigned char
    "0021": 2,  # unsigned short
    "0022": 4,  # unsigned long
    "0023": 8,  # unsigned __int64
}


@dataclass(frozen=True)
class KphDynEntry:
    """One PE row used to seed profile generation.

    Inputs:
    - arch/version/file_name/hash/timestamp/size/class_name: Identity values
      from kphdyn.xml or a local PE dry-run.
    - machine: COFF machine type stored in generated JSON for R3/R0 matching.

    Processing:
    - Instances are immutable data carriers; download and validation helpers
      consume them without mutating profile identity.

    Return behavior:
    - The dataclass has no behavior beyond holding normalized PE metadata.
    """

    arch: str
    version: str
    file_name: str
    sha256: str
    timestamp: int
    size_of_image: int
    class_name: str
    machine: int = 0x8664


@dataclass(frozen=True)
class PdbIdentity:
    """CodeView RSDS identity parsed from the PE image.

    Inputs:
    - pdb_name/pdb_guid/pdb_age: Values from the CodeView RSDS record.
    - symbol_key: Microsoft symbol-server key built from GUID without dashes and
      hexadecimal age.

    Processing:
    - Instances are immutable and are reused for both symbol download paths and
      generated JSON module identity.

    Return behavior:
    - The dataclass does not perform I/O.
    """

    pdb_name: str
    pdb_guid: str
    pdb_age: int
    symbol_key: str


@dataclass(frozen=True)
class SymbolAddress:
    """One CodeView section-relative symbol address from llvm-pdbutil output.

    Inputs:
    - name/kind/source: Symbol metadata from a publics or globals dump.
    - section/offset: CodeView address tuple printed as "section:offset".

    Processing:
    - The address is converted to an RVA later using the PE section table so the
      parser does not rely on optional PDB section-header formatting.

    Return behavior:
    - Instances are immutable data records.
    """

    name: str
    kind: str
    source: str
    section: int
    offset: int


def parse_int(text: str) -> int:
    """Parse decimal or hex text and return an integer."""
    value = text.strip()
    return int(value, 16 if value.lower().startswith("0x") else 10)


def parse_codeview_addr_int(text: str) -> int:
    """Parse one llvm-pdbutil section:offset integer token.

    Inputs:
    - text: The section or offset token from an "addr = section:offset" line.

    Processing:
    - llvm-pdbutil normally prints these tokens as decimal values with leading
      zero padding, for example "0006:345376".
    - Some future/older formats may include hexadecimal letters or a 0x prefix,
      so the helper switches to base 16 only when the token clearly requires it.

    Return behavior:
    - Returns an integer or raises ValueError for malformed text.
    """
    value = text.strip()
    if value.lower().startswith("0x") or re.search(r"[A-Fa-f]", value):
        return int(value, 16)
    return int(value, 10)


def normalize_record_id(record_id: str) -> str:
    """Normalize llvm-pdbutil type IDs for stable comparisons."""
    return record_id.strip().lower().removeprefix("0x")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse CLI options and return argparse namespace.

    Inputs:
    - argv: Optional command-line argument list for tests; None uses sys.argv.

    Processing:
    - Keeps the existing kphdyn-driven corpus workflow as the default mode.
    - Adds an explicit local dry-run mode for one PE/PDB pair so callback item
      parsing can be validated without downloads, Release writes, or corpus
      refreshes.

    Return behavior:
    - Returns argparse.Namespace with normalized option values only; no I/O is
      performed by this helper.
    """
    parser = argparse.ArgumentParser(description="Generate KswordARK PDB offset profiles")
    parser.add_argument("--kphdyn", default="third_party/systeminformer_dyn/kphdyn.xml", help="Path to kphdyn.xml")
    parser.add_argument("--symbol-root", default=DEFAULT_SYMBOL_ROOT, help="Corpus/cache root")
    parser.add_argument("--symbol-server", default=DEFAULT_SYMBOL_SERVER, help="Microsoft symbol server base URL")
    parser.add_argument("--llvm-pdbutil", default=DEFAULT_LLVM_PDBUTIL, help="Path to llvm-pdbutil.exe")
    parser.add_argument("--arch", default="amd64", help="kphdyn arch filter")
    parser.add_argument("--version", default=None, help="Optional version prefix filter, e.g. 10.0.26100")
    parser.add_argument("--limit", type=int, default=0, help="Stop after N generated profiles; 0 means no limit")
    parser.add_argument("--workers", type=int, default=1, help="Parallel profile workers; use 1 for deterministic diagnostics")
    parser.add_argument("--skip-existing", action="store_true", help="Do not regenerate existing JSON profiles")
    parser.add_argument(
        "--refresh-v4-existing",
        action="store_true",
        help="Refresh only v4 timer/DPC items in existing JSON profiles; skip symbol dumps",
    )
    parser.add_argument("--offline", action="store_true", help="Use only cached PE/PDB files and fail fast when an artifact is absent")
    parser.add_argument("--file", default="ntoskrnl.exe,ntkrla57.exe", help="Comma-separated PE file names")
    parser.add_argument("--dry-run", action="store_true", help="Process exactly one --local-pe/--local-pdb pair without symbol downloads")
    parser.add_argument("--local-pe", default="", help="Local PE path for dry-run validation")
    parser.add_argument("--local-pdb", default="", help="Local PDB path for dry-run validation")
    parser.add_argument(
        "--output",
        default="",
        help=r"Optional JSON output path for dry-run; use D:\PDB\scratch or another temp directory",
    )
    return parser.parse_args(argv)


def module_class_for_file(file_name: str) -> str:
    """Map a PE file name to the profile class consumed by KswordARK."""
    lowered = file_name.lower()
    if lowered == "ntkrla57.exe":
        return "ntkrla57"
    return "ntoskrnl"


def iter_kphdyn_entries(path: Path, arch: str, version: str | None, files: set[str]) -> Iterable[KphDynEntry]:
    """Yield matching kphdyn.xml rows with complete PE identity attributes."""
    tree = ET.parse(path)
    root = tree.getroot()
    for data in root.findall("data"):
        file_name = (data.get("file") or "").strip()
        if file_name.lower() not in files:
            continue
        row_arch = (data.get("arch") or "").strip()
        if arch and row_arch != arch:
            continue
        row_version = (data.get("version") or "").strip()
        if version and not row_version.startswith(version):
            continue
        sha256 = (data.get("hash") or "").strip().lower()
        timestamp = (data.get("timestamp") or "").strip()
        size_of_image = (data.get("size") or "").strip()
        if not sha256 or not timestamp or not size_of_image:
            continue
        yield KphDynEntry(
            arch=row_arch,
            version=row_version,
            file_name=file_name,
            sha256=sha256,
            timestamp=parse_int(timestamp),
            size_of_image=parse_int(size_of_image),
            class_name=module_class_for_file(file_name),
        )


def pe_symbol_key(entry: KphDynEntry) -> str:
    """Build the public symbol-server key for a PE image."""
    return f"{entry.timestamp:08X}{entry.size_of_image:X}"


def download_file(url: str, destination: Path) -> bool:
    """Download one URL atomically and return True when a new file was written."""
    if destination.exists() and destination.stat().st_size > 0:
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = destination.with_suffix(destination.suffix + ".tmp")
    response = requests.get(url, timeout=HTTP_TIMEOUT_SECONDS)
    response.raise_for_status()
    tmp_path.write_bytes(response.content)
    tmp_path.replace(destination)
    return True


def sha256_file(path: Path) -> str:
    """Hash a file and return lowercase SHA256 hex."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json_atomic(path: Path, payload: dict[str, object]) -> None:
    """Write JSON through a process-local temporary file and atomically replace it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        temporary_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary_path, path)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass


def pe_machine_to_arch(machine: int) -> str:
    """Map a COFF machine value to the profile architecture string.

    Inputs:
    - machine: IMAGE_FILE_HEADER.Machine from a PE image.

    Processing:
    - Recognizes the architectures the generator can reasonably label today.
    - Uses a stable hexadecimal fallback so dry-run output remains explicit for
      unexpected machine values.

    Return behavior:
    - Returns a short architecture string and performs no I/O.
    """
    if machine == 0x8664:
        return "amd64"
    if machine == 0xAA64:
        return "arm64"
    if machine == 0x014C:
        return "x86"
    return f"machine_0x{machine:04X}"


def file_version_from_pe(pe: pefile.PE) -> str:
    """Extract a best-effort file version string from a parsed PE.

    Inputs:
    - pe: pefile.PE instance that may contain VERSIONINFO resources.

    Processing:
    - First checks StringFileInfo because it preserves the human-readable build
      number used by existing corpus file names.
    - Falls back to VS_FIXEDFILEINFO numeric components when string metadata is
      absent.

    Return behavior:
    - Returns a non-empty version string; "0.0.0.0" indicates no version data.
    """
    for file_info in getattr(pe, "FileInfo", []) or []:
        for child in getattr(file_info, "StringTable", []) or []:
            entries = getattr(child, "entries", {}) or {}
            for key, value in entries.items():
                key_text = key.decode("utf-8", errors="ignore") if isinstance(key, bytes) else str(key)
                if key_text.lower() != "fileversion":
                    continue
                value_text = value.decode("utf-8", errors="ignore") if isinstance(value, bytes) else str(value)
                normalized = value_text.replace(",", ".").strip()
                if normalized:
                    return normalized
    fixed_info = getattr(pe, "VS_FIXEDFILEINFO", []) or []
    if fixed_info:
        info = fixed_info[0]
        return (
            f"{info.FileVersionMS >> 16}.{info.FileVersionMS & 0xFFFF}."
            f"{info.FileVersionLS >> 16}.{info.FileVersionLS & 0xFFFF}"
        )
    return "0.0.0.0"


def entry_from_local_pe(pe_path: Path) -> KphDynEntry:
    """Build a KphDynEntry from one local PE for dry-run validation.

    Inputs:
    - pe_path: Existing PE image path supplied by --local-pe.

    Processing:
    - Reads COFF identity fields, SizeOfImage, file version, SHA256, and module
      class without downloading or writing corpus files.

    Return behavior:
    - Returns a KphDynEntry suitable for build_profile; raises pefile/OSError
      exceptions if the PE cannot be parsed.
    """
    pe = pefile.PE(str(pe_path), fast_load=False)
    try:
        machine = int(pe.FILE_HEADER.Machine)
        file_name = pe_path.name
        return KphDynEntry(
            arch=pe_machine_to_arch(machine),
            version=file_version_from_pe(pe),
            file_name=file_name,
            sha256=sha256_file(pe_path),
            timestamp=int(pe.FILE_HEADER.TimeDateStamp),
            size_of_image=int(pe.OPTIONAL_HEADER.SizeOfImage),
            class_name=module_class_for_file(file_name),
            machine=machine,
        )
    finally:
        pe.close()


def rva_from_section_offset(pe_path: Path, section_number: int, section_offset: int) -> int | None:
    """Convert a CodeView section:offset tuple to a PE RVA.

    Inputs:
    - pe_path: PE image whose section table corresponds to the PDB.
    - section_number: One-based section index from llvm-pdbutil symbol output.
    - section_offset: Offset within that section from llvm-pdbutil.

    Processing:
    - Uses the PE section table, not PDB section-header text, because PE parsing
      is already required for module identity and is less sensitive to
      llvm-pdbutil formatting changes.
    - Rejects section number zero, indexes outside the PE section table, and
      offsets beyond the larger of VirtualSize and SizeOfRawData.

    Return behavior:
    - Returns an integer RVA on success; returns None when the address cannot be
      safely mapped.
    """
    if section_number <= 0:
        return None
    pe = pefile.PE(str(pe_path), fast_load=True)
    try:
        if section_number > len(pe.sections):
            return None
        section = pe.sections[section_number - 1]
        section_span = max(int(section.Misc_VirtualSize), int(section.SizeOfRawData))
        if section_offset < 0 or section_offset > section_span:
            return None
        return int(section.VirtualAddress) + section_offset
    finally:
        pe.close()


def pe_path_for_entry(root: Path, entry: KphDynEntry) -> Path:
    """Return the local corpus path for a PE entry."""
    nested_path = root / "pe-store" / entry.arch / entry.file_name / f"{entry.file_name}.{entry.version}" / entry.sha256 / entry.file_name
    legacy_path = root / "pe-store" / entry.arch / f"{entry.file_name}.{entry.version}" / entry.sha256 / entry.file_name
    if nested_path.exists() or not legacy_path.exists():
        return nested_path
    return legacy_path


def pdb_path_for_identity(root: Path, entry: KphDynEntry, pdb: PdbIdentity) -> Path:
    """Return the local corpus path for a PDB identity."""
    return root / "pdb-cache" / entry.arch / pdb.pdb_name / pdb.symbol_key / pdb.pdb_name


def profile_path_for_entry(root: Path, entry: KphDynEntry, pdb: PdbIdentity) -> Path:
    """Return the output JSON profile path for a PE/PDB identity."""
    safe_version = entry.version.replace(".", "_")
    safe_key = pdb.symbol_key.lower()
    return root / "profiles" / "ark_dyndata" / f"{entry.class_name}_{entry.arch}_{safe_version}_{safe_key}.json"


def parse_rsds_identity(pe_path: Path) -> PdbIdentity:
    """Parse CodeView RSDS data from a PE and return PDB name/GUID/age."""
    pe = pefile.PE(str(pe_path), fast_load=False)
    try:
        if not hasattr(pe, "DIRECTORY_ENTRY_DEBUG"):
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"]])
        for debug_entry in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
            if debug_entry.struct.Type != 2:
                continue
            offset = debug_entry.struct.PointerToRawData
            size = debug_entry.struct.SizeOfData
            data = pe.__data__[offset : offset + size]
            if len(data) < 24 or data[:4] != b"RSDS":
                continue
            guid_bytes = data[4:20]
            age = int.from_bytes(data[20:24], "little")
            pdb_bytes = data[24:].split(b"\x00", 1)[0]
            pdb_name = Path(pdb_bytes.decode("utf-8", errors="replace")).name
            guid_text = (
                f"{int.from_bytes(guid_bytes[0:4], 'little'):08X}-"
                f"{int.from_bytes(guid_bytes[4:6], 'little'):04X}-"
                f"{int.from_bytes(guid_bytes[6:8], 'little'):04X}-"
                f"{guid_bytes[8]:02X}{guid_bytes[9]:02X}-"
                f"{''.join(f'{byte:02X}' for byte in guid_bytes[10:16])}"
            )
            symbol_key_text = guid_text.replace("-", "") + f"{age:X}"
            return PdbIdentity(pdb_name=pdb_name, pdb_guid=guid_text, pdb_age=age, symbol_key=symbol_key_text)
    finally:
        pe.close()
    raise RuntimeError(f"No RSDS CodeView record found in {pe_path}")


def run_llvm_pdbutil_dump(pdb_path: Path, pdbutil_path: str, *options: str) -> str:
    """Run one llvm-pdbutil dump command and return stdout text.

    Inputs:
    - pdb_path: PDB file to inspect.
    - pdbutil_path: llvm-pdbutil executable path or command name.
    - options: Dump options such as -types, -globals, or -publics.

    Processing:
    - Invokes llvm-pdbutil with a fixed timeout and captures stdout/stderr.
    - Leaves error handling to callers so required type parsing can fail loudly
      while optional callback symbol parsing can degrade into diagnostics.

    Return behavior:
    - Returns stdout text on success; subprocess exceptions propagate.
    """
    completed = subprocess.run(
        [pdbutil_path, "dump", *options, str(pdb_path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=PDBUTIL_TIMEOUT_SECONDS,
    )
    return completed.stdout


def run_llvm_pdbutil_types(pdb_path: Path, pdbutil_path: str) -> str:
    """Run llvm-pdbutil dump -types and return stdout text.

    Inputs:
    - pdb_path/pdbutil_path: Required PDB and llvm-pdbutil executable.

    Processing:
    - Delegates to run_llvm_pdbutil_dump with -types because legacy fields and
      callback struct offsets both depend on TPI records.

    Return behavior:
    - Returns the raw type dump text; raises if llvm-pdbutil fails.
    """
    return run_llvm_pdbutil_dump(pdb_path, pdbutil_path, "-types")


def find_field_list_ids(lines: list[str], struct_name: str) -> list[str]:
    """Find LF_FIELDLIST records referenced by concrete definitions.

    Inputs:
    - lines: Split `llvm-pdbutil dump -types` output.
    - struct_name: Kernel type name whose direct member list is required.

    Processing:
    - Accepts structures/classes and unions because Windows kernel PDBs expose
      `_HANDLE_TABLE_ENTRY` as LF_UNION while other DynData records are normally
      LF_STRUCTURE.
    - Skips forward references so the returned field list belongs to a concrete
      definition that can be used for offset extraction.

    Return behavior:
    - Returns every referenced LF_FIELDLIST id in PDB order. Some kernels carry
      both a shortened and a full `_KPRCB` definition, so callers must inspect
      all concrete definitions instead of trusting the first one.
    """
    allowed = {"LF_STRUCTURE", "LF_STRUCTURE2", "LF_CLASS", "LF_CLASS2", "LF_UNION", "LF_UNION2"}
    result: list[str] = []
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header:
            continue
        _record_id, kind = header.groups()
        if kind not in allowed or f"`{struct_name}`" not in line or "forward ref" in line:
            continue
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            if "forward ref" in body_line:
                break
            match = FIELD_LIST_RE.search(body_line)
            if match:
                normalized = normalize_record_id(match.group(1))
                if all(normalize_record_id(item) != normalized for item in result):
                    result.append(match.group(1))
                break
    return result


def find_field_list_id(lines: list[str], struct_name: str) -> str | None:
    """Return the first concrete field list for legacy callers."""
    field_lists = find_field_list_ids(lines, struct_name)
    return field_lists[0] if field_lists else None


def resolve_type_size(types_text: str, struct_name: str) -> int | None:
    """Resolve a concrete structure size from llvm-pdbutil type text.

    Inputs:
    - types_text: Full `llvm-pdbutil dump -types` output.
    - struct_name: Concrete type name such as `_UNLOADED_DRIVERS`.

    Processing:
    - Finds the non-forward LF_STRUCTURE/LF_CLASS record and reads its size
      field from the header/body text. The parser stays conservative: missing
      or unrecognized size text returns None instead of guessing.

    Return behavior:
    - Returns the type size in bytes on success; returns None when unavailable.
    """
    allowed = {"LF_STRUCTURE", "LF_STRUCTURE2", "LF_CLASS", "LF_CLASS2"}
    lines = types_text.splitlines()
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header:
            continue
        _record_id, kind = header.groups()
        if kind not in allowed or f"`{struct_name}`" not in line or "forward ref" in line:
            continue
        search_lines = [line]
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            if "forward ref" in body_line:
                break
            search_lines.append(body_line)
        for candidate in search_lines:
            match = TYPE_SIZE_RE.search(candidate)
            if match:
                return parse_int(match.group(1))
    return None


def field_list_lines(lines: list[str], field_list_id: str) -> list[str]:
    """Return the body lines of one LF_FIELDLIST record."""
    wanted = normalize_record_id(field_list_id)
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header:
            continue
        record_id, kind = header.groups()
        if kind != "LF_FIELDLIST" or normalize_record_id(record_id) != wanted:
            continue
        result: list[str] = []
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            result.append(body_line)
        return result
    return []


def member_name_from_line(line: str) -> str | None:
    """Extract an LF_MEMBER name from one llvm-pdbutil output line."""
    match = re.search(r"member name = `([^`]+)`", line)
    if match:
        return match.group(1)
    match = re.search(r"name\s*=\s*`([^`]+)`", line)
    return match.group(1) if match else None


def resolve_member_offset(types_text: str, struct_name: str, member_name: str) -> int | None:
    """Resolve one direct structure member offset from llvm-pdbutil type dump text."""
    lines = types_text.splitlines()
    for field_list in find_field_list_ids(lines, struct_name):
        for line in field_list_lines(lines, field_list):
            if "LF_MEMBER" not in line or member_name_from_line(line) != member_name:
                continue
            match = OFFSET_RE.search(line)
            if match:
                return parse_int(match.group(1))
    return None


def resolve_bit_field(types_text: str, struct_name: str, member_name: str) -> tuple[int, int, int, int] | None:
    """Resolve one direct LF_BITFIELD member.

    Returns (byte_offset, bit_offset, bit_count, storage_bytes). Missing or
    non-primitive storage types are rejected instead of being guessed.
    """
    lines = types_text.splitlines()
    member_offset: int | None = None
    bit_field_type_id = ""
    for field_list in find_field_list_ids(lines, struct_name):
        for line in field_list_lines(lines, field_list):
            if "LF_MEMBER" not in line or member_name_from_line(line) != member_name:
                continue
            offset_match = OFFSET_RE.search(line)
            type_match = TYPE_REF_RE.search(line)
            if offset_match and type_match:
                member_offset = parse_int(offset_match.group(1))
                bit_field_type_id = normalize_record_id(type_match.group(1))
            break
        if member_offset is not None:
            break
    if member_offset is None or not bit_field_type_id:
        return None

    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header:
            continue
        record_id, kind = header.groups()
        if kind != "LF_BITFIELD" or normalize_record_id(record_id) != bit_field_type_id:
            continue

        body_lines = [line]
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            body_lines.append(body_line)
        body_text = " ".join(body_lines)
        storage_match = TYPE_REF_RE.search(body_text)
        bit_offset_match = BIT_OFFSET_RE.search(body_text)
        bit_count_match = BIT_COUNT_RE.search(body_text)
        if not storage_match or not bit_offset_match or not bit_count_match:
            return None
        storage_bytes = CODEVIEW_PRIMITIVE_TYPE_SIZES.get(normalize_record_id(storage_match.group(1)))
        if storage_bytes is None:
            return None
        return (
            member_offset,
            parse_int(bit_offset_match.group(1)),
            parse_int(bit_count_match.group(1)),
            storage_bytes,
        )
    return None


def resolve_member_offsets_batch(
    types_text: str,
    requests: Iterable[tuple[str, str]],
) -> dict[tuple[str, str], int]:
    """Resolve many member offsets while indexing each structure only once."""
    lines = types_text.splitlines()
    members_by_struct: dict[str, set[str]] = {}
    for struct_name, member_name in requests:
        members_by_struct.setdefault(struct_name, set()).add(member_name)

    resolved: dict[tuple[str, str], int] = {}
    for struct_name, wanted_members in members_by_struct.items():
        for field_list in find_field_list_ids(lines, struct_name):
            for line in field_list_lines(lines, field_list):
                if "LF_MEMBER" not in line:
                    continue
                member_name = member_name_from_line(line)
                if member_name not in wanted_members or (struct_name, member_name) in resolved:
                    continue
                offset_match = OFFSET_RE.search(line)
                if offset_match:
                    resolved[(struct_name, member_name)] = parse_int(offset_match.group(1))
    return resolved


def resolve_type_sizes_batch(types_text: str, struct_names: Iterable[str]) -> dict[str, int]:
    """Resolve concrete type sizes with one scan of the PDB type stream."""
    wanted = set(struct_names)
    resolved: dict[str, int] = {}
    allowed = {"LF_STRUCTURE", "LF_STRUCTURE2", "LF_CLASS", "LF_CLASS2"}
    lines = types_text.splitlines()
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header or header.group(2) not in allowed or "forward ref" in line:
            continue
        name_match = re.search(r"`([^`]+)`", line)
        if not name_match:
            continue
        struct_name = name_match.group(1)
        if struct_name not in wanted or struct_name in resolved:
            continue
        body_lines = [line]
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            if "forward ref" in body_line:
                body_lines = []
                break
            body_lines.append(body_line)
        for candidate in body_lines:
            size_match = TYPE_SIZE_RE.search(candidate)
            if size_match:
                resolved[struct_name] = parse_int(size_match.group(1))
                break
    return resolved


def resolve_bit_fields_batch(
    types_text: str,
    requests: Iterable[tuple[str, str]],
) -> dict[tuple[str, str], tuple[int, int, int, int]]:
    """Resolve many LF_BITFIELD members with one bit-record index."""
    lines = types_text.splitlines()
    wanted = set(requests)
    member_records: dict[tuple[str, str], tuple[int, str]] = {}
    for struct_name in sorted({request[0] for request in wanted}):
        for field_list in find_field_list_ids(lines, struct_name):
            for line in field_list_lines(lines, field_list):
                member_name = member_name_from_line(line)
                key = (struct_name, member_name or "")
                if "LF_MEMBER" not in line or key not in wanted or key in member_records:
                    continue
                offset_match = OFFSET_RE.search(line)
                type_match = TYPE_REF_RE.search(line)
                if offset_match and type_match:
                    member_records[key] = (parse_int(offset_match.group(1)), normalize_record_id(type_match.group(1)))

    bit_records: dict[str, tuple[int, int, int]] = {}
    wanted_type_ids = {record[1] for record in member_records.values()}
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header or header.group(2) != "LF_BITFIELD":
            continue
        type_id = normalize_record_id(header.group(1))
        if type_id not in wanted_type_ids:
            continue
        body_lines = [line]
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            body_lines.append(body_line)
        body_text = " ".join(body_lines)
        storage_match = TYPE_REF_RE.search(body_text)
        bit_offset_match = BIT_OFFSET_RE.search(body_text)
        bit_count_match = BIT_COUNT_RE.search(body_text)
        if not storage_match or not bit_offset_match or not bit_count_match:
            continue
        storage_bytes = CODEVIEW_PRIMITIVE_TYPE_SIZES.get(normalize_record_id(storage_match.group(1)))
        if storage_bytes is not None:
            bit_records[type_id] = (parse_int(bit_offset_match.group(1)), parse_int(bit_count_match.group(1)), storage_bytes)

    resolved: dict[tuple[str, str], tuple[int, int, int, int]] = {}
    for key, (member_offset, type_id) in member_records.items():
        bit_record = bit_records.get(type_id)
        if bit_record is not None:
            resolved[key] = (member_offset, bit_record[0], bit_record[1], bit_record[2])
    return resolved


def resolve_type_layouts_batch(
    types_text: str,
    member_requests: Iterable[tuple[str, str]],
    type_size_requests: Iterable[str],
    bit_field_requests: Iterable[tuple[str, str]],
) -> tuple[
    dict[tuple[str, str], int],
    dict[str, int],
    dict[tuple[str, str], tuple[int, int, int, int]],
]:
    """Index one llvm-pdbutil type dump and resolve all requested layouts."""
    lines = types_text.splitlines()
    records: dict[str, tuple[str, str, list[str]]] = {}
    index = 0
    while index < len(lines):
        header = TYPE_HEADER_RE.match(lines[index])
        if not header:
            index += 1
            continue
        record_id, kind = header.groups()
        body: list[str] = []
        next_index = index + 1
        while next_index < len(lines) and not TYPE_HEADER_RE.match(lines[next_index]):
            body.append(lines[next_index])
            next_index += 1
        records[normalize_record_id(record_id)] = (kind, lines[index], body)
        index = next_index

    field_lists: dict[str, list[str]] = {
        record_id: body
        for record_id, (kind, _header_line, body) in records.items()
        if kind == "LF_FIELDLIST"
    }
    concrete_kinds = {"LF_STRUCTURE", "LF_STRUCTURE2", "LF_CLASS", "LF_CLASS2", "LF_UNION", "LF_UNION2"}
    struct_field_lists: dict[str, list[str]] = {}
    resolved_type_sizes: dict[str, int] = {}
    wanted_type_sizes = set(type_size_requests)
    for _record_id, (kind, header_line, body) in records.items():
        if kind not in concrete_kinds:
            continue
        record_text = " ".join([header_line, *body])
        if "forward ref" in record_text:
            continue
        name_match = re.search(r"`([^`]+)`", header_line)
        if not name_match:
            continue
        struct_name = name_match.group(1)
        field_list_match = FIELD_LIST_RE.search(record_text)
        if field_list_match:
            field_list_id = normalize_record_id(field_list_match.group(1))
            if field_list_id in field_lists and field_list_id not in struct_field_lists.setdefault(struct_name, []):
                struct_field_lists[struct_name].append(field_list_id)
        if struct_name in wanted_type_sizes and struct_name not in resolved_type_sizes:
            size_match = TYPE_SIZE_RE.search(record_text)
            if size_match:
                resolved_type_sizes[struct_name] = parse_int(size_match.group(1))

    wanted_members: dict[str, set[str]] = {}
    for struct_name, member_name in member_requests:
        wanted_members.setdefault(struct_name, set()).add(member_name)
    resolved_offsets: dict[tuple[str, str], int] = {}
    member_type_ids: dict[tuple[str, str], str] = {}
    wanted_bit_fields = set(bit_field_requests)
    for struct_name, member_names in wanted_members.items():
        for field_list_id in struct_field_lists.get(struct_name, []):
            for line in field_lists.get(field_list_id, []):
                if "LF_MEMBER" not in line:
                    continue
                member_name = member_name_from_line(line)
                if member_name not in member_names:
                    continue
                key = (struct_name, member_name)
                offset_match = OFFSET_RE.search(line)
                if offset_match and key not in resolved_offsets:
                    resolved_offsets[key] = parse_int(offset_match.group(1))
                if key in wanted_bit_fields and key not in member_type_ids:
                    type_match = TYPE_REF_RE.search(line)
                    if type_match:
                        member_type_ids[key] = normalize_record_id(type_match.group(1))

    resolved_bit_fields: dict[tuple[str, str], tuple[int, int, int, int]] = {}
    for key, type_id in member_type_ids.items():
        record = records.get(type_id)
        if record is None or record[0] != "LF_BITFIELD" or key not in resolved_offsets:
            continue
        record_text = " ".join([record[1], *record[2]])
        storage_match = TYPE_REF_RE.search(record_text)
        bit_offset_match = BIT_OFFSET_RE.search(record_text)
        bit_count_match = BIT_COUNT_RE.search(record_text)
        if not storage_match or not bit_offset_match or not bit_count_match:
            continue
        storage_bytes = CODEVIEW_PRIMITIVE_TYPE_SIZES.get(normalize_record_id(storage_match.group(1)))
        if storage_bytes is None:
            continue
        resolved_bit_fields[key] = (
            resolved_offsets[key],
            parse_int(bit_offset_match.group(1)),
            parse_int(bit_count_match.group(1)),
            storage_bytes,
        )
    return resolved_offsets, resolved_type_sizes, resolved_bit_fields


def parse_symbol_addresses(symbols_text: str, source: str) -> list[SymbolAddress]:
    """Parse CodeView symbol addresses from llvm-pdbutil text.

    Inputs:
    - symbols_text: Output from llvm-pdbutil dump -globals or -publics.
    - source: Human-readable source label stored with parsed addresses.

    Processing:
    - Tracks symbol header records such as S_PUB32 and their following addr line.
    - Does not filter by candidate name here; exact matching is handled by the
      callback builder so the parser remains reusable across item sets.

    Return behavior:
    - Returns every parsed section-relative symbol address. Missing addr lines
      are ignored because not every symbol record carries an address.
    """
    addresses: list[SymbolAddress] = []
    pending: tuple[str, str] | None = None
    for line in symbols_text.splitlines():
        header = SYMBOL_HEADER_RE.match(line)
        if header:
            _record_offset, symbol_kind, symbol_name = header.groups()
            pending = (symbol_kind, symbol_name)
            inline_address = SYMBOL_ADDR_RE.search(line)
            if inline_address:
                section_text, offset_text = inline_address.groups()
                addresses.append(
                    SymbolAddress(
                        name=symbol_name,
                        kind=symbol_kind,
                        source=source,
                        section=parse_codeview_addr_int(section_text),
                        offset=parse_codeview_addr_int(offset_text),
                    )
                )
                pending = None
            continue
        if pending is None:
            continue
        address = SYMBOL_ADDR_RE.search(line)
        if not address:
            continue
        section_text, offset_text = address.groups()
        symbol_kind, symbol_name = pending
        addresses.append(
            SymbolAddress(
                name=symbol_name,
                kind=symbol_kind,
                source=source,
                section=parse_codeview_addr_int(section_text),
                offset=parse_codeview_addr_int(offset_text),
            )
        )
        pending = None
    return addresses


def collect_symbol_addresses(pdb_path: Path, pdbutil_path: str) -> tuple[dict[str, list[SymbolAddress]], list[dict[str, str]]]:
    """Collect optional globals/publics addresses from one PDB.

    Inputs:
    - pdb_path/pdbutil_path: PDB and llvm-pdbutil executable used for callback
      global RVA candidates.

    Processing:
    - Attempts both -globals and -publics because Microsoft public kernel PDBs
      commonly expose data symbols through the public stream while globals may
      be empty.
    - Treats dump failures as diagnostics, not hard failures, because callback
      items are optional extensions over the legacy field profile.

    Return behavior:
    - Returns (addresses_by_name, dump_failures). The first value maps exact
      symbol names to one or more parsed addresses; the second is JSON-ready
      diagnostic metadata.
    """
    addresses_by_name: dict[str, list[SymbolAddress]] = {}
    dump_failures: list[dict[str, str]] = []
    for option, source in (("-globals", "globals"), ("-publics", "publics")):
        try:
            dump_text = run_llvm_pdbutil_dump(pdb_path, pdbutil_path, option)
        except Exception as exc:  # noqa: BLE001 - optional diagnostics preserve any tool failure.
            dump_failures.append({"source": source, "error": str(exc)})
            continue
        for address in parse_symbol_addresses(dump_text, source):
            addresses_by_name.setdefault(address.name, []).append(address)
    return addresses_by_name, dump_failures


def choose_symbol_address(addresses: list[SymbolAddress]) -> SymbolAddress:
    """Choose a deterministic address when a symbol appears in multiple streams.

    Inputs:
    - addresses: Exact-name matches from globals and/or publics.

    Processing:
    - Prefers globals over publics when both are available because globals carry
      richer private-symbol intent in full PDBs.
    - Uses section and offset as stable tie-breakers to avoid nondeterministic
      JSON output.

    Return behavior:
    - Returns one SymbolAddress. Callers must pass a non-empty list.
    """
    source_rank = {"globals": 0, "publics": 1}
    return sorted(addresses, key=lambda item: (source_rank.get(item.source, 9), item.section, item.offset))[0]


def build_global_rva_items(
    pe_path: Path,
    symbol_addresses: dict[str, list[SymbolAddress]],
    symbol_names: Iterable[str],
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """Resolve optional global symbols into JSON GlobalRva items.

    Inputs:
    - pe_path: PE image used to convert CodeView section:offset addresses to
      RVAs.
    - symbol_addresses: Exact-name symbol address map from globals/publics.
    - symbol_names: Candidate global symbol names to resolve.

    Processing:
    - Looks up each requested symbol by exact name, with narrow aliases for
      known public-PDB naming differences.
    - Chooses a deterministic address when both globals and publics expose a
      symbol.
    - Converts the chosen section-relative address to an RVA with the PE section
      table.

    Return behavior:
    - Returns (items, missing_globals). items are JSON-ready GlobalRva records;
      missing_globals contains JSON-ready diagnostics and does not make profile
      generation fail.
    """
    items: list[dict[str, Any]] = []
    missing_globals: list[dict[str, str]] = []

    for symbol_name in symbol_names:
        symbol_candidates = GLOBAL_RVA_SYMBOL_ALIASES.get(symbol_name, (symbol_name,))
        matched_symbol_name = symbol_candidates[0]
        matches: list[SymbolAddress] = []
        for candidate_name in symbol_candidates:
            candidate_matches = symbol_addresses.get(candidate_name, [])
            if candidate_matches:
                matched_symbol_name = candidate_name
                matches = candidate_matches
                break
        if not matches:
            missing_globals.append({"kind": "GlobalRva", "name": symbol_name, "reason": "symbol_not_found"})
            continue
        address = choose_symbol_address(matches)
        rva = rva_from_section_offset(pe_path, address.section, address.offset)
        if rva is None:
            missing_globals.append(
                {
                    "kind": "GlobalRva",
                    "name": symbol_name,
                    "reason": "section_offset_unmapped",
                    "section": f"0x{address.section:04X}",
                    "sectionOffset": f"0x{address.offset:X}",
                }
            )
            continue
        items.append(
            {
                "kind": "GlobalRva",
                "name": symbol_name,
                "value": f"0x{rva:08X}",
                "source": address.source,
                "symbolKind": address.kind,
                "sourceSymbol": matched_symbol_name,
                "section": f"0x{address.section:04X}",
                "sectionOffset": f"0x{address.offset:X}",
            }
        )

    return items, missing_globals


def build_callback_items(
    types_text: str,
    pe_path: Path,
    symbol_addresses: dict[str, list[SymbolAddress]],
    symbol_dump_failures: list[dict[str, str]],
    resolved_member_offsets: dict[tuple[str, str], int] | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Build callback v2 item payload and diagnostics.

    Inputs:
    - types_text: llvm-pdbutil -types output used for struct offset candidates.
    - pe_path: PE image used to convert CodeView section:offset symbols to RVA.
    - symbol_addresses: Exact-name symbol address map from globals/publics.
    - symbol_dump_failures: Optional dump failure records from collection.

    Processing:
    - Resolves GlobalRva candidates from publics/globals and maps them to image
      RVAs using the PE section table.
    - Resolves StructOffset candidates from the same type dump used by legacy
      fields.
    - Records every missing or unmappable candidate in diagnostics instead of
      failing profile generation.

    Return behavior:
    - Returns (callback_items, diagnostics). callback_items is JSON-ready and
      diagnostics contains missingItems plus optional symbolDumpFailures.
    """
    callback_items, missing_items = build_global_rva_items(
        pe_path=pe_path,
        symbol_addresses=symbol_addresses,
        symbol_names=CALLBACK_GLOBAL_RVA_NAMES,
    )

    for item_name, (struct_name, member_name) in CALLBACK_STRUCT_FIELD_MAP.items():
        offset = (resolved_member_offsets or {}).get((struct_name, member_name))
        if offset is None and resolved_member_offsets is None:
            offset = resolve_member_offset(types_text, struct_name, member_name)
        if offset is None:
            missing_items.append(
                {
                    "kind": "StructOffset",
                    "name": item_name,
                    "reason": "member_not_found",
                    "structName": struct_name,
                    "memberName": member_name,
                }
            )
            continue
        callback_items.append(
            {
                "kind": "StructOffset",
                "name": item_name,
                "value": f"0x{offset:04X}",
                "structName": struct_name,
                "memberName": member_name,
            }
        )

    diagnostics: dict[str, Any] = {
        "missingItems": missing_items,
    }
    if symbol_dump_failures:
        diagnostics["symbolDumpFailures"] = symbol_dump_failures
    return callback_items, diagnostics


def build_v4_items(
    types_text: str,
    resolved_member_offsets: dict[tuple[str, str], int] | None = None,
    resolved_type_sizes: dict[str, int] | None = None,
    resolved_bit_fields: dict[tuple[str, str], tuple[int, int, int, int]] | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """Build stable timer/DPC v4 items from one PDB type dump."""
    items: list[dict[str, Any]] = []
    missing: list[dict[str, str]] = []
    for item_name, (item_id, item_kind, group_id) in V4_ITEM_DEFINITIONS.items():
        if item_name in V4_FIELD_MAP:
            struct_name, member_name = V4_FIELD_MAP[item_name]
            offset = (resolved_member_offsets or {}).get((struct_name, member_name))
            if offset is None and resolved_member_offsets is None:
                offset = resolve_member_offset(types_text, struct_name, member_name)
            if offset is None:
                missing.append({"kind": item_kind, "name": item_name, "reason": "member_not_found", "structName": struct_name, "memberName": member_name})
                continue
            item: dict[str, Any] = {
                "itemId": item_id,
                "name": item_name,
                "kind": item_kind,
                "flags": "required",
                "capabilityGroupId": group_id,
                "value": f"0x{offset:04X}",
                "aux0": 0,
                "aux1": 0,
                "aux2": 0,
                "aux3": 0,
            }
        elif item_name in V4_BIT_FIELD_MAP:
            struct_name, member_name = V4_BIT_FIELD_MAP[item_name]
            resolved = (resolved_bit_fields or {}).get((struct_name, member_name))
            if resolved is None and resolved_bit_fields is None:
                resolved = resolve_bit_field(types_text, struct_name, member_name)
            if resolved is None:
                missing.append({"kind": item_kind, "name": item_name, "reason": "bit_field_not_found", "structName": struct_name, "memberName": member_name})
                continue
            offset, bit_offset, bit_count, storage_bytes = resolved
            item = {
                "itemId": item_id,
                "name": item_name,
                "kind": item_kind,
                "flags": "required",
                "capabilityGroupId": group_id,
                "value": f"0x{offset:04X}",
                "aux0": bit_offset,
                "aux1": bit_count,
                "aux2": storage_bytes,
                "aux3": 0,
            }
        else:
            struct_name = V4_TYPE_SIZE_MAP[item_name]
            type_size = (resolved_type_sizes or {}).get(struct_name)
            if type_size is None and resolved_type_sizes is None:
                type_size = resolve_type_size(types_text, struct_name)
            if type_size is None:
                missing.append({"kind": item_kind, "name": item_name, "reason": "type_not_found", "structName": struct_name})
                continue
            item = {
                "itemId": item_id,
                "name": item_name,
                "kind": item_kind,
                "flags": "required",
                "capabilityGroupId": group_id,
                "value": f"0x{type_size:04X}",
                "aux0": 0,
                "aux1": 0,
                "aux2": 0,
                "aux3": 0,
            }
        items.append(item)
    return items, missing


def resolve_v4_layouts(types_text: str) -> tuple[
    dict[tuple[str, str], int],
    dict[str, int],
    dict[tuple[str, str], tuple[int, int, int, int]],
]:
    """Resolve v4 members, type sizes, and bitfields from one PDB type dump."""
    member_requests = list(V4_FIELD_MAP.values()) + list(V4_BIT_FIELD_MAP.values())
    member_requests.extend(request for alias in V4_MEMBER_ALIASES.values() for request in alias)
    resolved_member_offsets, resolved_type_sizes, resolved_bit_fields = resolve_type_layouts_batch(
        types_text,
        member_requests,
        V4_TYPE_SIZE_MAP.values(),
        V4_BIT_FIELD_MAP.values(),
    )
    for target, (base_member, nested_member) in V4_MEMBER_ALIASES.items():
        if target in resolved_member_offsets:
            continue
        base_offset = resolved_member_offsets.get(base_member)
        nested_offset = resolved_member_offsets.get(nested_member)
        if base_offset is not None and nested_offset is not None:
            resolved_member_offsets[target] = base_offset + nested_offset
    return resolved_member_offsets, resolved_type_sizes, resolved_bit_fields


def refresh_v4_profile(
    profile_path: Path,
    types_text: str,
) -> None:
    """Refresh v4 payload while preserving legacy/callback data in a profile."""
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    if not isinstance(profile, dict):
        raise ValueError(f"profile root is not an object: {profile_path}")
    resolved_member_offsets, resolved_type_sizes, resolved_bit_fields = resolve_v4_layouts(types_text)
    v4_items, v4_missing_items = build_v4_items(
        types_text,
        resolved_member_offsets=resolved_member_offsets,
        resolved_type_sizes=resolved_type_sizes,
        resolved_bit_fields=resolved_bit_fields,
    )
    profile["v4Items"] = v4_items
    profile["v4MissingItems"] = v4_missing_items
    diagnostics = profile.get("diagnostics")
    if not isinstance(diagnostics, dict):
        diagnostics = {}
    diagnostics["v4MissingItems"] = v4_missing_items
    profile["diagnostics"] = diagnostics
    profile["generatedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    profile["generator"] = "tools/pdb_offset_generator/ksword_pdb_profile_generator.py"
    write_json_atomic(profile_path, profile)


def v4_profile_complete(profile_path: Path) -> bool:
    """Return whether an existing generator profile already has every v4 item."""
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return False
    if not isinstance(profile, dict) or profile.get("v4MissingItems"):
        return False
    raw_items = profile.get("v4Items")
    if not isinstance(raw_items, list):
        return False
    expected_ids = {definition[0] for definition in V4_ITEM_DEFINITIONS.values()}
    actual_ids = {
        item.get("itemId")
        for item in raw_items
        if isinstance(item, dict) and isinstance(item.get("itemId"), int)
    }
    return actual_ids == expected_ids


def build_profile(
    entry: KphDynEntry,
    pe_path: Path,
    pdb_identity: PdbIdentity,
    types_text: str,
    symbol_addresses: dict[str, list[SymbolAddress]] | None = None,
    symbol_dump_failures: list[dict[str, str]] | None = None,
) -> dict[str, object]:
    """Build the Ksword JSON profile object from resolved PDB data.

    Inputs:
    - entry/pe_path/pdb_identity: Stable PE/PDB identity values.
    - types_text: llvm-pdbutil -types output for legacy and callback offsets.
    - symbol_addresses/symbol_dump_failures: Optional globals/publics data used
      only for callback GlobalRva items.

    Processing:
    - Preserves the legacy fields/missingFields schema exactly for current R3
      consumers and v1 release pack tooling.
    - Adds callbackItems as an optional v2 extension and puts all missing
      callback candidates under diagnostics.missingItems.

    Return behavior:
    - Returns a JSON-serializable dictionary; it does not write files.
    """
    member_requests = list(FIELD_MAP.values()) + list(CALLBACK_STRUCT_FIELD_MAP.values())
    member_requests.extend(list(V4_FIELD_MAP.values()) + list(V4_BIT_FIELD_MAP.values()))
    member_requests.extend(request for alias in V4_MEMBER_ALIASES.values() for request in alias)
    resolved_member_offsets, resolved_type_size_values, resolved_bit_fields = resolve_type_layouts_batch(
        types_text,
        member_requests,
        list(TYPE_SIZE_MAP.values()) + list(V4_TYPE_SIZE_MAP.values()),
        V4_BIT_FIELD_MAP.values(),
    )
    for target, (base_member, nested_member) in V4_MEMBER_ALIASES.items():
        if target in resolved_member_offsets:
            continue
        base_offset = resolved_member_offsets.get(base_member)
        nested_offset = resolved_member_offsets.get(nested_member)
        if base_offset is not None and nested_offset is not None:
            resolved_member_offsets[target] = base_offset + nested_offset
    resolved_fields: dict[str, str] = {}
    missing_fields: list[str] = []
    for ksword_name, (struct_name, member_name) in FIELD_MAP.items():
        offset = resolved_member_offsets.get((struct_name, member_name))
        if offset is None:
            missing_fields.append(f"{struct_name}->{member_name}")
            continue
        resolved_fields[ksword_name] = f"0x{offset:04X}"

    resolved_type_sizes: dict[str, str] = {}
    missing_type_sizes: list[str] = []
    for ksword_name, struct_name in TYPE_SIZE_MAP.items():
        type_size = resolved_type_size_values.get(struct_name)
        if type_size is None:
            missing_type_sizes.append(struct_name)
            continue
        resolved_type_sizes[ksword_name] = f"0x{type_size:04X}"

    callback_items, diagnostics = build_callback_items(
        types_text=types_text,
        pe_path=pe_path,
        symbol_addresses=symbol_addresses or {},
        symbol_dump_failures=symbol_dump_failures or [],
        resolved_member_offsets=resolved_member_offsets,
    )
    kernel_global_items, missing_global_details = build_global_rva_items(
        pe_path=pe_path,
        symbol_addresses=symbol_addresses or {},
        symbol_names=KERNEL_GLOBAL_RVA_NAMES,
    )
    missing_globals = [str(item.get("name", "")) for item in missing_global_details if str(item.get("name", "")).strip()]
    v4_items, v4_missing_items = build_v4_items(
        types_text,
        resolved_member_offsets=resolved_member_offsets,
        resolved_type_sizes=resolved_type_size_values,
        resolved_bit_fields=resolved_bit_fields,
    )
    diagnostics["v4MissingItems"] = v4_missing_items
    typed_items: list[dict[str, Any]] = []
    for field_name, offset_text in sorted(resolved_fields.items()):
        typed_items.append({"kind": "StructOffset", "name": field_name, "value": offset_text})
    for field_name, size_text in sorted(resolved_type_sizes.items()):
        typed_items.append({"kind": "TypeSize", "name": field_name, "value": size_text})
    typed_items.extend(callback_items)
    typed_items.extend(kernel_global_items)

    total_candidates = (
        len(FIELD_MAP) +
        len(TYPE_SIZE_MAP) +
        len(CALLBACK_GLOBAL_RVA_NAMES) +
        len(CALLBACK_STRUCT_FIELD_MAP) +
        len(KERNEL_GLOBAL_RVA_NAMES)
    )
    coverage_percent = 0.0 if total_candidates == 0 else (len(typed_items) / float(total_candidates)) * 100.0

    profile_name = f"{entry.class_name}_{entry.arch}_{entry.version}_{pdb_identity.symbol_key.lower()}"
    return {
        "schemaVersion": KSW_SCHEMA_VERSION,
        "callbackItemSchemaVersion": CALLBACK_ITEM_SCHEMA_VERSION,
        "typedItemSchemaVersion": TYPED_ITEM_SCHEMA_VERSION,
        "profileName": profile_name,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "generator": "tools/pdb_offset_generator/ksword_pdb_profile_generator.py",
        "module": {
            "class": entry.class_name,
            "file": entry.file_name,
            "arch": entry.arch,
            "version": entry.version,
            "machine": f"0x{entry.machine:04X}",
            "timeDateStamp": f"0x{entry.timestamp:08X}",
            "sizeOfImage": f"0x{entry.size_of_image:08X}",
            "sha256": sha256_file(pe_path),
            "pdbName": pdb_identity.pdb_name,
            "pdbGuid": pdb_identity.pdb_guid,
            "pdbAge": pdb_identity.pdb_age,
            "pdbSymbolKey": pdb_identity.symbol_key,
        },
        "fields": resolved_fields,
        "missingFields": missing_fields,
        "typeSizes": resolved_type_sizes,
        "missingTypeSizes": missing_type_sizes,
        "callbackItems": callback_items,
        "typedItems": typed_items,
        "v4Items": v4_items,
        "v4MissingItems": v4_missing_items,
        "missingGlobals": missing_globals,
        "coveragePercent": round(coverage_percent, 1),
        "diagnostics": {
            **diagnostics,
            "missingFields": missing_fields,
            "missingTypeSizes": missing_type_sizes,
            "missingGlobals": missing_global_details,
        },
    }


def process_entry(
    entry: KphDynEntry,
    root: Path,
    symbol_server: str,
    pdbutil_path: str,
    skip_existing: bool,
    offline: bool = False,
    refresh_v4_existing: bool = False,
) -> Path | None:
    """Download PE/PDB for one entry, generate JSON, and return the profile path.

    Inputs:
    - entry: kphdyn-derived PE identity to process.
    - root: Corpus/cache root where PE/PDB/profile files are stored.
    - symbol_server: Symbol server base URL.
    - pdbutil_path: llvm-pdbutil executable.
    - skip_existing: Whether to preserve an already generated JSON profile.

    Processing:
    - Preserves the existing corpus workflow: download PE, validate SHA256,
      parse RSDS identity, download matching PDB, then parse PDB data.
    - Adds optional callback symbol collection before profile construction.

    Return behavior:
    - Returns the profile path when a profile exists or is written; returns None
      only for future extension points that intentionally skip a row.
    """
    pe_path = pe_path_for_entry(root, entry)
    pe_url = f"{symbol_server.rstrip('/')}/{entry.file_name}/{pe_symbol_key(entry)}/{entry.file_name}"
    print(f"[PE] {entry.version} {entry.file_name} {pe_symbol_key(entry)}")
    if offline and not pe_path.exists():
        raise FileNotFoundError(f"cached PE not found: {pe_path}")
    if not offline:
        download_file(pe_url, pe_path)
    actual_hash = sha256_file(pe_path)
    if actual_hash.lower() != entry.sha256.lower():
        raise RuntimeError(f"SHA256 mismatch for {pe_path}: expected {entry.sha256}, got {actual_hash}")

    pdb_identity = parse_rsds_identity(pe_path)
    profile_path = profile_path_for_entry(root, entry, pdb_identity)
    if refresh_v4_existing and profile_path.exists() and v4_profile_complete(profile_path):
        print(f"[V4-SKIP] {profile_path}")
        return profile_path
    if skip_existing and profile_path.exists() and not refresh_v4_existing:
        print(f"[SKIP] {profile_path}")
        return profile_path

    pdb_path = pdb_path_for_identity(root, entry, pdb_identity)
    pdb_url = f"{symbol_server.rstrip('/')}/{pdb_identity.pdb_name}/{pdb_identity.symbol_key}/{pdb_identity.pdb_name}"
    print(f"[PDB] {pdb_identity.pdb_name} {pdb_identity.symbol_key}")
    if offline and not pdb_path.exists():
        raise FileNotFoundError(f"cached PDB not found: {pdb_path}")
    if not offline:
        download_file(pdb_url, pdb_path)

    print(f"[TYPES] {pdb_path}")
    types_text = run_llvm_pdbutil_types(pdb_path, pdbutil_path)
    if refresh_v4_existing and profile_path.exists():
        refresh_v4_profile(profile_path, types_text)
        print(f"[V4-OK] {profile_path}")
        return profile_path
    print(f"[SYMS] {pdb_path}")
    symbol_addresses, symbol_dump_failures = collect_symbol_addresses(pdb_path, pdbutil_path)
    profile = build_profile(entry, pe_path, pdb_identity, types_text, symbol_addresses, symbol_dump_failures)
    write_json_atomic(profile_path, profile)
    print(f"[OK] {profile_path}")
    return profile_path


def process_entry_worker(
    entry: KphDynEntry,
    root_text: str,
    symbol_server: str,
    pdbutil_path: str,
    skip_existing: bool,
    offline: bool,
    refresh_v4_existing: bool,
) -> tuple[bool, str]:
    """Process one entry in a child process and return a compact result."""
    try:
        result_path = process_entry(
            entry,
            Path(root_text),
            symbol_server,
            pdbutil_path,
            skip_existing,
            offline,
            refresh_v4_existing,
        )
        return result_path is not None, str(result_path or "")
    except Exception as exc:  # noqa: BLE001 - worker must preserve per-entry failure and continue the matrix.
        return False, f"{entry.version} {entry.file_name}: {exc}"


def default_dry_run_output_path(entry: KphDynEntry, pdb_identity: PdbIdentity) -> Path:
    """Return the default local dry-run output path under D:/PDB/scratch.

    Inputs:
    - entry/pdb_identity: Values used to build a deterministic file name.

    Processing:
    - Mirrors profile_path_for_entry naming without using the corpus profile
      directory, so dry-run validation cannot accidentally refresh runtime
      profile corpus files.

    Return behavior:
    - Returns a Path; the caller creates parent directories when writing.
    """
    safe_version = entry.version.replace(".", "_")
    safe_key = pdb_identity.symbol_key.lower()
    return Path(r"D:\PDB\scratch") / f"{entry.class_name}_{entry.arch}_{safe_version}_{safe_key}.json"


def run_local_dry_run(args: argparse.Namespace) -> int:
    """Run callback parsing against exactly one local PE/PDB pair.

    Inputs:
    - args: Parsed CLI namespace containing --local-pe, --local-pdb, and optional
      --output.

    Processing:
    - Does not download symbols, build projects, write Release directories, or
      touch the normal profile corpus.
    - Writes the JSON only to --output or D:/PDB/scratch by default.

    Return behavior:
    - Returns 0 on success and 2 for argument/path validation failures.
    """
    pe_path = Path(args.local_pe) if args.local_pe else None
    pdb_path = Path(args.local_pdb) if args.local_pdb else None
    if pe_path is None or pdb_path is None:
        print("--dry-run requires --local-pe and --local-pdb", file=sys.stderr)
        return 2
    if not pe_path.exists():
        print(f"local PE not found: {pe_path}", file=sys.stderr)
        return 2
    if not pdb_path.exists():
        print(f"local PDB not found: {pdb_path}", file=sys.stderr)
        return 2

    entry = entry_from_local_pe(pe_path)
    pdb_identity = parse_rsds_identity(pe_path)
    output_path = Path(args.output) if args.output else default_dry_run_output_path(entry, pdb_identity)

    print(f"[DRY-RUN PE] {pe_path}")
    print(f"[DRY-RUN PDB] {pdb_path}")
    print(f"[TYPES] {pdb_path}")
    types_text = run_llvm_pdbutil_types(pdb_path, args.llvm_pdbutil)
    print(f"[SYMS] {pdb_path}")
    symbol_addresses, symbol_dump_failures = collect_symbol_addresses(pdb_path, args.llvm_pdbutil)
    profile = build_profile(entry, pe_path, pdb_identity, types_text, symbol_addresses, symbol_dump_failures)

    write_json_atomic(output_path, profile)
    print(f"[OK] {output_path}")
    print(f"callbackItems={len(profile.get('callbackItems', []))}")
    print(f"typedItems={len(profile.get('typedItems', []))}")
    print(f"missingGlobals={len(profile.get('missingGlobals', []))}")
    print(f"coveragePercent={profile.get('coveragePercent', 0.0)}")
    diagnostics = profile.get("diagnostics", {})
    missing_items = diagnostics.get("missingItems", []) if isinstance(diagnostics, dict) else []
    print(f"missingItems={len(missing_items) if isinstance(missing_items, list) else 0}")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Program entry point. Returns a process exit code.

    Inputs:
    - argv: Optional argument list; None uses process command-line arguments.

    Processing:
    - Validates llvm-pdbutil once.
    - Dispatches either a local dry-run or the existing kphdyn-driven corpus
      generation loop.

    Return behavior:
    - Returns 0 when at least one requested profile path is produced, non-zero
      for validation failures or no generated profiles.
    """
    args = parse_args(argv)
    if not Path(args.llvm_pdbutil).exists() and os.path.sep in args.llvm_pdbutil:
        print(f"llvm-pdbutil not found: {args.llvm_pdbutil}", file=sys.stderr)
        return 2
    if args.dry_run:
        return run_local_dry_run(args)

    root = Path(args.symbol_root)
    files = {item.strip().lower() for item in args.file.split(",") if item.strip()}
    entries = list(iter_kphdyn_entries(Path(args.kphdyn), args.arch, args.version, files))
    if not entries:
        print("No matching kphdyn entries found.", file=sys.stderr)
        return 2

    generated = 0
    if args.workers > 1:
        worker_count = max(1, min(int(args.workers), 32))
        selected_entries = entries[: args.limit] if args.limit else entries
        with concurrent.futures.ProcessPoolExecutor(max_workers=worker_count) as executor:
            futures = [
                executor.submit(
                    process_entry_worker,
                    entry,
                    str(root),
                    args.symbol_server,
                    args.llvm_pdbutil,
                    args.skip_existing,
                    args.offline,
                    args.refresh_v4_existing,
                )
                for entry in selected_entries
            ]
            for future in concurrent.futures.as_completed(futures):
                succeeded, detail = future.result()
                if succeeded:
                    generated += 1
                else:
                    print(f"[ERR] {detail}", file=sys.stderr)
        return 0 if generated > 0 else 1

    for entry in entries:
        try:
            result_path = process_entry(
                entry,
                root,
                args.symbol_server,
                args.llvm_pdbutil,
                args.skip_existing,
                args.offline,
                args.refresh_v4_existing,
            )
            if result_path is not None:
                generated += 1
            if args.limit and generated >= args.limit:
                break
        except requests.HTTPError as exc:
            print(f"[HTTP] {entry.version} {entry.file_name}: {exc}", file=sys.stderr)
        except Exception as exc:
            print(f"[ERR] {entry.version} {entry.file_name}: {exc}", file=sys.stderr)
    return 0 if generated > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
