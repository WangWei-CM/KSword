#!/usr/bin/env python3
"""Extract and audit location-aware KSword language-pack entries."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


HAN_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")
LATIN_RE = re.compile(r"[A-Za-z]")
PLACEHOLDER_RE = re.compile(r"%(?:L?\d+|n)|\{\d+\}")
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx", ".ui"}


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
    document.setdefault("context_translations", {})
    if not isinstance(document["context_translations"], dict):
        raise ValueError(f"context_translations must be an object: {path}")
    return document


def placeholders(text: str) -> list[str]:
    return sorted(PLACEHOLDER_RE.findall(text))


def allows_han_in_english_source(source_text: str) -> bool:
    """Allow identity data or embedded matching rules that intentionally keep Han text."""
    return (
        source_text
        == "Mapleleaf,存钱买油条（云舟API）,Extrella_Explorer,NtKrnl64,一花一树叶,hzh"
        or source_text.lstrip().startswith("$verdict = if($lower -match 'audit|审计|")
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
        elif isinstance(en_semantic[semantic_key], str) and HAN_RE.search(en_semantic[semantic_key]):
            errors.append(f"en-US semantic translation still contains Han text: {semantic_key!r}")
    zh_context = zh_pack.get("context_translations", {})
    en_context = en_pack.get("context_translations", {})
    for context_key in sorted(set(zh_context) | set(en_context)):
        if context_key not in zh_context:
            errors.append(f"missing zh-CN context translation: {context_key!r}")
        if context_key not in en_context:
            errors.append(f"missing en-US context translation: {context_key!r}")
        if context_key in zh_context and not isinstance(zh_context[context_key], str):
            errors.append(f"non-string zh-CN context translation: {context_key!r}")
        if context_key in en_context and not isinstance(en_context[context_key], str):
            errors.append(f"non-string en-US context translation: {context_key!r}")
        if context_key in zh_context and context_key in en_context:
            zh_value = zh_context[context_key]
            en_value = en_context[context_key]
            if placeholders(zh_value) != placeholders(en_value):
                errors.append(f"context placeholder mismatch: {context_key!r}")
            if zh_value.count("\n") != en_value.count("\n"):
                errors.append(f"context newline mismatch: {context_key!r}")
            if HAN_RE.search(en_value):
                errors.append(f"en-US context translation still contains Han text: {context_key!r}")

    # Runtime UI translation uses source_translations only as a controlled
    # fallback for otherwise-unbound widget properties and model headers. Every
    # extractable source string must therefore exist in both packs with matching
    # placeholders/newlines; explicit context keys remain the preferred contract.
    zh_source = zh_pack.get("source_translations", {})
    en_source = en_pack.get("source_translations", {})
    if not isinstance(zh_source, dict):
        errors.append("zh-CN source_translations must be an object")
        zh_source = {}
    if not isinstance(en_source, dict):
        errors.append("en-US source_translations must be an object")
        en_source = {}

    for source_text in sorted(extracted):
        if source_text not in zh_source:
            errors.append(f"missing zh-CN source translation: {source_text!r}")
        if source_text not in en_source:
            errors.append(f"missing en-US source translation: {source_text!r}")
        if source_text not in zh_source or source_text not in en_source:
            continue

        zh_value = zh_source[source_text]
        en_value = en_source[source_text]
        if not isinstance(zh_value, str):
            errors.append(f"non-string zh-CN source translation: {source_text!r}")
            continue
        if not isinstance(en_value, str):
            errors.append(f"non-string en-US source translation: {source_text!r}")
            continue
        source_placeholders = placeholders(source_text)
        if placeholders(zh_value) != source_placeholders:
            errors.append(f"zh-CN source placeholder mismatch: {source_text!r}")
        if placeholders(en_value) != source_placeholders:
            errors.append(f"en-US source placeholder mismatch: {source_text!r}")
        if zh_value.count("\n") != source_text.count("\n"):
            errors.append(f"zh-CN source newline mismatch: {source_text!r}")
        if en_value.count("\n") != source_text.count("\n"):
            errors.append(f"en-US source newline mismatch: {source_text!r}")
        if (
            HAN_RE.search(source_text)
            and HAN_RE.search(en_value)
            and not allows_han_in_english_source(source_text)
        ):
            errors.append(f"en-US source translation still contains Han text: {source_text!r}")
    return errors


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("report", "sync", "translate", "audit"))
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--zh-pack", type=Path, required=True)
    parser.add_argument("--en-pack", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    extracted = extract_source_strings(arguments.source_root)
    zh_pack = load_pack(arguments.zh_pack)
    en_pack = load_pack(arguments.en_pack)
    if arguments.report is not None:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(
            json.dumps(build_report(extracted), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    if arguments.command == "report":
        print(f"extracted {len(extracted)} unique source strings")
        return 0

    if arguments.command == "translate":
        raise RuntimeError(
            "Automatic source-string translation is disabled. Add a stable "
            "context_translations key at the original UI call site and translate "
            "that key in both language packs."
        )

    if arguments.command == "sync":
        print(
            f"source report contains {len(extracted)} strings; "
            "language packs were not modified"
        )
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
