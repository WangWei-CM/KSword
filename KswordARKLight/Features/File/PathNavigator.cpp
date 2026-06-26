#include "PathNavigator.h"

#include <algorithm>

namespace Ksword::Features::File {
namespace {

// TrimWhitespace removes leading and trailing ASCII/Unicode whitespace from a
// path-like string. Input is any string; output keeps interior characters.
std::wstring TrimWhitespace(const std::wstring& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    });
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    }).base();
    return std::wstring(first, last);
}

// TrimWrappingQuotes removes one matching pair of command-line style quotes.
// Input is a trimmed string; output is unquoted only when both ends match.
std::wstring TrimWrappingQuotes(const std::wstring& value) {
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// ExpandEnvironmentPath expands %VAR% fragments using ExpandEnvironmentStringsW.
// Input is a user supplied path; output falls back to the input if expansion
// fails or if the result is unexpectedly empty.
std::wstring ExpandEnvironmentPath(const std::wstring& value) {
    const DWORD needed = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }
    std::wstring expanded(needed, L'\0');
    const DWORD written = ::ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
    if (written == 0 || written > needed) {
        return value;
    }
    if (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }
    return expanded.empty() ? value : expanded;
}

// EndsWithSeparator reports whether a path already ends with a Windows path
// separator. Input is a path string; output is a simple boolean.
bool EndsWithSeparator(const std::wstring& path) {
    return !path.empty() && (path.back() == L'\\' || path.back() == L'/');
}

} // namespace

PathNavigator::PathNavigator() = default;

const std::wstring& PathNavigator::currentPath() const {
    return currentPath_;
}

std::wstring PathNavigator::navigateTo(const std::wstring& path) {
    const std::wstring normalized = normalizeDirectoryPath(path);
    if (normalized != currentPath_) {
        pushHistory(currentPath_);
        currentPath_ = normalized;
        forwardStack_.clear();
    }
    return currentPath_;
}

std::wstring PathNavigator::navigateUp() {
    return navigateTo(parentPath(currentPath_));
}

bool PathNavigator::navigateBack() {
    if (backStack_.empty()) {
        return false;
    }
    forwardStack_.push_back(currentPath_);
    currentPath_ = backStack_.back();
    backStack_.pop_back();
    return true;
}

bool PathNavigator::navigateForward() {
    if (forwardStack_.empty()) {
        return false;
    }
    backStack_.push_back(currentPath_);
    currentPath_ = forwardStack_.back();
    forwardStack_.pop_back();
    return true;
}

bool PathNavigator::canNavigateBack() const {
    return !backStack_.empty();
}

bool PathNavigator::canNavigateForward() const {
    return !forwardStack_.empty();
}

std::wstring PathNavigator::normalizeDirectoryPath(const std::wstring& path) {
    std::wstring normalized = ExpandEnvironmentPath(TrimWrappingQuotes(TrimWhitespace(path)));
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized == L"此电脑" || normalized == L"计算机") {
        return {};
    }
    if (normalized == L"." || normalized == L".\\") {
        wchar_t buffer[MAX_PATH]{};
        const DWORD len = ::GetCurrentDirectoryW(MAX_PATH, buffer);
        if (len > 0 && len < MAX_PATH) {
            normalized.assign(buffer, len);
        }
    }
    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    if (normalized.size() == 2 && normalized[1] == L':') {
        normalized.push_back(L'\\');
    }
    return normalized;
}

std::wstring PathNavigator::parentPath(const std::wstring& path) {
    const std::wstring normalized = normalizeDirectoryPath(path);
    if (normalized.empty() || isDriveRoot(normalized)) {
        return {};
    }
    if (normalized.size() <= 2) {
        return {};
    }

    std::wstring trimmed = normalized;
    while (trimmed.size() > 3 && trimmed.back() == L'\\') {
        trimmed.pop_back();
    }
    const std::wstring::size_type slash = trimmed.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return {};
    }
    if (slash == 2 && trimmed.size() >= 3 && trimmed[1] == L':') {
        return trimmed.substr(0, 3);
    }
    if (slash == 0) {
        return L"\\";
    }
    return trimmed.substr(0, slash);
}

std::wstring PathNavigator::joinChildPath(const std::wstring& directory, const std::wstring& childName) {
    if (directory.empty()) {
        return childName;
    }
    if (EndsWithSeparator(directory)) {
        return directory + childName;
    }
    return directory + L"\\" + childName;
}

std::wstring PathNavigator::makeSearchPattern(const std::wstring& directory) {
    if (directory.empty()) {
        return L"*";
    }
    return joinChildPath(directory, L"*");
}

bool PathNavigator::isDriveRoot(const std::wstring& path) {
    return path.size() == 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' &&
        path[2] == L'\\';
}

void PathNavigator::pushHistory(const std::wstring& path) {
    if (!backStack_.empty() && backStack_.back() == path) {
        return;
    }
    backStack_.push_back(path);
}

} // namespace Ksword::Features::File
