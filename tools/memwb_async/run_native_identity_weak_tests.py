"""Compile exact production NativeIdentity against fake Win32 and fake ports."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--vs-dev-cmd', type=Path)
    args = parser.parse_args()
    root = args.repository_root.resolve()
    source_root = args.source_root.resolve() if args.source_root else root
    out = args.output_root.resolve() if args.output_root else root / '.codex-build-logs/native-identity-weak-tests'
    out.mkdir(parents=True, exist_ok=True)
    relative = Path('Ksword5.1/Ksword5.1/UI/MemoryWorkbench')
    isolated, production = source_root / relative, root / relative
    native = isolated / 'WorkbenchFrozenWriteContext.Native.cpp'
    native_bytes = native.read_bytes()
    source = native_bytes.decode('utf-8')
    start = source.index('        std::uint64_t Time(')
    end = source.index('        class NativeDdmaWriteScope', start)
    fragment = source[start:end]
    assert fragment.count('class NativeIdentity final') == 1
    assert 'MakeNativeFrozenWriteFactories' not in fragment
    include = 'namespace ks::ui { namespace {\n' + fragment + '\n} }\n'
    (out / 'NativeIdentity.extract.inc').write_text(include, encoding='utf-8', newline='')
    core = root / 'shared/evidence/memory_workbench'
    names = ['MemoryIoByteStore.cpp', 'MemoryKernelMutation.cpp', 'MemoryTargetSession.cpp',
             'PointerChainBindings.cpp', 'MemoryAddressBook.cpp', 'SessionAddressResolver.cpp',
             'MemoryModuleDirectory.cpp', 'MemoryAddressExpr.cpp']
    sources = [source_root / 'tools/memwb_async/NativeIdentityWeakTests.cpp',
               isolated / 'WorkbenchFrozenWriteContext.cpp']
    sources += [core / name for name in names]
    sources += [root / 'shared/evidence/PointerChain.cpp']
    flags = ['/nologo', '/W4', '/WX', '/EHsc', '/std:c++20', '/Zc:__cplusplus', '/utf-8', '/MD', '/O2',
             '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', f'/I"{isolated}"', f'/I"{production}"', f'/I"{out}"',
             f'/Fe"{out / "native-identity-tests.exe"}"', f'/Fo"{str(out)}/"']
    flags += [f'"{item}"' for item in sources]
    (out / 'tests.rsp').write_text(' '.join(flags) + '\n', encoding='utf-8')
    candidates = [args.vs_dev_cmd] if args.vs_dev_cmd else [
        Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat'),
        Path('D:/Software/VS/Common7/Tools/VsDevCmd.bat')]
    dev_cmd = next((path for path in candidates if path.is_file()), None)
    if not dev_cmd:
        raise RuntimeError('Pass --vs-dev-cmd for the installed 64-bit MSVC development environment.')
    lines = ['@echo off', f'call "{dev_cmd}" -arch=x64 -host_arch=x64',
             'if errorlevel 1 exit /b %errorlevel%', f'cl.exe @"{out / "tests.rsp"}"',
             'if errorlevel 1 exit /b %errorlevel%', f'"{out / "native-identity-tests.exe"}"',
             'if errorlevel 1 exit /b %errorlevel%',
             f'dumpbin /imports "{out / "native-identity-tests.exe"}" > "{out / "imports.txt"}"',
             'exit /b %errorlevel%']
    (out / 'compile.cmd').write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    run = subprocess.run(['cmd', '/d', '/c', str(out / 'compile.cmd')], cwd=out, capture_output=True)
    (out / 'compile.log').write_bytes(run.stdout + run.stderr)
    imports = (out / 'imports.txt').read_text(errors='replace') if (out / 'imports.txt').exists() else ''
    forbidden = [name for name in ['OpenProcess', 'GetProcessTimes', 'GetProcessId', 'GetExitCodeProcess',
        'IsWow64Process', 'ReadProcessMemory', 'WriteProcessMemory', 'DeviceIoControl'] if name in imports]
    proof = {'exit_code': run.returncode, 'native_source_sha256': hashlib.sha256(native_bytes).hexdigest(),
             'exact_fragment_sha256': hashlib.sha256(fragment.encode('utf-8')).hexdigest(),
             'exact_fragment_matches_current_source': native.read_bytes() == native_bytes,
             'native_target_api_imports': forbidden,
             'output': (run.stdout + run.stderr).decode('utf-8', errors='replace'),
             'boundary': 'Exact NativeIdentity extraction plus real frozen context/MemoryIoByteStore; fake Win32 and fake ports only; no actual target/driver API'}
    (out / 'result.json').write_text(json.dumps(proof, indent=2) + '\n', encoding='utf-8')
    print(proof['output'])
    print(json.dumps({key: value for key, value in proof.items() if key != 'output'}))
    return 0 if run.returncode == 0 and not forbidden and proof['exact_fragment_matches_current_source'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
