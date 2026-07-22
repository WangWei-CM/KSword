#!/usr/bin/env python3
"""Validate and import one extracted Ksword Launcher report directory.

The default mode is read-only.  ``--commit`` stages every PE/PDB/profile first
and only writes the canonical corpus after all validation steps succeed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import pefile
import requests

from ksword_pdb_profile_generator import entry_from_local_pe, parse_rsds_identity


DEFAULT_CORPUS_ROOT = Path(r"E:\KswordPDB\PDB")
DEFAULT_LLVM_PDBUTIL = Path(r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe")
DEFAULT_SYMBOL_SERVER = "https://msdl.microsoft.com/download/symbols"
REPORT_METADATA_FILES = ("README.txt", "report.json", "report.txt", "SHA256SUMS.txt", "launcher.log")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PDB_GUID_RE = re.compile(r"^\s*GUID:\s*\{?([^}\s]+)\}?\s*$", re.IGNORECASE | re.MULTILINE)


class IntakeError(RuntimeError):
    """A report is malformed, inconsistent, or unsafe to import."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate/import an extracted Ksword Launcher report")
    parser.add_argument("report_dir", type=Path, help="Extracted 'Compress and send' directory")
    parser.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    parser.add_argument("--llvm-pdbutil", type=Path, default=DEFAULT_LLVM_PDBUTIL)
    parser.add_argument("--symbol-server", default=DEFAULT_SYMBOL_SERVER)
    parser.add_argument("--commit", action="store_true", help="Stage, download symbols, and import validated data")
    parser.add_argument("--json-output", type=Path, help="Optional validation result path")
    return parser.parse_args(argv)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_guid(value: Any) -> str:
    return re.sub(r"[^0-9A-Fa-f]", "", str(value or "")).upper()


def normalize_symbol_key(pdb_guid: str, pdb_age: int) -> str:
    return f"{normalize_guid(pdb_guid)}{pdb_age}".upper()


def safe_file_name(value: Any, label: str) -> str:
    name = str(value or "").strip()
    if not name or Path(name).name != name or name in {".", ".."}:
        raise IntakeError(f"unsafe {label}: {name!r}")
    return name


def load_json_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise IntakeError(f"cannot parse {path.name}: {exc}") from exc
    if not isinstance(value, dict):
        raise IntakeError(f"{path.name} must contain a JSON object")
    return value


def load_checksums(path: Path) -> dict[str, str]:
    checksums: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except (OSError, UnicodeError) as exc:
        raise IntakeError(f"cannot read {path.name}: {exc}") from exc
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9A-Fa-f]{64})\s+(.+)", line.strip())
        if not match:
            raise IntakeError(f"invalid SHA256SUMS line {line_number}")
        name = safe_file_name(match.group(2).strip(), "checksum file name")
        key = name.casefold()
        if key in checksums:
            raise IntakeError(f"duplicate checksum entry: {name}")
        checksums[key] = match.group(1).lower()
    if not checksums:
        raise IntakeError("SHA256SUMS.txt is empty")
    return checksums


def pe_string_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    image = pefile.PE(str(path), fast_load=False)
    try:
        file_info_rows: list[Any] = []
        for row in getattr(image, "FileInfo", []) or []:
            file_info_rows.extend(row if isinstance(row, list) else [row])
        for file_info in file_info_rows:
            for table in getattr(file_info, "StringTable", []) or []:
                for key, value in (getattr(table, "entries", {}) or {}).items():
                    key_text = key.decode("utf-8", errors="replace") if isinstance(key, bytes) else str(key)
                    value_text = value.decode("utf-8", errors="replace") if isinstance(value, bytes) else str(value)
                    values[key_text.casefold()] = value_text.strip()
    finally:
        image.close()
    return values


def module_rows(report: dict[str, Any]) -> list[dict[str, Any]]:
    modules = report.get("modules")
    if not isinstance(modules, list) or not modules:
        raise IntakeError("report.json modules must be a non-empty array")
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, raw in enumerate(modules):
        if not isinstance(raw, dict) or not isinstance(raw.get("identity"), dict):
            raise IntakeError(f"module row {index} has no identity")
        identity = raw["identity"]
        file_name = safe_file_name(identity.get("fileName") or raw.get("name"), "module file name")
        key = file_name.casefold()
        if key in seen:
            raise IntakeError(f"duplicate module identity: {file_name}")
        seen.add(key)
        rows.append(raw)
    return rows


def find_bin_files(report_dir: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for path in report_dir.iterdir():
        if path.is_symlink():
            raise IntakeError(f"symbolic links are not accepted: {path.name}")
        if not path.is_file() or path.suffix.casefold() != ".bin":
            continue
        key = path.name.casefold()
        if key in result:
            raise IntakeError(f"duplicate binary name: {path.name}")
        result[key] = path
    return result


def validate_module(raw: dict[str, Any], bin_path: Path, expected_sha256: str) -> dict[str, Any]:
    identity = raw["identity"]
    file_name = safe_file_name(identity.get("fileName") or raw.get("name"), "module file name")
    actual_sha256 = sha256_file(bin_path)
    errors: list[str] = []
    if actual_sha256 != expected_sha256:
        errors.append("sha256_mismatch")

    try:
        entry = entry_from_local_pe(bin_path)
    except Exception as exc:  # pefile reports detailed parser errors.
        return {
            "fileName": file_name,
            "source": str(bin_path),
            "classId": raw.get("classId"),
            "valid": False,
            "errors": errors + [f"pe_parse_failed:{exc}"],
        }

    strings = pe_string_values(bin_path)
    product_name = strings.get("productname", "")
    file_description = strings.get("filedescription", "")
    if "wine" in product_name.casefold() or "wine" in file_description.casefold():
        errors.append("wine_runtime_image")
    if entry.machine != 0x8664:
        errors.append(f"unsupported_machine:0x{entry.machine:04X}")
    if int(identity.get("machine") or 0) != entry.machine:
        errors.append("machine_mismatch")
    if int(identity.get("timeDateStamp") or 0) != entry.timestamp:
        errors.append("timestamp_mismatch")
    if int(identity.get("sizeOfImage") or 0) != entry.size_of_image:
        errors.append("size_of_image_mismatch")

    pdb: dict[str, Any] = {}
    try:
        parsed_pdb = parse_rsds_identity(bin_path)
        pdb = {
            "name": parsed_pdb.pdb_name,
            "guid": normalize_guid(parsed_pdb.pdb_guid),
            "age": parsed_pdb.pdb_age,
            "symbolKey": parsed_pdb.symbol_key.upper(),
        }
    except Exception as exc:
        errors.append(f"rsds_missing:{exc}")

    report_valid = bool(identity.get("valid"))
    if not report_valid:
        errors.append(f"launcher_identity_invalid:{identity.get('error') or 'unknown'}")
    if pdb:
        report_pdb_name = str(identity.get("pdbName") or "")
        report_guid = normalize_guid(identity.get("pdbGuid"))
        report_age = int(identity.get("pdbAge") or 0)
        report_key = str(identity.get("pdbSymbolKey") or "").upper()
        if report_pdb_name.casefold() != pdb["name"].casefold():
            errors.append("pdb_name_mismatch")
        if report_guid != pdb["guid"]:
            errors.append("pdb_guid_mismatch")
        if report_age != pdb["age"]:
            errors.append("pdb_age_mismatch")
        if report_key != pdb["symbolKey"]:
            errors.append("pdb_symbol_key_mismatch")

    return {
        "fileName": file_name,
        "canonicalFileName": file_name.casefold(),
        "source": str(bin_path),
        "classId": int(raw.get("classId") if raw.get("classId") is not None else -1),
        "compatibilityRequired": bool(raw.get("compatibilityRequired")),
        "collectionOnly": bool(raw.get("collectionOnly")),
        "arch": entry.arch,
        "machine": entry.machine,
        "version": entry.version,
        "timeDateStamp": entry.timestamp,
        "sizeOfImage": entry.size_of_image,
        "sha256": actual_sha256,
        "productName": product_name,
        "fileDescription": file_description,
        "pdb": pdb,
        "valid": not errors,
        "errors": errors,
    }


def validate_report(report_dir: Path) -> dict[str, Any]:
    report_dir = report_dir.resolve()
    if not report_dir.is_dir():
        raise IntakeError(f"report directory does not exist: {report_dir}")
    report_path = report_dir / "report.json"
    sums_path = report_dir / "SHA256SUMS.txt"
    if not report_path.is_file() or not sums_path.is_file():
        raise IntakeError("report.json and SHA256SUMS.txt are required")

    report = load_json_object(report_path)
    checksums = load_checksums(sums_path)
    bins = find_bin_files(report_dir)
    if set(checksums) != set(bins):
        missing = sorted(set(checksums) - set(bins))
        unlisted = sorted(set(bins) - set(checksums))
        raise IntakeError(f"binary/checksum set mismatch; missing={missing}; unlisted={unlisted}")

    modules: list[dict[str, Any]] = []
    expected_bins: set[str] = set()
    for raw in module_rows(report):
        identity = raw["identity"]
        file_name = safe_file_name(identity.get("fileName") or raw.get("name"), "module file name")
        bin_key = f"{file_name}.bin".casefold()
        expected_bins.add(bin_key)
        bin_path = bins.get(bin_key)
        if bin_path is None:
            modules.append({
                "fileName": file_name,
                "classId": raw.get("classId"),
                "valid": False,
                "errors": ["binary_missing"],
            })
            continue
        modules.append(validate_module(raw, bin_path, checksums[bin_key]))

    extra_bins = sorted(set(bins) - expected_bins)
    errors = [f"extra_binary:{name}" for name in extra_bins]
    errors.extend(
        f"{module.get('fileName')}:{error}"
        for module in modules
        for error in module.get("errors", [])
    )
    report_id = sha256_file(report_path)[:16]
    os_info = report.get("os") if isinstance(report.get("os"), dict) else {}
    return {
        "schemaVersion": 1,
        "reportId": report_id,
        "reportDirectory": str(report_dir),
        "reportSha256": sha256_file(report_path),
        "manifestSha256": str(report.get("manifestSha256") or ""),
        "os": os_info,
        "missingCount": int(report.get("missingCount") or 0),
        "collectionCandidateCount": int(report.get("collectionCandidateCount") or 0),
        "modules": modules,
        "valid": not errors and bool(modules),
        "errors": errors,
    }


def pdb_guid_from_file(path: Path, llvm_pdbutil: Path) -> str:
    completed = subprocess.run(
        [str(llvm_pdbutil), "dump", "-summary", str(path)],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if completed.returncode != 0:
        raise IntakeError(f"llvm-pdbutil rejected {path.name}: {completed.stderr.strip()}")
    match = PDB_GUID_RE.search(completed.stdout)
    if not match:
        raise IntakeError(f"PDB summary has no GUID: {path}")
    return normalize_guid(match.group(1))


def download_pdb(module: dict[str, Any], destination: Path, symbol_server: str, llvm_pdbutil: Path) -> None:
    pdb = module["pdb"]
    url = f"{symbol_server.rstrip('/')}/{pdb['name']}/{pdb['symbolKey']}/{pdb['name']}"
    destination.parent.mkdir(parents=True, exist_ok=True)
    print(f"[PDB download] {module['fileName']} {pdb['symbolKey']}", flush=True)
    temporary = destination.with_name(destination.name + ".part")
    if temporary.exists():
        temporary.unlink()
    last_error: requests.RequestException | None = None
    for attempt in range(1, 6):
        try:
            if temporary.exists():
                temporary.unlink()
            with requests.get(url, stream=True, timeout=(30, 300), headers={"User-Agent": "ksword-launcher-report-intake/1.0"}) as response:
                response.raise_for_status()
                with temporary.open("wb") as output:
                    for chunk in response.iter_content(chunk_size=1024 * 1024):
                        if chunk:
                            output.write(chunk)
            if temporary.stat().st_size < 1024:
                raise IntakeError(f"downloaded PDB is unexpectedly small: {pdb['name']}")
            if pdb_guid_from_file(temporary, llvm_pdbutil) != pdb["guid"]:
                raise IntakeError(f"downloaded PDB GUID mismatch: {pdb['name']}")
            os.replace(temporary, destination)
            return
        except requests.RequestException as exc:
            last_error = exc
            if attempt == 5:
                break
            delay = min(2 ** attempt, 20)
            print(f"[PDB retry] {module['fileName']} attempt={attempt + 1} delay={delay}s error={exc}", flush=True)
            time.sleep(delay)
        finally:
            if temporary.exists():
                temporary.unlink()
    assert last_error is not None
    raise last_error


def atomic_copy(source: Path, destination: Path, expected_sha256: str | None = None) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        existing_hash = sha256_file(destination)
        if expected_sha256 and existing_hash != expected_sha256:
            raise IntakeError(f"existing destination hash mismatch: {destination}")
        return "existing"
    temporary = destination.with_name(destination.name + ".intake.tmp")
    if temporary.exists():
        temporary.unlink()
    try:
        shutil.copy2(source, temporary)
        if expected_sha256 and sha256_file(temporary) != expected_sha256:
            raise IntakeError(f"staged copy hash mismatch: {destination}")
        os.replace(temporary, destination)
        return "copied"
    finally:
        if temporary.exists():
            temporary.unlink()


def canonical_pe_path(corpus_root: Path, module: dict[str, Any]) -> Path:
    name = module["canonicalFileName"]
    return corpus_root / "pe-store" / module["arch"] / name / f"{name}.{module['version']}" / module["sha256"] / name


def canonical_pdb_path(corpus_root: Path, module: dict[str, Any]) -> Path:
    pdb = module["pdb"]
    return corpus_root / "pdb-cache" / module["arch"] / pdb["name"] / pdb["symbolKey"] / pdb["name"]


def run_profile_generator(module: dict[str, Any], normalized_pe: Path, pdb_path: Path, output_dir: Path, llvm_pdbutil: Path) -> Path:
    generator = Path(__file__).with_name("ksword_pdb_profile_generator.py")
    profile_name = (
        f"{'ntkrla57' if module['classId'] == 1 else 'ntoskrnl'}_"
        f"{module['arch']}_{module['version'].replace('.', '_')}_{module['pdb']['symbolKey'].lower()}.json"
    )
    output_path = output_dir / profile_name
    output_path.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [
            sys.executable,
            str(generator),
            "--dry-run",
            "--local-pe",
            str(normalized_pe),
            "--local-pdb",
            str(pdb_path),
            "--llvm-pdbutil",
            str(llvm_pdbutil),
            "--output",
            str(output_path),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=600,
    )
    if completed.returncode != 0 or not output_path.is_file():
        raise IntakeError(f"profile generation failed for {module['fileName']}: {completed.stderr.strip()}")
    profile = load_json_object(output_path)
    profile_module = profile.get("module") if isinstance(profile.get("module"), dict) else {}
    if normalize_guid(profile_module.get("pdbGuid")) != module["pdb"]["guid"]:
        raise IntakeError(f"generated profile PDB identity mismatch: {module['fileName']}")
    if int(str(profile_module.get("timeDateStamp") or "0"), 0) != module["timeDateStamp"]:
        raise IntakeError(f"generated profile timestamp mismatch: {module['fileName']}")
    return output_path


def validate_generated_profiles(profile_dir: Path, staging: Path) -> dict[str, Any]:
    sync = Path(__file__).with_name("ksword_profile_release_sync.py")
    report_path = staging / "profile-validation.json"
    release_root = staging / "validation-release"
    completed = subprocess.run(
        [
            sys.executable,
            str(sync),
            "--source",
            str(profile_dir),
            "--release-root",
            str(release_root),
            "--dry-run",
            "--emit-pack",
            "--pack-version",
            "3",
            "--report",
            str(report_path),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )
    if completed.returncode != 0 or not report_path.is_file():
        raise IntakeError(f"profile validation failed: {completed.stderr.strip()}")
    report = load_json_object(report_path)
    if int(report.get("rejectedProfiles") or 0) != 0:
        raise IntakeError(f"generated profile was rejected: {report.get('rejected')}")
    return report


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def commit_report(result: dict[str, Any], corpus_root: Path, report_dir: Path, symbol_server: str, llvm_pdbutil: Path) -> None:
    if not result["valid"]:
        raise IntakeError("refusing to commit an invalid report")
    if not corpus_root.is_dir():
        raise IntakeError(f"corpus root does not exist: {corpus_root}")
    if not llvm_pdbutil.is_file():
        raise IntakeError(f"llvm-pdbutil does not exist: {llvm_pdbutil}")

    report_id = result["reportId"]
    staging = corpus_root / "scratch" / "launcher-report-intake" / report_id
    metadata_dir = staging / "source-metadata"
    normalized_dir = staging / "normalized-pe"
    staged_pdb_dir = staging / "pdb"
    generated_profile_dir = staging / "generated-profiles"
    staging.mkdir(parents=True, exist_ok=True)
    for name in REPORT_METADATA_FILES:
        source = report_dir / name
        if source.is_file():
            atomic_copy(source, metadata_dir / name)

    for module in result["modules"]:
        print(f"[stage PE] {module['fileName']} {module['version']}", flush=True)
        source = Path(module["source"])
        normalized = normalized_dir / module["canonicalFileName"]
        atomic_copy(source, normalized, module["sha256"])
        module["normalizedPe"] = str(normalized)
        canonical_pdb = canonical_pdb_path(corpus_root, module)
        if canonical_pdb.is_file():
            print(f"[PDB existing] {module['fileName']} {module['pdb']['symbolKey']}", flush=True)
            if pdb_guid_from_file(canonical_pdb, llvm_pdbutil) != module["pdb"]["guid"]:
                raise IntakeError(f"cached PDB GUID mismatch: {canonical_pdb}")
            staged_pdb = canonical_pdb
            module["pdbStageStatus"] = "existing"
        else:
            staged_pdb = staged_pdb_dir / module["pdb"]["name"] / module["pdb"]["symbolKey"] / module["pdb"]["name"]
            if not staged_pdb.is_file():
                download_pdb(module, staged_pdb, symbol_server, llvm_pdbutil)
            elif pdb_guid_from_file(staged_pdb, llvm_pdbutil) != module["pdb"]["guid"]:
                raise IntakeError(f"staged PDB GUID mismatch: {staged_pdb}")
            module["pdbStageStatus"] = "downloaded"
        module["stagedPdb"] = str(staged_pdb)

    generated_profiles: list[tuple[dict[str, Any], Path]] = []
    for module in result["modules"]:
        if module["classId"] not in {0, 1}:
            continue
        print(f"[profile generate] {module['fileName']}", flush=True)
        profile_path = run_profile_generator(
            module,
            Path(module["normalizedPe"]),
            Path(module["stagedPdb"]),
            generated_profile_dir,
            llvm_pdbutil,
        )
        generated_profiles.append((module, profile_path))
    if not generated_profiles:
        raise IntakeError("report contains no compatibility profile module")
    validation = validate_generated_profiles(generated_profile_dir, staging)
    print("[profile validate] accepted", flush=True)
    result["profileValidation"] = {
        "publishedProfiles": validation.get("publishedProfiles"),
        "rejectedProfiles": validation.get("rejectedProfiles"),
        "duplicateGroups": validation.get("duplicateGroups"),
    }

    for module in result["modules"]:
        print(f"[commit] {module['fileName']}", flush=True)
        module["peDestination"] = str(canonical_pe_path(corpus_root, module))
        module["peCommitStatus"] = atomic_copy(Path(module["normalizedPe"]), Path(module["peDestination"]), module["sha256"])
        destination_pdb = canonical_pdb_path(corpus_root, module)
        module["pdbDestination"] = str(destination_pdb)
        module["pdbCommitStatus"] = atomic_copy(Path(module["stagedPdb"]), destination_pdb)
    for module, profile_path in generated_profiles:
        destination = corpus_root / "profiles" / "ark_dyndata" / profile_path.name
        module["profileDestination"] = str(destination)
        module["profileCommitStatus"] = atomic_copy(profile_path, destination)

    result["committed"] = True
    result["committedAtUtc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    write_json(staging / "intake.json", result)
    write_json(corpus_root / "logs" / "launcher_report_intake" / f"{report_id}.json", result)


def print_summary(result: dict[str, Any]) -> None:
    os_info = result.get("os", {})
    print(f"reportId={result.get('reportId', '<none>')}")
    print(f"os={os_info.get('major', 0)}.{os_info.get('minor', 0)}.{os_info.get('build', 0)}")
    print(f"valid={str(bool(result.get('valid'))).lower()}")
    print(f"committed={str(bool(result.get('committed'))).lower()}")
    print(f"modules={len(result.get('modules', []))}")
    for module in result.get("modules", []):
        status = "ok" if module.get("valid") else "rejected"
        print(
            f"module={module.get('fileName')} status={status} classId={module.get('classId')} "
            f"version={module.get('version', '<unknown>')} errors={','.join(module.get('errors', [])) or '-'}"
        )
    for error in result.get("errors", []):
        print(f"error={error}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = validate_report(args.report_dir)
        result["committed"] = False
        if args.commit:
            commit_report(result, args.corpus_root.resolve(), args.report_dir.resolve(), args.symbol_server, args.llvm_pdbutil.resolve())
        if args.json_output:
            write_json(args.json_output, result)
        print_summary(result)
        return 0 if result["valid"] else 2
    except (IntakeError, OSError, requests.RequestException, subprocess.SubprocessError) as exc:
        print(f"intake failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
