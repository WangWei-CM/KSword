#pragma once

#include "../../Core/Win32Lean.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Features::Process {

// ProcessSnapshotRow is the raw R3 process record produced from
// NtQuerySystemInformation(SystemProcessInformation). Inputs are kernel-returned
// SYSTEM_PROCESS_INFORMATION fields plus optional Win32 image-path enrichment;
// consumers should treat each row as a point-in-time snapshot.
struct ProcessSnapshotRow {
    DWORD processId = 0;
    DWORD parentProcessId = 0;
    ULONG handleCount = 0;
    ULONG sessionId = 0;
    ULONG threadCount = 0;
    LONG basePriority = 0;
    ULONGLONG kernelTime100ns = 0;
    ULONGLONG userTime100ns = 0;
    ULONGLONG cycleTime = 0;
    SIZE_T workingSetBytes = 0;
    SIZE_T privatePageBytes = 0;
    SIZE_T virtualSizeBytes = 0;
    ULONG pageFaultCount = 0;
    double cpuUsagePercent = 0.0;
    std::wstring imageName;
    std::wstring imagePath;
};

// ProcessEnumerationResult groups all rows from one enumeration pass. success is
// false only for fatal NtApi failures; partial per-process enrichment failures
// leave individual imagePath fields empty and keep success true.
struct ProcessEnumerationResult {
    bool success = false;
    LONG ntStatus = 0;
    std::wstring diagnosticText;
    std::vector<ProcessSnapshotRow> rows;
};

// EnumerateProcessesByNtQuerySystemInformation queries the system process list
// using dynamically-bound NtQuerySystemInformation. Inputs: none. Processing:
// grow a raw buffer, parse SYSTEM_PROCESS_INFORMATION entries, and enrich image
// paths through QueryFullProcessImageNameW when permissions allow. Return value:
// a ProcessEnumerationResult containing rows or a fatal diagnostic.
ProcessEnumerationResult EnumerateProcessesByNtQuerySystemInformation();

// QueryProcessImagePath opens one process with PROCESS_QUERY_LIMITED_INFORMATION
// and queries its full executable path. Input is a PID. Processing is best-effort
// and never terminates or modifies the target process. Return value is an empty
// string when access is denied, PID is invalid, or the image path is unavailable.
std::wstring QueryProcessImagePath(DWORD processId);

} // namespace Ksword::Features::Process
