#pragma once

#include "../../Core/Win32Lean.h"

#include <string>
#include <vector>

namespace Ksword::Features::File {

// FileEntryKind describes how the file view should render and open an entry.
// Values come only from Win32 drive enumeration and FindFirstFileW data.
enum class FileEntryKind {
    Drive,
    Directory,
    File
};

// FileEntry is the UI-neutral row model for the File page. Inputs are populated
// by FileSystemEnumerator; consumers read name/path/attributes/size to display
// rows and to choose context-menu actions.
struct FileEntry {
    FileEntryKind kind = FileEntryKind::File;
    std::wstring name;
    std::wstring fullPath;
    DWORD attributes = 0;
    ULONGLONG size = 0;
    FILETIME lastWriteTime{};
    bool reparsePoint = false;
};

// DirectoryEnumerationResult carries either a normal directory listing or the
// virtual drive-root listing. Inputs are a requested path; processing fills rows
// from Windows APIs; status fields preserve failures without throwing.
struct DirectoryEnumerationResult {
    std::wstring directory;
    std::vector<FileEntry> entries;
    DWORD errorCode = ERROR_SUCCESS;
    std::wstring statusText;
    bool virtualDriveRoot = false;
};

// FileSystemEnumerator performs file-system discovery for the lightweight file
// module. Inputs are normalized Windows paths; processing uses only
// GetLogicalDriveStringsW, GetLogicalDrives, FindFirstFileW and FindNextFileW
// for enumeration; output is a display-ready result object.
class FileSystemEnumerator final {
public:
    // enumerate returns drives when directory is empty, otherwise it lists the
    // requested directory. It never blocks on recursive traversal and never
    // throws; failures are reported in errorCode/statusText.
    DirectoryEnumerationResult enumerate(const std::wstring& directory) const;

    // formatAttributes converts WIN32_FIND_DATAW attributes into compact text.
    // Input is a DWORD attribute bitset; output is suitable for a list column.
    static std::wstring formatAttributes(DWORD attributes);

    // formatSize converts a byte count into a readable string. Inputs are a byte
    // count and entry kind; output is blank for directories and drives.
    static std::wstring formatSize(ULONGLONG bytes, FileEntryKind kind);

    // formatLastWriteTime converts FILETIME into local date/time text. Input is
    // a FILETIME from FindFirstFileW; output is blank if conversion fails.
    static std::wstring formatLastWriteTime(const FILETIME& fileTime);

private:
    DirectoryEnumerationResult enumerateDrives() const;
    DirectoryEnumerationResult enumerateDirectory(const std::wstring& directory) const;
};

} // namespace Ksword::Features::File
