#include "string.h"

// Windows 字符串转换依赖 Win32 API。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm> // std::find_if_not：用于 trim 逻辑。
#include <cctype>    // std::isspace：判断空白字符。
#include <cstdio>    // std::snprintf：格式化时间文本。
#include <vector>    // std::vector<wchar_t>/char：转换缓冲区。

namespace ks::str
{
    std::string Utf16ToUtf8(const std::wstring& utf16Text)
    {
        // 空串快速返回，避免调用 API。
        if (utf16Text.empty())
        {
            return std::string();
        }

        // 第一步查询 UTF-8 目标长度（含结尾 '\0'）。
        const int targetLengthWithNull = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            utf16Text.c_str(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);

        if (targetLengthWithNull <= 0)
        {
            return std::string();
        }

        // 分配缓冲区并执行转换。
        std::vector<char> utf8Buffer(static_cast<std::size_t>(targetLengthWithNull), '\0');
        const int convertedLengthWithNull = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            utf16Text.c_str(),
            -1,
            utf8Buffer.data(),
            targetLengthWithNull,
            nullptr,
            nullptr);

        if (convertedLengthWithNull <= 0)
        {
            return std::string();
        }

        // 去掉尾部 '\0'，构造 std::string。
        return std::string(utf8Buffer.data());
    }

    std::wstring Utf8ToUtf16(const std::string& utf8Text)
    {
        // 空串快速返回。
        if (utf8Text.empty())
        {
            return std::wstring();
        }

        // 第一步查询 UTF-16 目标长度（含结尾 L'\0'）。
        const int targetLengthWithNull = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8Text.c_str(),
            -1,
            nullptr,
            0);

        if (targetLengthWithNull <= 0)
        {
            return std::wstring();
        }

        // 分配缓冲区并执行转换。
        std::vector<wchar_t> utf16Buffer(static_cast<std::size_t>(targetLengthWithNull), L'\0');
        const int convertedLengthWithNull = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8Text.c_str(),
            -1,
            utf16Buffer.data(),
            targetLengthWithNull);

        if (convertedLengthWithNull <= 0)
        {
            return std::wstring();
        }

        // 去掉尾部 L'\0'，构造 std::wstring。
        return std::wstring(utf16Buffer.data());
    }

    std::string TrimCopy(const std::string& textValue)
    {
        // 局部 lambda：统一判定“是否空白字符”。
        const auto isSpace = [](const unsigned char charValue) -> bool
            {
                return std::isspace(charValue) != 0;
            };

        // 左 trim：找到第一个非空白位置。
        auto beginIterator = std::find_if_not(
            textValue.begin(),
            textValue.end(),
            [&](const char currentChar) { return isSpace(static_cast<unsigned char>(currentChar)); });

        // 若全为空白，直接返回空串。
        if (beginIterator == textValue.end())
        {
            return std::string();
        }

        // 右 trim：从尾部反向找到第一个非空白位置。
        auto reverseEndIterator = std::find_if_not(
            textValue.rbegin(),
            textValue.rend(),
            [&](const char currentChar) { return isSpace(static_cast<unsigned char>(currentChar)); });

        const auto endIterator = reverseEndIterator.base();
        return std::string(beginIterator, endIterator);
    }

    std::uint64_t FileTimeToUint64(const std::uint32_t highPart, const std::uint32_t lowPart)
    {
        // 按位拼接 FILETIME 的高低 32 位。
        return (static_cast<std::uint64_t>(highPart) << 32) | static_cast<std::uint64_t>(lowPart);
    }

    std::string FileTime100nsToLocalText(const std::uint64_t fileTime100ns)
    {
        // 把 64 位值拆分回 FILETIME 结构体。
        FILETIME utcFileTime{};
        utcFileTime.dwLowDateTime = static_cast<DWORD>(fileTime100ns & 0xFFFFFFFFULL);
        utcFileTime.dwHighDateTime = static_cast<DWORD>((fileTime100ns >> 32) & 0xFFFFFFFFULL);

        // UTC FILETIME -> 本地 FILETIME。
        FILETIME localFileTime{};
        if (::FileTimeToLocalFileTime(&utcFileTime, &localFileTime) == FALSE)
        {
            return std::string();
        }

        // FILETIME -> SYSTEMTIME，便于格式化输出。
        SYSTEMTIME localSystemTime{};
        if (::FileTimeToSystemTime(&localFileTime, &localSystemTime) == FALSE)
        {
            return std::string();
        }

        // 使用安全格式化函数构造固定格式时间文本。
        char timeBuffer[64] = {};
        const int writtenLength = std::snprintf(
            timeBuffer,
            sizeof(timeBuffer),
            "%04u-%02u-%02u %02u:%02u:%02u",
            static_cast<unsigned>(localSystemTime.wYear),
            static_cast<unsigned>(localSystemTime.wMonth),
            static_cast<unsigned>(localSystemTime.wDay),
            static_cast<unsigned>(localSystemTime.wHour),
            static_cast<unsigned>(localSystemTime.wMinute),
            static_cast<unsigned>(localSystemTime.wSecond));

        if (writtenLength <= 0)
        {
            return std::string();
        }

        return std::string(timeBuffer);
    }

    void ReplaceAllInPlace(std::string& textValue, const std::string& fromText, const std::string& toText)
    {
        // 空 fromText 会导致死循环，直接返回。
        if (fromText.empty())
        {
            return;
        }

        // 逐段查找并替换，直到无匹配项。
        std::size_t currentPosition = 0;
        while (true)
        {
            currentPosition = textValue.find(fromText, currentPosition);
            if (currentPosition == std::string::npos)
            {
                break;
            }

            textValue.replace(currentPosition, fromText.length(), toText);
            currentPosition += toText.length();
        }
    }
}
