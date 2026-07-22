#!/usr/bin/env python3
"""Generate the Launcher identity index from the published DynData v3 pack."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
from pathlib import Path
from typing import Any


REQUIRED_PROFILE_KEYS = (
    "moduleClassId",
    "machine",
    "timeDateStamp",
    "sizeOfImage",
    "pdbName",
    "pdbGuid",
    "pdbAge",
)

# 这些字段在当前 DynData 诊断中属于可选能力；缺失时不应把整个内核 profile
# 判定为不可用。其它缺失字段仍会让 Launcher 报告该身份需要开发者关注。
OPTIONAL_MISSING_FIELDS = {
    "_EPROCESS->NumberOfLockedPages",
    "_HANDLE_TABLE->HandleCount",
    "_UNLOADED_DRIVERS->Name",
    "_UNLOADED_DRIVERS->StartAddress",
    "_UNLOADED_DRIVERS->EndAddress",
    "_UNLOADED_DRIVERS->CurrentTime",
}


def load_object(path: Path) -> dict[str, Any]:
    """读取 JSON 对象；输入文件路径，输出顶层字典，格式错误时直接终止构建。"""
    with path.open("r", encoding="utf-8-sig") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def validate_source(source: dict[str, Any]) -> dict[int, dict[str, Any]]:
    """校验手写模块目录；输入源对象，输出按 classId 索引的模块映射。"""
    if source.get("schemaVersion") != 1:
        raise ValueError("support_manifest_source.json schemaVersion must be 1")
    modules = source.get("modules")
    if not isinstance(modules, list) or len(modules) != 11:
        raise ValueError("the Launcher catalog must contain exactly 11 v4 module classes")
    indexed: dict[int, dict[str, Any]] = {}
    for module in modules:
        if not isinstance(module, dict):
            raise ValueError("every module entry must be an object")
        class_id = module.get("classId")
        names = module.get("fileNames")
        if not isinstance(class_id, int) or class_id in indexed:
            raise ValueError(f"invalid or duplicate module classId: {class_id!r}")
        if not isinstance(names, list) or not names or not all(isinstance(name, str) and name for name in names):
            raise ValueError(f"module {class_id} must contain fileNames")
        compatibility_required = module.get("compatibilityRequired")
        collection_only = module.get("collectionOnly")
        if not isinstance(compatibility_required, bool) or not isinstance(collection_only, bool):
            raise ValueError(f"module {class_id} must declare compatibilityRequired and collectionOnly")
        if compatibility_required and collection_only:
            raise ValueError(f"module {class_id} cannot be compatibilityRequired and collectionOnly")
        indexed[class_id] = module
    expected_class_ids = {0, 1, 2, 16, 17, 18, 32, 33, 34, 48, 64}
    if set(indexed) != expected_class_ids:
        raise ValueError("module classIds do not match the shared DynData v4 class IDs")
    if not indexed[0]["compatibilityRequired"] or not indexed[1]["compatibilityRequired"]:
        raise ValueError("NTOS and NTKRLA57 must remain compatibility-required")
    return indexed


def profile_is_complete(profile: dict[str, Any]) -> bool:
    """判断发布 profile 是否完整；输入 v3 profile，输出 Launcher 使用的完整性标记。"""
    coverage = float(profile.get("coveragePercent", 0.0))
    missing_fields = profile.get("missingFields", [])
    missing_globals = profile.get("missingGlobals", [])
    # coveragePercent 是所有字段的数学覆盖率，不能单独作为核心可用性的判断；
    # 当前 profile 可能只缺少可选的句柄计数/卸载驱动历史字段。
    return not missing_globals and all(field in OPTIONAL_MISSING_FIELDS for field in missing_fields)


def normalize_guid(value: str) -> str:
    """统一 PDB GUID 格式；输入可带连字符/花括号，输出 32 位大写十六进制。"""
    normalized = re.sub(r"[-{}\s]", "", value).upper()
    if not re.fullmatch(r"[0-9A-F]{32}", normalized):
        raise ValueError(f"invalid PDB GUID: {value!r}")
    return normalized


def compact_profile(profile: dict[str, Any]) -> dict[str, Any]:
    """丢弃偏移数组，仅保留 PE/PDB 身份和覆盖状态；输入原 profile，输出轻量记录。"""
    missing = [key for key in REQUIRED_PROFILE_KEYS if key not in profile]
    if missing:
        raise ValueError(f"profile {profile.get('profileName', '<unnamed>')} misses {missing}")
    return {
        "moduleClassId": int(profile["moduleClassId"]),
        "machine": int(profile["machine"]),
        "timeDateStamp": int(profile["timeDateStamp"]),
        "sizeOfImage": int(profile["sizeOfImage"]),
        "pdbName": str(profile["pdbName"]),
        "pdbGuid": normalize_guid(str(profile["pdbGuid"])),
        "pdbAge": int(profile["pdbAge"]),
        "complete": profile_is_complete(profile),
        "coveragePercent": float(profile.get("coveragePercent", 0.0)),
        "profileName": str(profile.get("profileName", "")),
    }


def identity_key(profile: dict[str, Any]) -> tuple[Any, ...]:
    """生成稳定去重键；输入轻量 profile，输出覆盖全部匹配字段的元组。"""
    return tuple(profile[key] for key in REQUIRED_PROFILE_KEYS)


def generate(source: dict[str, Any], pack: dict[str, Any]) -> dict[str, Any]:
    """合并目录与已发布矩阵；输入源目录和 v3 pack，输出 Launcher 清单。"""
    modules_by_id = validate_source(source)
    if int(pack.get("packVersion", 0)) < 3 or not isinstance(pack.get("profiles"), list):
        raise ValueError("ark_dyndata_pack_v3.json must be a v3 profile pack")

    deduplicated: dict[tuple[Any, ...], dict[str, Any]] = {}
    for raw_profile in pack["profiles"]:
        if not isinstance(raw_profile, dict):
            raise ValueError("every profile must be an object")
        profile = compact_profile(raw_profile)
        if profile["moduleClassId"] not in modules_by_id:
            raise ValueError(f"unknown moduleClassId in profile pack: {profile['moduleClassId']}")
        key = identity_key(profile)
        previous = deduplicated.get(key)
        if previous is None or (profile["complete"] and not previous["complete"]):
            deduplicated[key] = profile

    profiles = sorted(
        deduplicated.values(),
        key=lambda item: (
            item["moduleClassId"],
            item["pdbName"].lower(),
            item["pdbGuid"],
            item["pdbAge"],
        ),
    )
    output_modules: list[dict[str, Any]] = []
    for class_id in sorted(modules_by_id):
        module = dict(modules_by_id[class_id])
        rows = [profile for profile in profiles if profile["moduleClassId"] == class_id]
        complete_count = sum(1 for profile in rows if profile["complete"])
        if not rows:
            status = "unpublished"
        elif complete_count == len(rows):
            status = "complete"
        else:
            status = "partial"
        module["publishedProfileCount"] = len(rows)
        module["completeProfileCount"] = complete_count
        module["coverageStatus"] = status
        output_modules.append(module)

    return {
        "schemaVersion": 1,
        "generatedUtc": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "product": source.get("product", "KswordARK"),
        "osPolicy": source["osPolicy"],
        "modules": output_modules,
        "profiles": profiles,
    }


def validate_output(output: dict[str, Any]) -> None:
    """执行无需第三方库的严格结构校验；输入输出对象，失败时抛出异常。"""
    if set(output) != {"schemaVersion", "generatedUtc", "product", "osPolicy", "modules", "profiles"}:
        raise ValueError("generated manifest contains unexpected top-level keys")
    if output["schemaVersion"] != 1 or len(output["modules"]) != 11:
        raise ValueError("generated manifest has an invalid schema or module count")
    for profile in output["profiles"]:
        for key in REQUIRED_PROFILE_KEYS + ("complete", "coveragePercent", "profileName"):
            if key not in profile:
                raise ValueError(f"generated profile misses {key}")


def main() -> int:
    """解析命令行并原子写出清单；输入脚本参数，输出进程退出码。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--pack", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()

    output = generate(load_object(args.source), load_object(args.pack))
    validate_output(output)
    if not args.validate_only:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_suffix(args.output.suffix + ".tmp")
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(output, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        temporary.replace(args.output)
        print(f"Generated {args.output} with {len(output['profiles'])} identities")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
