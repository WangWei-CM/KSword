#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ks::privilege::legacy
{
    inline constexpr std::uint32_t Success = 0;
    inline constexpr std::uint32_t MoreData = 234;
    inline constexpr std::uint32_t InvalidData = 13;
    inline constexpr std::uint32_t Canceled = 1223;
    enum ReadField : unsigned { Users = 1U, Groups = 2U, ProcessPrivileges = 4U, All = 7U };
    enum class FailureStage { None, Enumeration, Pagination, OpenToken, TokenInformation, Canceled };
    struct ReadStatus
    {
        bool requested = false;
        bool complete = false;
        std::uint32_t code = Success;
        FailureStage stage = FailureStage::None;
        QString nativeError;
    };
    struct LocalUser
    {
        QString name;
        QString fullName;
        QString comment;
        std::uint32_t flags = 0;
        std::uint32_t lastLogon = 0;
    };
    struct LocalGroup { QString name; QString comment; };
    struct TokenPrivilege
    {
        QString name;
        QString displayName;
        std::uint32_t attributes = 0;
        bool nameKnown = false;
        bool displayNameKnown = false;
    };
    template<typename Row> struct ValuePage
    {
        std::vector<Row> rows;
        std::uint64_t resume = 0;
        std::uint64_t total = 0;
        std::uint32_t code = Success;
    };
    struct TokenRead { std::vector<TokenPrivilege> rows; ReadStatus status; };
    // Level 2 preserves the legacy fields; level 1 lets existing basic account consumers share the raw service.
    struct ReadRequest { unsigned fields = All; std::uint32_t preferredLength = 65536; unsigned userLevel = 2; };
    struct ReadSnapshot
    {
        std::vector<LocalUser> users;
        std::vector<LocalGroup> groups;
        std::vector<TokenPrivilege> privileges;
        ReadStatus userStatus;
        ReadStatus groupStatus;
        ReadStatus tokenStatus;
        std::uint64_t totalUsers = 0;
        std::uint64_t totalGroups = 0;
    };
    using CancelCheck = std::function<bool()>;
    struct ReadPorts
    {
        std::function<ValuePage<LocalUser>(std::uint64_t, std::uint32_t)> users;
        std::function<ValuePage<LocalGroup>(std::uint64_t, std::uint32_t)> groups;
        std::function<TokenRead(const CancelCheck&)> token;
        std::function<QString(std::uint32_t)> error;
    };
    // Value aggregation is shared by consumers and the fake source. No UI or language manager.
    ReadSnapshot collectSnapshot(const ReadRequest& request, const ReadPorts& ports,
        const CancelCheck& canceled = {});
    ReadSnapshot queryNative(const ReadRequest& request, const CancelCheck& canceled = {});
    QString nativeErrorText(std::uint32_t code);

    struct UserIdentity
    {
        QString name;
        QByteArray sid;
        std::uint32_t code = Success;
        bool valid = false;
        QString nativeError;
    };
    UserIdentity queryNativeIdentity(const QString& name);
    struct WriteStatus { std::uint32_t code = Success; std::uint32_t parameter = 0; };
    WriteStatus createNativeUser(const QString& name, const std::wstring& password);
    WriteStatus resetNativePassword(const QString& name, const std::wstring& password);
}
