#include "StartupText.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <vector>
namespace ks::r3::startup::detail {
std::string Utf16ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string out(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), required, nullptr, nullptr) != required) return {};
    return out;
}
std::wstring Utf8ToUtf16(const std::string& text) {
    if (text.empty()) return {};
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring out(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), required) != required) return {};
    return out;
}
std::string TrimCopy(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; }).base();
    return first >= last ? std::string{} : std::string(first, last);
}
}
