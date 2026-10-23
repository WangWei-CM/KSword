"""Compile the production create-request body against a Qt Core fake backend.

No widgets, real processes, tokens, services, drivers or product GUI are started.
The fixture compiles the method verbatim, replacing only its surrounding Dock,
button and backend with data-only test doubles. Requires x64 MSVC and Qt Core.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time

PREAMBLE = r'''
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QPromise>
#include <QRunnable>
#include <QThreadPool>
#include <QThread>
#include <QTimer>
#include <QStringList>
#include <atomic>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <QSemaphore>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

constexpr std::uint32_t ERROR_UNHANDLED_EXCEPTION = 574;
struct kLogEvent {};
struct Log { template<class T> Log& operator<<(const T&) { return *this; } };
Log info, err;
int eol = 0;
namespace ks::i18n { QString sourceText(const QString& value) { return value; } }
struct FakeProcess {
    std::uint32_t pid=7; std::uint64_t creation=100;
    std::atomic_int references{0},closed{0}; std::atomic_bool alive{true};
    bool denyOpen=false, denyTimes=false;
};
std::shared_ptr<FakeProcess> fakeSource=std::make_shared<FakeProcess>();
std::vector<std::shared_ptr<FakeProcess>> allSources{fakeSource};
void resetSource(std::uint64_t creation=100) { auto next=std::make_shared<FakeProcess>();next->creation=creation;allSources.push_back(next);fakeSource=std::move(next); }
bool tryReuseSourcePid() { if(fakeSource->references!=0) return false;resetSource(200);return true; }
using HANDLE=FakeProcess*;using BOOL=int;using DWORD=std::uint32_t;
constexpr BOOL FALSE=0;constexpr DWORD ERROR_SUCCESS=0,ERROR_PROCESS_ABORTED=1067,PROCESS_QUERY_LIMITED_INFORMATION=0x1000;
struct FILETIME { std::uint32_t dwLowDateTime=0,dwHighDateTime=0; };
DWORD GetLastError() { return 5; }
HANDLE OpenProcess(DWORD,BOOL,std::uint32_t pid) { if(!fakeSource || fakeSource->denyOpen || pid!=fakeSource->pid)return nullptr;++fakeSource->references;return fakeSource.get(); }
BOOL GetProcessTimes(HANDLE process,FILETIME* creation,FILETIME* exited,FILETIME* kernel,FILETIME* user) {
    if(!process || process->denyTimes)return FALSE;
    creation->dwLowDateTime=static_cast<std::uint32_t>(process->creation);creation->dwHighDateTime=static_cast<std::uint32_t>(process->creation>>32U);
    *exited={process->alive ? 0U : 1U,0};*kernel={};*user={};return 1;
}
BOOL CloseHandle(HANDLE process) { assert(process && process->references>0);--process->references;++process->closed;return 1; }
std::string formatProcessWin32Error(const char* step,DWORD error) { return std::string(step)+" failed, error="+std::to_string(error); }
namespace ks::process {
struct ProcessRecord {std::uint32_t pid=7;std::uint64_t creationTime100ns=100;};
bool QueryProcessCreationTimeByPid(std::uint32_t pid,std::uint64_t* creation,std::string* error) {
    *creation=0;HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process){*error="OpenProcess failed(5)";return false;}
    FILETIME created{},exited{},kernel{},user{};BOOL ok=GetProcessTimes(process,&created,&exited,&kernel,&user);CloseHandle(process);
    if(!ok){*error="GetProcessTimes failed(5)";return false;}
    *creation=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32U)|created.dwLowDateTime;return *creation!=0;
}
struct CreateProcessRequest { bool tokenModeEnabled = false; std::string applicationName;std::uint32_t tokenSourcePid=7;bool duplicatePrimaryToken=true; };
struct CreateProcessResult {
    bool success = false;
    std::uint32_t win32Error = 0;
    std::string detailText;
    bool usedTokenPath = false;
    bool processInfoAvailable = false;
    std::uint64_t hProcess = 0, hThread = 0;
    std::uint32_t dwProcessId = 0, dwThreadId = 0;
};
struct Backend {
    int delay = 0;
    bool succeeds = true, throws = false;
    std::atomic_int started{0}, completed{0};
    std::string capturedName;
    bool capturedToken = false, ranOffGui = false;
    std::uint64_t capturedSourceCreation=0;
    std::atomic_int privilegeEditsApplied{0};
};
std::shared_ptr<Backend> next;
bool LaunchProcess(const CreateProcessRequest& request, CreateProcessResult* result) {
    auto backend = next;
    backend->ranOffGui = QThread::currentThread() != QCoreApplication::instance()->thread();
    backend->capturedName = request.applicationName;
    backend->capturedToken = request.tokenModeEnabled;
    if(request.tokenModeEnabled) {
        assert(fakeSource->references>0);backend->capturedSourceCreation=fakeSource->creation;
        if(!request.duplicatePrimaryToken)++backend->privilegeEditsApplied;
    }
    ++backend->started;
    std::this_thread::sleep_for(std::chrono::milliseconds(backend->delay));
    ++backend->completed;
    if (backend->throws) throw std::runtime_error("fake exception");
    result->success = backend->succeeds;
    result->usedTokenPath = request.tokenModeEnabled;
    result->win32Error = backend->succeeds ? 0 : 5;
    result->detailText = request.applicationName;
    result->processInfoAvailable = backend->succeeds;
    result->dwProcessId = 111;
    result->dwThreadId = 222;
    result->hProcess = 333;
    result->hThread = 444;
    return backend->succeeds;
}
}
void require(bool condition, const char* reason) {
    if (!condition) { std::cerr << reason << '\n'; std::exit(1); }
}
struct QPushButton {
    bool ownEnabled = true, parentEnabled = true;
    QPushButton* parentWidget() { return nullptr; }
    bool isEnabledTo(QPushButton*) const { return ownEnabled; }
    void setEnabled(bool enabled) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(), "button changed off GUI");
        ownEnabled = enabled;
    }
};
__IDENTITY_HELPERS__
class ProcessDock : public QObject {
public:
    struct CacheEntry {ks::process::ProcessRecord record;bool isExitedInLatestRound=false;};
    std::unordered_map<std::string,CacheEntry> m_cacheByIdentity{{"source",{{7,100},false}}};
    bool m_createProcessInProgress = false;
    QPushButton button;
    QPushButton* m_launchProcessButton = &button;
    ks::process::CreateProcessRequest input;
    bool valid = true;
    int refreshes = 0;
    QStringList lines;
    void executeCreateProcessRequest();
    ks::process::CreateProcessRequest buildCreateProcessRequestFromUi(bool* ok, QString* error) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(), "UI snapshot read off GUI");
        *ok = valid; *error = QStringLiteral("fake invalid"); return input;
    }
    void appendCreateResultLine(const QString& line) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(), "result changed off GUI");
        lines.push_back(line);
    }
    void requestAsyncRefresh(bool) { ++refreshes; }
};
'''

MAIN = r'''
template<class Predicate> void pumpUntil(Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "future did not finish");
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    QCoreApplication::processEvents();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int heartbeat = 0;
    QTimer timer; QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; }); timer.start(1);
    int cases = 0;
    for (const int delay : {100, 1000}) {
        auto backend = std::make_shared<ks::process::Backend>(); backend->delay = delay;
        ks::process::next = backend;
        ProcessDock dock; dock.input.applicationName = "immutable request"; dock.input.tokenModeEnabled = true;
        int before = heartbeat;
        dock.executeCreateProcessRequest();
        require(dock.m_createProcessInProgress && !dock.button.ownEnabled, "missing busy/button state");
        require(backend->completed == 0, "method waited for backend");
        dock.executeCreateProcessRequest();
        dock.input.applicationName = "edited after submit"; dock.input.tokenModeEnabled = false;
        pumpUntil([&] { return !dock.m_createProcessInProgress; });
        require(backend->started == 1 && backend->completed == 1, "duplicate launch accepted");
        require(backend->ranOffGui && backend->capturedToken, "backend thread/token changed");
        require(backend->capturedName == "immutable request", "request was not snapshotted");
        require(heartbeat > before + 10, "Core event loop heartbeat starved");
        require(dock.button.ownEnabled && dock.refreshes == 1 && dock.lines.size() == 5, "success delivered incorrectly");
        require(dock.lines.join('\n').contains("111") && dock.lines.join('\n').contains("14d"), "PI snapshot lost");
        QCoreApplication::processEvents();
        require(dock.refreshes == 1 && dock.lines.size() == 5, "duplicate result");
        ++cases;
    }
    for (bool throws : {false, true}) {
        auto backend = std::make_shared<ks::process::Backend>(); backend->succeeds = false; backend->throws = throws;
        ks::process::next = backend;
        ProcessDock dock; dock.button.ownEnabled = false;
        dock.executeCreateProcessRequest();
        pumpUntil([&] { return !dock.m_createProcessInProgress; });
        require(!dock.button.ownEnabled && dock.refreshes == 0 && dock.lines.size() == 4, "failed request enabled or refreshed");
        require(dock.lines.join('\n').contains(throws ? "574" : "5"), "failure code lost");
        ++cases;
    }
    {
        auto backend = std::make_shared<ks::process::Backend>(); ks::process::next = backend;
        ProcessDock dock; dock.valid = false; dock.executeCreateProcessRequest();
        require(!dock.m_createProcessInProgress && backend->started == 0 && dock.lines.size() == 1, "invalid request submitted");
        ++cases;
    }
    {
        auto backend = std::make_shared<ks::process::Backend>(); backend->delay = 1000; ks::process::next = backend;
        resetSource();
        auto* dock = new ProcessDock(); dock->input.tokenModeEnabled=true;dock->executeCreateProcessRequest();
        pumpUntil([&] { return backend->started != 0; });
        delete dock;
        require(backend->completed == 0 && fakeSource->references==1, "Dock destructor joined backend or dropped source pin");
        pumpUntil([&] { return backend->completed != 0; });
        QThreadPool::globalInstance()->waitForDone();
        require(fakeSource->references==0,"source pin leaked after Dock close/backend completion");
        QCoreApplication::processEvents();
        ++cases;
    }
    {
        auto backend = std::make_shared<ks::process::Backend>(); ks::process::next = backend;
        ProcessDock dock; dock.button.parentEnabled = false; dock.executeCreateProcessRequest();
        pumpUntil([&] { return !dock.m_createProcessInProgress; });
        require(dock.button.ownEnabled && !dock.button.parentEnabled, "parent-disabled state became explicit disabled");
        ++cases;
    }

    // A stale cached source identity must not bind a recycled PID at acceptance.
    {
        resetSource(200);auto backend=std::make_shared<ks::process::Backend>();ks::process::next=backend;
        ProcessDock dock;dock.input.tokenModeEnabled=true;dock.input.duplicatePrimaryToken=false;dock.executeCreateProcessRequest();
        require(!dock.m_createProcessInProgress && backend->started==0 && backend->privilegeEditsApplied==0,"recycled source accepted");
        require(fakeSource->references==0 && dock.lines.size()==2,"recycled source leaked or unclear failure");++cases;
    }
    // Unknown/current identity may be obtained at acceptance, but resource/exit failures never queue.
    for(int failure=0;failure<5;++failure) {
        resetSource();auto backend=std::make_shared<ks::process::Backend>();ks::process::next=backend;
        ProcessDock dock;dock.input.tokenModeEnabled=true;
        if(failure==0)fakeSource->denyOpen=true;
        if(failure==1)fakeSource->denyTimes=true;
        if(failure==2)fakeSource->alive=false;
        if(failure==3){dock.m_cacheByIdentity.clear();fakeSource->denyOpen=true;}
        if(failure==4)dock.m_cacheByIdentity.begin()->second.isExitedInLatestRound=true;
        dock.executeCreateProcessRequest();
        require(!dock.m_createProcessInProgress && backend->started==0 && fakeSource->references==0,"failed/unknown/exited identity queued or leaked");
        require(dock.button.ownEnabled && dock.lines.size()==2,"identity failure changed busy/button state");++cases;
    }
    {
        resetSource();auto backend=std::make_shared<ks::process::Backend>();ks::process::next=backend;
        ProcessDock dock;dock.input.tokenModeEnabled=true;dock.m_cacheByIdentity.clear();dock.executeCreateProcessRequest();
        pumpUntil([&]{return !dock.m_createProcessInProgress;});QThreadPool::globalInstance()->waitForDone();
        require(backend->started==1 && backend->capturedSourceCreation==100 && fakeSource->references==0,"unknown live click-time identity not pinned");++cases;
    }
    // Simulate a saturated pool: source exits after acceptance, before the create task starts.
    {
        resetSource();auto backend=std::make_shared<ks::process::Backend>();ks::process::next=backend;
        auto* pool=QThreadPool::globalInstance();pool->waitForDone();const int previousMaximum=pool->maxThreadCount();pool->setMaxThreadCount(1);
        QSemaphore entered,released;pool->start(QRunnable::create([&]{entered.release();released.acquire();}));entered.acquire();
        ProcessDock dock;dock.input.tokenModeEnabled=true;dock.input.duplicatePrimaryToken=false;dock.executeCreateProcessRequest();
        require(dock.m_createProcessInProgress && fakeSource->references==1,"queue did not retain pin");
        fakeSource->alive=false;require(!tryReuseSourcePid(),"source PID reusable while queued pin exists");released.release();
        pumpUntil([&]{return !dock.m_createProcessInProgress;});pool->waitForDone();pool->setMaxThreadCount(previousMaximum);
        require(backend->started==0 && backend->privilegeEditsApplied==0,"exited source mutated a token");
        require(dock.lines.join('\n').contains("1067") && fakeSource->references==0,"worker identity refusal failed to finish/release");
        require(tryReuseSourcePid(),"source handle remained pinned after task");++cases;
    }
    {
        resetSource();auto backend=std::make_shared<ks::process::Backend>();backend->throws=true;ks::process::next=backend;
        ProcessDock dock;dock.input.tokenModeEnabled=true;dock.executeCreateProcessRequest();
        pumpUntil([&]{return !dock.m_createProcessInProgress;});QThreadPool::globalInstance()->waitForDone();
        require(fakeSource->references==0 && dock.lines.join('\n').contains("574"),"exception leaked source pin or busy");++cases;
    }
    std::cout << "{\"cases\":" << cases << ",\"heartbeat\":" << heartbeat << ",\"result\":\"passed\"}\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--qt-dir', type=Path, default=os.environ.get('KSWORD_QT_DIR'))
    parser.add_argument('--vcvars64', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    qt = args.qt_dir or root / '.deps/Qt/6.9.3/msvc2022_64'
    build = args.build_dir or root / '.codex-build-logs/process-create-async-tests'
    build.mkdir(parents=True, exist_ok=True)
    source = (root / 'Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp').read_text(encoding='utf-8-sig')
    begin = source.index('void ProcessDock::executeCreateProcessRequest()\n')
    end = source.index('\nbool ProcessDock::isTreeModeEnabled() const', begin)
    method = source[begin:end]
    assert 'QPromise<LaunchOutcome>' in method and 'QFutureWatcher<LaunchOutcome>' in method
    fixture = build / 'create_async_fixture.cpp'
    helper_begin = source.index('    class ScopedProcessActionHandle final')
    helper_end = source.index('    // enableProcessContextPrivilege 作用：', helper_begin)
    helpers = source[helper_begin:helper_end]
    fixture.write_text(PREAMBLE.replace('__IDENTITY_HELPERS__', helpers) + method + MAIN, encoding='utf-8')
    exe = build / 'create_async_fixture.exe'
    command = build / 'compile.cmd'
    command.write_text(
        '@echo off\n'
        f'call "{args.vcvars64}"\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        'cl.exe /nologo /std:c++17 /permissive- /EHsc /utf-8 /MD /W4 /WX /Zc:__cplusplus '
        f'/I"{qt / "include"}" /I"{qt / "include/QtCore"}" '
        f'"{fixture}" /Fo"{build / "fixture.obj"}" /Fe"{exe}" '
        f'/link /LIBPATH:"{qt / "lib"}" Qt6Core.lib\n'
        'exit /b %errorlevel%\n', encoding='utf-8')
    compile_result = subprocess.run(['cmd.exe', '/d', '/c', str(command)], capture_output=True, text=True)
    (build / 'compile.log').write_text(compile_result.stdout + compile_result.stderr, encoding='utf-8')
    if compile_result.returncode:
        raise RuntimeError((build / 'compile.log').read_text(encoding='utf-8'))
    env = dict(os.environ, PATH=str(qt / 'bin') + os.pathsep + os.environ.get('PATH', ''))
    started = time.perf_counter()
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=20)
    report = {'returncode': result.returncode, 'elapsed_seconds': round(time.perf_counter() - started, 3),
              'stdout': result.stdout, 'stderr': result.stderr,
              'scope': 'Production method, Qt Core event loop, fake backend and data-only Dock/button; no GUI or system mutation'}
    (build / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
