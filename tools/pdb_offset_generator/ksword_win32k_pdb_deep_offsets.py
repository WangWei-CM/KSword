#!/usr/bin/env python3
"""
Ksword win32k PDB 深度目录生成器。

用途：
- 从本机 win32k / win32kbase / win32kfull PDB 缓存读取公开类型、枚举和 public 符号；
- 为窗口、GUI 线程、Hotkey、Hook、Desktop/Session 等 R0 只读审计准备可发布的离线事实库；
- 明确记录 public PDB 缺少 tagWND/tagTHREADINFO/tagQ/tagHOOK/tagHOTKEY/tagTIMER/tagEVENTHOOK 私有结构时的能力缺口。

边界：
- 只读 PDB 文件，不下载符号，不访问驱动，不运行程序；
- public 符号中的 section:offset 不是最终 RVA，R0 使用前必须结合已加载 PE 节表和 PDB/PE 身份校验；
- 没有私有结构字段时，只输出 missingPrivateTypes，不伪造偏移。
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import time
from pathlib import Path
from typing import Any

# 复用 ntos 生成器里的 TPI 解析器，避免复制一套易漂移的 PDB 文本解析逻辑。
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from ksword_ntos_pdb_deep_offsets import (  # noqa: E402
    build_flat_rows,
    build_type_info,
    extract_type_fields,
    parse_summary,
    run_pdbutil,
    split_type_records,
)

DEFAULT_LLVM_PDBUTIL = r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe"
DEFAULT_OUTPUT_DIR = r"D:\Temp\ksword_pdb_deep_offsets"
DEFAULT_REPO_JSON = (
    r"D:\Projects\Ksword5.1\Ksword5.1\Ksword5.1\profiles\pdb_deep_offsets"
    r"\win32k_gui_public_7bd3_a8a6_2d74_deep_offsets.json"
)

DEFAULT_PDBS = [
    r"E:\KswordPDB\PDB\pdb-cache\amd64\win32k.pdb\7BD3B4D17A3C35551C4972B31B8155361\win32k.pdb",
    r"E:\KswordPDB\PDB\pdb-cache\amd64\win32kbase.pdb\A8A69A7FD22B0D044A322341F88A665F1\win32kbase.pdb",
    r"E:\KswordPDB\PDB\pdb-cache\amd64\win32kfull.pdb\2D745AB4CE6186F2D19839B96062ED851\win32kfull.pdb",
]

# P0 GUI 私有类型清单：这些类型缺失时，不能声明 tagWND/tagQ/Hook/Hotkey 运行时读取已经可用。
PRIVATE_GUI_TYPES = [
    "tagWND",
    "tagTHREADINFO",
    "tagQ",
    "tagHOOK",
    "tagHOTKEY",
    "tagTIMER",
    "tagEVENTHOOK",
    "tagDESKTOP",
    "tagWINDOWSTATION",
]

# 这些关键字用于从 public PDB 的公开类型中筛出和 GUI 审计相关的结构/枚举。
TYPE_KEYWORDS = [
    "wnd",
    "window",
    "desktop",
    "station",
    "threadinfo",
    "hook",
    "hotkey",
    "timer",
    "input",
    "queue",
    "qmsg",
    "pointer",
    "cursor",
    "caret",
    "clipboard",
    "menu",
    "composition",
    "dwm",
    "monitor",
    "display",
    "dxgk",
    "gdi",
    "user",
]

# 公开符号分类关键字：用于把 NtUser/NtGdi/xxx/tagWND/Hotkey/Hook 等符号分组。
SYMBOL_GROUP_KEYWORDS: list[tuple[str, list[str]]] = [
    ("window_timer", ["timer", "settimer", "killtimer", "validatetimercallback"]),
    ("event_hook", ["winevent", "eventhook", "wineventhook", "gpeventhooks"]),
    ("hotkey_hook", ["hotkey", "hook", "unhook", "setwindowshook"]),
    ("window_object", ["tagwnd", "window", "hwnd", "foreground", "focus", "capture", "caret"]),
    ("gui_thread_queue", ["tagthreadinfo", "inputqueue", "thread", "queue", "qmsg"]),
    ("desktop_session", ["desktop", "windowstation", "session", "silo"]),
    ("clipboard_message", ["clipboard", "message", "postmessage", "sendmessage"]),
    ("gdi_display", ["ntgdi", "dxg", "dddi", "display", "monitor", "composition", "dwm"]),
    ("ntuser_api", ["ntuser", "xxx"]),
]

# 运行时详情域：
# - requiredPrivateTypes 表示安全读取对象字段前必须具备的私有 GUI layout；
# - usefulPublicSymbolGroups 表示 public PDB 至少能提供的函数/符号归因证据；
# - 本结构用于生成 runtimeDetailCatalog，让 UI 和审计脚本不要只显示散落摘要。
RUNTIME_DETAIL_DOMAINS: dict[str, dict[str, Any]] = {
    "window_detail": {
        "displayName": "Window detail / tagWND",
        "requiredPrivateTypes": ["tagWND", "tagTHREADINFO", "tagQ"],
        "usefulPublicSymbolGroups": ["window_object", "gui_thread_queue", "ntuser_api"],
        "intendedUse": "扩充单 HWND 详情、窗口 cross-view、focus/capture/caret 归因。",
    },
    "gui_thread_detail": {
        "displayName": "GUI thread / tagTHREADINFO + tagQ",
        "requiredPrivateTypes": ["tagTHREADINFO", "tagQ"],
        "usefulPublicSymbolGroups": ["gui_thread_queue", "window_object", "ntuser_api"],
        "intendedUse": "扩充 GUI 线程表、输入队列和活动窗口关系。",
    },
    "hotkey_detail": {
        "displayName": "Hotkey table / tagHOTKEY",
        "requiredPrivateTypes": ["tagHOTKEY", "tagWND", "tagTHREADINFO"],
        "usefulPublicSymbolGroups": ["hotkey_hook", "window_object", "gui_thread_queue"],
        "intendedUse": "扩充热键表中的 hotkey object、窗口和线程归属字段。",
    },
    "hook_detail": {
        "displayName": "Hook chain / tagHOOK",
        "requiredPrivateTypes": ["tagHOOK", "tagTHREADINFO", "tagDESKTOP"],
        "usefulPublicSymbolGroups": ["hotkey_hook", "gui_thread_queue", "desktop_session"],
        "intendedUse": "扩充 Hook 链、过程地址、目标线程和桌面归属字段。",
    },
    "timer_detail": {
        "displayName": "Window timer / tagTIMER",
        "requiredPrivateTypes": ["tagTIMER", "tagTHREADINFO", "tagWND"],
        "usefulPublicSymbolGroups": ["window_timer", "gui_thread_queue", "window_object", "ntuser_api"],
        "intendedUse": "扩充窗口定时器对象、间隔、flags、回调、窗口和线程归属字段。",
    },
    "event_hook_detail": {
        "displayName": "WinEvent hook / tagEVENTHOOK",
        "requiredPrivateTypes": ["tagEVENTHOOK", "tagTHREADINFO"],
        "usefulPublicSymbolGroups": ["event_hook", "hotkey_hook", "gui_thread_queue"],
        "intendedUse": "扩充 WinEvent Hook 链、事件范围、回调、模块和目标线程归属字段。",
    },
    "desktop_session_detail": {
        "displayName": "Desktop / WindowStation / Session",
        "requiredPrivateTypes": ["tagDESKTOP", "tagWINDOWSTATION"],
        "usefulPublicSymbolGroups": ["desktop_session", "window_object", "ntuser_api"],
        "intendedUse": "扩充桌面、窗口站和 Session readiness 审计。",
    },
}

# 若未来 private PDB 可用，这些 alias 会把字段直接映射到 shared/driver 的 win32k offset 结构名。
WIN32K_FIELD_ALIASES: dict[tuple[str, str], str] = {
    ("tagWND", "pti"): "tagWndThreadInfo",
    ("tagWND", "spwndParent"): "tagWndParent",
    ("tagWND", "spwndOwner"): "tagWndOwner",
    ("tagWND", "style"): "tagWndStyle",
    ("tagWND", "ExStyle"): "tagWndExStyle",
    ("tagWND", "rcWindow"): "tagWndRect",
    ("tagWND", "rcClient"): "tagWndClientRect",
    ("tagWND", "pcls"): "tagWndClass",
    ("tagWND", "strName"): "tagWndTitle",
    ("tagTHREADINFO", "pq"): "tagThreadInfoQueue",
    ("tagTHREADINFO", "rpdesk"): "tagThreadInfoDesktop",
    ("tagQ", "spwndActive"): "tagQActiveWindow",
    ("tagQ", "spwndFocus"): "tagQFocusWindow",
    ("tagQ", "spwndCapture"): "tagQCaptureWindow",
    ("tagQ", "spwndCaret"): "tagQCaretWindow",
    ("tagHOOK", "phkNext"): "tagHookNext",
    ("tagHOOK", "iHook"): "tagHookType",
    ("tagHOOK", "offPfn"): "tagHookProcedure",
    ("tagHOOK", "ptiHooked"): "tagHookTargetThreadInfo",
    ("tagHOTKEY", "phkNext"): "hotkeyNext",
    ("tagHOTKEY", "pti"): "hotkeyThreadInfo",
    ("tagHOTKEY", "spwnd"): "hotkeyWindow",
    ("tagHOTKEY", "fsModifiers"): "hotkeyModifiers",
    ("tagHOTKEY", "vk"): "hotkeyVirtualKey",
    ("tagHOTKEY", "id"): "hotkeyId",
    ("tagTIMER", "pti"): "timerPrimaryThreadInfo",
    ("tagTIMER", "pfn"): "timerCallback",
    ("tagTIMER", "cmsCountdown"): "timerCountdown",
    ("tagTIMER", "cmsTolerance"): "timerTolerance",
    ("tagTIMER", "flags"): "timerFlags",
    ("tagTIMER", "cmsRate"): "timerInterval",
    ("tagTIMER", "spwnd"): "timerWindow",
    ("tagTIMER", "nID"): "timerId",
    ("tagTIMER", "ptiCreator"): "timerAlternateThreadInfo",
    ("tagTIMER", "leHash"): "timerHashListEntry",
    ("tagTIMER", "dwTime"): "timerTimestamp",
    ("tagEVENTHOOK", "hEventHook"): "eventHookHandle",
    ("tagEVENTHOOK", "pti"): "eventHookOwnerThreadInfo",
    ("tagEVENTHOOK", "phkNext"): "eventHookNext",
    ("tagEVENTHOOK", "eventMin"): "eventHookEventMin",
    ("tagEVENTHOOK", "eventMax"): "eventHookEventMax",
    ("tagEVENTHOOK", "dwFlags"): "eventHookFlags",
    ("tagEVENTHOOK", "idProcess"): "eventHookTargetProcessId",
    ("tagEVENTHOOK", "idThread"): "eventHookTargetThreadId",
    ("tagEVENTHOOK", "offPfn"): "eventHookCallbackOffset",
    ("tagEVENTHOOK", "atomMod"): "eventHookModuleAtom",
    ("tagEVENTHOOK", "timeLast"): "eventHookTimestamp",
}

PUBLIC_HEADER_RE = re.compile(r"^\s*(?P<record>\d+)\s+\|\s+S_PUB32\s+\[size\s*=\s*(?P<size>\d+)\]\s+`(?P<name>[^`]+)`")
PUBLIC_DETAIL_RE = re.compile(r"flags\s*=\s*(?P<flags>[^,]+),\s*addr\s*=\s*(?P<section>[0-9A-Fa-f]+):(?P<offset>[0-9A-Fa-f]+)")
RECORD_COUNT_RE = re.compile(r"Showing\s+([0-9,]+)\s+records")


def parse_record_count(text: str) -> int:
    """从 llvm-pdbutil 输出中解析 Showing N records。"""
    match = RECORD_COUNT_RE.search(text)
    if not match:
        return -1
    return int(match.group(1).replace(",", ""))


def classify_text(text: str, fallback: str) -> str:
    """把类型名或符号名按 GUI 审计用途粗分组。"""
    lowered = text.lower()
    for group_name, keywords in SYMBOL_GROUP_KEYWORDS:
        if any(keyword in lowered for keyword in keywords):
            return group_name
    return fallback


def is_interesting_type(type_name: str) -> bool:
    """判断一个公开类型是否值得进入 win32k GUI 事实库。"""
    lowered = type_name.lower()
    return any(keyword in lowered for keyword in TYPE_KEYWORDS)


def apply_win32k_aliases(target: dict[str, Any]) -> None:
    """为未来 private PDB 字段补充 Ksword win32k offset alias。"""
    type_name = str(target.get("typeName", ""))
    for field_entry in target.get("fields", []):
        if not isinstance(field_entry, dict):
            continue
        alias = WIN32K_FIELD_ALIASES.get((type_name, str(field_entry.get("name", ""))))
        if alias:
            field_entry["kswordItemName"] = alias


def parse_public_symbols(publics_text: str, module_name: str) -> list[dict[str, Any]]:
    """解析 public 符号，并只保留 GUI 审计相关名称。"""
    rows: list[dict[str, Any]] = []
    pending: dict[str, Any] | None = None
    for line in publics_text.splitlines():
        header = PUBLIC_HEADER_RE.match(line)
        if header:
            if pending is not None:
                rows.append(pending)
            pending = {
                "moduleName": module_name,
                "record": int(header.group("record")),
                "recordSize": int(header.group("size")),
                "name": header.group("name"),
                "group": classify_text(header.group("name"), "other_public"),
                "flags": "",
                "section": "",
                "offset": 0,
                "offsetHex": "",
                "sectionOffset": "",
            }
            continue
        if pending is not None:
            detail = PUBLIC_DETAIL_RE.search(line)
            if detail:
                pending["flags"] = detail.group("flags").strip()
                pending["section"] = detail.group("section")
                pending["offset"] = int(detail.group("offset"), 10)
                pending["offsetHex"] = f"0x{pending['offset']:08X}"
                pending["sectionOffset"] = f"{pending['section']}:{int(detail.group('offset'), 10):08d}"
    if pending is not None:
        rows.append(pending)

    filtered = []
    for row in rows:
        name = str(row.get("name", ""))
        lowered = name.lower()
        if row.get("group") != "other_public" or "ntuser" in lowered or "ntgdi" in lowered or "tagwnd" in lowered:
            filtered.append(row)
    filtered.sort(key=lambda item: (str(item.get("group", "")), str(item.get("name", ""))))
    return filtered


def extract_module_catalog(pdbutil_path: str, pdb_path: Path, cache_dir: Path) -> dict[str, Any]:
    """提取单个 win32k-family PDB 的类型、枚举和 public 符号目录。"""
    module_name = pdb_path.name
    started = time.time()
    summary_text = run_pdbutil(pdbutil_path, pdb_path, "-summary", timeout=120)
    summary = parse_summary(summary_text, pdb_path)

    cache_dir.mkdir(parents=True, exist_ok=True)
    cache_stem = f"{module_name}_{str(summary.get('pdbGuid', '')).replace('-', '').lower()}_age{summary.get('pdbAge', 0)}"
    types_cache = cache_dir / f"{cache_stem}_types.txt"
    publics_cache = cache_dir / f"{cache_stem}_publics.txt"

    if types_cache.exists():
        types_text = types_cache.read_text(encoding="utf-8", errors="replace")
    else:
        types_text = run_pdbutil(pdbutil_path, pdb_path, "-types", timeout=900)
        types_cache.write_text(types_text, encoding="utf-8")

    if publics_cache.exists():
        publics_text = publics_cache.read_text(encoding="utf-8", errors="replace")
    else:
        publics_text = run_pdbutil(pdbutil_path, pdb_path, "-publics", timeout=900)
        publics_cache.write_text(publics_text, encoding="utf-8")

    records = split_type_records(types_text)
    type_infos = build_type_info(records)
    selected_names = sorted({info.name for info in type_infos.values() if info.name and not info.forward_ref and is_interesting_type(info.name)})

    targets: list[dict[str, Any]] = []
    selected_but_missing_fields: list[str] = []
    for type_name in selected_names:
        group_name = classify_text(type_name, "public_gui_type")
        extracted = extract_type_fields(type_name, group_name, type_infos, records)
        if extracted is None:
            selected_but_missing_fields.append(type_name)
            continue
        extracted["moduleName"] = module_name
        apply_win32k_aliases(extracted)
        targets.append(extracted)

    private_present = sorted({name for name in PRIVATE_GUI_TYPES if any(info.name == name and not info.forward_ref for info in type_infos.values())})
    private_missing = [name for name in PRIVATE_GUI_TYPES if name not in private_present]
    flat_rows = build_flat_rows(targets)
    for row in flat_rows:
        row["moduleName"] = module_name
    alias_rows = [row for row in flat_rows if row.get("kswordItemName")]
    public_symbols = parse_public_symbols(publics_text, module_name)

    public_groups: dict[str, int] = {}
    for symbol in public_symbols:
        public_groups[str(symbol.get("group", ""))] = public_groups.get(str(symbol.get("group", "")), 0) + 1

    return {
        "moduleName": module_name,
        "source": summary,
        "stats": {
            "typeRecordCount": len(records),
            "reportedTypeRecordCount": parse_record_count(types_text),
            "selectedTypeCount": len(targets),
            "selectedTypeWithoutFieldListCount": len(selected_but_missing_fields),
            "fieldCount": len(flat_rows),
            "enumValueCount": sum(int(target.get("enumValueCount", 0) or 0) for target in targets),
            "kswordAliasFieldCount": len(alias_rows),
            "publicSymbolCount": len(public_symbols),
            "publicSymbolGroupCounts": public_groups,
            "elapsedSeconds": round(time.time() - started, 3),
        },
        "privateTypeReadiness": {
            "ready": len(private_missing) == 0,
            "presentPrivateTypes": private_present,
            "missingPrivateTypes": private_missing,
            "reason": "private_win32k_types_available" if not private_missing else "public_pdb_does_not_expose_required_gui_private_types",
        },
        "targets": targets,
        "flatFields": flat_rows,
        "kswordAliasFields": alias_rows,
        "publicSymbols": public_symbols,
        "selectedTypesWithoutFieldList": selected_but_missing_fields,
        "notes": [
            "publicSymbols 的 section:offset 需要结合目标 PE 节表转换，不能直接当作运行时 RVA。",
            "privateTypeReadiness.ready 为 false 时，不能启用 tagWND/tagTHREADINFO/tagQ/tagHOOK/tagHOTKEY/tagTIMER/tagEVENTHOOK 字段读取。",
        ],
    }


def write_combined_csv(path: Path, modules: list[dict[str, Any]]) -> None:
    """把所有模块的 flatFields 写成一个 CSV，方便人工审阅。"""
    rows: list[dict[str, Any]] = []
    for module in modules:
        for row in module.get("flatFields", []):
            if isinstance(row, dict):
                rows.append(row)
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "moduleName",
        "group",
        "typeName",
        "typeSize",
        "fieldName",
        "qualifiedName",
        "offset",
        "offsetHex",
        "fieldType",
        "typeId",
        "kswordItemName",
        "runtimeItemId",
        "runtimeItemIdHex",
        "collisionAttempt",
        "bitBaseType",
        "bitOffset",
        "bitSize",
    ]
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def collect_runtime_public_symbol_examples(
    modules: list[dict[str, Any]],
    group_names: list[str],
    max_examples_per_group: int = 5,
) -> dict[str, list[dict[str, Any]]]:
    """按符号组收集 runtime 详情可用的 public symbol 示例。

    输入：
    - modules：extract_module_catalog 返回的 win32k-family 模块目录；
    - group_names：runtime domain 关心的 public symbol 分组；
    - max_examples_per_group：每个分组最多保留的示例数。
    处理：
    - 遍历每个模块 publicSymbols；
    - 只保留 name/moduleName/sectionOffset/flags 等审计展示必需字段；
    - 对同一分组限制数量，避免 JSON 被 public symbol 示例无限放大。
    返回：
    - dict[groupName] -> 示例列表；没有证据的分组返回空列表。
    """
    examples_by_group: dict[str, list[dict[str, Any]]] = {group_name: [] for group_name in group_names}
    wanted_groups = set(group_names)
    for module in modules:
        module_name = str(module.get("moduleName", ""))
        for symbol in module.get("publicSymbols", []):
            if not isinstance(symbol, dict):
                continue
            group_name = str(symbol.get("group", ""))
            if group_name not in wanted_groups:
                continue
            group_examples = examples_by_group.setdefault(group_name, [])
            if len(group_examples) >= max_examples_per_group:
                continue
            group_examples.append({
                "moduleName": module_name,
                "name": symbol.get("name", ""),
                "sectionOffset": symbol.get("sectionOffset", ""),
                "flags": symbol.get("flags", ""),
            })
    return examples_by_group


def build_runtime_detail_catalog(modules: list[dict[str, Any]]) -> dict[str, Any]:
    """生成 win32k 运行时详情域 readiness 目录。

    输入：
    - modules：全部 win32k / win32kbase / win32kfull 模块目录。
    处理：
    - 汇总每个模块已出现的私有 GUI 类型、缺失类型和 public symbol 分组计数；
    - 按 RUNTIME_DETAIL_DOMAINS 判断 window/gui-thread/hotkey/hook/event-hook/desktop 域是否具备字段读取条件；
    - 对 public PDB 可用的符号证据给出分组计数和代表符号，供 UI 详情页展示具体内容。
    返回：
    - JSON 可序列化目录；ready=false 时 blockedBy 会明确指出缺少 private layout。
    """
    present_private_types: set[str] = set()
    missing_by_module: dict[str, list[str]] = {}
    public_group_counts: dict[str, int] = {}
    field_counts_by_private_type: dict[str, int] = {type_name: 0 for type_name in PRIVATE_GUI_TYPES}

    for module in modules:
        module_name = str(module.get("moduleName", ""))
        readiness = module.get("privateTypeReadiness", {})
        if isinstance(readiness, dict):
            for type_name in readiness.get("presentPrivateTypes", []):
                present_private_types.add(str(type_name))
            missing_types = [
                str(type_name)
                for type_name in readiness.get("missingPrivateTypes", [])
            ] if isinstance(readiness.get("missingPrivateTypes", []), list) else []
            if missing_types:
                missing_by_module[module_name] = missing_types

        stats = module.get("stats", {})
        if isinstance(stats, dict) and isinstance(stats.get("publicSymbolGroupCounts", {}), dict):
            for group_name, count_value in stats["publicSymbolGroupCounts"].items():
                try:
                    public_group_counts[str(group_name)] = public_group_counts.get(str(group_name), 0) + int(count_value)
                except (TypeError, ValueError):
                    continue

        for target in module.get("targets", []):
            if not isinstance(target, dict):
                continue
            type_name = str(target.get("typeName", ""))
            if type_name in field_counts_by_private_type:
                field_counts_by_private_type[type_name] += int(target.get("fieldCount", 0) or 0)

    domains: dict[str, dict[str, Any]] = {}
    ready_domain_count = 0
    for domain_id, domain_definition in RUNTIME_DETAIL_DOMAINS.items():
        required_types = [str(item) for item in domain_definition.get("requiredPrivateTypes", [])]
        symbol_groups = [str(item) for item in domain_definition.get("usefulPublicSymbolGroups", [])]
        missing_types = [type_name for type_name in required_types if type_name not in present_private_types]
        ready = not missing_types
        if ready:
            ready_domain_count += 1

        public_examples = collect_runtime_public_symbol_examples(modules, symbol_groups)
        concrete_field_count = sum(field_counts_by_private_type.get(type_name, 0) for type_name in required_types)
        domain_group_counts = {
            group_name: public_group_counts.get(group_name, 0)
            for group_name in symbol_groups
        }
        domains[domain_id] = {
            "displayName": domain_definition.get("displayName", domain_id),
            "ready": ready,
            "requiredPrivateTypes": required_types,
            "presentPrivateTypes": [type_name for type_name in required_types if type_name in present_private_types],
            "missingPrivateTypes": missing_types,
            "concreteFieldCount": concrete_field_count,
            "publicSymbolGroups": domain_group_counts,
            "publicSymbolExamples": public_examples,
            "publicEvidenceAvailable": any(count > 0 for count in domain_group_counts.values()),
            "blockedBy": ""
            if ready
            else "missing private win32k GUI layout types: " + ", ".join(missing_types),
            "intendedUse": domain_definition.get("intendedUse", ""),
        }

    return {
        "schemaVersion": 1,
        "ready": ready_domain_count == len(RUNTIME_DETAIL_DOMAINS),
        "readyDomainCount": ready_domain_count,
        "blockedDomainCount": len(RUNTIME_DETAIL_DOMAINS) - ready_domain_count,
        "presentPrivateTypes": sorted(present_private_types),
        "missingPrivateTypesByModule": missing_by_module,
        "publicSymbolGroupCounts": public_group_counts,
        "domains": domains,
        "notes": [
            "ready=false 不代表 public PDB 没有价值；public symbol examples 仍可用于 UI 归因和审计解释。",
            "只有 requiredPrivateTypes 全部存在时，R0 才能把相应 runtime detail 域从 readiness 提升为字段读取。",
        ],
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析命令行参数。"""
    parser = argparse.ArgumentParser(description="Extract win32k-family public PDB GUI audit catalogs.")
    parser.add_argument("--llvm-pdbutil", default=DEFAULT_LLVM_PDBUTIL, help="llvm-pdbutil executable path")
    parser.add_argument("--pdb", action="append", default=[], help="win32k-family PDB path; can be repeated")
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR, help="directory for JSON/CSV output")
    parser.add_argument("--json-name", default="win32k_gui_public_deep_offsets.json", help="JSON output file name")
    parser.add_argument("--csv-name", default="win32k_gui_public_deep_offsets.csv", help="CSV output file name")
    parser.add_argument("--repo-json", default=DEFAULT_REPO_JSON, help="optional repository JSON output path")
    parser.add_argument("--cache-dir", default=r"D:\Temp\ksword_pdb_deep_offsets\win32k_cache", help="raw pdbutil cache directory")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """主入口：生成 win32k-family deep/public catalog JSON。"""
    args = parse_args(argv)
    pdbutil_path = str(Path(args.llvm_pdbutil))
    if not Path(pdbutil_path).exists():
        print(f"llvm-pdbutil not found: {pdbutil_path}", file=sys.stderr)
        return 2

    pdb_paths = [Path(item) for item in (args.pdb or DEFAULT_PDBS)]
    missing_paths = [str(path) for path in pdb_paths if not path.exists()]
    if missing_paths:
        print("PDB not found: " + "; ".join(missing_paths), file=sys.stderr)
        return 2

    started = time.time()
    cache_dir = Path(args.cache_dir)
    modules = [extract_module_catalog(pdbutil_path, pdb_path, cache_dir) for pdb_path in pdb_paths]
    all_fields = sum(int(module.get("stats", {}).get("fieldCount", 0) or 0) for module in modules)
    all_aliases = sum(int(module.get("stats", {}).get("kswordAliasFieldCount", 0) or 0) for module in modules)
    all_symbols = sum(int(module.get("stats", {}).get("publicSymbolCount", 0) or 0) for module in modules)
    missing_private: dict[str, list[str]] = {}
    for module in modules:
        readiness = module.get("privateTypeReadiness", {})
        if isinstance(readiness, dict) and not readiness.get("ready", False):
            missing_private[str(module.get("moduleName", ""))] = list(readiness.get("missingPrivateTypes", []))
    runtime_detail_catalog = build_runtime_detail_catalog(modules)

    result = {
        "schemaVersion": 1,
        "kind": "KswordWin32kDeepOffsetLibrary",
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "generator": "tools/pdb_offset_generator/ksword_win32k_pdb_deep_offsets.py",
        "stats": {
            "moduleCount": len(modules),
            "fieldCount": all_fields,
            "kswordAliasFieldCount": all_aliases,
            "publicSymbolCount": all_symbols,
            "privateTypeReady": not missing_private,
            "elapsedSeconds": round(time.time() - started, 3),
        },
        "missingPrivateTypesByModule": missing_private,
        "runtimeDetailCatalog": runtime_detail_catalog,
        "modules": modules,
        "notes": [
            "当前 public win32kbase/win32kfull PDB 可能报告 Has Types=true，但 TPI dump 实际为 0 records；本库会如实记录。",
            "tagWND/tagTHREADINFO/tagQ/tagHOOK/tagHOTKEY/tagTIMER/tagEVENTHOOK 私有结构缺失时，R0 运行时 detail IOCTL 只能报告 readiness，不能读取对象字段。",
            "publicSymbols 可用于函数符号提取和 UI 归因；结构字段读取仍需要 private PDB 或其它经验证 profile。",
        ],
    }

    output_dir = Path(args.output_dir)
    json_path = output_dir / args.json_name
    csv_path = output_dir / args.csv_name
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_combined_csv(csv_path, modules)

    repo_json = str(args.repo_json).strip()
    if repo_json:
        repo_path = Path(repo_json)
        repo_path.parent.mkdir(parents=True, exist_ok=True)
        repo_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"json={json_path}")
    print(f"csv={csv_path}")
    if repo_json:
        print(f"repoJson={repo_json}")
    print(f"modules={len(modules)} fields={all_fields} aliases={all_aliases} publicSymbols={all_symbols} privateTypeReady={not missing_private}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
