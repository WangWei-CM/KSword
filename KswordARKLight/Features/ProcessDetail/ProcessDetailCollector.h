#pragma once

#include "ProcessDetailTypes.h"

namespace Ksword::Features::ProcessDetail {

// ProcessDetailCollector performs the read-only process detail snapshot.
// Inputs are a PID selected by the process module; processing uses Win32 and
// dynamically resolved ntdll/Kernel32 exports; output is a detached snapshot
// suitable for UI rendering without keeping target handles open.
class ProcessDetailCollector final {
public:
    // Collect builds the Basic/Threads/Modules snapshot for one process ID.
    // Input is processId; processing tolerates protected, exited or access
    // denied targets; output always contains per-section status text.
    ProcessDetailSnapshot Collect(DWORD processId) const;
};

} // namespace Ksword::Features::ProcessDetail
