#include "DriverMemoryModel.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace Ksword::Features::Memory {
namespace {

// TrimWhitespace removes leading and trailing Unicode whitespace. Input is any
// string; processing scans from both ends with iswspace; output is the trimmed
// copy used by parsers.
std::wstring TrimWhitespace(const std::wstring& text) {
    std::size_t first = 0;
    while (first < text.size() && std::iswspace(text[first])) {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::iswspace(text[last - 1])) {
        --last;
    }

    return text.substr(first, last - first);
}

// IsHexDigit reports whether one character can appear in a hexadecimal token.
// Input is one wide character; processing delegates to iswxdigit; output is
// true for 0-9/a-f/A-F and false otherwise.
bool IsHexDigit(wchar_t ch) {
    return std::iswxdigit(ch) != 0;
}

// AppendParseFailure writes a consistent parser diagnostic. Inputs are the
// logical field name and reason; processing concatenates them; output is stored
// in errorText and no value is returned.
void AppendParseFailure(const wchar_t* fieldName, const wchar_t* reason, std::wstring& errorText) {
    errorText = fieldName ? fieldName : L"Field";
    errorText += L": ";
    errorText += reason ? reason : L"invalid value";
}

} // namespace

bool ParseUnsignedInteger(const std::wstring& text,
    std::uint64_t maxValue,
    const wchar_t* fieldName,
    std::uint64_t& value,
    std::wstring& errorText) {
    const std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        AppendParseFailure(fieldName, L"value is required", errorText);
        return false;
    }

    int base = 10;
    std::size_t offset = 0;
    if (trimmed.size() > 2 && trimmed[0] == L'0' && (trimmed[1] == L'x' || trimmed[1] == L'X')) {
        base = 16;
        offset = 2;
    }

    if (offset >= trimmed.size()) {
        AppendParseFailure(fieldName, L"missing digits", errorText);
        return false;
    }

    std::uint64_t parsed = 0;
    for (std::size_t index = offset; index < trimmed.size(); ++index) {
        const wchar_t ch = trimmed[index];
        unsigned digit = 0;
        if (ch >= L'0' && ch <= L'9') {
            digit = static_cast<unsigned>(ch - L'0');
        } else if (base == 16 && ch >= L'a' && ch <= L'f') {
            digit = static_cast<unsigned>(10 + ch - L'a');
        } else if (base == 16 && ch >= L'A' && ch <= L'F') {
            digit = static_cast<unsigned>(10 + ch - L'A');
        } else {
            AppendParseFailure(fieldName, L"contains invalid characters", errorText);
            return false;
        }

        if (digit >= static_cast<unsigned>(base)) {
            AppendParseFailure(fieldName, L"contains invalid digits for its base", errorText);
            return false;
        }

        const std::uint64_t limit = (maxValue - digit) / static_cast<std::uint64_t>(base);
        if (parsed > limit) {
            AppendParseFailure(fieldName, L"value is out of range", errorText);
            return false;
        }
        parsed = parsed * static_cast<std::uint64_t>(base) + digit;
    }

    value = parsed;
    errorText.clear();
    return true;
}

bool ParseReadRequest(const std::wstring& processIdText,
    const std::wstring& addressText,
    const std::wstring& lengthText,
    DriverMemoryReadRequest& request,
    std::wstring& errorText) {
    std::uint64_t processId = 0;
    std::uint64_t address = 0;
    std::uint64_t length = 0;

    if (!ParseUnsignedInteger(processIdText, std::numeric_limits<DWORD>::max(), L"PID", processId, errorText)) {
        return false;
    }
    if (processId == 0) {
        AppendParseFailure(L"PID", L"must be non-zero", errorText);
        return false;
    }
    if (!ParseUnsignedInteger(addressText, std::numeric_limits<std::uint64_t>::max(), L"Address", address, errorText)) {
        return false;
    }
    if (!ParseUnsignedInteger(lengthText, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()), L"Length", length, errorText)) {
        return false;
    }
    if (length == 0) {
        AppendParseFailure(L"Length", L"must be non-zero", errorText);
        return false;
    }

    request.processId = static_cast<DWORD>(processId);
    request.address = address;
    request.length = static_cast<std::size_t>(length);
    errorText.clear();
    return true;
}

bool ParseWriteRequest(const std::wstring& processIdText,
    const std::wstring& addressText,
    const std::wstring& hexText,
    DriverMemoryWriteRequest& request,
    std::wstring& errorText) {
    std::uint64_t processId = 0;
    std::uint64_t address = 0;

    if (!ParseUnsignedInteger(processIdText, std::numeric_limits<DWORD>::max(), L"PID", processId, errorText)) {
        return false;
    }
    if (processId == 0) {
        AppendParseFailure(L"PID", L"must be non-zero", errorText);
        return false;
    }
    if (!ParseUnsignedInteger(addressText, std::numeric_limits<std::uint64_t>::max(), L"Address", address, errorText)) {
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!ParseHexBytes(hexText, bytes, errorText)) {
        return false;
    }
    if (bytes.empty()) {
        AppendParseFailure(L"Hex bytes", L"at least one byte is required", errorText);
        return false;
    }

    request.processId = static_cast<DWORD>(processId);
    request.address = address;
    request.bytes = std::move(bytes);
    errorText.clear();
    return true;
}

bool ParseHexBytes(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorText) {
    bytes.clear();

    std::wistringstream stream(text);
    std::wstring token;
    std::size_t tokenIndex = 0;
    while (stream >> token) {
        ++tokenIndex;
        if (token.size() > 2 && token[0] == L'0' && (token[1] == L'x' || token[1] == L'X')) {
            token.erase(0, 2);
        }
        if (token.empty() || token.size() > 2 || !std::all_of(token.begin(), token.end(), IsHexDigit)) {
            errorText = L"Hex bytes: token ";
            errorText += std::to_wstring(tokenIndex);
            errorText += L" is not a valid byte";
            bytes.clear();
            return false;
        }

        unsigned parsed = 0;
        for (wchar_t ch : token) {
            unsigned digit = 0;
            if (ch >= L'0' && ch <= L'9') {
                digit = static_cast<unsigned>(ch - L'0');
            } else if (ch >= L'a' && ch <= L'f') {
                digit = static_cast<unsigned>(10 + ch - L'a');
            } else {
                digit = static_cast<unsigned>(10 + ch - L'A');
            }
            parsed = (parsed << 4) | digit;
        }
        bytes.push_back(static_cast<std::uint8_t>(parsed));
    }

    errorText.clear();
    return true;
}

std::wstring FormatHexBytesForDisplay(const std::vector<std::uint8_t>& bytes, std::size_t bytesPerLine) {
    if (bytes.empty()) {
        return std::wstring();
    }
    if (bytesPerLine == 0) {
        bytesPerLine = 16;
    }

    std::wostringstream out;
    out << std::uppercase << std::hex << std::setfill(L'0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index > 0) {
            if ((index % bytesPerLine) == 0) {
                out << L"\r\n";
            } else {
                out << L' ';
            }
        }
        out << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return out.str();
}

} // namespace Ksword::Features::Memory
