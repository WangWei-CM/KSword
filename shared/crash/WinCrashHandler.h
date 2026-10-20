#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace ks
{
    namespace crash
    {
        // These arguments are internal process-lifecycle markers. They are shared by
        // Ksword5.1.exe and Launcher.exe so either executable can restart itself only
        // after the crashed predecessor has terminated.
        extern const wchar_t kCrashRestartWaitPidArgument[];
        extern const wchar_t kCrashRestartedArgument[];

        struct Configuration
        {
            const wchar_t* productName = nullptr;
            const wchar_t* dumpFilePrefix = nullptr;
            bool preferLauncherReporter = false;
        };

        struct DialogText
        {
            const wchar_t* title = nullptr;
            const wchar_t* instruction = nullptr;
            const wchar_t* exceptionCodeLabel = nullptr;
            const wchar_t* exceptionAddressLabel = nullptr;
            const wchar_t* dumpPathLabel = nullptr;
            const wchar_t* dumpUnavailableText = nullptr;
            const wchar_t* restartQuestion = nullptr;
            const wchar_t* repeatedCrashText = nullptr;
            const wchar_t* restartFailedText = nullptr;
        };

        // InstallCrashHandler must be called at the beginning of main/wWinMain. It
        // preloads DbgHelp and prepares all paths before a process failure can damage
        // the heap or loader state.
        void InstallCrashHandler(const Configuration& configuration);

        // UpdateCrashDialogText copies the supplied strings into fixed internal
        // buffers. The caller may release its source strings after this call.
        void UpdateCrashDialogText(const DialogText& text);

        // WaitForCrashRestartTargetFromCommandLine consumes the internal wait-PID
        // argument. A true result means the current process is the direct child of
        // the same executable, and that predecessor was observed exiting within the
        // timeout. Callers may bypass the normal instance check only on true.
        bool WaitForCrashRestartTargetFromCommandLine(DWORD timeoutMilliseconds = 30000);

        enum class RestartPredecessorStatus
        {
            NotRequested,
            InvalidArgument,
            ValidationRejected,
            WaitFailed,
            TimedOut,
            PredecessorExited,
            PredecessorAlreadyExited
        };
        // Normal privilege handoff needs a fail-closed result matrix. The legacy
        // crash-reporter boolean API above keeps its existing behavior.
        RestartPredecessorStatus InspectRestartPredecessorFromCommandLine(DWORD timeoutMilliseconds);
        // Parent-side launch validation prevents a brokered non-child from
        // triggering shutdown of the original instance without a valid handoff.
        bool ValidateRestartChildProcess(HANDLE childProcess);

        bool CommandLineHasArgument(const wchar_t* argumentName);
    }
}
