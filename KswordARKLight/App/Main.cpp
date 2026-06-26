#include "MainWindow.h"

#include "../Core/Privilege.h"

#include "../Core/Win32Lean.h"

#include <string>

namespace {

// TraceStartupStep writes one millisecond timing line to the debugger. Inputs
// are the step name plus the measured duration; processing is non-modal and
// intentionally avoids MessageBoxW so it does not perturb UAC timing; no value
// is returned.
void TraceStartupStep(const wchar_t* step, const ULONGLONG elapsedMs) {
    std::wstring line = L"[KswordARKLight startup] ";
    line += step ? step : L"<unknown>";
    line += L": ";
    line += std::to_wstring(elapsedMs);
    line += L" ms\r\n";
    ::OutputDebugStringW(line.c_str());
}

// TraceStartupText writes one startup status line. Input is static diagnostic
// text; processing goes only to the debugger output stream; no value is
// returned.
void TraceStartupText(const wchar_t* text) {
    std::wstring line = L"[KswordARKLight startup] ";
    line += text ? text : L"<null>";
    line += L"\r\n";
    ::OutputDebugStringW(line.c_str());
}

} // namespace

// wWinMain is the pure Win32 process entry point. Inputs are the standard Win32
// instance/show parameters; processing optionally relaunches elevated, creates
// the main shell, and enters the message loop; output is the process exit code.
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const ULONGLONG afterProbeMessage = ::GetTickCount64();
    const bool runningAsAdmin = Ksword::Core::IsRunningAsAdmin();
    TraceStartupStep(L"IsRunningAsAdmin", ::GetTickCount64() - afterProbeMessage);

    if (!runningAsAdmin) {
        const ULONGLONG beforeRunas = ::GetTickCount64();
        TraceStartupText(L"calling ShellExecuteExW(runas)");
        if (Ksword::Core::RelaunchElevated()) {
            TraceStartupStep(L"RelaunchElevated returned success", ::GetTickCount64() - beforeRunas);
            return 0;
        }
        TraceStartupStep(L"RelaunchElevated returned failure/cancel", ::GetTickCount64() - beforeRunas);
    }

    const ULONGLONG beforeMainWindowCreate = ::GetTickCount64();
    Ksword::App::MainWindow window;
    if (!window.create(instance, showCommand)) {
        ::MessageBoxW(nullptr, L"Failed to create KswordARKLight main window.", L"KswordARKLight", MB_ICONERROR | MB_OK);
        return 1;
    }
    TraceStartupStep(L"MainWindow::create", ::GetTickCount64() - beforeMainWindowCreate);
    return window.run();
}
