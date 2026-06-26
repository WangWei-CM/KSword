#include "EtwEventModel.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace Ksword::Features::Monitor {

EtwEventModel::EtwEventModel(const std::size_t maxRows)
    : maxRows_(maxRows == 0 ? 1 : maxRows) {}

void EtwEventModel::append(const EtwEvent& eventRow) {
    std::lock_guard<std::mutex> lock(mutex_);
    rows_.push_back(eventRow);
    if (rows_.size() > maxRows_) {
        const std::size_t extraCount = rows_.size() - maxRows_;
        rows_.erase(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(extraCount));
    }
}

void EtwEventModel::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    rows_.clear();
}

std::vector<EtwEvent> EtwEventModel::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rows_;
}

std::size_t EtwEventModel::rowCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rows_.size();
}

std::wstring GuidToString(const GUID& value) {
    wchar_t buffer[64] = {};
    const int written = std::swprintf(
        buffer,
        std::size(buffer),
        L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        static_cast<unsigned long>(value.Data1),
        value.Data2,
        value.Data3,
        value.Data4[0],
        value.Data4[1],
        value.Data4[2],
        value.Data4[3],
        value.Data4[4],
        value.Data4[5],
        value.Data4[6],
        value.Data4[7]);
    return written > 0 ? std::wstring(buffer, static_cast<std::size_t>(written)) : L"{}";
}

std::wstring FileTimeToLocalText(const LARGE_INTEGER& timestamp) {
    FILETIME utcTime{};
    utcTime.dwLowDateTime = timestamp.LowPart;
    utcTime.dwHighDateTime = static_cast<DWORD>(timestamp.HighPart);

    FILETIME localTime{};
    SYSTEMTIME systemTime{};
    if (::FileTimeToLocalFileTime(&utcTime, &localTime) == FALSE ||
        ::FileTimeToSystemTime(&localTime, &systemTime) == FALSE) {
        return L"-";
    }

    wchar_t buffer[64] = {};
    const int written = std::swprintf(
        buffer,
        std::size(buffer),
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        systemTime.wYear,
        systemTime.wMonth,
        systemTime.wDay,
        systemTime.wHour,
        systemTime.wMinute,
        systemTime.wSecond,
        systemTime.wMilliseconds);
    return written > 0 ? std::wstring(buffer, static_cast<std::size_t>(written)) : L"-";
}

} // namespace Ksword::Features::Monitor
