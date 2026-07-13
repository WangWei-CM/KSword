#!/usr/bin/env python3
"""Extract, synchronize, translate, and audit KSword source language strings."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


HAN_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")
LATIN_RE = re.compile(r"[A-Za-z]")
PLACEHOLDER_RE = re.compile(r"%(?:L?\d+|n)|\{\d+\}")
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx", ".ui"}
COMMON_UI_WORDS = {
    "Add", "Apply", "Cancel", "Clear", "Close", "Current", "Debug", "Delete", "Detail",
    "Details", "Disabled", "Driver", "Drivers", "Empty", "Enabled", "Error", "Exit",
    "Export", "File", "Files", "Filter", "Hardware", "Handle", "Handles", "Import", "Info",
    "Information", "Interface", "Kernel", "License", "Memory", "Monitor", "Monitoring", "Network",
    "Object", "Objects", "Open", "Operation", "Overview", "Pause", "Permissions", "Plugins",
    "Process", "Processes", "Refresh", "Registry", "Remove", "Restart", "Run", "Save", "Search",
    "Settings", "Start", "Startup", "Status", "Stop", "Success", "System", "Unknown", "Update",
    "View", "Warning", "Watch", "Window", "Windows", "Utilities", "Welcome", "CPU", "GPU", "Disk",
}
MANUAL_ZH_TRANSLATIONS = {
    "Admin": "管理员",
    "CID HandleIndex：%1": "CID 句柄索引：%1",
    "ClientRect: [%1,%2,%3,%4]\n": "ClientRect：[%1,%2,%3,%4]\n",
    "Debug": "调试",
    "DenoiseFlags：%1 (%2)": "去噪标志：%1（%2）",
    "DetailStatus：%1 (%2)": "详细状态：%1（%2）",
    "ETW Providers": "ETW 提供程序",
    "Entry Array CRC32": "条目数组 CRC32",
    "Fatal": "致命",
    "Info": "信息",
    "Information": "信息",
    "Invalid": "无效",
    "Invalid language pack size: %1": "无效的语言包大小：%1",
    "Invalid namespace": "无效命名空间",
    "FileObject Failed": "FileObject 查询失败",
    "Interface": "接口",
    "Internet Explorer Explorer Bar": "Internet Explorer 浏览器栏",
    "Internet Explorer Toolbar": "Internet Explorer 工具栏",
    "Internet Explorer URL Search Hooks": "Internet Explorer URL 搜索钩子",
    "InstallGlobalMessageBoxTheme finished": "全局消息框主题安装完成",
    "InstallGlobalTableColumnAutoFit finished": "全局表格列自动调整大小完成",
    "Invariant TSC": "不变 TSC",
    "JMP rel32": "JMP rel32",
    "JMP rel8": "JMP rel8",
    "JMP [RIP+rel32]": "JMP [RIP+rel32]",
    "KSW_CAP_PROCESS_PROTECTION_PATCH present": "KSW_CAP_PROCESS_PROTECTION_PATCH 已存在",
    "Kernel HandleTable": "内核句柄表",
    "Kernel Only": "仅内核",
    "Kernel VA": "内核虚拟地址",
    "Ksword Driver Capability Diagnostic Report": "Ksword 驱动能力诊断报告",
    "Ksword DynData Diagnostic Report": "Ksword DynData 诊断报告",
    "Ksword DynData PDB Profile Report": "Ksword DynData PDB 配置文件报告",
    "Ksword extra table": "Ksword 额外表",
    "Ksword HTTPS Root CA": "Ksword HTTPS 根 CA",
    "Ksword network readonly audit session": "Ksword 网络只读审计会话",
    "Ksword runtime offset": "Ksword 运行时偏移",
    "PDB Profile Active": "PDB 配置已启用",
    "Runtime pattern": "运行时特征",
    "Ksword runtime pattern": "Ksword 运行时特征",
    "Ksword WFP firewall event monitor": "Ksword WFP 防火墙事件监视器",
    "KswordARK DeviceIoControl": "KswordARK DeviceIoControl",
    "KswordARK Driver Service": "KswordARK 驱动服务",
    "Level 1": "级别 1",
    "Level 2": "级别 2",
    "LinkedToken: Unavailable\n": "链接令牌：不可用\n",
    "LOCAL SERVICE": "本地服务",
    "Linux LVM": "Linux LVM",
    "Linux Swap": "Linux Swap",
    "Object Failed": "对象查询失败",
    "OpenProcess failed": "OpenProcess 失败",
    "OpenProcess failed %1": "OpenProcess 失败 %1",
    "OpenProcess(for DLL inject) failed: ": "OpenProcess（用于 DLL 注入）失败： ",
    "OpenProcess(for injection) failed: ": "OpenProcess（用于注入）失败： ",
    "OpenProcess(for job terminate) failed: ": "OpenProcess（用于作业终止）失败： ",
    "OpenProcess(for nt job terminate) failed: ": "OpenProcess（用于 NT 作业终止）失败： ",
    "OpenProcess(for shellcode inject) failed: ": "OpenProcess（用于 Shellcode 注入）失败： ",
    "OpenProcess(for token) failed: ": "OpenProcess（用于令牌）失败： ",
    "OpenProcess(for unload module) failed: ": "OpenProcess（用于卸载模块）失败： ",
    "OpenProcess(PROCESS_DUP_HANDLE) failed: ": "OpenProcess(PROCESS_DUP_HANDLE) 失败： ",
    "OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) failed: ": "OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) 失败： ",
    "OpenProcess(PROCESS_SET_INFORMATION) failed: ": "OpenProcess(PROCESS_SET_INFORMATION) 失败： ",
    "OpenProcess(PROCESS_SUSPEND_RESUME) failed: ": "OpenProcess(PROCESS_SUSPEND_RESUME) 失败： ",
    "OpenProcess(PROCESS_TERMINATE) failed: ": "OpenProcess(PROCESS_TERMINATE) 失败： ",
    "OpenProcess(PROCESS_VM_OPERATION) failed: ": "OpenProcess(PROCESS_VM_OPERATION) 失败： ",
    "OpenProcessToken failed, error: ": "OpenProcessToken 失败，错误： ",
    "OpenProcessToken failed: ": "OpenProcessToken 失败： ",
    "OpenProcessToken on target process failed, error: ": "目标进程的 OpenProcessToken 失败，错误： ",
    "OpenSCManagerW failed": "OpenSCManagerW 失败",
    "OpenServiceW failed": "OpenServiceW 失败",
    "OpenThread failed %1": "OpenThread 失败 %1",
    "OpenThread(THREAD_SUSPEND_RESUME) failed: ": "OpenThread(THREAD_SUSPEND_RESUME) 失败： ",
    "OpenThread(THREAD_TERMINATE) failed: ": "OpenThread(THREAD_TERMINATE) 失败： ",
    "Owner process hidden": "所有者进程已隐藏",
    "PDB profile active": "PDB 配置已启用",
    "PDB profile pack": "PDB 配置包",
    "PDB profile scattered JSON": "PDB 分散 JSON 配置",
    "PSAPI fallback open process failed: ": "PSAPI 回退打开进程失败： ",
    "ResumeThread failed: ": "ResumeThread 失败： ",
    "System Informer": "System Informer",
    "System Informer DynData": "System Informer DynData",
    "Warn": "警告",
    "Error": "错误",
    "pattern not found": "未找到特征",
    "after startup process privilege request": "启动进程特权请求之后",
    "before startup process privilege request": "启动进程特权请求之前",
    "powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand ": "powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand ",
}
PRESERVED_ZH_TECHNICAL = {
    "DynData R3 IO", "EFI PART", "ETW DynData", "FAT32 CHS", "FAT32 LBA", "GPT %1",
    "JMP [RIP+rel32]", "JMP rel32", "JMP rel8", "KswordARK DeviceIoControl", "Linux LVM", "Linux Swap",
    "GlobalGetAtomNameW + GetClipboardFormatNameW", "Internet Explorer",
    "NVIDIA GPU", "NtOpenDirectoryObject + NtQueryDirectoryObject",
    "NtOpenDirectoryObject + NtQueryDirectoryObject + NtOpenSymbolicLinkObject + NtQuerySymbolicLinkObject",
    "POSIX CUI", "POST %1", "PPL %1",
    "UTF-16 BE", "UTF-16 LE", "Windows", "Windows API", "Windows CUI", "Windows GUI",
    "Winlogon AppSetup", "Winlogon Shell", "Winlogon Taskman", "Winlogon Userinit",
    "bash -c", "bcdedit %1",
    "powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand",
    "System Informer", "System Informer DynData",
    "win32k GlobalHookChain", "win32k HotkeyTable", "win32k ThreadHookChain",
    "yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd HH:mm:ss.zzz",
}


@dataclass
class Occurrence:
    path: str
    line: int


@dataclass
class ExtractedString:
    text: str
    occurrences: list[Occurrence] = field(default_factory=list)

    @property
    def stable_key(self) -> str:
        digest = hashlib.sha1(self.text.encode("utf-8")).hexdigest()[:16]
        return f"source.{digest}"


def is_extractable_literal(text: str) -> bool:
    """Keep UI-like source text while excluding paths, styles, and code tokens."""
    if HAN_RE.search(text):
        return True
    value = text.strip()
    if not value or not LATIN_RE.search(value):
        return False
    if any(marker in value for marker in ("\\", "://", "::", "{", "}", ";", "#")):
        return False
    if value.startswith((":/", "--", "-", "<", "$")):
        return False
    if "/" in value or "\\" in value:
        return False
    if re.fullmatch(r"[A-Za-z0-9_+=.?&*|<>:-]+", value) and not re.search(r"[A-Z][a-z]", value):
        return False
    return True


def requires_translation(source_text: str) -> bool:
    """Identify human-facing English labels that must have a Chinese entry."""
    if HAN_RE.search(source_text):
        return True
    value = source_text.strip()
    if not value or value.startswith("(") and value.endswith(")"):
        return False
    if not value[0].isalpha():
        return False
    if value.startswith(("%", '"', "'", "$")) or "--" in value:
        return False
    if value in PRESERVED_ZH_TECHNICAL:
        return False
    if value in COMMON_UI_WORDS:
        return True
    if re.search(r"\bR[03]\b|\bQ[A-Z][A-Za-z0-9_]*\b|\b(?:NDIS|TCP|UDP|IO)\b", value):
        return False
    if "SELECT " in value.upper() or " LIKE " in value.upper() or "TargetInstance" in value:
        return False
    if re.fullmatch(r"(?:TLS|TX)\s+.*", value):
        return False
    if re.fullmatch(r"G%\d+(?: CPU%\d+)(?: V%\d+)?", value):
        return False
    if not re.search(r"\s", value):
        return False
    if re.search(r"=|0x|\.(?:dll|lib|exe|sys|pdb|json)$", value, re.IGNORECASE):
        return False
    if "<" in value or "*." in value or re.fullmatch(r"[A-Za-z0-9_]+\([^)]*\)", value):
        return False
    if any(marker in value for marker in ("StubPath", "Dlls", "DROP (", "ACL:")):
        return False
    if PLACEHOLDER_RE.search(value) and value.split()[0].rstrip(":") in {"CPU", "GPU", "PID", "R0"}:
        return False
    if not value[0].isalnum() or value[-1] in "=|":
        return False
    if re.search(r"[A-Za-z0-9_.-]+\s*:\s*%", value):
        return False
    if re.search(r"\b(?:true|false|unknown|n/a|io|status|flags?)\b\s*[:=]", value, re.IGNORECASE):
        return False
    return bool(
        LATIN_RE.search(value)
        and (re.search(r"[A-Z][a-z]", value) or re.search(r"\s", value))
    )


def decode_cpp_string_body(body: str) -> str:
    result: list[str] = []
    index = 0
    simple_escapes = {
        "a": "\a",
        "b": "\b",
        "f": "\f",
        "n": "\n",
        "r": "\r",
        "t": "\t",
        "v": "\v",
        "\\": "\\",
        '"': '"',
        "'": "'",
        "?": "?",
    }
    while index < len(body):
        character = body[index]
        if character != "\\" or index + 1 >= len(body):
            result.append(character)
            index += 1
            continue

        escape = body[index + 1]
        if escape in simple_escapes:
            result.append(simple_escapes[escape])
            index += 2
            continue
        if escape == "\n":
            index += 2
            continue
        if escape == "\r":
            index += 2
            if index < len(body) and body[index] == "\n":
                index += 1
            continue
        if escape in "xuU":
            if escape == "x":
                match = re.match(r"[0-9A-Fa-f]+", body[index + 2 :])
                width = len(match.group(0)) if match else 0
            else:
                width = 4 if escape == "u" else 8
            digits = body[index + 2 : index + 2 + width]
            if digits and all(value in "0123456789abcdefABCDEF" for value in digits):
                try:
                    result.append(chr(int(digits, 16)))
                    index += 2 + width
                    continue
                except ValueError:
                    pass
        if escape in "01234567":
            match = re.match(r"[0-7]{1,3}", body[index + 1 :])
            digits = match.group(0) if match else escape
            result.append(chr(int(digits, 8)))
            index += 1 + len(digits)
            continue

        result.append("\\")
        result.append(escape)
        index += 2
    return "".join(result)


def extract_cpp_literals(source_text: str) -> Iterable[tuple[str, int]]:
    index = 0
    line = 1
    length = len(source_text)
    while index < length:
        character = source_text[index]
        if character == "\n":
            line += 1
            index += 1
            continue
        if source_text.startswith("//", index):
            newline_index = source_text.find("\n", index + 2)
            if newline_index < 0:
                return
            index = newline_index
            continue
        if source_text.startswith("/*", index):
            close_index = source_text.find("*/", index + 2)
            if close_index < 0:
                return
            line += source_text.count("\n", index, close_index + 2)
            index = close_index + 2
            continue
        if character == "'":
            index += 1
            while index < length:
                if source_text[index] == "\\":
                    index += 2
                elif source_text[index] == "'":
                    index += 1
                    break
                else:
                    if source_text[index] == "\n":
                        line += 1
                    index += 1
            continue

        prefix = next(
            (
                candidate
                for candidate in (
                    'u8R"',
                    'uR"',
                    'UR"',
                    'LR"',
                    'R"',
                    'u8"',
                    'u"',
                    'U"',
                    'L"',
                    '"',
                )
                if source_text.startswith(candidate, index)
            ),
            None,
        )
        if prefix is None:
            index += 1
            continue

        token_start = index
        token_line = line
        index += len(prefix)
        if "R" in prefix:
            delimiter_end = source_text.find("(", index)
            if delimiter_end < 0:
                index = token_start + 1
                continue
            delimiter = source_text[index:delimiter_end]
            terminator = ")" + delimiter + '"'
            body_start = delimiter_end + 1
            body_end = source_text.find(terminator, body_start)
            if body_end < 0:
                index = token_start + 1
                continue
            body = source_text[body_start:body_end]
            line += source_text.count("\n", token_start, body_end + len(terminator))
            index = body_end + len(terminator)
            if is_extractable_literal(body):
                yield body, token_line
            continue

        body_characters: list[str] = []
        while index < length:
            current = source_text[index]
            if current == "\\" and index + 1 < length:
                body_characters.append(current)
                body_characters.append(source_text[index + 1])
                if source_text[index + 1] == "\n":
                    line += 1
                index += 2
                continue
            if current == '"':
                index += 1
                break
            body_characters.append(current)
            if current == "\n":
                line += 1
            index += 1
        decoded_text = decode_cpp_string_body("".join(body_characters))
        if is_extractable_literal(decoded_text):
            yield decoded_text, token_line


def extract_ui_strings(source_text: str) -> Iterable[tuple[str, int]]:
    string_re = re.compile(r"<string(?:\s+[^>]*)?>(.*?)</string>", re.DOTALL)
    for match in string_re.finditer(source_text):
        text = (
            match.group(1)
            .replace("&lt;", "<")
            .replace("&gt;", ">")
            .replace("&quot;", '"')
            .replace("&amp;", "&")
        )
        if is_extractable_literal(text):
            yield text, source_text.count("\n", 0, match.start()) + 1


def extract_source_strings(source_root: Path) -> dict[str, ExtractedString]:
    extracted: dict[str, ExtractedString] = {}
    for source_path in sorted(source_root.rglob("*")):
        if not source_path.is_file() or source_path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        try:
            source_text = source_path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            source_text = source_path.read_text(encoding="gb18030")
        iterator = (
            extract_ui_strings(source_text)
            if source_path.suffix.lower() == ".ui"
            else extract_cpp_literals(source_text)
        )
        relative_path = source_path.relative_to(source_root).as_posix()
        for text, line in iterator:
            entry = extracted.setdefault(text, ExtractedString(text=text))
            entry.occurrences.append(Occurrence(relative_path, line))
    return extracted


def load_pack(path: Path) -> dict:
    with path.open("r", encoding="utf-8-sig") as handle:
        document = json.load(handle)
    if not isinstance(document, dict):
        raise ValueError(f"Language pack root must be an object: {path}")
    document.setdefault("source_translations", {})
    if not isinstance(document["source_translations"], dict):
        raise ValueError(f"source_translations must be an object: {path}")
    return document


def save_pack(path: Path, document: dict) -> None:
    translations = document.get("translations", {})
    source_translations = document.get("source_translations", {})
    output_document = dict(document)
    output_document["translations"] = dict(sorted(translations.items()))
    output_document["source_translations"] = dict(sorted(source_translations.items()))
    path.write_text(
        json.dumps(output_document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def placeholders(text: str) -> list[str]:
    return sorted(PLACEHOLDER_RE.findall(text))


def han_count(text: str) -> int:
    return len(HAN_RE.findall(text))


def is_embedded_executable_text(text: str) -> bool:
    lowered = text.lower()
    powershell_markers = ("-match", "elseif", "write-output", "write-host", "param(", "$verdict")
    return "$" in text and any(marker in lowered for marker in powershell_markers)


def has_invalid_han_residue(source_text: str, translated_text: str) -> bool:
    translated_han_count = han_count(translated_text)
    if translated_han_count == 0:
        return False
    return not (
        is_embedded_executable_text(source_text)
        and translated_han_count < han_count(source_text)
    )


def build_report(extracted: dict[str, ExtractedString]) -> dict:
    return {
        "schema": "ksword-i18n-source-report",
        "format_version": 1,
        "string_count": len(extracted),
        "strings": [
            {
                "key": entry.stable_key,
                "source": entry.text,
                "occurrences": [occurrence.__dict__ for occurrence in entry.occurrences],
            }
            for entry in sorted(extracted.values(), key=lambda value: value.stable_key)
        ],
    }


def audit(
    extracted: dict[str, ExtractedString],
    zh_pack: dict,
    en_pack: dict,
) -> list[str]:
    errors: list[str] = []
    zh_semantic = zh_pack.get("translations", {})
    en_semantic = en_pack.get("translations", {})
    for semantic_key in sorted(set(zh_semantic) | set(en_semantic)):
        if semantic_key not in zh_semantic:
            errors.append(f"missing zh-CN semantic translation: {semantic_key!r}")
        if semantic_key not in en_semantic:
            errors.append(f"missing en-US semantic translation: {semantic_key!r}")
    zh_map = zh_pack["source_translations"]
    en_map = en_pack["source_translations"]
    source_set = set(extracted)

    for source_text, entry in sorted(extracted.items()):
        location = entry.occurrences[0]
        location_text = f"{location.path}:{location.line}"
        source_is_chinese = bool(HAN_RE.search(source_text))
        zh_translation = zh_map.get(source_text)
        en_translation = en_map.get(source_text)
        if source_is_chinese:
            if zh_translation != source_text:
                errors.append(f"zh-CN source translation changed the source: {location_text}: {source_text!r}")
            target_translation = en_translation
            target_name = "en-US"
        else:
            if en_translation != source_text:
                errors.append(f"en-US source translation changed the source: {location_text}: {source_text!r}")
            target_translation = zh_translation
            target_name = "zh-CN"
        if not isinstance(target_translation, str) or not target_translation.strip():
            errors.append(f"missing or empty {target_name} source translation: {location_text}: {source_text!r}")
            continue
        if requires_translation(source_text) and target_translation == source_text:
            errors.append(f"untranslated {target_name} source translation: {location_text}: {source_text!r}")
        if source_is_chinese and has_invalid_han_residue(source_text, target_translation):
            errors.append(f"Han characters remain in en-US translation: {location_text}: {target_translation!r}")
        if placeholders(source_text) != placeholders(target_translation):
            errors.append(
                "placeholder mismatch: "
                f"{location_text}: {source_text!r} -> {target_translation!r}"
            )
        if source_text.count("\n") != target_translation.count("\n"):
            errors.append(
                "newline count mismatch: "
                f"{location_text}: {source_text!r} -> {target_translation!r}"
            )

    stale_zh = sorted(set(zh_map) - source_set)
    stale_en = sorted(set(en_map) - source_set)
    if stale_zh:
        errors.append(f"stale zh-CN source translations: {len(stale_zh)}")
    if stale_en:
        errors.append(f"stale en-US source translations: {len(stale_en)}")
    return errors


def ollama_translate_batch(
    model: str,
    items: list[tuple[str, str]],
    target_language: str,
) -> dict[str, str]:
    input_items = []
    placeholder_tokens_by_id: dict[str, dict[str, str]] = {}
    for item_id, text in items:
        protected_text = text.strip()
        placeholder_tokens: dict[str, str] = {}
        for placeholder_index, placeholder in enumerate(dict.fromkeys(PLACEHOLDER_RE.findall(protected_text))):
            token_letter = chr(ord("A") + placeholder_index)
            token = f"__KSWPH_{token_letter}__"
            protected_text = protected_text.replace(placeholder, token)
            placeholder_tokens[token] = placeholder
        protected_text = protected_text.replace("\n", "__KSW_NEWLINE__")
        protected_text = protected_text.replace("\t", "__KSW_TAB__")
        placeholder_tokens["__KSW_NEWLINE__"] = "\n"
        placeholder_tokens["__KSW_TAB__"] = "\t"
        placeholder_tokens_by_id[item_id] = placeholder_tokens
        input_items.append({"id": item_id, "text": protected_text})
    item_ids = [item_id for item_id, _ in items]
    response_schema = {
        "type": "object",
        "properties": {
            "translations": {
                "type": "array",
                "minItems": len(items),
                "maxItems": len(items),
                "items": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "string", "enum": item_ids},
                        "text": {"type": "string"},
                    },
                    "required": ["id", "text"],
                    "additionalProperties": False,
                },
            }
        },
        "required": ["translations"],
        "additionalProperties": False,
    }
    if target_language == "zh-CN":
        translation_instruction = (
            "Translate every English source string into concise professional Simplified Chinese. "
            "Translate standalone words, separators, labels, UI text, status text, diagnostics, "
            "and logs one item at a time. Do not mechanically replace fragments or merge items. "
            "For mixed Chinese-English fields, translate every natural-language Chinese word while "
            "preserving embedded identifiers and protocol names. "
            "Preserve product names, technical identifiers, paths, command switches, registry keys, "
            "API names, and code punctuation when they are not human prose."
        )
    else:
        translation_instruction = (
            "Translate every Chinese source string into concise professional US English. "
            "Translate standalone words, separators, labels, UI text, status text, diagnostics, "
            "and logs one item at a time. Do not mechanically replace fragments or merge items. "
            "For mixed Chinese-English fields, translate every natural-language Chinese word while "
            "preserving embedded identifiers and protocol names. "
            "Preserve product names, technical identifiers, paths, command switches, registry keys, "
            "API names, and code punctuation when they are not human prose."
        )
    request_body = {
        "model": model,
        "stream": False,
        "think": False,
        "format": response_schema,
        "options": {"temperature": 0, "num_predict": 16384},
        "messages": [
            {
                "role": "system",
                "content": (
                    translation_instruction + " Preserve tokens such as __KSWPH_A__, "
                    "__KSWPH_B__, __KSWPH_C__, __KSW_NEWLINE__, and __KSW_TAB__ exactly; never "
                    "rename, remove, duplicate, reorder, or translate these tokens. Preserve "
                    "technical identifiers, paths, command "
                    "switches, registry keys, API names, and code punctuation. Never refuse, "
                    "explain, merge, or omit an item. Return one translation for every input id."
                ),
            },
            {
                "role": "user",
                "content": json.dumps(input_items, ensure_ascii=False),
            },
        ],
    }
    request = urllib.request.Request(
        "http://127.0.0.1:11434/api/chat",
        data=json.dumps(request_body, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            outer_response = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Ollama API request failed: {error}") from error

    content_text = outer_response.get("message", {}).get("content", "")
    try:
        structured_response = json.loads(content_text)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"Ollama returned invalid structured content: {content_text[:800]}") from error

    translation_items = structured_response.get("translations", [])
    if not isinstance(translation_items, list):
        raise RuntimeError("Ollama structured response has no translations array")
    result: dict[str, str] = {}
    for item in translation_items:
        if not isinstance(item, dict):
            continue
        item_id = str(item.get("id", ""))
        translated_text = str(item.get("text", ""))
        for token, placeholder in placeholder_tokens_by_id.get(item_id, {}).items():
            translated_text = translated_text.replace(token, placeholder)
        result[item_id] = translated_text
    return result


def ollama_translate_batch_resilient(
    model: str,
    items: list[tuple[str, str]],
    target_language: str,
) -> dict[str, str]:
    try:
        return ollama_translate_batch(model, items, target_language)
    except RuntimeError:
        if len(items) <= 1:
            raise
        midpoint = len(items) // 2
        result = ollama_translate_batch_resilient(model, items[:midpoint], target_language)
        result.update(ollama_translate_batch_resilient(model, items[midpoint:], target_language))
        return result


def translate_multiline_preserving_layout(model: str, source_text: str, target_language: str) -> str:
    leading_whitespace = source_text[: len(source_text) - len(source_text.lstrip())]
    trailing_whitespace = source_text[len(source_text.rstrip()) :]
    core_text = source_text.strip()
    source_lines = core_text.split("\n")
    line_items = [
        (str(index), line_text)
        for index, line_text in enumerate(source_lines)
        if line_text.strip()
    ]
    if not line_items:
        return source_text
    translated_lines = ollama_translate_batch_resilient(model, line_items, target_language)
    rebuilt_lines = [
        translated_lines.get(str(index), "") if line_text.strip() else line_text
        for index, line_text in enumerate(source_lines)
    ]
    return leading_whitespace + "\n".join(rebuilt_lines) + trailing_whitespace


def translate_missing(
    extracted: dict[str, ExtractedString],
    target_pack: dict,
    target_pack_path: Path,
    model: str,
    batch_size: int,
    batch_characters: int,
    limit: int | None,
    target_language: str,
) -> int:
    target_map = target_pack["source_translations"]
    pending = [
        text
        for text in sorted(extracted)
        if (
            (HAN_RE.search(text) and target_language == "en-US")
            or (not HAN_RE.search(text) and target_language == "zh-CN" and requires_translation(text))
        )
        and (
            text not in target_map
            or not isinstance(target_map[text], str)
            or not target_map[text].strip()
            or target_map[text] == text
            or (target_language == "en-US" and has_invalid_han_residue(text, target_map[text]))
            or placeholders(text) != placeholders(target_map[text])
            or text.count("\n") != target_map[text].count("\n")
        )
    ]
    if limit is not None:
        pending = pending[:limit]
    batches: list[list[str]] = []
    current_batch: list[str] = []
    current_character_count = 0
    for source_text in pending:
        source_character_count = len(source_text)
        if current_batch and (
            len(current_batch) >= batch_size
            or current_character_count + source_character_count > batch_characters
        ):
            batches.append(current_batch)
            current_batch = []
            current_character_count = 0
        current_batch.append(source_text)
        current_character_count += source_character_count
    if current_batch:
        batches.append(current_batch)

    translated_count = 0
    for batch_texts in batches:
        unresolved = list(batch_texts)
        rejection_details: dict[str, tuple[str, list[str]]] = {}
        for attempt in range(3):
            if not unresolved:
                break
            attempt_items = [(str(index), text) for index, text in enumerate(unresolved)]
            result = ollama_translate_batch_resilient(model, attempt_items, target_language)
            next_unresolved: list[str] = []
            for index, source_text in enumerate(unresolved):
                raw_translation = result.get(str(index), "").strip()
                leading_whitespace = source_text[: len(source_text) - len(source_text.lstrip())]
                trailing_whitespace = source_text[len(source_text.rstrip()) :]
                translation = leading_whitespace + raw_translation + trailing_whitespace
                if source_text.count("\n") != translation.count("\n") and "\n" in source_text:
                    try:
                        translation = translate_multiline_preserving_layout(
                            model, source_text, target_language)
                        raw_translation = translation.strip()
                    except RuntimeError:
                        pass
                rejection_reasons: list[str] = []
                if not raw_translation:
                    rejection_reasons.append("empty or omitted")
                if translation == source_text:
                    rejection_reasons.append("unchanged")
                if target_language == "en-US" and has_invalid_han_residue(source_text, translation):
                    rejection_reasons.append("contains Han characters")
                if placeholders(source_text) != placeholders(translation):
                    rejection_reasons.append(
                        f"placeholders {placeholders(source_text)!r} != {placeholders(translation)!r}"
                    )
                if source_text.count("\n") != translation.count("\n"):
                    rejection_reasons.append(
                        f"newline count {source_text.count(chr(10))} != {translation.count(chr(10))}"
                    )
                if rejection_reasons:
                    rejection_details[source_text] = (translation, rejection_reasons)
                    next_unresolved.append(source_text)
                    continue
                target_map[source_text] = translation
                translated_count += 1
            unresolved = next_unresolved
        if unresolved:
            failed_preview = "\n".join(
                f"source={value!r}\ntranslation={rejection_details.get(value, ('', []))[0]!r}\n"
                f"reasons={rejection_details.get(value, ('', []))[1]!r}"
                for value in unresolved[:10]
            )
            raise RuntimeError(
                f"ollama failed to produce {len(unresolved)} valid translation(s) "
                f"after retries:\n{failed_preview}"
            )
        save_pack(target_pack_path, target_pack)
        print(f"translated {translated_count}/{len(pending)}", flush=True)
    return translated_count


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("report", "sync", "translate", "audit"))
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--zh-pack", type=Path, required=True)
    parser.add_argument("--en-pack", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--model", default="qwen3.5:4b")
    parser.add_argument("--batch-size", type=int, default=24)
    parser.add_argument("--batch-characters", type=int, default=12000)
    parser.add_argument("--limit", type=int)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    extracted = extract_source_strings(arguments.source_root)
    zh_pack = load_pack(arguments.zh_pack)
    en_pack = load_pack(arguments.en_pack)
    zh_pack["source_translations"].update(MANUAL_ZH_TRANSLATIONS)

    if arguments.report is not None:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(
            json.dumps(build_report(extracted), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    if arguments.command == "report":
        print(f"extracted {len(extracted)} unique source strings")
        return 0

    if arguments.command in {"sync", "translate"}:
        zh_map = zh_pack["source_translations"]
        en_map = en_pack["source_translations"]
        source_set = set(extracted)
        for source_text in source_set:
            zh_map.setdefault(source_text, source_text)
            en_map.setdefault(source_text, source_text)
        for stale_text in set(zh_map) - source_set:
            del zh_map[stale_text]
        for stale_text in set(en_map) - source_set:
            del en_map[stale_text]
        save_pack(arguments.zh_pack, zh_pack)
        save_pack(arguments.en_pack, en_pack)

    if arguments.command == "translate":
        en_translated_count = translate_missing(
            extracted,
            en_pack,
            arguments.en_pack,
            arguments.model,
            arguments.batch_size,
            arguments.batch_characters,
            arguments.limit,
            "en-US",
        )
        zh_translated_count = translate_missing(
            extracted,
            zh_pack,
            arguments.zh_pack,
            arguments.model,
            arguments.batch_size,
            arguments.batch_characters,
            arguments.limit,
            "zh-CN",
        )
        print(
            f"added {en_translated_count} en-US and {zh_translated_count} zh-CN translations"
        )
        return 0

    if arguments.command == "sync":
        print(f"synchronized {len(extracted)} source strings")
        return 0

    errors = audit(extracted, zh_pack, en_pack)
    if errors:
        print(f"i18n audit failed with {len(errors)} issue(s):", file=sys.stderr)
        for error in errors[:500]:
            print(f"- {error}", file=sys.stderr)
        if len(errors) > 500:
            print(f"- ... {len(errors) - 500} additional issue(s)", file=sys.stderr)
        return 1
    print(f"i18n audit passed: {len(extracted)} source strings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
