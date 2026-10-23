#include "PrivilegeLegacyReadService.h"
#include <QSet>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Lm.h>
#pragma comment(lib, "Netapi32.lib")
#pragma comment(lib, "Advapi32.lib")

namespace ks::privilege::legacy
{
    namespace
    {
        bool stopped(const CancelCheck& canceled) { return canceled && canceled(); }
        template<typename Row, typename Reader>
        void collectPages(std::vector<Row>& rows, ReadStatus& status, std::uint64_t& total,
            const std::uint32_t preferredLength, const Reader& reader,
            const ReadPorts& ports, const CancelCheck& canceled)
        {
            status.requested = true;
            QSet<QString> names;
            std::uint64_t resume = 0;
            do
            {
                if (stopped(canceled))
                {
                    status.code = Canceled;
                    status.stage = FailureStage::Canceled;
                    break;
                }
                auto page = reader(resume, preferredLength);
                total = std::max(total, page.total);
                status.code = page.code;
                if (page.code != Success && page.code != MoreData)
                {
                    status.stage = FailureStage::Enumeration;
                    break;
                }
                std::size_t added = 0;
                for (auto& row : page.rows)
                {
                    const QString key = row.name.toCaseFolded();
                    if (names.contains(key)) continue;
                    names.insert(key);
                    rows.push_back(std::move(row));
                    ++added;
                }
                if (page.code == Success)
                {
                    status.complete = true;
                    break;
                }
                if (page.resume == resume || added == 0)
                {
                    status.code = InvalidData;
                    status.stage = FailureStage::Pagination;
                    break;
                }
                resume = page.resume;
            } while (true);
            if (status.code != Success && ports.error) status.nativeError = ports.error(status.code);
        }
        struct NetBuffer
        {
            LPBYTE value = nullptr;
            ~NetBuffer() { if (value) ::NetApiBufferFree(value); }
        };
        struct TokenHandle
        {
            HANDLE value = nullptr;
            ~TokenHandle() { if (value) ::CloseHandle(value); }
        };
        ReadPorts nativePorts(const unsigned userLevel)
        {
            ReadPorts ports;
            ports.error = nativeErrorText;
            ports.users = [userLevel](const std::uint64_t resumeValue, const std::uint32_t preferred)
            {
                ValuePage<LocalUser> page;
                DWORD resume = static_cast<DWORD>(resumeValue), count = 0, total = 0;
                NetBuffer buffer;
                page.code = ::NetUserEnum(nullptr, userLevel, FILTER_NORMAL_ACCOUNT, &buffer.value,
                    preferred, &count, &total, &resume);
                page.resume = resume; page.total = total;
                if (page.code == Success || page.code == MoreData)
                {
                    if (count != 0 && buffer.value == nullptr) { page.code = InvalidData; return page; }
                    page.rows.reserve(count);
                    for (DWORD index = 0; index < count; ++index)
                    {
                        if (userLevel == 1)
                        {
                            const auto& source = reinterpret_cast<const USER_INFO_1*>(buffer.value)[index];
                            page.rows.push_back({source.usri1_name ? QString::fromWCharArray(source.usri1_name) : QStringLiteral("<null>"), {},
                                source.usri1_comment ? QString::fromWCharArray(source.usri1_comment) : QString(), source.usri1_flags, 0});
                        }
                        else
                        {
                            const auto& source = reinterpret_cast<const USER_INFO_2*>(buffer.value)[index];
                            page.rows.push_back({source.usri2_name ? QString::fromWCharArray(source.usri2_name) : QStringLiteral("<null>"),
                                source.usri2_full_name ? QString::fromWCharArray(source.usri2_full_name) : QString(),
                                source.usri2_comment ? QString::fromWCharArray(source.usri2_comment) : QString(),
                                source.usri2_flags, source.usri2_last_logon});
                        }
                    }
                }
                return page;
            };
            ports.groups = [](const std::uint64_t resumeValue, const std::uint32_t preferred)
            {
                ValuePage<LocalGroup> page;
                DWORD_PTR resume = static_cast<DWORD_PTR>(resumeValue);
                DWORD count = 0, total = 0;
                NetBuffer buffer;
                page.code = ::NetLocalGroupEnum(nullptr, 1, &buffer.value, preferred, &count, &total, &resume);
                page.resume = resume; page.total = total;
                if (page.code == Success || page.code == MoreData)
                {
                    const auto* data = reinterpret_cast<const LOCALGROUP_INFO_1*>(buffer.value);
                    if (count != 0 && data == nullptr) { page.code = InvalidData; return page; }
                    page.rows.reserve(count);
                    for (DWORD index = 0; index < count; ++index)
                        page.rows.push_back({data[index].lgrpi1_name ? QString::fromWCharArray(data[index].lgrpi1_name) : QStringLiteral("<null>"),
                            data[index].lgrpi1_comment ? QString::fromWCharArray(data[index].lgrpi1_comment) : QString()});
                }
                return page;
            };
            ports.token = [](const CancelCheck& canceled)
            {
                TokenRead result;
                result.status.requested = true;
                TokenHandle token;
                const auto fail = [&result](const DWORD code, const FailureStage stage)
                {
                    result.status.code = code;
                    result.status.stage = stage;
                    result.status.nativeError = nativeErrorText(code);
                };
                if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token.value))
                {
                    fail(::GetLastError(), FailureStage::OpenToken);
                    return result;
                }
                DWORD bytes = 0;
                ::GetTokenInformation(token.value, TokenPrivileges, nullptr, 0, &bytes);
                if (bytes < offsetof(TOKEN_PRIVILEGES, Privileges))
                {
                    fail(::GetLastError(), FailureStage::TokenInformation);
                    return result;
                }
                std::vector<unsigned char> buffer(bytes);
                if (!::GetTokenInformation(token.value, TokenPrivileges, buffer.data(), bytes, &bytes))
                {
                    fail(::GetLastError(), FailureStage::TokenInformation);
                    return result;
                }
                if (bytes < offsetof(TOKEN_PRIVILEGES, Privileges) || bytes > buffer.size())
                {
                    fail(ERROR_INVALID_DATA, FailureStage::TokenInformation);
                    return result;
                }
                const auto* data = reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
                const std::size_t maximum = (static_cast<std::size_t>(bytes) - offsetof(TOKEN_PRIVILEGES, Privileges)) / sizeof(LUID_AND_ATTRIBUTES);
                if (data->PrivilegeCount > maximum)
                {
                    fail(ERROR_INVALID_DATA, FailureStage::TokenInformation);
                    return result;
                }
                result.rows.reserve(data->PrivilegeCount);
                for (DWORD index = 0; index < data->PrivilegeCount; ++index)
                {
                    if (stopped(canceled)) { fail(ERROR_CANCELLED, FailureStage::Canceled); return result; }
                    const auto& raw = data->Privileges[index];
                    TokenPrivilege entry;
                    entry.attributes = raw.Attributes;
                    DWORD length = 0;
                    ::LookupPrivilegeNameW(nullptr, const_cast<LUID*>(&raw.Luid), nullptr, &length);
                    std::vector<wchar_t> name(static_cast<std::size_t>(length) + 1);
                    DWORD available = static_cast<DWORD>(name.size());
                    if (::LookupPrivilegeNameW(nullptr, const_cast<LUID*>(&raw.Luid), name.data(), &available))
                    {
                        entry.name = QString::fromWCharArray(name.data(), static_cast<int>(available));
                        entry.nameKnown = true;
                        DWORD displayLength = 0, language = 0;
                        ::LookupPrivilegeDisplayNameW(nullptr, name.data(), nullptr, &displayLength, &language);
                        std::vector<wchar_t> display(static_cast<std::size_t>(displayLength) + 1);
                        available = static_cast<DWORD>(display.size());
                        if (::LookupPrivilegeDisplayNameW(nullptr, name.data(), display.data(), &available, &language))
                        {
                            entry.displayName = QString::fromWCharArray(display.data(), static_cast<int>(available));
                            entry.displayNameKnown = true;
                        }
                    }
                    result.rows.push_back(std::move(entry));
                }
                result.status.complete = true;
                return result;
            };
            return ports;
        }
    }
    ReadSnapshot collectSnapshot(const ReadRequest& request, const ReadPorts& ports, const CancelCheck& canceled)
    {
        ReadSnapshot result;
        if (request.fields & Users)
            collectPages(result.users, result.userStatus, result.totalUsers, request.preferredLength, ports.users, ports, canceled);
        if (request.fields & Groups)
            collectPages(result.groups, result.groupStatus, result.totalGroups, request.preferredLength, ports.groups, ports, canceled);
        if (request.fields & ProcessPrivileges)
        {
            if (stopped(canceled))
                result.tokenStatus = {true, false, Canceled, FailureStage::Canceled, ports.error ? ports.error(Canceled) : QString()};
            else
            {
                auto token = ports.token(canceled);
                result.privileges = std::move(token.rows);
                result.tokenStatus = std::move(token.status);
            }
        }
        return result;
    }
    ReadSnapshot queryNative(const ReadRequest& request, const CancelCheck& canceled)
    {
        if ((request.fields & Users) && request.userLevel != 1 && request.userLevel != 2)
        {
            ReadSnapshot result;
            result.userStatus = {true, false, InvalidData, FailureStage::Enumeration, nativeErrorText(InvalidData)};
            return result;
        }
        return collectSnapshot(request, nativePorts(request.userLevel), canceled);
    }
    QString nativeErrorText(const std::uint32_t code)
    {
        wchar_t* buffer = nullptr;
        const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        const QString text = length != 0 && buffer ? QString::fromWCharArray(buffer).trimmed() : QString();
        if (buffer) ::LocalFree(buffer);
        return text.isEmpty() ? QString::number(code) : QStringLiteral("%1 (code=%2)").arg(text).arg(code);
    }
    UserIdentity queryNativeIdentity(const QString& name)
    {
        UserIdentity result;
        NetBuffer buffer;
        result.code = ::NetUserGetInfo(nullptr, reinterpret_cast<LPCWSTR>(name.utf16()), 4, &buffer.value);
        if (result.code != Success)
        {
            result.nativeError = nativeErrorText(result.code);
            return result;
        }
        const auto* data = reinterpret_cast<const USER_INFO_4*>(buffer.value);
        if (data == nullptr || data->usri4_name == nullptr || data->usri4_user_sid == nullptr || !::IsValidSid(data->usri4_user_sid))
        {
            result.code = InvalidData;
            result.nativeError = nativeErrorText(result.code);
            return result;
        }
        result.name = QString::fromWCharArray(data->usri4_name);
        result.sid = QByteArray(reinterpret_cast<const char*>(data->usri4_user_sid), static_cast<qsizetype>(::GetLengthSid(data->usri4_user_sid)));
        result.valid = !result.sid.isEmpty();
        return result;
    }
    WriteStatus createNativeUser(const QString& name, const std::wstring& password)
    {
        USER_INFO_1 data{};
        data.usri1_name = const_cast<wchar_t*>(reinterpret_cast<LPCWSTR>(name.utf16()));
        data.usri1_password = const_cast<wchar_t*>(password.c_str());
        data.usri1_priv = USER_PRIV_USER;
        data.usri1_flags = UF_SCRIPT;
        DWORD parameter = 0;
        const auto code = ::NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&data), &parameter);
        return {code, parameter};
    }
    WriteStatus resetNativePassword(const QString& name, const std::wstring& password)
    {
        USER_INFO_1003 data{};
        data.usri1003_password = const_cast<wchar_t*>(password.c_str());
        return {::NetUserSetInfo(nullptr, reinterpret_cast<LPCWSTR>(name.utf16()), 1003, reinterpret_cast<LPBYTE>(&data), nullptr), 0};
    }
}
