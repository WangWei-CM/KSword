"""用生产判定函数验证纯响应解释，不执行CLI实验或访问驱动。"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]  # 仅使用当前仓库及已知工具链落点。
OUTPUT = ROOT / 'output/review-fixes-20261017/result-reporting'


def extract_function(source: str, signature: str) -> str:
    """保持生产函数逐字节内容，只屏蔽注释/字符串中的括号以确定函数边界。"""
    literals = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    masked = literals.sub(lambda match: ' ' * len(match[0]), source)
    start = source.index(signature)
    opening = masked.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        if masked[cursor] == '{':
            depth += 1
        elif masked[cursor] == '}':
            depth -= 1
        cursor += 1
    return source[start:cursor]


def run(command: list[str], environment: dict[str, str]) -> None:
    """保留完整诊断；失败不能用已有二进制代替本轮结果。"""
    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True,
                            text=True, encoding='utf-8', errors='replace', timeout=120)
    with (OUTPUT / 'validation.log').open('a', encoding='utf-8') as log:
        log.write(result.stdout + result.stderr + f'\nEXIT_CODE={result.returncode}\n')
    print(result.stdout + result.stderr, end='', flush=True)
    if result.returncode:
        raise RuntimeError(f'Command failed with exit {result.returncode}: {command[0]}')


def main() -> None:
    """编译实际生产响应解释逻辑与受控结果夹具，所有输入均为内存POD。"""
    OUTPUT.mkdir(parents=True, exist_ok=True)
    vc = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
    sdk = Path('C:/Program Files (x86)/Windows Kits/10')
    version = '10.0.26100.0'
    compiler = vc / 'bin/Hostx64/x64/cl.exe'
    qt = ROOT / '.deps/Qt/6.9.3/msvc2022_64'
    if not compiler.is_file() or not (qt / 'lib/Qt6Core.lib').is_file():
        raise RuntimeError('Configured HostX64 MSVC and repository Qt SDK are required.')
    production = ROOT / 'Ksword5.1/Ksword5.1/MiscDock/HvciBypass/HvciBypassBackend.cpp'
    source = production.read_text(encoding='utf-8-sig')
    helpers = [extract_function(source, item) for item in (
        'QString formatNtStatus(', 'QString stageResultLine(')]
    verdict = extract_function(source, 'QString verdictText(')
    generated = OUTPUT / 'production_verdict.cpp'
    header = ROOT / 'Ksword5.1/Ksword5.1/MiscDock/HvciBypass/HvciBypassBackend.h'
    response_header = ROOT / 'shared/driver/KswordArkMemoryResult.h'
    generated.write_text('#include <Windows.h>\n#include "' + header.as_posix() + '"\n'
        + '#include "' + response_header.as_posix() + '"\n'
        + 'namespace {\n' + '\n'.join(helpers) + '\n}\nnamespace ks::misc::hvci_bypass {\n'
        + verdict + '\n}\n', encoding='utf-8')
    (OUTPUT / 'source.json').write_text(json.dumps({'production': str(production),
        'source_sha256': hashlib.sha256(production.read_bytes()).hexdigest(),
        'extracted_functions': ['formatNtStatus', 'stageResultLine', 'verdictText'],
        'hardware_calls': 0}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    environment = dict(os.environ)
    environment['PATH'] = str(vc / 'bin/Hostx64/x64') + os.pathsep + str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
    includes = ['/external:I' + str(vc / 'include')]
    includes += ['/external:I' + str(sdk / f'Include/{version}/{part}') for part in ('ucrt', 'shared', 'um')]
    includes += ['/external:I' + str(qt / 'include'), '/external:I' + str(qt / 'include/QtCore')]
    libraries = ['/LIBPATH:' + str(vc / 'lib/x64'), '/LIBPATH:' + str(qt / 'lib')]
    libraries += ['/LIBPATH:' + str(sdk / f'Lib/{version}/{part}/x64') for part in ('ucrt', 'um')]
    (OUTPUT / 'validation.log').write_text('HOST_TOOL_ARCHITECTURE=x64\nHARDWARE_CALLS=0\n', encoding='utf-8')
    executable = OUTPUT / 'memory_result_reporting_tests.exe'
    run([str(compiler), '/nologo', '/std:c++17', '/Zc:__cplusplus', '/permissive-', '/utf-8', '/EHsc', '/MD',
         '/W4', '/WX', '/external:W0', '/O1', '/DNOMINMAX', '/DUNICODE', '/D_UNICODE',
         '/DREVIEW_HVCI_TEXT_TESTS', *includes, str(ROOT / 'tools/tests/memory_result_reporting_tests.cpp'),
         str(generated), '/Fo' + str(OUTPUT) + '/', '/Fe' + str(executable), '/link', *libraries, 'Qt6Core.lib'], environment)
    run([str(executable)], environment)


if __name__ == '__main__':
    main()
