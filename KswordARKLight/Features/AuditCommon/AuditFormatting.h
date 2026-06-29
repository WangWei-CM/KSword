#pragma once

#include "../../Core/Win32Lean.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Features::AuditCommon {

// FormatHexAddress formats a pointer-sized or protocol address value. Input is
// an integer address; processing emits uppercase hexadecimal with a 0x prefix;
// output is a stable display/export string.
std::wstring FormatHexAddress(std::uint64_t value);

// FormatHexValue formats a generic bitfield or numeric code. Input is an
// integer value and optional minimum digit count; processing uses uppercase
// hexadecimal; output is a stable display/export string.
std::wstring FormatHexValue(std::uint64_t value, int minimumDigits = 0);

// FormatNtStatus formats an NTSTATUS value with both hexadecimal and optional
// classification text. Input is a signed status value; processing preserves the
// low 32-bit representation; output is a human-readable diagnostic string.
std::wstring FormatNtStatus(LONG status);

// FormatWin32Error formats a Win32 error code. Input is a DWORD error value;
// processing calls FormatMessageW when possible and falls back to a numeric
// label; output is safe for UI labels, tables and exports.
std::wstring FormatWin32Error(DWORD errorCode);

// SanitizeTsvCell removes control separators that would corrupt a TSV export.
// Input is one cell string; processing replaces tabs/newlines with spaces;
// output is a single TSV-safe cell.
std::wstring SanitizeTsvCell(const std::wstring& text);

// BuildTsv serializes headers and rectangular rows. Inputs are display headers
// and already-rendered cells; processing sanitizes cells and joins them with
// tabs/CRLF; output is clipboard/file ready TSV text.
std::wstring BuildTsv(
    const std::vector<std::wstring>& headers,
    const std::vector<std::vector<std::wstring>>& rows);

// CopyTextToClipboard writes Unicode text to the Windows clipboard. Inputs are
// an owner HWND and payload; processing transfers a movable CF_UNICODETEXT
// allocation to the shell; output reports whether the operation succeeded.
bool CopyTextToClipboard(HWND owner, const std::wstring& text);

} // namespace Ksword::Features::AuditCommon
