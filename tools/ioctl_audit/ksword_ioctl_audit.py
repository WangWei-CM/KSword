#!/usr/bin/env python3
"""Static KswordARKDriver IOCTL protocol and registry auditor.

Inputs:
    * shared/driver/*Ioctl.h for IOCTL_KSWORD_ARK_* CTL_CODE definitions.
    * KswordARKDriver/src/dispatch/ioctl_registry.c for registered handlers.
    * tools/ioctl_audit/ioctl_audit_rules.json for local audit policy.

Processing:
    The script parses source text only. It does not compile, load, or execute
    driver code. It resolves simple macro expressions, compares shared protocol
    definitions with the central registry, and applies conservative access
    heuristics to mutating and query-style IOCTL names.

Outputs:
    A text, markdown, or JSON report. When --out is omitted the default markdown
    and JSON reports are written under tools/ioctl_audit/out/. The process
    returns 2 only when --fail-on-risk is set and HIGH findings exist.
"""

from __future__ import annotations

import argparse
import ast
import datetime as dt
import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


DEFAULT_CONSTANTS: dict[str, int] = {
    "FILE_DEVICE_UNKNOWN": 0x00000022,
    "METHOD_BUFFERED": 0,
    "METHOD_IN_DIRECT": 1,
    "METHOD_OUT_DIRECT": 2,
    "METHOD_NEITHER": 3,
    "FILE_ANY_ACCESS": 0,
    "FILE_READ_ACCESS": 0x0001,
    "FILE_WRITE_ACCESS": 0x0002,
}

DEFAULT_MUTATING_KEYWORDS = [
    "SET",
    "WRITE",
    "PATCH",
    "TERMINATE",
    "KILL",
    "UNLOAD",
    "DELETE",
    "CREATE",
    "RENAME",
    "HIDE",
    "PROTECT",
    "APPLY",
    "SUSPEND",
    "CONTROL",
    "CANCEL",
    "REMOVE",
    "ANSWER",
    "DKOM",
]

DEFAULT_QUERY_KEYWORDS = [
    "QUERY",
    "READ",
    "ENUM",
    "GET",
    "SCAN",
    "WAIT",
    "DRAIN",
    "TRANSLATE",
    "STATUS",
]


@dataclass(slots=True)
class MacroDef:
    """A flattened #define block read from a shared header.

    Input fields record the macro name, body, source path, and start line.
    Processing is intentionally delayed so the same collection can feed scalar
    macro resolution and IOCTL extraction. Return behavior is passive data
    storage for report generation.
    """

    name: str
    body: str
    source: str
    line: int


@dataclass(slots=True)
class IoctlDef:
    """A resolved IOCTL_KSWORD_ARK_* CTL_CODE definition.

    Inputs are the four CTL_CODE expressions and their resolved values.
    Processing computes the final control code with the Windows CTL_CODE bit
    layout. Output behavior is JSON/Markdown serialization only.
    """

    name: str
    device_type_expr: str
    device_type_value: int | None
    function_expr: str
    function_id: int | None
    method_expr: str
    method_value: int | None
    method_name: str
    access_expr: str
    access_value: int | None
    access_name: str
    control_code: int | None
    source: str
    line: int
    registered: bool = False
    handler: str | None = None
    registry_line: int | None = None
    mutating_keywords: list[str] = field(default_factory=list)
    query_keywords: list[str] = field(default_factory=list)


@dataclass(slots=True)
class RegistryEntry:
    """One row from g_KswordArkIoctlTable.

    Inputs are the registered IOCTL macro, handler symbol, optional string name,
    source path, and line number. Processing is limited to table parsing; no
    business handler files are read. Output behavior is report serialization.
    """

    name: str
    handler: str
    string_name: str | None
    source: str
    line: int


@dataclass(slots=True)
class Finding:
    """One consistency or access-control finding.

    Inputs are severity, category, affected IOCTL/key, message, and structured
    details. Processing code appends these records during checks. Output
    behavior is report serialization.
    """

    severity: str
    category: str
    name: str
    message: str
    details: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class Rules:
    """Audit policy loaded from ioctl_audit_rules.json.

    Inputs are whitelist names, mutating/query keywords, ignored headers, and
    free-form notes. Processing normalizes names to uppercase and paths to
    forward slashes. Return behavior is read-only policy data for checks.
    """

    allowed_any_access: set[str]
    mutating_keywords: list[str]
    query_keywords: list[str]
    ignored_headers: set[str]
    notes: Any


class ExprResolver:
    """Resolve simple integer macro expressions without invoking a compiler."""

    def __init__(self, constants: dict[str, int | str]) -> None:
        """Store macro constants used by later expression resolution."""

        self.constants = constants

    def resolve(self, expr: str, stack: tuple[str, ...] = ()) -> int | None:
        """Return an integer value for a safe C-style macro expression.

        Inputs are an expression string and an internal recursion guard.
        Processing strips integer suffixes, substitutes known macro names, and
        evaluates a restricted AST containing only integer arithmetic and
        bitwise operations. The method returns None when a token is unknown.
        """

        cleaned = strip_suffixes(expr.strip())
        if not cleaned:
            return None
        substituted = cleaned
        names = sorted(set(re.findall(r"\b[A-Za-z_]\w*\b", cleaned)), key=len, reverse=True)
        for name in names:
            if name not in self.constants or name in stack:
                return None
            raw = self.constants[name]
            value = raw if isinstance(raw, int) else self.resolve(raw, stack + (name,))
            if value is None:
                return None
            substituted = re.sub(rf"\b{re.escape(name)}\b", str(value), substituted)
        try:
            return int(self._eval(ast.parse(substituted, mode="eval").body))
        except (SyntaxError, TypeError, ValueError, ZeroDivisionError):
            return None

    def _eval(self, node: ast.AST) -> int:
        """Evaluate only integer constants and safe unary/binary operators."""

        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return int(node.value)
        if isinstance(node, ast.UnaryOp):
            value = self._eval(node.operand)
            if isinstance(node.op, ast.UAdd):
                return value
            if isinstance(node.op, ast.USub):
                return -value
            if isinstance(node.op, ast.Invert):
                return ~value
        if isinstance(node, ast.BinOp):
            left = self._eval(node.left)
            right = self._eval(node.right)
            if isinstance(node.op, ast.BitOr):
                return left | right
            if isinstance(node.op, ast.BitAnd):
                return left & right
            if isinstance(node.op, ast.BitXor):
                return left ^ right
            if isinstance(node.op, ast.LShift):
                return left << right
            if isinstance(node.op, ast.RShift):
                return left >> right
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            if isinstance(node.op, ast.Mult):
                return left * right
            if isinstance(node.op, ast.FloorDiv):
                return left // right
        raise TypeError(f"unsupported expression: {ast.dump(node)}")


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    """Parse CLI options and return an argparse namespace.

    Inputs are an optional argv list supplied by tests or None for sys.argv.
    Processing defines the repository root, output format, output path, and
    fail-on-risk switch. The return value is the parsed argparse namespace; bad
    arguments are handled by argparse with its standard exit behavior.
    """

    parser = argparse.ArgumentParser(description="Audit KswordARKDriver IOCTL definitions")
    parser.add_argument("--repo-root", required=True, help="Repository root to scan.")
    parser.add_argument("--format", choices=("text", "json", "markdown"), default="markdown")
    parser.add_argument("--out", help="Report output path. Relative paths resolve under --repo-root.")
    parser.add_argument("--fail-on-risk", action="store_true", help="Exit 2 when HIGH findings are present.")
    return parser.parse_args(argv)


def rel(path: Path, root: Path) -> str:
    """Return a stable repository-relative path string for reports.

    Inputs are a path and the repository root. Processing attempts to make the
    path relative to the root and normalizes separators to forward slashes. The
    return value is a display string; no filesystem state is changed.
    """

    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def strip_comments(text: str) -> str:
    """Remove C block and line comments before macro/table parsing.

    Input is raw source text. Processing strips comments with regular
    expressions because the audit only needs macro and registry table syntax.
    The return value is comment-free text; the original file is untouched.
    """

    no_blocks = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*", "", no_blocks)


def strip_suffixes(expr: str) -> str:
    """Remove C integer suffixes such as UL and ULL from numeric literals.

    Input is a C-style numeric expression. Processing removes suffixes attached
    to decimal or hexadecimal literals only. The return value is suitable for
    restricted Python AST parsing.
    """

    return re.sub(r"(?i)(0x[0-9a-f]+|\b\d+)(?:ull|llu|ul|lu|u|l)\b", r"\1", expr)


def split_args(text: str) -> list[str]:
    """Split a comma-separated argument list while respecting nesting/strings.

    Input is the text inside a macro call or initializer. Processing tracks
    parenthesis depth and string literal state so nested commas are preserved.
    The return value is a list of trimmed argument strings.
    """

    parts: list[str] = []
    current: list[str] = []
    depth = 0
    in_string = False
    escaped = False
    for char in text:
        if in_string:
            current.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
            current.append(char)
        elif char == "(":
            depth += 1
            current.append(char)
        elif char == ")":
            depth = max(0, depth - 1)
            current.append(char)
        elif char == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(char)
    if current:
        parts.append("".join(current).strip())
    return parts


def parse_macros(header: Path, root: Path) -> list[MacroDef]:
    """Read one header and return flattened macro definitions.

    Inputs are a header path and repo root. Processing joins backslash
    continuation lines and records the starting source line. The function opens
    the file read-only and returns MacroDef rows.
    """

    lines = strip_comments(header.read_text(encoding="utf-8-sig", errors="replace")).splitlines()
    macros: list[MacroDef] = []
    index = 0
    while index < len(lines):
        stripped = lines[index].strip()
        if not stripped.startswith("#define"):
            index += 1
            continue
        start_line = index + 1
        chunks = [stripped]
        while chunks[-1].rstrip().endswith("\\") and index + 1 < len(lines):
            index += 1
            chunks.append(lines[index].strip())
        flattened = " ".join(chunk.rstrip("\\").strip() for chunk in chunks)
        match = re.match(r"#define\s+([A-Za-z_]\w*)\b(?:\s+(.*))?$", flattened)
        if match:
            macros.append(MacroDef(match.group(1), (match.group(2) or "").strip(), rel(header, root), start_line))
        index += 1
    return macros


def ctl_args(body: str) -> list[str] | None:
    """Extract four CTL_CODE arguments from a macro body.

    Input is a flattened macro body. Processing finds the matching CTL_CODE
    parenthesis pair and splits the inner argument list. The return value is a
    four-item list or None when the macro is malformed or not an IOCTL.
    """

    start = body.find("CTL_CODE")
    if start < 0:
        return None
    open_index = body.find("(", start)
    if open_index < 0:
        return None
    depth = 0
    for index in range(open_index, len(body)):
        if body[index] == "(":
            depth += 1
        elif body[index] == ")":
            depth -= 1
            if depth == 0:
                args = split_args(body[open_index + 1 : index])
                return args if len(args) == 4 else None
    return None


def load_rules(root: Path) -> Rules:
    """Load rules JSON, apply defaults, and normalize names for matching.

    Input is the repository root. Processing reads the optional JSON rules file,
    supplies defaults for missing keys, uppercases keyword/name policy, and adds
    basename aliases for ignored headers. The return value is a Rules object.
    """

    path = root / "tools" / "ioctl_audit" / "ioctl_audit_rules.json"
    raw: dict[str, Any] = {}
    if path.exists():
        raw = json.loads(path.read_text(encoding="utf-8-sig"))
    ignored = {str(item).replace("\\", "/") for item in raw.get("ignoredHeaders", [])}
    ignored.update(Path(item).name for item in list(ignored))
    return Rules(
        allowed_any_access={str(item).upper() for item in raw.get("allowedAnyAccess", [])},
        mutating_keywords=[str(item).upper() for item in raw.get("mutatingKeywords", DEFAULT_MUTATING_KEYWORDS)],
        query_keywords=[str(item).upper() for item in raw.get("queryKeywords", DEFAULT_QUERY_KEYWORDS)],
        ignored_headers=ignored,
        notes=raw.get("notes", {}),
    )


def discover_headers(root: Path, rules: Rules) -> list[Path]:
    """Return shared/driver/*Ioctl.h files that are not ignored by policy.

    Inputs are the repository root and loaded rules. Processing glob-matches
    shared IOCTL headers and filters repository-relative or basename ignore
    entries. The return value is a sorted list of header paths.
    """

    headers = sorted((root / "shared" / "driver").glob("*Ioctl.h"))
    selected: list[Path] = []
    for header in headers:
        relative = rel(header, root)
        if relative not in rules.ignored_headers and header.name not in rules.ignored_headers:
            selected.append(header)
    return selected


def build_constants(macros: list[MacroDef]) -> dict[str, int | str]:
    """Build a name-to-expression map used by ExprResolver.

    Inputs are all parsed macros. Processing seeds Windows constants and then
    adds scalar project macros while skipping IOCTL CTL_CODE bodies. The return
    value is read by ExprResolver only.
    """

    constants: dict[str, int | str] = dict(DEFAULT_CONSTANTS)
    for macro in macros:
        if not macro.body or macro.name == "CTL_CODE" or "CTL_CODE" in macro.body:
            continue
        constants[macro.name] = macro.body
    return constants


def method_name(value: int | None, expr: str) -> str:
    """Return a readable METHOD_* name for report output.

    Inputs are the resolved method value and original expression. Processing maps
    known WDK method values to symbolic names. The return value is the mapped
    name or the original expression when unknown/unresolved.
    """

    names = {0: "METHOD_BUFFERED", 1: "METHOD_IN_DIRECT", 2: "METHOD_OUT_DIRECT", 3: "METHOD_NEITHER"}
    return names.get(value, expr.strip()) if value is not None else expr.strip()


def access_name(value: int | None, expr: str) -> str:
    """Return a readable FILE_* access name for report output.

    Inputs are the resolved access value and original expression. Processing maps
    common access masks, including read|write, to symbolic names. The return
    value is the mapped name or the original expression when unknown/unresolved.
    """

    names = {
        0: "FILE_ANY_ACCESS",
        1: "FILE_READ_ACCESS",
        2: "FILE_WRITE_ACCESS",
        3: "FILE_READ_ACCESS|FILE_WRITE_ACCESS",
    }
    return names.get(value, expr.strip()) if value is not None else expr.strip()


def parse_ioctl_defs(headers: list[Path], root: Path) -> tuple[list[IoctlDef], int]:
    """Parse shared header IOCTL definitions into structured rows.

    Inputs are selected headers and repo root. Processing first parses all
    macros, then resolves IOCTL_KSWORD_ARK_* CTL_CODE arguments using scalar
    macros from the same header set. The return value is definitions plus a raw
    macro count for report metadata.
    """

    macros: list[MacroDef] = []
    for header in headers:
        macros.extend(parse_macros(header, root))
    resolver = ExprResolver(build_constants(macros))
    definitions: list[IoctlDef] = []
    for macro in macros:
        if not macro.name.startswith("IOCTL_KSWORD_ARK_"):
            continue
        args = ctl_args(macro.body)
        if args is None:
            continue
        dev_expr, fun_expr, meth_expr, acc_expr = [arg.strip() for arg in args]
        dev_val = resolver.resolve(dev_expr)
        fun_val = resolver.resolve(fun_expr)
        meth_val = resolver.resolve(meth_expr)
        acc_val = resolver.resolve(acc_expr)
        control = None
        if None not in (dev_val, fun_val, meth_val, acc_val):
            control = (int(dev_val) << 16) | (int(acc_val) << 14) | (int(fun_val) << 2) | int(meth_val)
        definitions.append(
            IoctlDef(
                name=macro.name,
                device_type_expr=dev_expr,
                device_type_value=dev_val,
                function_expr=fun_expr,
                function_id=fun_val,
                method_expr=meth_expr,
                method_value=meth_val,
                method_name=method_name(meth_val, meth_expr),
                access_expr=acc_expr,
                access_value=acc_val,
                access_name=access_name(acc_val, acc_expr),
                control_code=control,
                source=macro.source,
                line=macro.line,
            )
        )
    return definitions, len(macros)


def parse_registry(root: Path) -> list[RegistryEntry]:
    """Parse g_KswordArkIoctlTable rows from ioctl_registry.c.

    Inputs are the repo root. Processing uses a row-level initializer regex
    against the central table source and does not open any handler file. The
    return value is a list of registry entries with approximate source lines.
    """

    path = root / "KswordARKDriver" / "src" / "dispatch" / "ioctl_registry.c"
    text = strip_comments(path.read_text(encoding="utf-8-sig", errors="replace"))
    row_re = re.compile(
        r"\{\s*(IOCTL_KSWORD_ARK_[A-Z0-9_]+)\s*,\s*([A-Za-z_]\w*)\s*,\s*\"([^\"]*)\"",
        re.MULTILINE,
    )
    entries: list[RegistryEntry] = []
    for match in row_re.finditer(text):
        entries.append(
            RegistryEntry(
                name=match.group(1),
                handler=match.group(2),
                string_name=match.group(3),
                source=rel(path, root),
                line=text[: match.start()].count("\n") + 1,
            )
        )
    return entries


def ioctl_tokens(name: str) -> list[str]:
    """Return uppercase semantic tokens from an IOCTL macro name.

    Input is a full IOCTL_KSWORD_ARK_* macro name. Processing removes the common
    prefix and splits the remainder by underscores. The return value is used by
    access keyword and handler naming checks.
    """

    suffix = re.sub(r"^IOCTL_KSWORD_ARK_", "", name)
    return [token for token in suffix.upper().split("_") if token]


def matches_keywords(name: str, keywords: list[str]) -> list[str]:
    """Return keyword matches for an IOCTL name.

    Inputs are the IOCTL name and normalized keyword list. Processing is token
    based with a substring fallback for compound names. The output is sorted for
    stable reports.
    """

    tokens = set(ioctl_tokens(name))
    upper = name.upper()
    return sorted({keyword for keyword in keywords if keyword in tokens or keyword in upper})


def camel_tokens(symbol: str) -> set[str]:
    """Split a handler symbol into normalized lowercase tokens.

    Input is a C handler symbol. Processing splits camel-case words and drops
    boilerplate terms such as Ksword, ARK, Ioctl, and Handler. The return value
    is a set of normalized tokens for fuzzy comparison.
    """

    words: list[str] = []
    for chunk in re.sub(r"[^A-Za-z0-9]+", " ", symbol).split():
        words.extend(re.findall(r"[A-Z]+(?=[A-Z][a-z]|\d|$)|[A-Z]?[a-z]+|\d+", chunk))
    ignored = {"ksword", "ark", "ioctl", "handler"}
    return {normalize(word.lower()) for word in words if normalize(word.lower()) not in ignored}


def normalize(token: str) -> str:
    """Normalize singular/plural spelling enough for handler-name matching.

    Input is a lowercase token. Processing removes a simple trailing plural
    suffix for words longer than three characters. The return value is the token
    used for comparisons.
    """

    return token[:-1] if len(token) > 3 and token.endswith("s") else token


def handler_mismatch(ioctl_name: str, handler: str) -> tuple[bool, list[str], list[str]]:
    """Return whether the handler name obviously diverges from the IOCTL name.

    Inputs are a macro name and handler symbol. Processing compares significant
    IOCTL tokens with normalized handler tokens and flags missing action words or
    broad token drift. The return value is mismatch state, missing tokens, and
    observed handler tokens.
    """

    significant = [normalize(token.lower()) for token in ioctl_tokens(ioctl_name)]
    handler_set = camel_tokens(handler)
    missing = [token for token in significant if token not in handler_set]
    action_words = {"set", "write", "patch", "terminate", "kill", "unload", "delete", "create", "rename", "apply", "query", "read", "enum", "get"}
    action_missing = any(token in action_words for token in missing)
    too_many_missing = len(missing) >= max(2, (len(significant) + 1) // 2)
    return action_missing or too_many_missing, missing, sorted(handler_set)


def add_finding(findings: list[Finding], severity: str, category: str, name: str, message: str, **details: Any) -> None:
    """Append one finding to the mutable finding list and return None.

    Inputs are the target list plus finding fields. Processing wraps the fields
    into a Finding dataclass and appends it. The function has no return value.
    """

    findings.append(Finding(severity=severity, category=category, name=name, message=message, details=details))


def audit(defs: list[IoctlDef], registry: list[RegistryEntry], rules: Rules) -> list[Finding]:
    """Run all consistency, convention, access, and naming checks.

    Inputs are parsed definitions, parsed registry entries, and rules. Processing
    links registry rows to shared definitions, detects missing/duplicate items,
    checks METHOD_BUFFERED convention, applies access-risk keywords, and performs
    a conservative handler-name comparison. The return value is severity-sorted.
    """

    findings: list[Finding] = []
    by_name = {item.name: item for item in defs}
    registry_by_name = {item.name: item for item in registry}
    for item in defs:
        entry = registry_by_name.get(item.name)
        if entry:
            item.registered = True
            item.handler = entry.handler
            item.registry_line = entry.line
        item.mutating_keywords = matches_keywords(item.name, rules.mutating_keywords)
        item.query_keywords = matches_keywords(item.name, rules.query_keywords)

    for item in defs:
        if not item.registered:
            add_finding(findings, "MEDIUM", "defined_not_registered", item.name, "Shared header IOCTL is not registered.", source=item.source, line=item.line)
    for entry in registry:
        if entry.name not in by_name:
            add_finding(findings, "HIGH", "registered_not_defined", entry.name, "Registry IOCTL was not found in shared headers.", handler=entry.handler, source=entry.source, line=entry.line)
        if entry.string_name and entry.string_name != entry.name:
            add_finding(findings, "LOW", "registry_string_mismatch", entry.name, "Registry string literal differs from the macro name.", stringName=entry.string_name, source=entry.source, line=entry.line)

    names: dict[str, list[IoctlDef]] = {}
    function_ids: dict[int, list[IoctlDef]] = {}
    registry_names: dict[str, list[RegistryEntry]] = {}
    for item in defs:
        names.setdefault(item.name, []).append(item)
        if item.function_id is not None:
            function_ids.setdefault(item.function_id, []).append(item)
    for entry in registry:
        registry_names.setdefault(entry.name, []).append(entry)
    for name, items in names.items():
        if len(items) > 1:
            add_finding(findings, "HIGH", "duplicate_ioctl_name", name, "IOCTL macro name is defined more than once.", locations=[f"{x.source}:{x.line}" for x in items])
    for function_id, items in function_ids.items():
        unique = sorted({x.name for x in items})
        if len(unique) > 1:
            add_finding(findings, "HIGH", "duplicate_function_id", f"0x{function_id:X}", "Function id is reused by multiple IOCTLs.", ioctls=unique, locations=[f"{x.source}:{x.line}" for x in items])
    for name, items in registry_names.items():
        if len(items) > 1:
            add_finding(findings, "HIGH", "duplicate_registry_name", name, "Registry table contains duplicate IOCTL registrations.", locations=[f"{x.source}:{x.line}" for x in items])

    for item in defs:
        if item.method_value != DEFAULT_CONSTANTS["METHOD_BUFFERED"]:
            add_finding(findings, "MEDIUM", "method_convention", item.name, "IOCTL method does not match METHOD_BUFFERED convention.", method=item.method_name, source=item.source, line=item.line)
        allowed_any = item.name.upper() in rules.allowed_any_access
        if item.mutating_keywords and item.access_value == DEFAULT_CONSTANTS["FILE_ANY_ACCESS"] and not allowed_any:
            add_finding(findings, "HIGH", "mutating_any_access", item.name, "Mutating keyword matched but access is FILE_ANY_ACCESS.", keywords=item.mutating_keywords, source=item.source, line=item.line)
        if item.query_keywords and not item.mutating_keywords and item.access_value is not None and (item.access_value & DEFAULT_CONSTANTS["FILE_WRITE_ACCESS"]):
            add_finding(findings, "MEDIUM", "query_write_access", item.name, "Query/read-only keyword matched but access requires FILE_WRITE_ACCESS.", keywords=item.query_keywords, source=item.source, line=item.line)
        if item.registered and item.handler:
            mismatch, missing, handler_words = handler_mismatch(item.name, item.handler)
            if mismatch:
                add_finding(findings, "LOW", "handler_name_mismatch", item.name, "Handler name does not clearly match IOCTL tokens.", handler=item.handler, missingTokens=missing, handlerTokens=handler_words, registryLine=item.registry_line)

    rank = {"HIGH": 0, "MEDIUM": 1, "LOW": 2}
    return sorted(findings, key=lambda finding: (rank.get(finding.severity, 9), finding.category, finding.name))


def summarize(defs: list[IoctlDef], registry: list[RegistryEntry], findings: list[Finding]) -> dict[str, Any]:
    """Return summary counters for the report model.

    Inputs are parsed definitions, registry rows, and findings. Processing
    counts registration coverage, finding severities, and finding categories.
    The return value is a JSON-serializable dictionary.
    """

    severity = {"HIGH": 0, "MEDIUM": 0, "LOW": 0}
    categories: dict[str, int] = {}
    for finding in findings:
        severity[finding.severity] = severity.get(finding.severity, 0) + 1
        categories[finding.category] = categories.get(finding.category, 0) + 1
    return {
        "definedIoctlCount": len(defs),
        "registeredIoctlCount": len(registry),
        "registeredDefinitions": sum(1 for item in defs if item.registered),
        "unregisteredDefinitions": sum(1 for item in defs if not item.registered),
        "highFindings": severity.get("HIGH", 0),
        "mediumFindings": severity.get("MEDIUM", 0),
        "lowFindings": severity.get("LOW", 0),
        "findingCategories": dict(sorted(categories.items())),
    }


def build_report(root: Path, rules: Rules) -> dict[str, Any]:
    """Build the complete JSON-serializable report model.

    Inputs are repo root and rules. Processing discovers headers, parses shared
    definitions and registry rows, runs checks, and assembles metadata. The
    return value is consumed by renderers and is not written here.
    """

    headers = discover_headers(root, rules)
    defs, macro_count = parse_ioctl_defs(headers, root)
    registry = parse_registry(root)
    findings = audit(defs, registry, rules)
    return {
        "schemaVersion": 1,
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repoRoot": str(root),
        "inputs": {
            "headers": [rel(header, root) for header in headers],
            "registry": "KswordARKDriver/src/dispatch/ioctl_registry.c",
            "macroCount": macro_count,
        },
        "rules": {
            "allowedAnyAccess": sorted(rules.allowed_any_access),
            "mutatingKeywords": rules.mutating_keywords,
            "queryKeywords": rules.query_keywords,
            "ignoredHeaders": sorted(rules.ignored_headers),
            "notes": rules.notes,
        },
        "summary": summarize(defs, registry, findings),
        "ioctls": [asdict(item) for item in sorted(defs, key=lambda row: (row.source, row.line, row.name))],
        "registry": [asdict(item) for item in sorted(registry, key=lambda row: row.line)],
        "findings": [asdict(item) for item in findings],
    }


def fmt_hex(value: int | None) -> str:
    """Return an optional integer as a compact hexadecimal display string.

    Input is an integer or None. Processing formats integers as 0x-prefixed
    uppercase hexadecimal and renders unresolved values as N/A. The return value
    is display-only text.
    """

    return "N/A" if value is None else f"0x{value:X}"


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    """Render a small markdown table with escaped cell separators.

    Inputs are column headers and row cells. Processing escapes pipe characters
    and newlines so generated markdown remains valid. The return value is the
    complete table text.
    """

    def clean(value: str) -> str:
        return str(value).replace("|", "\\|").replace("\n", "<br>")

    lines = ["| " + " | ".join(clean(header) for header in headers) + " |"]
    lines.append("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        lines.append("| " + " | ".join(clean(cell) for cell in row) + " |")
    return "\n".join(lines)


def render_markdown(report: dict[str, Any]) -> str:
    """Render report data as markdown for human review.

    Input is the complete report dictionary. Processing formats summary,
    high-risk findings, all findings, IOCTL inventory, and rules into sections.
    The return value is markdown text and no files are written here.
    """

    summary = report["summary"]
    lines = [
        "# KswordARKDriver IOCTL Audit Report",
        "",
        f"- Generated at: `{report['generatedAt']}`",
        f"- Repository root: `{report['repoRoot']}`",
        f"- Headers scanned: `{len(report['inputs']['headers'])}`",
        f"- Registry scanned: `{report['inputs']['registry']}`",
        "",
        "## Summary",
        "",
        markdown_table(
            ["Metric", "Value"],
            [
                ["Shared IOCTL definitions", str(summary["definedIoctlCount"])],
                ["Registry entries", str(summary["registeredIoctlCount"])],
                ["Registered definitions", str(summary["registeredDefinitions"])],
                ["Unregistered definitions", str(summary["unregisteredDefinitions"])],
                ["HIGH findings", str(summary["highFindings"])],
                ["MEDIUM findings", str(summary["mediumFindings"])],
                ["LOW findings", str(summary["lowFindings"])],
            ],
        ),
        "",
        "## High-risk findings",
        "",
    ]
    high_rows = [
        [item["category"], item["name"], item["message"], json.dumps(item["details"], ensure_ascii=False)]
        for item in report["findings"]
        if item["severity"] == "HIGH"
    ]
    lines.append(markdown_table(["Category", "IOCTL", "Message", "Details"], high_rows) if high_rows else "No HIGH findings.")
    lines.extend(["", "## All findings", ""])
    finding_rows = [[item["severity"], item["category"], item["name"], item["message"]] for item in report["findings"]]
    lines.append(markdown_table(["Severity", "Category", "Name", "Message"], finding_rows) if finding_rows else "No findings.")
    lines.extend(["", "## IOCTL inventory", ""])
    inventory_rows: list[list[str]] = []
    for item in report["ioctls"]:
        inventory_rows.append(
            [
                item["name"],
                fmt_hex(item["function_id"]),
                item["method_name"],
                item["access_name"],
                "yes" if item["registered"] else "no",
                item.get("handler") or "",
                f"{item['source']}:{item['line']}",
            ]
        )
    lines.append(markdown_table(["IOCTL", "Function", "Method", "Access", "Registered", "Handler", "Source"], inventory_rows))
    lines.extend(["", "## Rule notes", "", "```json", json.dumps(report["rules"], ensure_ascii=False, indent=2), "```", ""])
    return "\n".join(lines)


def render_text(report: dict[str, Any]) -> str:
    """Render a compact plain-text report for console use.

    Input is the complete report dictionary. Processing flattens summary and
    findings into line-oriented text. The return value is the console report
    string.
    """

    lines = ["KswordARKDriver IOCTL audit", ""]
    for key, value in report["summary"].items():
        lines.append(f"{key}: {value}")
    lines.extend(["", "Findings:"])
    for finding in report["findings"]:
        lines.append(f"[{finding['severity']}] {finding['category']} {finding['name']}: {finding['message']}")
    if not report["findings"]:
        lines.append("No findings.")
    return "\n".join(lines) + "\n"


def render(report: dict[str, Any], report_format: str) -> str:
    """Return the report string in text, markdown, or JSON format.

    Inputs are the report dictionary and requested format. Processing dispatches
    to the matching renderer or JSON serializer. The return value is rendered
    text ready for stdout or file writing.
    """

    if report_format == "json":
        return json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if report_format == "text":
        return render_text(report)
    return render_markdown(report)


def write_file(path: Path, content: str) -> None:
    """Create parent directories, write UTF-8 output, and return None.

    Inputs are the destination path and report content. Processing creates
    missing parent directories and writes UTF-8 text. The function has no return
    value.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    """CLI entry point.

    Inputs are optional argv values for tests. Processing loads rules, builds the
    report, writes either --out or default markdown/json outputs, and applies
    --fail-on-risk semantics. The return value is 0 for normal completion or 2
    for fail-on-risk HIGH findings.
    """

    args = parse_args(argv)
    root = Path(args.repo_root).resolve()
    rules = load_rules(root)
    report = build_report(root, rules)
    if args.out:
        out = Path(args.out)
        if not out.is_absolute():
            out = root / out
        write_file(out, render(report, args.format))
    else:
        out_dir = root / "tools" / "ioctl_audit" / "out"
        write_file(out_dir / "ioctl_audit_report.md", render(report, "markdown"))
        write_file(out_dir / "ioctl_audit_report.json", render(report, "json"))
        sys.stdout.write(render(report, args.format))
    if args.fail_on_risk and report["summary"].get("highFindings", 0) > 0:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
