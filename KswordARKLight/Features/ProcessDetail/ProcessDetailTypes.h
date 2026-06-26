#pragma once

#include "../../Core/Win32Lean.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Features::ProcessDetail {

// ProcessBasicInfo carries the read-only fields shown by the Basic page.
// Inputs are collected from Win32 and ntdll process queries; processing converts
// native values into UI-ready strings; consumers only render the fields.
struct ProcessBasicInfo {
    DWORD processId = 0;
    DWORD parentProcessId = 0;
    DWORD sessionId = 0;
    std::wstring imagePath;
    std::wstring commandLine;
    std::wstring bitness;
    std::wstring userName;
    std::wstring integrityLevel;
    std::wstring statusText;
};

// ProcessThreadInfo describes one thread row. Inputs come from Toolhelp thread
// snapshots and optional OpenThread queries; processing keeps metadata cheap and
// non-invasive; consumers display the row in the Threads tab.
struct ProcessThreadInfo {
    DWORD threadId = 0;
    DWORD ownerProcessId = 0;
    LONG basePriority = 0;
    LONG deltaPriority = 0;
    DWORD suspendCount = 0;
    std::uintptr_t startAddress = 0;
    std::wstring statusText;
};

// ProcessModuleInfo describes one module row. Inputs come from PSAPI module
// enumeration; processing resolves path/name/base/size where available;
// consumers display the row in the Modules tab.
struct ProcessModuleInfo {
    std::wstring moduleName;
    std::wstring modulePath;
    std::uintptr_t baseAddress = 0;
    DWORD imageSize = 0;
    DWORD representativeThreadId = 0;
    std::wstring statusText;
};

// ProcessDetailSnapshot is a complete page refresh result. Input is a target
// PID; processing is performed by ProcessDetailCollector; output is copied into
// the page and remains valid without holding process handles.
struct ProcessDetailSnapshot {
    ProcessBasicInfo basic;
    std::vector<ProcessThreadInfo> threads;
    std::vector<ProcessModuleInfo> modules;
    std::wstring errorText;
    bool basicSucceeded = false;
    bool threadsSucceeded = false;
    bool modulesSucceeded = false;
};

} // namespace Ksword::Features::ProcessDetail
