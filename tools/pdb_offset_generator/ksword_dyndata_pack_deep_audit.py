#!/usr/bin/env python3
"""
Ksword DynData pack / deep-offset 覆盖审计脚本。

用途：
- 只读检查主 GUI 随包携带的 ark_dyndata_pack_v3.json；
- 只读检查 profiles/pdb_deep_offsets 里的 ntoskrnl / win32k 深度偏移库；
- 验证 deep alias 字段是否已经进入 v3 pack 的 fields/items；
- 记录 win32k public PDB 是否已具备 tagWND/tagTHREADINFO 等私有 GUI 布局；
- 输出 JSON 报告，帮助发布前确认程序不依赖 E 盘 PDB 缓存。

边界：
- 不解析 PDB，不访问驱动，不编译，不修改 profile pack；
- 只读取仓库内 JSON，并把审计报告写到用户指定位置。
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PROFILE_ROOT = REPO_ROOT / "Ksword5.1" / "Ksword5.1" / "profiles"
DEFAULT_PACK_PATH = DEFAULT_PROFILE_ROOT / "ark_dyndata_pack_v3.json"
DEFAULT_MANIFEST_PATH = DEFAULT_PROFILE_ROOT / "ark_dyndata_manifest.json"
DEFAULT_OUTPUT_PATH = Path(r"D:\Temp\ksword_pdb_deep_offsets\ksword_dyndata_pack_deep_audit.json")
SYMBOL_CACHE_KEY_RE = re.compile(r"^(?P<guid>[0-9A-Fa-f]{32})(?P<age>[0-9A-Fa-f]+)$")
FILENAME_AGE_RE = re.compile(r"_age(?P<age>\d+)_deep_offsets\.json$", re.IGNORECASE)


PROCESS_DETAIL_REQUIRED = [
    "EpUniqueProcessId",
    "EpActiveProcessLinks",
    "EpThreadListHead",
    "EpImageFileName",
    "EpToken",
    "EpObjectTable",
    "EpSectionObject",
    "EpProtection",
    "EpSignatureLevel",
    "EpSectionSignatureLevel",
]

THREAD_DETAIL_REQUIRED = [
    "EtCid",
    "EtThreadListEntry",
    "EtStartAddress",
    "EtWin32StartAddress",
    "KtProcess",
    "KtInitialStack",
    "KtStackLimit",
    "KtStackBase",
    "KtKernelStack",
    "KtReadOperationCount",
    "KtWriteOperationCount",
    "KtOtherOperationCount",
    "KtReadTransferCount",
    "KtWriteTransferCount",
    "KtOtherTransferCount",
]

MODULE_DRIVER_REQUIRED = [
    "KldrInLoadOrderLinks",
    "KldrDllBase",
    "KldrSizeOfImage",
    "KldrFullDllName",
    "KldrBaseDllName",
    "DoDriverStart",
    "DoDriverSize",
    "DoDriverSection",
    "DoMajorFunction",
    "DoDriverUnload",
]


@dataclass(frozen=True)
class PackProfileView:
    """保存一个 pack profile 的归一化视图。"""

    profile: dict[str, Any]
    field_names: set[str]
    item_names: set[str]


@dataclass(frozen=True)
class PackProfileMatch:
    """保存一个 deep library 与 pack profile 的匹配证据。"""

    profile: dict[str, Any]
    match_method: str
    identity_strict: bool
    matched_age: int | None
    identity_notes: tuple[str, ...]


def read_json(path: Path) -> dict[str, Any]:
    """读取 JSON 文件。

    输入：
    - path：目标 JSON 路径。
    处理：
    - 使用 UTF-8 读取并解析对象。
    返回：
    - dict JSON 对象；格式不符时抛出 ValueError。
    """
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"JSON root is not an object: {path}")
    return data


def normalize_guid(value: Any) -> str:
    """归一化 PDB GUID 字符串。

    输入：
    - value：JSON 内的 GUID 字段。
    处理：
    - 去掉大括号和横线，并转成小写。
    返回：
    - 可用于比较的 32 位十六进制文本。
    """
    text = str(value or "").strip().strip("{}")
    return text.replace("-", "").lower()


def optional_int(value: Any) -> int | None:
    """把 JSON 数字字段安全转换为 int。"""
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def symbol_cache_age_from_path(path_text: str, source_guid: str) -> int | None:
    """从 PDB symbol-cache 路径父目录解析 GUID+Age。

    输入：
    - path_text：deep source.pdbPath；
    - source_guid：归一化后的 GUID。
    处理：
    - 检查父目录是否为 32 hex GUID + hex age；
    - GUID 必须与 source_guid 一致。
    返回：
    - 匹配到的 symbol-cache age；否则 None。
    """
    if not path_text or not source_guid:
        return None
    key_text = Path(path_text).parent.name.strip()
    match = SYMBOL_CACHE_KEY_RE.match(key_text)
    if not match or match.group("guid").lower() != source_guid:
        return None
    try:
        return int(match.group("age"), 16)
    except ValueError:
        return None


def filename_age(deep_path: Path) -> int | None:
    """从 deep JSON 文件名中的 _ageN_ 片段解析年龄。"""
    match = FILENAME_AGE_RE.search(deep_path.name)
    if not match:
        return None
    return optional_int(match.group("age"))


def add_age_candidate(output: list[dict[str, Any]], method: str, age_value: int | None) -> None:
    """追加去重后的 age 候选。

    输入：
    - output：候选列表；
    - method：候选来源；
    - age_value：候选 age。
    处理：
    - 忽略 None/负数；
    - 同一 age 只保留第一次来源，保证优先级稳定。
    返回：
    - 无返回值，直接修改 output。
    """
    if age_value is None or age_value < 0:
        return
    for candidate in output:
        if candidate["age"] == age_value:
            return
    output.append({"method": method, "age": age_value})


def deep_identity(deep_library: dict[str, Any], deep_path: Path) -> dict[str, Any]:
    """提取 deep library 的多来源 PDB 身份。

    输入：
    - deep_library：deep-offset JSON；
    - deep_path：deep JSON 路径。
    处理：
    - runtime 匹配优先使用 source.pdbAge / symbolCacheAge / pdbPath 父目录 GUID+Age；
    - 保留 pdbSummaryAge 和 filename age 作为诊断。
    返回：
    - 包含 guid、ageCandidates 和诊断字段的字典。
    """
    source = deep_library.get("source", {})
    if not isinstance(source, dict):
        source = {}
    source_guid = normalize_guid(source.get("pdbGuid"))
    candidates: list[dict[str, Any]] = []
    add_age_candidate(candidates, "source.pdbAge", optional_int(source.get("pdbAge")))
    add_age_candidate(candidates, "source.symbolCacheAge", optional_int(source.get("symbolCacheAge")))
    add_age_candidate(candidates, "source.pdbPath.symbolCacheKey", symbol_cache_age_from_path(str(source.get("pdbPath", "")), source_guid))
    add_age_candidate(candidates, "filename.age", filename_age(deep_path))
    add_age_candidate(candidates, "source.pdbSummaryAge", optional_int(source.get("pdbSummaryAge")))
    return {
        "pdbGuid": source.get("pdbGuid", ""),
        "normalizedPdbGuid": source_guid,
        "pdbAge": optional_int(source.get("pdbAge")),
        "pdbSummaryAge": optional_int(source.get("pdbSummaryAge")),
        "symbolCacheAge": optional_int(source.get("symbolCacheAge")),
        "filenameAge": filename_age(deep_path),
        "identitySource": source.get("identitySource", ""),
        "ageCandidates": candidates,
    }


def deep_library_paths(manifest: dict[str, Any], manifest_path: Path) -> list[Path]:
    """从 manifest 中解析 deep-offset 库路径。

    输入：
    - manifest：ark_dyndata_manifest.json 对象。
    - manifest_path：manifest 文件路径，用于解析相对路径。
    处理：
    - 读取 deepOffsetLibraries[].path。
    返回：
    - 仓库内实际 JSON 路径列表。
    """
    output: list[Path] = []
    manifest_dir = manifest_path.parent
    for entry in manifest.get("deepOffsetLibraries", []):
        if not isinstance(entry, dict):
            continue
        relative_path = str(entry.get("path", "")).strip()
        if not relative_path:
            continue
        candidate = (manifest_dir.parent / relative_path).resolve()
        if not candidate.exists():
            candidate = (REPO_ROOT / relative_path).resolve()
        output.append(candidate)
    return output


def build_profile_view(profile: dict[str, Any], field_dictionary: list[str]) -> PackProfileView:
    """把 compact pack profile 转成可审计集合。

    输入：
    - profile：pack 中的一条 profile。
    - field_dictionary：pack 级字段字典。
    处理：
    - fields 用索引还原为字段名；
    - items 直接抽取 name。
    返回：
    - PackProfileView，供覆盖检查使用。
    """
    field_names: set[str] = set()
    for pair in profile.get("fields", []):
        if not isinstance(pair, list) or len(pair) < 2:
            continue
        index = pair[0]
        if isinstance(index, int) and 0 <= index < len(field_dictionary):
            field_names.add(field_dictionary[index])

    item_names: set[str] = set()
    for item in profile.get("items", []):
        if isinstance(item, dict) and str(item.get("name", "")).strip():
            item_names.add(str(item["name"]).strip())

    return PackProfileView(profile=profile, field_names=field_names, item_names=item_names)


def find_matching_profiles(pack: dict[str, Any], deep_library: dict[str, Any], deep_path: Path) -> list[PackProfileMatch]:
    """查找与 deep 库 PDB identity 匹配的 pack profile。

    输入：
    - pack：ark_dyndata_pack_v3.json 对象。
    - deep_library：单个 deep-offset JSON 对象。
    处理：
    - 优先使用 deep source.pdbGuid/pdbAge；
    - 同时兼容 source.symbolCacheAge、pdbPath 父目录 GUID+Age 和文件名 age；
    - 只有 GUID+候选 age 命中才算 strict identity。
    返回：
    - 匹配 profile 列表及匹配证据。
    """
    identity = deep_identity(deep_library, deep_path)
    source_guid = str(identity.get("normalizedPdbGuid", ""))
    age_candidates = [
        (str(candidate.get("method", "")), optional_int(candidate.get("age")))
        for candidate in identity.get("ageCandidates", [])
        if isinstance(candidate, dict)
    ]

    profiles = [profile for profile in pack.get("profiles", []) if isinstance(profile, dict)]
    matches: list[PackProfileMatch] = []
    seen_ids: set[int] = set()
    for method, source_age_int in age_candidates:
        if source_age_int is None:
            continue
        for profile in profiles:
            if source_guid and normalize_guid(profile.get("pdbGuid")) != source_guid:
                continue
            profile_age = optional_int(profile.get("pdbAge"))
            if profile_age != source_age_int:
                continue
            profile_id = id(profile)
            if profile_id in seen_ids:
                continue
            seen_ids.add(profile_id)
            notes: list[str] = []
            if identity.get("pdbSummaryAge") is not None and identity.get("pdbSummaryAge") != source_age_int:
                notes.append(
                    f"llvm-pdbutil summary age {identity.get('pdbSummaryAge')} differs from matched runtime age {source_age_int}."
                )
            matches.append(PackProfileMatch(
                profile=profile,
                match_method=method,
                identity_strict=True,
                matched_age=source_age_int,
                identity_notes=tuple(notes),
            ))
    if matches:
        return matches

    # 没有 strict 命中时退到 GUID-only 诊断：这不证明运行时可安全匹配，
    # 但能帮助定位 pack/deep 库的 age 来源差异。
    for profile in pack.get("profiles", []):
        if not isinstance(profile, dict):
            continue
        if source_guid and normalize_guid(profile.get("pdbGuid")) != source_guid:
            continue
        profile_age = optional_int(profile.get("pdbAge"))
        candidate_text = ", ".join(
            f"{method}={age}" for method, age in age_candidates if age is not None
        )
        matches.append(PackProfileMatch(
            profile=profile,
            match_method="guidOnlyAgeMismatch",
            identity_strict=False,
            matched_age=profile_age,
            identity_notes=(f"profile age {profile_age} did not match any deep age candidate: {candidate_text}",),
        ))
    return matches


def required_status(view: PackProfileView, required_names: list[str]) -> dict[str, Any]:
    """检查一组 runtime detail 必需字段是否存在。

    输入：
    - view：pack profile 归一化视图。
    - required_names：功能所需字段名。
    处理：
    - 同时接受 fields 和 items 中的字段。
    返回：
    - present/missing/ready 三元状态。
    """
    present: list[str] = []
    missing: list[str] = []
    combined = view.field_names | view.item_names
    for name in required_names:
        if name in combined:
            present.append(name)
        else:
            missing.append(name)
    return {
        "ready": not missing,
        "present": present,
        "missing": missing,
    }


def audit_deep_library(pack: dict[str, Any], deep_path: Path, deep_library: dict[str, Any]) -> dict[str, Any]:
    """审计一个 deep-offset 库与 pack 的覆盖关系。

    输入：
    - pack：发布 pack；
    - deep_path：deep JSON 路径；
    - deep_library：deep JSON 对象。
    处理：
    - 统计 alias 是否进入匹配 profile 的 fields/items；
    - 检查 process/thread/module detail 关键字段 ready 状态。
    返回：
    - JSON 可序列化审计结果。
    """
    field_dictionary = pack.get("fieldDictionary", [])
    if not isinstance(field_dictionary, list):
        field_dictionary = []
    field_dictionary = [str(name) for name in field_dictionary]

    alias_rows = [
        row for row in deep_library.get("kswordAliasFields", [])
        if isinstance(row, dict) and str(row.get("kswordItemName", "")).strip()
    ]
    alias_names = sorted({str(row["kswordItemName"]).strip() for row in alias_rows})
    identity = deep_identity(deep_library, deep_path)
    matching_profiles = find_matching_profiles(pack, deep_library, deep_path)

    profile_reports: list[dict[str, Any]] = []
    for profile_match in matching_profiles:
        profile = profile_match.profile
        view = build_profile_view(profile, field_dictionary)
        combined_names = view.field_names | view.item_names
        missing_aliases = [name for name in alias_names if name not in combined_names]
        profile_reports.append({
            "profileName": profile.get("profileName", ""),
            "pdbGuid": profile.get("pdbGuid", ""),
            "pdbAge": profile.get("pdbAge", 0),
            "matchMethod": profile_match.match_method,
            "identityStrict": profile_match.identity_strict,
            "matchedAge": profile_match.matched_age,
            "identityNotes": list(profile_match.identity_notes),
            "fieldCount": len(view.field_names),
            "itemCount": len(view.item_names),
            "aliasCount": len(alias_names),
            "presentAliasCount": len(alias_names) - len(missing_aliases),
            "missingAliases": missing_aliases,
            "processDetail": required_status(view, PROCESS_DETAIL_REQUIRED),
            "threadDetail": required_status(view, THREAD_DETAIL_REQUIRED),
            "moduleDriverDetail": required_status(view, MODULE_DRIVER_REQUIRED),
        })

    return {
        "path": str(deep_path),
        "schemaVersion": deep_library.get("schemaVersion"),
        "kind": deep_library.get("kind"),
        "stats": deep_library.get("stats", {}),
        "source": deep_library.get("source", {}),
        "deepIdentity": identity,
        "aliasCount": len(alias_names),
        "aliases": alias_names,
        "matchingProfileCount": len(profile_reports),
        "profiles": profile_reports,
    }


def audit_win32k_deep_library(deep_path: Path, deep_library: dict[str, Any]) -> dict[str, Any]:
    """审计 win32k public 深度库的仓库内可用性。

    输入：
    - deep_path：win32k deep-offset JSON 路径；
    - deep_library：已解析的 JSON 对象。
    处理：
    - 汇总每个 win32k* PDB 模块的 PDB identity、字段数和 public symbol 数；
    - 汇总 tagWND/tagTHREADINFO/tagHOOK 等私有 GUI 类型缺失状态；
    - 不与 ntoskrnl dyn-data pack 做匹配，因为当前 win32k public 库是旁路审计资料。
    返回：
    - JSON 可序列化的 win32k 审计结果；privateTypeReady=false 表示运行时对象细读仍需私有布局来源。
    """
    stats = deep_library.get("stats", {})
    if not isinstance(stats, dict):
        stats = {}

    missing_by_module = deep_library.get("missingPrivateTypesByModule", {})
    if not isinstance(missing_by_module, dict):
        missing_by_module = {}

    modules: list[dict[str, Any]] = []
    for module in deep_library.get("modules", []):
        if not isinstance(module, dict):
            continue
        source = module.get("source", {})
        if not isinstance(source, dict):
            source = {}
        module_stats = module.get("stats", {})
        if not isinstance(module_stats, dict):
            module_stats = {}
        readiness = module.get("privateTypeReadiness", {})
        if not isinstance(readiness, dict):
            readiness = {}

        module_name = str(module.get("moduleName") or source.get("pdbName") or "").strip()
        modules.append({
            "moduleName": module_name,
            "pdbGuid": source.get("pdbGuid", ""),
            "pdbAge": optional_int(source.get("pdbAge")),
            "pdbSummaryAge": optional_int(source.get("pdbSummaryAge")),
            "symbolCacheAge": optional_int(source.get("symbolCacheAge")),
            "fieldCount": optional_int(module_stats.get("fieldCount")) or 0,
            "enumValueCount": optional_int(module_stats.get("enumValueCount")) or 0,
            "publicSymbolCount": optional_int(module_stats.get("publicSymbolCount")) or 0,
            "privateTypeReady": bool(readiness.get("ready")),
            "missingPrivateTypes": list(readiness.get("missingPrivateTypes", []))
            if isinstance(readiness.get("missingPrivateTypes", []), list)
            else list(missing_by_module.get(module_name, []))
            if isinstance(missing_by_module.get(module_name, []), list)
            else [],
        })

    missing_private_types = sorted({
        str(type_name)
        for type_list in missing_by_module.values()
        if isinstance(type_list, list)
        for type_name in type_list
    })
    private_type_ready = bool(stats.get("privateTypeReady"))
    runtime_catalog = deep_library.get("runtimeDetailCatalog", {})
    if not isinstance(runtime_catalog, dict):
        runtime_catalog = {}

    runtime_domains: list[dict[str, Any]] = []
    domains_object = runtime_catalog.get("domains", {})
    if isinstance(domains_object, dict):
        for domain_id, domain_value in domains_object.items():
            if not isinstance(domain_value, dict):
                continue
            runtime_domains.append({
                "domainId": str(domain_id),
                "displayName": domain_value.get("displayName", str(domain_id)),
                "ready": bool(domain_value.get("ready")),
                "requiredPrivateTypes": list(domain_value.get("requiredPrivateTypes", []))
                if isinstance(domain_value.get("requiredPrivateTypes", []), list)
                else [],
                "missingPrivateTypes": list(domain_value.get("missingPrivateTypes", []))
                if isinstance(domain_value.get("missingPrivateTypes", []), list)
                else [],
                "concreteFieldCount": optional_int(domain_value.get("concreteFieldCount")) or 0,
                "publicEvidenceAvailable": bool(domain_value.get("publicEvidenceAvailable")),
                "publicSymbolGroups": domain_value.get("publicSymbolGroups", {})
                if isinstance(domain_value.get("publicSymbolGroups", {}), dict)
                else {},
                "blockedBy": domain_value.get("blockedBy", ""),
                "intendedUse": domain_value.get("intendedUse", ""),
            })

    runtime_domains.sort(key=lambda item: str(item.get("domainId", "")))
    ready_domain_count = sum(1 for item in runtime_domains if item.get("ready"))
    blocked_domain_count = len(runtime_domains) - ready_domain_count

    return {
        "path": str(deep_path),
        "schemaVersion": deep_library.get("schemaVersion"),
        "kind": deep_library.get("kind"),
        "stats": stats,
        "moduleCount": len(modules),
        "modules": modules,
        "privateTypeReady": private_type_ready,
        "runtimeDetailReady": bool(runtime_catalog.get("ready")) if runtime_catalog else private_type_ready,
        "runtimeDetailCatalogPresent": bool(runtime_catalog),
        "runtimeReadyDomainCount": (optional_int(runtime_catalog.get("readyDomainCount")) or ready_domain_count)
        if runtime_catalog else ready_domain_count,
        "runtimeBlockedDomainCount": (optional_int(runtime_catalog.get("blockedDomainCount")) or blocked_domain_count)
        if runtime_catalog else blocked_domain_count,
        "runtimeDomains": runtime_domains,
        "missingPrivateTypes": missing_private_types,
        "missingPrivateTypesByModule": missing_by_module,
        "runtimeDetailBlockedBy": ""
        if (bool(runtime_catalog.get("ready")) if runtime_catalog else private_type_ready)
        else "public win32k PDB cache does not expose private GUI object layouts such as tagWND/tagTHREADINFO.",
    }


def build_report(pack_path: Path, manifest_path: Path) -> dict[str, Any]:
    """构建完整审计报告。

    输入：
    - pack_path：ark_dyndata_pack_v3.json。
    - manifest_path：ark_dyndata_manifest.json。
    处理：
    - 读取 pack/manifest/deep libraries；
    - 汇总错误、警告和每个 deep 库的覆盖状态。
    返回：
    - JSON 报告对象。
    """
    pack = read_json(pack_path)
    manifest = read_json(manifest_path)
    deep_paths = deep_library_paths(manifest, manifest_path)

    errors: list[str] = []
    warnings: list[str] = []
    incomplete: list[str] = []
    libraries: list[dict[str, Any]] = []

    if int(pack.get("packVersion", 0) or 0) < 3:
        errors.append("ark_dyndata_pack_v3.json packVersion is lower than 3.")
    if not deep_paths:
        errors.append("manifest has no deepOffsetLibraries entries.")

    for deep_path in deep_paths:
        if not deep_path.exists():
            errors.append(f"deep-offset library missing: {deep_path}")
            continue
        deep_library = read_json(deep_path)
        deep_kind = str(deep_library.get("kind", "")).strip()
        if deep_kind == "KswordWin32kDeepOffsetLibrary":
            library_report = audit_win32k_deep_library(deep_path, deep_library)
            libraries.append(library_report)
            if not library_report["privateTypeReady"]:
                incomplete.append(
                    f"{deep_path.name} has public win32k symbols, but private GUI layout types are absent; "
                    "tagWND/tagTHREADINFO runtime detail remains unavailable."
                )
            if int(library_report.get("stats", {}).get("publicSymbolCount", 0) or 0) == 0:
                warnings.append(f"{deep_path.name} contains no public win32k symbols.")
            continue

        if deep_kind and deep_kind != "KswordNtosDeepOffsetLibrary":
            warnings.append(f"unknown deep-offset library kind {deep_kind}: {deep_path.name}")

        library_report = audit_deep_library(pack, deep_path, deep_library)
        libraries.append(library_report)
        if library_report["matchingProfileCount"] == 0:
            warnings.append(f"no pack profile matches deep library identity: {deep_path.name}")
            continue
        for profile_report in library_report["profiles"]:
            if profile_report["missingAliases"]:
                warnings.append(
                    f"{profile_report['profileName']} misses "
                    f"{len(profile_report['missingAliases'])} deep aliases."
                )
            if not profile_report["processDetail"]["ready"]:
                warnings.append(f"{profile_report['profileName']} process detail required fields are incomplete.")
            if not profile_report["threadDetail"]["ready"]:
                warnings.append(f"{profile_report['profileName']} thread detail required fields are incomplete.")

    return {
        "schemaVersion": 1,
        "kind": "KswordDynDataPackDeepAudit",
        "packPath": str(pack_path),
        "manifestPath": str(manifest_path),
        "packVersion": pack.get("packVersion"),
        "profileCount": len(pack.get("profiles", [])) if isinstance(pack.get("profiles"), list) else 0,
        "fieldDictionaryCount": len(pack.get("fieldDictionary", [])) if isinstance(pack.get("fieldDictionary"), list) else 0,
        "deepLibraryCount": len(libraries),
        "libraries": libraries,
        "errors": errors,
        "warnings": warnings,
        "incomplete": incomplete,
        "ok": not errors,
        "notes": [
            "This audit proves repository JSON coverage only; it does not prove the loaded driver consumed the pack.",
            "ntoskrnl deep libraries cannot provide win32k tagWND/tagTHREADINFO fields; win32k private layout data is still required for full window runtime detail.",
        ],
    }


def main() -> int:
    """命令行入口。

    输入：
    - --pack/--manifest/--output 参数。
    处理：
    - 构建报告并写入 JSON。
    返回：
    - 0 表示审计文件写出；存在 errors 时返回 2。
    """
    parser = argparse.ArgumentParser(description="Audit Ksword DynData pack coverage against deep-offset libraries.")
    parser.add_argument("--pack", default=str(DEFAULT_PACK_PATH), help="Path to ark_dyndata_pack_v3.json.")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST_PATH), help="Path to ark_dyndata_manifest.json.")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT_PATH), help="JSON report output path.")
    args = parser.parse_args()

    report = build_report(Path(args.pack), Path(args.manifest))
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"ok={report['ok']}")
    print(f"profileCount={report['profileCount']}")
    print(f"fieldDictionaryCount={report['fieldDictionaryCount']}")
    print(f"deepLibraryCount={report['deepLibraryCount']}")
    print(f"errors={len(report['errors'])}")
    print(f"warnings={len(report['warnings'])}")
    print(f"incomplete={len(report['incomplete'])}")
    print(f"output={output_path}")
    return 0 if report["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
