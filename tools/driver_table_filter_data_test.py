from pathlib import Path
import argparse
import os
import subprocess

parser = argparse.ArgumentParser(description='Compile and run the production DriverDock search matcher with QtCore only; no GUI, driver or SCM calls.')
parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--output-dir', type=Path)
args = parser.parse_args()
repo = args.source_root.resolve()
source = (repo / 'Ksword5.1/Ksword5.1/DriverDock/DriverDock.cpp').read_text(encoding='utf-8')
begin = source.index('namespace\n{\n    struct DriverFilterMatchResult')
end = source.index('void DriverDock::queueDriverTableFilter', begin)
unit = source[begin:end]
test = r'''
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>
''' + unit + r'''
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::atomic_uint64_t ticket{3};
    int checks = 0;
    const auto require = [&checks](const bool condition, const char* description)
    {
        ++checks;
        if (!condition) { std::cerr << description << '\n'; std::exit(1); }
    };
    const std::vector<QStringList> serviceRows = {
        {QStringLiteral("Alpha"), QStringLiteral("display"), QStringLiteral("C:\\One.sys"), QStringLiteral("first")},
        {QStringLiteral("beta"), QStringLiteral("中文驱动"), QStringLiteral("C:\\Two.sys"), QStringLiteral("second")},
        {QStringLiteral("gamma"), QStringLiteral("other"), QStringLiteral("C:\\three.sys"), QStringLiteral("last")}
    };
    auto result = matchDriverFilterRows(serviceRows, QStringLiteral("ALPHA"), -2, false, ticket, 3);
    require(result.matches == std::vector<std::uint8_t>({1,0,0}), "case insensitive service name");
    result = matchDriverFilterRows(serviceRows, QStringLiteral("中文"), -2, false, ticket, 3);
    require(result.matches == std::vector<std::uint8_t>({0,1,0}), "Chinese description field");
    result = matchDriverFilterRows(serviceRows, QStringLiteral("Alpha\ndisplay"), -2, false, ticket, 3);
    require(result.matches == std::vector<std::uint8_t>({0,0,0}), "service field boundaries remain independent");
    result = matchDriverFilterRows(serviceRows, QString(), -2, false, ticket, 3);
    require(result.matches == std::vector<std::uint8_t>({1,1,1}), "cleared search restores every canonical row");
    const std::vector<QStringList> moduleRows = {
        {QStringLiteral("A.sys"), QStringLiteral("C:\\A.sys"), QStringLiteral("Trusted")},
        {QStringLiteral("B.sys"), QStringLiteral("C:\\B.sys"), QStringLiteral("Invalid chain")}
    };
    result = matchDriverFilterRows(moduleRows, QStringLiteral("invalid"), -2, false, ticket, 3);
    require(result.matches == std::vector<std::uint8_t>({0,1}), "signature status remains searchable");
    std::vector<QStringList> unloadedRows;
    for (int index = 0; index < 4096; ++index)
    {
        QStringList row = {QStringLiteral("Driver%1.sys").arg(index), QStringLiteral("0x%1").arg(index,16,16,QChar('0')),
            QStringLiteral("0x0000000000001000"), QStringLiteral("0x12345678"),
            index % 8 == 0 ? QStringLiteral("0xC0000001") : QStringLiteral("-"), QStringLiteral("2026-10-20 12:00:00.001")};
        row.push_back(row.join(QLatin1Char('\n')));
        unloadedRows.push_back(std::move(row));
    }
    QElapsedTimer elapsed;
    elapsed.start();
    result = matchDriverFilterRows(unloadedRows, QStringLiteral("C0000001"), 4, false, ticket, 3);
    require(std::accumulate(result.matches.begin(), result.matches.end(), 0) == 512, "4096 rows have exact status count");
    for (int index = 0; index < 4096; ++index)
    {
        require(result.matches[static_cast<std::size_t>(index)] == (index % 8 == 0 ? 1 : 0), "4096 source indices stay stable");
    }
    result = matchDriverFilterRows(unloadedRows, QStringLiteral("^Driver[0-9]+\\.sys\\n0x"), -1, true, ticket, 3);
    require(std::accumulate(result.matches.begin(), result.matches.end(), 0) == 4096, "all-fields regex can cross original field separator");
    result = matchDriverFilterRows(unloadedRows, QStringLiteral("^Driver[0-9]+\\.sys\\n0x"), 0, true, ticket, 3);
    require(std::accumulate(result.matches.begin(), result.matches.end(), 0) == 0, "single-field regex never leaks adjacent field");
    result = matchDriverFilterRows(unloadedRows, QStringLiteral("["), -1, true, ticket, 3);
    require(!result.error.isEmpty(), "invalid regular expression exposes original Qt diagnostic");
    require(std::accumulate(result.matches.begin(), result.matches.end(), 0) == 0, "invalid regex displays zero rows without losing canonical data");
    ticket.store(4);
    result = matchDriverFilterRows(unloadedRows, QString(), -1, false, ticket, 3);
    require(result.canceled, "new query invalidates old worker before matching row");
    std::cout << "PASS checks=" << checks << " rows=4096 elapsed_ms=" << elapsed.elapsed() << '\n';
}
'''
out = (args.output_dir or repo / '.codex-build-logs/driver-table-filter-data-test').resolve()
out.mkdir(parents=True, exist_ok=True)
cpp = out / 'filter-data-tests.cpp'
cpp.write_text(test, encoding='utf-8')
qt = Path(os.environ.get('KSWORD_QT_DIR', r'D:\Software\Qt\6.9.3\msvc2022_64'))
devcmd = os.environ.get('KSWORD_VSDEVCMD', r'D:\Software\VS\Common7\Tools\VsDevCmd.bat')
exe = out / 'filter-data-tests.exe'
cmd = out / 'build-data-tests.cmd'
cmd.write_text(f'''@echo off
call "{devcmd}" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /permissive- /EHsc /W4 /WX /MD /utf-8 /Zc:__cplusplus /external:W0 /external:I "{qt / 'include'}" /external:I "{qt / 'include/QtCore'}" /DQT_CORE_LIB /Fo"{out / 'filter-data-tests.obj'}" /Fe"{exe}" "{cpp}" /link /LIBPATH:"{qt / 'lib'}" Qt6Core.lib
if errorlevel 1 exit /b %errorlevel%
set "PATH={qt / 'bin'};%PATH%"
"{exe}"
''', encoding='utf-8')
result = subprocess.run(['cmd.exe', '/d', '/c', str(cmd)], cwd=out, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(out / 'data-tests.log').write_text(result.stdout, encoding='utf-8')
print(result.stdout)
raise SystemExit(result.returncode)
