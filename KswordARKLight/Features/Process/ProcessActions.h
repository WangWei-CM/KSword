#pragma once

#include "ProcessModel.h"

#include <string>
#include <vector>

namespace Ksword::Features::Process {

enum class ProcessActionId {
    CopyCell,
    CopyRow,
    OpenDetails,
    TerminateProcess,
    R0TerminateProcess,
    R0SuspendProcess,
    R0HideUnlinkOnly,
    R0HidePatchPidOnly,
    R0HideLegacyBoth,
    R0UnhideProcess,
    R0ClearHiddenMarks,
    R0EnableBreakOnTermination,
    R0DisableBreakOnTermination,
    R0DisableApcInsertion,
    R0DkomRemoveFromCidTable,
    RefreshPplProtectionLevel,
    SuspendProcess,
    ResumeProcess,
    EnableEfficiencyMode,
    DisableEfficiencyMode,
    SetCriticalProcess,
    ClearCriticalProcess,
    OpenFolder,
    OpenMemoryOperation,
    ScanHotkeys,
    SetPriorityIdle,
    SetPriorityBelowNormal,
    SetPriorityNormal,
    SetPriorityAboveNormal,
    SetPriorityHigh,
    SetPriorityRealtime,
    R0SetPplNone,
    R0SetPplAuthenticode,
    R0SetPplCodeGen,
    R0SetPplAntimalware,
    R0SetPplLsa,
    R0SetPplWindows,
    R0SetPplWinTcb
};

struct ProcessActionMenuItem {
    ProcessActionId id = ProcessActionId::OpenDetails;
    std::wstring text;
};

struct ProcessActionResult {
    bool success = false;
    std::wstring title;
    std::wstring detail;
};

// ExecuteProcessAction runs the Win32 layer for one context-menu command. Inputs
// are action id, selected PIDs, and current model snapshot for path lookup.
// Processing performs local Win32 actions and retained ArkDriverClient R0
// operations. Output is a result
// message that callers must surface to the user/status area.
ProcessActionResult ExecuteProcessAction(
    ProcessActionId actionId,
    const std::vector<DWORD>& selectedPids,
    const std::vector<ProcessSnapshotRow>& snapshotRows);

// PriorityClassForAction maps menu priority actions to Win32 priority classes.
// Input is a ProcessActionId; output is zero when the id is not a priority item.
DWORD PriorityClassForAction(ProcessActionId actionId);

} // namespace Ksword::Features::Process
