#pragma once

#include "../../Core/Win32Lean.h"

#include "FileSystemEnumerator.h"

#include <string>

namespace Ksword::Features::File {

// FileActionId is the command namespace for the File page context menu. Values
// are intentionally local to the file module so this module can
// wire them without touching other feature modules.
enum class FileActionId : UINT {
    None = 0,
    OpenRun = 51001,
    CopyPath,
    CopyKernelModeAddress,
    CopyShortFileName,
    CopyLinkTarget,
    OpenLinkTarget,
    LocateLinkTarget,
    CopyToOppositePanel,
    MoveToOppositePanel,
    Rename,
    DeleteItem,
    DriverDelete,
    FileUnlocker,
    TakeOwnership,
    NewFile,
    NewFolder,
    OpenTerminal,
    SelectColumns,
    Hash,
    Signature,
    Entropy,
    HexView,
    PeViewer,
    MappedProcessScan
};

// FileActionContext describes the row and directory that an action operates on.
// Inputs come from FileView selection state; processing in FileActions never
// performs enumeration and returns only short status text for the UI.
struct FileActionContext {
    HWND owner = nullptr;
    std::wstring currentDirectory;
    FileEntry selectedEntry;
    bool hasSelection = false;
};

// FileActionResult carries the effect summary for a menu command. Inputs are an
// action id and context; output flags tell FileView whether to refresh or
// navigate after the action completes.
struct FileActionResult {
    bool handled = false;
    bool refreshRequested = false;
    bool navigateRequested = false;
    std::wstring navigatePath;
    std::wstring statusText;
};

// FileActions owns all right-click menu commands for the lightweight File page.
// Inputs are selected file rows and command ids; processing uses only local
// Win32/Shell APIs and never calls external UI, handle, network or driver
// modules; output is a FileActionResult for FileView.
class FileActions final {
public:
    // showContextMenu displays the complete original FileDock menu surface as
    // lightweight entries. Inputs are owner/context/screen point; processing
    // disables unsupported heavy actions; output is the chosen command id.
    static FileActionId showContextMenu(HWND owner, const FileActionContext& context, POINT screenPoint);

    // execute runs one command. Inputs are an action id and context; processing
    // performs the permitted local operation or reports that the entry is only
    // not retained in the light build; output tells the caller what changed.
    static FileActionResult execute(FileActionId action, const FileActionContext& context);

    // copyTextToClipboard writes Unicode text to the process clipboard. Inputs
    // are owner and text; processing allocates a CF_UNICODETEXT block; output is
    // true on success.
    static bool copyTextToClipboard(HWND owner, const std::wstring& text);
};

} // namespace Ksword::Features::File
