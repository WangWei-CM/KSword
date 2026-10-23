#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ks::misc::detail
{
    // Full editable document: fixed 32-byte lines, uppercase, no trailing empty block.
    inline std::wstring formatControllerHex(const std::vector<std::uint8_t>& bytes)
    {
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        std::wstring text;
        text.reserve(bytes.size() * 3U);
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            if (index != 0U) text.push_back(index % 32U == 0U ? L'\n' : L' ');
            text.push_back(digits[bytes[index] >> 4U]);
            text.push_back(digits[bytes[index] & 0xFU]);
        }
        return text;
    }

    // Reject malformed input instead of silently discarding invalid characters
    // or odd nibbles as QByteArray::fromHex does.
    inline bool parseControllerHex(const std::wstring& text,
        const std::uint32_t expectedLength, std::vector<std::uint8_t>& bytes)
    {
        bytes.clear();
        if (expectedLength == 0U || expectedLength > 256U * 1024U ||
            text.size() > static_cast<std::size_t>(expectedLength) * 4U + 1024U)
            return false;
        bytes.reserve(expectedLength);
        int highNibble = -1;
        for (const wchar_t value : text)
        {
            if (value == L' ' || value == L'\t' || value == L'\r' || value == L'\n')
                continue;
            const int nibble = value >= L'0' && value <= L'9' ? value - L'0'
                : value >= L'a' && value <= L'f' ? value - L'a' + 10
                : value >= L'A' && value <= L'F' ? value - L'A' + 10 : -1;
            if (nibble < 0 || bytes.size() >= expectedLength)
            {
                bytes.clear();
                return false;
            }
            if (highNibble < 0) highNibble = nibble;
            else
            {
                bytes.push_back(static_cast<std::uint8_t>((highNibble << 4) | nibble));
                highNibble = -1;
            }
        }
        if (highNibble >= 0 || bytes.size() != expectedLength)
        {
            bytes.clear();
            return false;
        }
        return true;
    }
}
