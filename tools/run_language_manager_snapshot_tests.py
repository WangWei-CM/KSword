"""Compile and run frozen translation values with a QCoreApplication fixture.

QtWidgets is linked only to resolve production symbols. No QApplication,
QWidget, product process, clipboard, driver or GUI/offscreen test is created.
Generated packs and all build artifacts stay in the ignored build-log tree.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def find_repository_root() -> Path:
    for candidate in Path(__file__).resolve().parents:
        if (candidate / "AGENTS.md").is_file() and (candidate / "Ksword5.1" / "Ksword5.1" / "Internationalization" / "LanguageManager.cpp").is_file():
            return candidate
    raise RuntimeError("Cannot locate repository root")


def choose_path(candidates: list[Path], required: str | None = None) -> Path:
    for candidate in candidates:
        exists = (candidate / required).is_file() if required else candidate.is_file()
        if exists:
            return candidate
    raise RuntimeError("Required x64 build dependency was not found: " + ", ".join(map(str, candidates)))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path,
                        help="Alternative LanguageManager.cpp/h directory for an isolated before/after candidate")
    parser.add_argument("--output-dir", type=Path,
                        help="Must be inside the repository .codex-build-logs directory")
    parser.add_argument("--generation-race", action="store_true",
                        help="Instrument a build-log source copy to pause an old query across a language change")
    parser.add_argument("--runtime-policy", action="store_true",
                        help="Count actual runtime traversal over QObject-only trees in a test-only source copy")
    parser.add_argument("--rediscover", action="store_true",
                        help="Make initialize rediscover fixture packs in a test-only source copy; does not add a production reload API")
    args = parser.parse_args()
    repository = find_repository_root()
    source_dir = (args.source_dir or repository / "Ksword5.1" / "Ksword5.1" / "Internationalization").resolve()
    if not all((source_dir / name).is_file() for name in ("LanguageManager.cpp", "LanguageManager.h")):
        raise RuntimeError("Source directory must contain LanguageManager.cpp and LanguageManager.h")

    artifact_root = (repository / ".codex-build-logs").resolve()
    out_dir = (args.output_dir or artifact_root / ("i18n-source-snapshot-regression-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))).resolve()
    if not out_dir.is_relative_to(artifact_root) or out_dir == artifact_root:
        raise RuntimeError("Output directory must remain within .codex-build-logs")
    out_dir.mkdir(parents=True, exist_ok=True)

    qt_candidates: list[Path] = []
    if os.environ.get("KSWORD_QT_DIR"):
        qt_candidates.append(Path(os.environ["KSWORD_QT_DIR"]))
    qt_candidates.extend((repository / ".deps" / "Qt" / "6.9.3" / "msvc2022_64",
                          Path(r"D:\Software\Qt\6.9.3\msvc2022_64")))
    qt_dir = choose_path(qt_candidates, "lib/Qt6Core.lib")
    vc_candidates: list[Path] = []
    if os.environ.get("KSWORD_VCVARS64"):
        vc_candidates.append(Path(os.environ["KSWORD_VCVARS64"]))
    vc_candidates.extend((
        Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"),
        Path(r"D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat")))
    vcvars = choose_path(vc_candidates)

    fixture = Path(__file__).with_name("language_manager_snapshot_tests.cpp")
    source = source_dir / "LanguageManager.cpp"
    compile_source = source
    original_bytes = source.read_bytes()
    if args.generation_race:
        newline = b"\r\n" if b"\r\n" in original_bytes else b"\n"
        include = b'#include "LanguageManager.h"' + newline
        cost = b"    const qint64 cacheCost ="
        if original_bytes.count(include) != 1 or original_bytes.count(cost) != 1:
            raise RuntimeError("Test-only generation hook requires one known include and cache-cost point")
        hook_bytes = original_bytes.replace(include,
            include + newline + b"extern void ksword_i18n_test_after_source_resolved();" + newline, 1)
        hook_bytes = hook_bytes.replace(cost, b"    ::ksword_i18n_test_after_source_resolved();" + newline + cost, 1)
        compile_source = out_dir / "LanguageManager.test-hook.cpp"
        compile_source.write_bytes(hook_bytes)
    if args.runtime_policy:
        hook_bytes = compile_source.read_bytes()
        newline = b"\r\n" if b"\r\n" in hook_bytes else b"\n"
        include = b'#include "LanguageManager.h"' + newline
        point = b"        if (QAction* action = qobject_cast<QAction*>(currentObject))"
        if hook_bytes.count(point) != 1:
            raise RuntimeError("Runtime-policy hook requires the known runtime visitor entry")
        hook_bytes = hook_bytes.replace(include, include + newline + b"extern void ksword_i18n_test_runtime_visited(QObject*);" + newline, 1)
        hook_bytes = hook_bytes.replace(point, b"        ::ksword_i18n_test_runtime_visited(currentObject);" + newline + point, 1)
        compile_source = out_dir / "LanguageManager.policy-hook.cpp"
        compile_source.write_bytes(hook_bytes)
    if args.rediscover:
        hook_bytes = compile_source.read_bytes()
        begin = hook_bytes.index(b"bool ks::i18n::LanguageManager::initialize(")
        condition = b"if (!hasAnyPack())"
        point = hook_bytes.index(condition, begin)
        # initialize currently reuses its discovered registry. Exercise the real
        # rediscovery body only in this isolated copy, with a runtime condition
        # that is true in the QCoreApplication fixture.
        hook_bytes = hook_bytes[:point] + b"if (QCoreApplication::instance() != nullptr)" + hook_bytes[point + len(condition):]
        compile_source = out_dir / "LanguageManager.rediscover-hook.cpp"
        compile_source.write_bytes(hook_bytes)
    exe = out_dir / "language_manager_snapshot_tests.exe"
    result = out_dir / "results.json"
    flags = [
        "cl", "/nologo", "/O2", "/EHsc", "/std:c++17", "/Zc:__cplusplus",
        "/permissive-", "/MD", "/utf-8", "/W4", "/WX", "/DNDEBUG",
        "/DQT_WIDGETS_LIB", "/DQT_GUI_LIB", "/DQT_CORE_LIB",
        "/I" + str(source_dir), "/I" + str(qt_dir / "include"),
        "/I" + str(qt_dir / "include" / "QtCore"),
        "/I" + str(qt_dir / "include" / "QtGui"),
        "/I" + str(qt_dir / "include" / "QtWidgets"),
        str(fixture), str(compile_source), "/Fo" + str(out_dir) + "\\", "/Fe" + str(exe),
        "/link", "/LIBPATH:" + str(qt_dir / "lib"),
        "Qt6Widgets.lib", "Qt6Gui.lib", "Qt6Core.lib", "user32.lib",
    ]
    if args.generation_race:
        flags.insert(1, "/DKSWORD_I18N_TEST_GENERATION_HOOK")
    if args.runtime_policy:
        flags.insert(1, "/DKSWORD_I18N_TEST_RUNTIME_POLICY")
    if args.rediscover:
        flags.insert(1, "/DKSWORD_I18N_TEST_REDISCOVER")
    response_file = out_dir / "compile.rsp"
    response_file.write_text(subprocess.list2cmdline(flags[1:]) + "\n", encoding="utf-8")
    batch = out_dir / "build.cmd"
    batch.write_text(
        '@echo off\ncall "' + str(vcvars) + '" >nul\n'
        'if errorlevel 1 exit /b %errorlevel%\ncl @"' + str(response_file) + '"'
        + '\nexit /b %errorlevel%\n', encoding="utf-8")
    build = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=out_dir,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out_dir / "build.log").write_bytes(build.stdout)
    sys.stdout.write(build.stdout.decode("utf-8", errors="replace"))

    manifest = {
        "source_sha256": hashlib.sha256(original_bytes).hexdigest(),
        "compiled_source_sha256": hashlib.sha256(compile_source.read_bytes()).hexdigest(),
        "test_only_generation_hook": args.generation_race,
        "test_only_runtime_policy_hook": args.runtime_policy,
        "test_only_rediscovery_hook": args.rediscover,
        "header_sha256": hashlib.sha256((source_dir / "LanguageManager.h").read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "build_returncode": build.returncode,
        "runtime_scope": "QCoreApplication only; no QApplication, QWidget or product process",
    }
    if build.returncode == 0:
        run_env = os.environ.copy()
        run_env["PATH"] = str(qt_dir / "bin") + os.pathsep + run_env["PATH"]
        # Reproducible cache-key lifetime counterexample; process-local only.
        run_env["QT_HASH_SEED"] = "0"
        run = subprocess.run([str(exe), str(result)], cwd=out_dir, env=run_env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out_dir / "run.log").write_bytes(run.stdout)
        sys.stdout.write(run.stdout.decode("utf-8", errors="replace"))
        manifest["run_returncode"] = run.returncode
        if result.exists():
            manifest["result"] = json.loads(result.read_text(encoding="utf-8"))
        no_packs_dir = out_dir / "no-packs"
        no_packs_dir.mkdir(exist_ok=True)
        no_packs_exe = no_packs_dir / exe.name
        shutil.copy2(exe, no_packs_exe)
        no_packs = subprocess.run([str(no_packs_exe), "--no-packs", str(no_packs_dir / "results.json")],
                                  cwd=Path(os.environ.get("SystemRoot", r"C:\Windows")), env=run_env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (no_packs_dir / "run.log").write_bytes(no_packs.stdout)
        sys.stdout.write(no_packs.stdout.decode("utf-8", errors="replace"))
        manifest["no_packs_returncode"] = no_packs.returncode
        if (no_packs_dir / "results.json").exists():
            manifest["no_packs_result"] = json.loads((no_packs_dir / "results.json").read_text(encoding="utf-8"))
        returncode = run.returncode or no_packs.returncode
    else:
        returncode = build.returncode
    (out_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("RESULT_DIRECTORY=" + str(out_dir))
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
