#pragma once
#include <string>
namespace ks::r3::startup::detail {
std::string Utf16ToUtf8(const std::wstring& text);
std::wstring Utf8ToUtf16(const std::string& text);
std::string TrimCopy(const std::string& text);
}
